using System.Diagnostics;
using System.Globalization;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The versions an update kept (KeptVersions), in the sidebar's version dropdown: the running version first, then each
// kept one; picking one switches to it. A switch closes this launcher: HellForge.Cli's versions-switch swaps the app
// from a copy outside app/ once this process has ended, then starts HellForge.exe again, which shows its outcome under
// the dropdown. With nothing kept the version stays a plain label. Settings > Installation sets how many updates keep.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> keptVersions = [];
    // the last switch's outcome, said under the dropdown until this launcher closes
    string? switchOutcome;
    bool versionPickerFilling;
    readonly DispatcherTimer versionBlockCheck = new() { Interval = TimeSpan.FromSeconds(3) };

    void InitializeKeptVersions(InstallLayout install)
    {
        if (!CanSwitchVersions)
        {
            KeepVersionsRow.IsVisible = KeptVersionsText.IsVisible = false;
            return;
        }
        KeepVersionsBox.ItemsSource = Enumerable.Range(KeptVersions.MinCount, KeptVersions.MaxCount - KeptVersions.MinCount + 1).ToList();
        KeepVersionsBox.SelectedItem = KeptVersions.KeepCount(install);
        KeepVersionsBox.SelectionChanged += (_, _) =>
        {
            if (KeepVersionsBox.SelectedItem is not int count || count == KeptVersions.KeepCount(install)) return;
            try { KeptVersions.SetKeepCount(install, count); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { Append("The number of kept versions was not saved: " + error.Message, Red); }
            ShowKeepCost(install);
        };
        ShowKeepCost(install);
        if (KeptVersions.TakeResult(install) is { } last)
        {
            var done = last.Status == "done";
            Append((done ? "" : "Switching versions failed: ") + last.Message, done ? Green : Red);
            switchOutcome = Privacy.Scrub(done ? last.Message : "The last switch did not happen: " + last.Message);
        }
        VersionPicker.SelectionChanged += async (_, _) => await PickVersion(install);
        VersionPicker.DropDownOpened += async (_, _) => await CheckVersionBlockers(install);
        versionBlockCheck.Tick += async (_, _) => await CheckVersionBlockers(install);
        FillVersionPicker(install);
    }

    static bool CanSwitchVersions => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine;

    // A kept version is the app without its Linux build plus a copy of the version's data: about 0.6 GB.
    void ShowKeepCost(InstallLayout install)
    {
        var kept = KeptVersions.List(install);
        KeptVersionsText.Text = $"About 0.6 GB each; {kept.Count} kept now ({KeptVersions.KeptBytes(install) / 1e9:0.0} GB). The oldest goes only when " +
            "an update would keep more, never the version you just came from. Switch versions from the version in the bottom-left corner.";
    }

    // The dropdown's items: the running version (selected), then each kept one, greyed with the reason when it can't run.
    void FillVersionPicker(InstallLayout install)
    {
        keptVersions = KeptVersions.List(install);
        var showPicker = keptVersions.Count > 0;
        VersionText.IsVisible = !showPicker;
        VersionPickerPanel.IsVisible = showPicker;
        if (!showPicker) { versionBlockCheck.Stop(); return; }
        var current = KeptVersions.Identify(install.App);
        VersionBuildText.Text = $"HELLDIVERS 2 build {SupportedBuild.Current.BuildId}";
        var items = new List<ComboBoxItem> { Item("VersionItemCurrent", "HellForge " + current.Text, true) };
        for (var index = 0; index < keptVersions.Count; index++)
        {
            var kept = keptVersions[index];
            var problem = KeptVersions.Problem(install, kept);
            var text = kept.Text + (kept.Identity.IsNewerThan(current) ? "  ·  newer" : "") + "\nkept " +
                kept.KeptUtc.ToLocalTime().ToString("d MMM yyyy, HH:mm", CultureInfo.CurrentCulture) + (problem != null ? "\n" + problem : "") +
                (KeptVersions.GameBuildNote(install, kept) is { } build ? "\n" + build : "");
            items.Add(Item("VersionItem" + (index + 1).ToString(CultureInfo.InvariantCulture), text, problem == null));
        }
        versionPickerFilling = true;
        VersionPicker.ItemsSource = items;
        VersionPicker.SelectedIndex = 0;
        versionPickerFilling = false;
        ShowVersionNote(null);
        versionBlockCheck.Start();
    }

    static ComboBoxItem Item(string id, string text, bool enabled)
    {
        var item = new ComboBoxItem { Name = id, IsEnabled = enabled, Content = text, FontSize = 11 };
        AutomationProperties.SetAutomationId(item, id);
        return item;
    }

    // Why no version can be picked now (the game, a session, a program from the installation), or null; the dropdown is
    // disabled meanwhile and says why under it and in its tooltip.
    async Task CheckVersionBlockers(InstallLayout install)
    {
        if (!VersionPickerPanel.IsVisible) return;
        var blocked = runner.IsRunning || setupRunning ? "Versions don't switch while a session (or setup) runs."
            : await Task.Run(() => KeptVersions.Blockers(install)) is { Count: > 0 } running ? "Close these to switch versions: " + string.Join(", ", running) + "." : null;
        if (blocked != null && VersionPicker.IsDropDownOpen) VersionPicker.IsDropDownOpen = false;
        VersionPicker.IsEnabled = blocked == null;
        ToolTip.SetTip(VersionPicker, blocked);
        ShowVersionNote(blocked);
    }

    void ShowVersionNote(string? blocked)
    {
        var text = blocked ?? switchOutcome;
        VersionPickerNote.Text = text ?? "";
        VersionPickerNote.IsVisible = text != null;
    }

    async Task PickVersion(InstallLayout install)
    {
        if (versionPickerFilling || VersionPicker.SelectedIndex <= 0 || VersionPicker.SelectedIndex > keptVersions.Count) return;
        var target = keptVersions[VersionPicker.SelectedIndex - 1];
        string? refused = runner.IsRunning || setupRunning ? "Stop the session (or setup) first."
            : await Task.Run(() => KeptVersions.Blockers(install)) is { Count: > 0 } blockers ? "Close these first: " + string.Join(", ", blockers) + "."
            : KeptVersions.Problem(install, target);
        // no confirmation: each version keeps its own data and mod list, so a switch loses nothing and switching back is one click
        if (refused == null && await StartSwitch(install, target))
        {
            closeConfirmed = true;
            Close();
            return;
        }
        if (refused != null) switchOutcome = refused;
        FillVersionPicker(install);
    }

    // HellForge.Cli versions-switch, which returns once its copy waits for this launcher to close. true: it did.
    async Task<bool> StartSwitch(InstallLayout install, KeptVersion target)
    {
        VersionPicker.IsEnabled = false;
        ShowVersionNote("Switching to " + target.Text + "...");
        var start = new ProcessStartInfo(Path.Combine(install.App, "cli", "HellForge.Cli.exe"))
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = install.Root
        };
        foreach (var arg in new[] { "versions-switch", "--install", install.Root, "--to", target.Name, "--wait-pid", Environment.ProcessId.ToString(CultureInfo.InvariantCulture), "--restart", "yes" })
            start.ArgumentList.Add(arg);
        try
        {
            using var cli = Process.Start(start) ?? throw new InvalidOperationException("HellForge.Cli did not start.");
            // the exit, not the end of its output: nothing else may hold the pipes open, but this launcher must not wait on them
            var error = cli.StandardError.ReadToEndAsync();
            _ = cli.StandardOutput.ReadToEndAsync();
            await cli.WaitForExitAsync();
            if (cli.ExitCode == 0) return true;
            await Task.WhenAny(error, Task.Delay(2000));
            switchOutcome = Privacy.Scrub(error.IsCompletedSuccessfully && error.Result.Replace("HellForge: ", "").Trim() is { Length: > 0 } why ? why : "The switch did not start.");
        }
        catch (Exception failure) when (failure is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            switchOutcome = "The switch could not start: " + failure.Message;
        }
        VersionPicker.IsEnabled = true;
        return false;
    }
}
