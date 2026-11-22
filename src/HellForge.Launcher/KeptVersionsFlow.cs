using System.Diagnostics;
using System.Globalization;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using HellForge.Core;

namespace HellForge.Launcher;

// The versions an update kept (KeptVersions), opened from the version in the sidebar's corner. A switch closes this
// launcher: HellForge.Cli's versions-switch swaps the app from a copy outside app/ once this process has ended, then
// starts HellForge.exe again, which shows its outcome here. Settings > Installation sets how many updates keep.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> keptVersions = [];
    // the last switch's outcome, said in the list until this launcher closes
    string? switchOutcome;

    void InitializeKeptVersions(InstallLayout install)
    {
        if (!CanSwitchVersions)
        {
            KeepVersionsRow.IsVisible = KeptVersionsText.IsVisible = VersionChevron.IsVisible = false;
            return;
        }
        VersionButton.Click += async (_, _) => await ToggleVersionMenu(install);
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
    }

    static bool CanSwitchVersions => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine;

    // A kept version is the app without its Linux build plus a copy of the version's data: about 0.6 GB.
    void ShowKeepCost(InstallLayout install)
    {
        var kept = KeptVersions.List(install);
        KeptVersionsText.Text = $"About 0.6 GB each; {kept.Count} kept now ({KeptVersions.KeptBytes(install) / 1e9:0.0} GB). The oldest goes only when " +
            "an update would keep more, never the version you just came from. Switch versions from the version in the bottom-left corner.";
    }

    async Task ToggleVersionMenu(InstallLayout install)
    {
        if (VersionMenu.IsVisible) { VersionMenu.IsVisible = false; VersionChevron.Text = "▴"; return; }
        await FillVersionMenu(install);
        VersionMenu.IsVisible = true;
        VersionChevron.Text = "▾";
    }

    // The running version first, then each kept one; one that can't be switched to now is greyed with the reason.
    async Task FillVersionMenu(InstallLayout install)
    {
        keptVersions = KeptVersions.List(install);
        var current = KeptVersions.Identify(install.App);
        VersionCurrentItem.Text = current.Text + "  ·  current";
        var blocked = runner.IsRunning || setupRunning ? "Stop the session (or setup) first: versions don't switch while one runs."
            : await Task.Run(() => KeptVersions.Blockers(install)) is { Count: > 0 } running ? "Close these first to switch: " + string.Join(", ", running) + "." : null;
        VersionKeptList.Children.Clear();
        for (var index = 0; index < keptVersions.Count; index++)
            VersionKeptList.Children.Add(KeptItem(install, keptVersions[index], index, current, blocked == null));
        VersionMenuNote.Text = blocked ?? switchOutcome ?? (keptVersions.Count == 0
            ? $"No earlier version is kept yet. Each update keeps the version it replaces (the last {KeptVersions.KeepCount(install)})."
            : "Pick one to switch to it. Each version keeps its own data and mod list; your mods and the game copy stay.");
    }

    Button KeptItem(InstallLayout install, KeptVersion kept, int index, AppIdentity current, bool allowed)
    {
        var problem = KeptVersions.Problem(install, kept);
        var name = "VersionKept" + (index + 1).ToString(CultureInfo.InvariantCulture);
        var text = new TextBlock
        {
            Name = name + "Text", FontSize = 11, TextWrapping = Avalonia.Media.TextWrapping.Wrap,
            Text = kept.Text + (kept.Identity.IsNewerThan(current) ? "  ·  newer" : "") + "\nkept " +
                kept.KeptUtc.ToLocalTime().ToString("d MMM yyyy, HH:mm", CultureInfo.CurrentCulture) + (problem != null ? "\n" + problem : "")
        };
        AutomationProperties.SetAutomationId(text, name + "Text");
        var button = new Button
        {
            Name = name + "Button", Classes = { "flat" }, Content = text, IsEnabled = allowed && problem == null,
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left
        };
        AutomationProperties.SetAutomationId(button, name + "Button");
        button.Click += async (_, _) => await AskSwitch(install, index);
        return button;
    }

    async Task AskSwitch(InstallLayout install, int index)
    {
        if (index >= keptVersions.Count || ConfirmOverlay.IsVisible) return;
        var target = keptVersions[index];
        if (runner.IsRunning || setupRunning) { VersionMenuNote.Text = "Stop the session (or setup) first."; return; }
        if (await Task.Run(() => KeptVersions.Blockers(install)) is { Count: > 0 } blockers) { VersionMenuNote.Text = "Close these first: " + string.Join(", ", blockers) + "."; return; }
        if (KeptVersions.Problem(install, target) is { } problem) { VersionMenuNote.Text = problem; return; }
        var current = KeptVersions.Identify(install.App);
        var forward = target.Identity.IsNewerThan(current);
        ConfirmTitle.Text = (forward ? "Go forward to " : "Go back to ") + target.Text + "?";
        ConfirmText.Text = $"HellForge closes, swaps in {target.Text} and starts again. " +
            (forward ? "Changes made since you went back" : "Changes made since you updated") + ", like new profiles or syncs, are not kept. " +
            "Your mods and the game stay. Each version keeps its own mods list: a mod you turned on in one isn't on in the other." +
            $"\n\nThe changes stay with {current.Text}, which is kept: you can come back to it from the same list" +
            " (from a version without this list, run the newest HellForge-Setup, kept in the installer folder)." +
            (KeptVersions.GameBuildNote(install, target) is { } note ? "\n\n" + note : "");
        (ConfirmCancel.Content, ConfirmStop.Content) = ("Cancel", forward ? "Go forward" : "Go back");
        ConfirmButtons.IsVisible = true;
        ConfirmOverlay.IsVisible = true;
        confirm = new TaskCompletionSource<bool>();
        var accepted = await confirm.Task;
        confirm = null;
        ConfirmOverlay.IsVisible = false;
        if (!accepted || !await StartSwitch(install, target)) return;
        closeConfirmed = true;
        Close();
    }

    // HellForge.Cli versions-switch, which returns once its copy waits for this launcher to close. true: it did.
    async Task<bool> StartSwitch(InstallLayout install, KeptVersion target)
    {
        foreach (var item in VersionKeptList.Children) item.IsEnabled = false;
        VersionMenuNote.Text = "Starting the switch...";
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
            VersionMenuNote.Text = Privacy.Scrub(error.IsCompletedSuccessfully && error.Result.Replace("HellForge: ", "").Trim() is { Length: > 0 } why ? why : "The switch did not start.");
        }
        catch (Exception failure) when (failure is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            VersionMenuNote.Text = "The switch could not start: " + failure.Message;
        }
        await FillVersionMenu(install);
        return false;
    }
}
