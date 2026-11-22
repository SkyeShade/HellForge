using System.Diagnostics;
using Avalonia.Controls;
using HellForge.Core;

namespace HellForge.Launcher;

// Settings > Installation: the versions an update kept (KeptVersions). A switch closes this launcher: HellForge.Cli's
// versions-switch swaps the app from a copy outside app/ once this process has ended, then starts HellForge.exe again,
// which shows its outcome here.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> keptVersions = [];

    void InitializeKeptVersions(InstallLayout install)
    {
        KeptVersion1Button.Click += async (_, _) => await AskSwitch(install, 0);
        KeptVersion2Button.Click += async (_, _) => await AskSwitch(install, 1);
        ShowKeptVersions(install);
        if (KeptVersions.TakeResult(install) is { } last)
        {
            Append((last.Status == "done" ? "" : "Switching versions failed: ") + last.Message, last.Status == "done" ? Green : Red);
            KeptStatus(last.Status == "done" ? last.Message : "The last switch did not happen: " + last.Message);
        }
    }

    static bool CanSwitchVersions => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine;

    void KeptStatus(string text)
    {
        KeptVersionsStatus.Text = Privacy.Scrub(text);
        KeptVersionsStatus.IsVisible = text.Length > 0;
    }

    void ShowKeptVersions(InstallLayout install)
    {
        if (!CanSwitchVersions)
        {
            KeptVersionsText.IsVisible = KeptVersion1Button.IsVisible = KeptVersion2Button.IsVisible = false;
            return;
        }
        keptVersions = KeptVersions.List(install);
        KeptVersionsText.Text = keptVersions.Count == 0
            ? $"When HellForge updates, it keeps the last {KeptVersions.Count} versions it replaced here, so you can go back to one. None is kept yet."
            : $"Kept from earlier updates (the last {KeptVersions.Count}). Going back swaps in that version's app and the HellForge data it last used; " +
              "your mods and the game copy stay. The version you leave is kept the same way.";
        var current = KeptVersions.Identify(install.App);
        foreach (var (button, index) in new[] { (KeptVersion1Button, 0), (KeptVersion2Button, 1) })
        {
            button.IsVisible = index < keptVersions.Count;
            if (index < keptVersions.Count) button.Content = (keptVersions[index].Identity.IsNewerThan(current) ? "Go forward to " : "Go back to ") + keptVersions[index].Text;
        }
    }

    async Task AskSwitch(InstallLayout install, int index)
    {
        if (index >= keptVersions.Count || ConfirmOverlay.IsVisible) return;
        var target = keptVersions[index];
        if (runner.IsRunning || setupRunning) { KeptStatus("Stop the session (or setup) first."); return; }
        var blockers = await Task.Run(() => KeptVersions.Blockers(install));
        if (blockers.Count > 0) { KeptStatus("Close these first: " + string.Join(", ", blockers) + "."); return; }
        if (KeptVersions.Problem(install, target) is { } problem) { KeptStatus(problem); return; }
        var current = KeptVersions.Identify(install.App);
        var forward = target.Identity.IsNewerThan(current);
        ConfirmTitle.Text = (forward ? "Go forward to " : "Go back to ") + target.Text + "?";
        ConfirmText.Text = $"HellForge closes, swaps in {target.Text} and starts again. " +
            (forward ? "Changes made since you went back" : "Changes made since you updated") + ", like new profiles or syncs, are not kept. " +
            $"Your mods and the game stay.\n\nThey stay with {current.Text}, which is kept: you can come back to it here." +
            (KeptVersions.GameBuildNote(install, target) is { } note ? "\n\n" + note : "");
        (ConfirmCancel.Content, ConfirmStop.Content) = ("Cancel", forward ? "Go forward" : "Go back");
        ConfirmButtons.IsVisible = true;
        ConfirmOverlay.IsVisible = true;
        confirm = new TaskCompletionSource<bool>();
        var accepted = await confirm.Task;
        confirm = null;
        ConfirmOverlay.IsVisible = false;
        if (!accepted) return;
        if (!await StartSwitch(install, target)) return;
        closeConfirmed = true;
        Close();
    }

    // HellForge.Cli versions-switch, which returns once its copy waits for this launcher to close. true: it did.
    async Task<bool> StartSwitch(InstallLayout install, KeptVersion target)
    {
        KeptVersion1Button.IsEnabled = KeptVersion2Button.IsEnabled = false;
        KeptStatus("Starting the switch...");
        var start = new ProcessStartInfo(Path.Combine(install.App, "cli", "HellForge.Cli.exe"))
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = install.Root
        };
        foreach (var arg in new[] { "versions-switch", "--install", install.Root, "--to", target.Name, "--wait-pid", Environment.ProcessId.ToString(), "--restart", "yes" })
            start.ArgumentList.Add(arg);
        try
        {
            using var cli = Process.Start(start) ?? throw new InvalidOperationException("HellForge.Cli did not start.");
            var error = cli.StandardError.ReadToEndAsync();
            await cli.StandardOutput.ReadToEndAsync();
            await cli.WaitForExitAsync();
            if (cli.ExitCode == 0) return true;
            KeptStatus((await error).Replace("HellForge: ", "").Trim() is { Length: > 0 } why ? why : "The switch did not start.");
        }
        catch (Exception failure) when (failure is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            KeptStatus("The switch could not start: " + failure.Message);
        }
        finally { KeptVersion1Button.IsEnabled = KeptVersion2Button.IsEnabled = true; }
        return false;
    }
}
