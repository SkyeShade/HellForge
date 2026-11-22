using System.Diagnostics;
using System.Globalization;
using Avalonia.Controls;
using HellForge.Core;

namespace HellForge.Launcher;

// The versions an update kept (KeptVersions), opened from the version in the sidebar's corner. A switch closes this
// launcher: HellForge.Cli's versions-switch swaps the app from a copy outside app/ once this process has ended, then
// starts HellForge.exe again, which shows its outcome here.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> keptVersions = [];

    void InitializeKeptVersions(InstallLayout install)
    {
        if (!CanSwitchVersions)
        {
            KeptVersionsText.IsVisible = false;
            VersionChevron.IsVisible = false;
            return;
        }
        VersionButton.Click += async (_, _) => await ToggleVersionMenu(install);
        VersionKept1Button.Click += async (_, _) => await AskSwitch(install, 0);
        VersionKept2Button.Click += async (_, _) => await AskSwitch(install, 1);
        if (KeptVersions.TakeResult(install) is { } last)
        {
            var done = last.Status == "done";
            Append((done ? "" : "Switching versions failed: ") + last.Message, done ? Green : Red);
            VersionMenuNote.Text = Privacy.Scrub(done ? last.Message : "The last switch did not happen: " + last.Message);
        }
    }

    static bool CanSwitchVersions => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine;

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
        foreach (var (button, text, index) in new[] { (VersionKept1Button, VersionKept1Text, 0), (VersionKept2Button, VersionKept2Text, 1) })
        {
            button.IsVisible = index < keptVersions.Count;
            if (index >= keptVersions.Count) continue;
            var kept = keptVersions[index];
            var problem = KeptVersions.Problem(install, kept);
            text.Text = kept.Text + (kept.Identity.IsNewerThan(current) ? "  ·  newer" : "") + "\nkept " +
                kept.KeptUtc.ToLocalTime().ToString("d MMM yyyy, HH:mm", CultureInfo.CurrentCulture) + (problem != null ? "\n" + problem : "");
            button.IsEnabled = problem == null && blocked == null;
        }
        VersionMenuNote.Text = blocked ?? (keptVersions.Count == 0
            ? $"No earlier version is kept yet. Each update keeps the version it replaces (the last {KeptVersions.Count})."
            : "Pick one to switch to it. Each version keeps its own data and mod list; your mods and the game copy stay.");
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
            $"\n\nThe changes stay with {current.Text}, which is kept: you can come back to it from the same list." +
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
        VersionKept1Button.IsEnabled = VersionKept2Button.IsEnabled = false;
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
            var error = cli.StandardError.ReadToEndAsync();
            await cli.StandardOutput.ReadToEndAsync();
            await cli.WaitForExitAsync();
            if (cli.ExitCode == 0) return true;
            VersionMenuNote.Text = Privacy.Scrub((await error).Replace("HellForge: ", "").Trim() is { Length: > 0 } why ? why : "The switch did not start.");
        }
        catch (Exception failure) when (failure is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            VersionMenuNote.Text = "The switch could not start: " + failure.Message;
        }
        await FillVersionMenu(install);
        return false;
    }
}
