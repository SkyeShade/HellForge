using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Layout;
using HellForge.Core;

namespace HellForge.Launcher;

// Profile > Account sync, after a capture: the review of what to import (one row per AccountImportSettings category, with
// the profile's and the account's side), the import itself, and profile backups with Restore. Previews, imports and
// restores run the local backend's profile commands (HellForge.LocalBackend AccountCommands), which hold the profile's
// lease, verify the snapshot's seal and back the profile up before changing it.
public partial class MainWindow
{
    // Include is null for a category without a checkbox (warbond unlocks), which its None choice leaves out instead.
    readonly List<(ImportCategory Category, CheckBox? Include, ComboBox Rule)> importRows = [];
    LocalProfile? importProfile;
    bool importBusy;

    static string RuleLabel(ImportCategory category, ImportRule rule) => (category.Id, rule) switch
    {
        ("warbondItems", ImportRule.Merge) => "As in vanilla",
        ("warbondItems", ImportRule.All) => "Every item",
        (_, ImportRule.None) => "None",
        (_, ImportRule.Higher) => "Keep the higher",
        (_, ImportRule.Merge) => "Merge both",
        _ => "Use vanilla"
    };

    void InitializeAccountImport()
    {
        ReviewImportButton.Click += async (_, _) => await ShowReviewAsync(Selected);
        ImportApplyButton.Click += async (_, _) => await ApplyReviewAsync();
        ImportCancelButton.Click += (_, _) =>
        {
            ImportPanel.IsVisible = false;
            SetSyncStatus("Nothing imported. Premium items you own still follow the sync.", Light);
        };
        RestoreBackupButton.Click += async (_, _) => await RestoreBackupAsync();
    }

    // The review button when the selected profile has a snapshot; the backups it has.
    void UpdateImportControls()
    {
        if (paths.InstallRoot == null) return;
        var profile = Selected;
        ReviewImportButton.IsVisible = InventorySync.HasSnapshot(InventorySync.SnapshotDirectory(paths.InstallRoot, profile.Name));
        if (importProfile != null && importProfile.Name != profile.Name) ImportPanel.IsVisible = false;
        var backups = ProfileBackups.List(profile.Folder);
        BackupCombo.ItemsSource = backups.Select(b => b.SavedUtc.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) + " — before " + b.Reason).ToList();
        BackupCombo.SelectedIndex = backups.Count > 0 ? 0 : -1;
        BackupCombo.IsVisible = RestoreBackupButton.IsVisible = backups.Count > 0;
    }

    // After a complete sync (inventory and progression): the review, or the saved choices.
    async Task AfterCaptureAsync(LocalProfile profile)
    {
        if (!AccountImportSettings.Load(AccountImportSettings.PathIn(profile.Folder)).AutoApply)
        {
            SetSyncStatus("Synced. Choose what to import below.", Green);
            await ShowReviewAsync(profile);
            return;
        }
        if (await RunImportAsync(profile)) SetSyncStatus("Synced and imported with your saved choices. A backup was made first.", Green);
    }

    async Task ShowReviewAsync(LocalProfile profile)
    {
        if (importBusy) return;
        if (ImportRefusal() is { } refusal)
        {
            SetSyncStatus(refusal, Orange);
            return;
        }
        importBusy = true;
        var (code, output, error) = await RunBackendAsync("profile", "import-preview", profile.Folder, profile.Template);
        importBusy = false;
        if (code != 0)
        {
            SetSyncStatus(BackendMessage(error, "The snapshot could not be read."), Red);
            return;
        }
        try
        {
            using var preview = JsonDocument.Parse(output);
            BuildReview(profile, preview.RootElement);
        }
        catch (Exception problem) when (problem is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            SetSyncStatus("The backend's review could not be read: " + problem.Message, Red);
        }
    }

    void BuildReview(LocalProfile profile, JsonElement preview)
    {
        importProfile = profile;
        var settings = AccountImportSettings.Load(AccountImportSettings.PathIn(profile.Folder));
        var sides = preview.GetProperty("categories");
        ImportPremiumText.Text = "Always synced: " + preview.GetProperty("premium").GetString() + " you own. Super Credits are never imported.";
        ImportRows.Children.Clear();
        importRows.Clear();
        var table = new Grid { ColumnDefinitions = new ColumnDefinitions("1.3*,*,*,Auto") };
        AddReviewHeader(table);
        var hint = preview.TryGetProperty("warbonds", out var warbonds) ? warbonds.GetString() + ". Owning them always follows the sync." : null;
        foreach (var category in AccountImportSettings.Categories)
            AddReviewRow(table, category, sides.GetProperty(category.Id), settings, category.Id == "warbondItems" ? hint : null);
        ImportRows.Children.Add(table);
        ImportAutoBox.IsChecked = settings.AutoApply;
        ImportPanel.IsVisible = true;
    }

    // The review is one table: category, the profile's side, the account's side, and the choice, so the columns line up and
    // a long value wraps in its own column instead of pushing the choice off the row.
    static void AddReviewHeader(Grid table)
    {
        table.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        string[] headers = ["", "HELLFORGE", "VANILLA", "IMPORT"];
        for (var column = 0; column < headers.Length; column++)
            AddCell(table, new TextBlock { Text = headers[column], Classes = { "muted" }, FontSize = 11, FontWeight = Avalonia.Media.FontWeight.SemiBold,
                Margin = new Avalonia.Thickness(column == 0 ? 0 : 12, 0, 0, 6) }, 0, column);
    }

    void AddReviewRow(Grid table, ImportCategory category, JsonElement side, AccountImportSettings settings, string? hint)
    {
        var row = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        var divider = new Border { BorderBrush = new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.Parse("#2E3138")), BorderThickness = new Avalonia.Thickness(0, 1, 0, 0) };
        Grid.SetColumnSpan(divider, 4);
        AddCell(table, divider, row, 0);
        var rule = new ComboBox
        {
            ItemsSource = category.Rules.Select(r => RuleLabel(category, r)).ToList(), SelectedIndex = category.Rules.ToList().IndexOf(settings.RuleFor(category.Id)),
            IsEnabled = category.Rules.Count > 1, Width = 170, VerticalAlignment = VerticalAlignment.Center, Margin = new Avalonia.Thickness(12, 8, 0, 8)
        };
        var include = category.Optional ? new CheckBox { IsChecked = settings.Includes(category.Id), VerticalAlignment = VerticalAlignment.Center } : null;
        if (include != null)
        {
            rule.IsEnabled &= include.IsChecked == true;
            include.IsCheckedChanged += (_, _) => rule.IsEnabled = category.Rules.Count > 1 && include.IsChecked == true;
        }
        AddCell(table, CategoryCell(category, include, hint), row, 0);
        AddCell(table, ValueCell(side.GetProperty("hellforge").GetString()), row, 1);
        AddCell(table, ValueCell(side.GetProperty("vanilla").GetString()), row, 2);
        AddCell(table, rule, row, 3);
        importRows.Add((category, include, rule));
    }

    // The category's name, with its checkbox when it has one (the name toggles it) and a note under it.
    static Control CategoryCell(ImportCategory category, CheckBox? include, string? hint)
    {
        var text = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = category.Label, TextWrapping = Avalonia.Media.TextWrapping.Wrap });
        if (hint != null) text.Children.Add(new TextBlock { Text = hint, Classes = { "muted" }, FontSize = 11, TextWrapping = Avalonia.Media.TextWrapping.Wrap });
        if (include == null) return new Border { Child = text, Padding = new Avalonia.Thickness(28, 8, 0, 8) };
        include.Content = text;
        include.Margin = new Avalonia.Thickness(0, 4);
        return include;
    }

    static TextBlock ValueCell(string? value) => new()
    {
        Text = value, Classes = { "muted" }, FontSize = 12, TextWrapping = Avalonia.Media.TextWrapping.Wrap,
        VerticalAlignment = VerticalAlignment.Center, Margin = new Avalonia.Thickness(12, 8, 0, 8)
    };

    static void AddCell(Grid table, Control cell, int row, int column)
    {
        Grid.SetRow(cell, row);
        Grid.SetColumn(cell, column);
        table.Children.Add(cell);
    }

    async Task ApplyReviewAsync()
    {
        if (importProfile is not { } profile || importBusy) return;
        var settings = new AccountImportSettings { AutoApply = ImportAutoBox.IsChecked == true };
        foreach (var (category, include, rule) in importRows)
            settings.Set(category.Id, include?.IsChecked ?? true, category.Rules[Math.Max(0, rule.SelectedIndex)]);
        try { settings.Save(AccountImportSettings.PathIn(profile.Folder)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            SetSyncStatus("Could not save the import choices: " + error.Message, Red);
            return;
        }
        if (!await RunImportAsync(profile)) return;
        ImportPanel.IsVisible = false;
        SetSyncStatus("Imported. The profile was backed up first; Restore backup puts it back.", Green);
    }

    async Task<bool> RunImportAsync(LocalProfile profile)
    {
        if (ImportRefusal() is { } refusal)
        {
            SetSyncStatus(refusal, Orange);
            return false;
        }
        importBusy = true;
        SetSyncStatus("Importing...", Gold);
        var (code, _, error) = await RunBackendAsync("profile", "import", profile.Folder, profile.Template);
        importBusy = false;
        if (code != 0)
        {
            SetSyncStatus(BackendMessage(error, "The import failed."), Red);
            return false;
        }
        Append("Account import applied to profile " + profile.Name + ".", LauncherLine);
        ShowProfile();
        UpdateImportControls();
        return true;
    }

    async Task RestoreBackupAsync()
    {
        var profile = Selected;
        var backups = ProfileBackups.List(profile.Folder);
        if (importBusy || BackupCombo.SelectedIndex < 0 || BackupCombo.SelectedIndex >= backups.Count) return;
        if (ImportRefusal() is { } refusal)
        {
            SetSyncStatus(refusal, Orange);
            return;
        }
        importBusy = true;
        var (code, _, error) = await RunBackendAsync("profile", "restore", profile.Folder, backups[BackupCombo.SelectedIndex].Path);
        importBusy = false;
        if (code != 0)
        {
            SetSyncStatus(BackendMessage(error, "The restore failed."), Red);
            return;
        }
        SetSyncStatus("Restored. The profile it replaced is the newest backup.", Green);
        Append("Profile " + profile.Name + " restored from a backup.", LauncherLine);
        ShowProfile();
        UpdateImportControls();
    }

    // The profile is in use while a session runs; the backend would refuse anyway.
    string? ImportRefusal() => runner.IsRunning ? "Stop the HellForge session first: it holds the profile." : SyncBusy ? "Wait for the sync to finish." : null;

    static string BackendMessage(string error, string fallback)
    {
        var line = error.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).LastOrDefault();
        return line == null ? fallback : line.Replace("HellForge local backend: ", "", StringComparison.Ordinal);
    }

    async Task<(int Code, string Output, string Error)> RunBackendAsync(params string[] arguments)
    {
        var start = new ProcessStartInfo(paths.Backend) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        try
        {
            using var process = Process.Start(start) ?? throw new IOException("The local backend did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            return (process.ExitCode, await output, await error);
        }
        catch (Exception problem) when (problem is IOException or System.ComponentModel.Win32Exception)
        {
            return (-1, "", "HellForge local backend: " + problem.Message);
        }
    }
}
