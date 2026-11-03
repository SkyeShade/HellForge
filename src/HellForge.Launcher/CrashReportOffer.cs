using System.Globalization;
using HellForge.Core;

namespace HellForge.Launcher;

// After a session that ended in a crash, the Play page says so, with the game's exit code and what it means
// (CrashReport.ExitText), and offers Save logs to Desktop (the session's logs for whoever helps, as the Console page's
// button) and Save crash report (CrashReport): one zip on the Desktop, for the player to attach to a GitHub issue; then
// Open a GitHub issue opens the repository's new-issue page, with nothing in its address. Nothing is uploaded. The crash
// dump goes in only when the player says so, after being told that a dump cannot be scrubbed.
public partial class MainWindow
{
    string? crashRun, crashEnding;

    void OfferCrashReport(string? run)
    {
        if (run == null || !CrashReport.Crashed(run)) return;
        crashRun = run;
        crashEnding = CrashReport.ExitText(run);
        CrashReportPanel.IsVisible = true;
        CrashReportButton.IsEnabled = true;
        CrashIssueButton.IsVisible = false;
        ShowCrashEnding();
        Append("The game crashed" + (crashEnding != null ? " (exit code " + crashEnding + ")" : "") + ". Save logs to Desktop keeps this " +
            "session's logs for whoever helps you; Save crash report (on the Play page) puts them, scrubbed for a public bug report, in one " +
            "zip on your Desktop to attach to a GitHub issue.", Orange);
    }

    // The Play page's status after the session that crashed: true when it is the one shown (the next session clears it).
    bool ShowCrashEnding()
    {
        if (crashRun == null || crashRun != runDirectory) return false;
        SetStatus("The game crashed.", Phase.Failed, (crashEnding != null ? "It ended with " + crashEnding + ". " : "") +
            "Save logs or a crash report below, then press PLAY to start another session.");
        return true;
    }

    async void SaveCrashReport()
    {
        if (crashRun is not { } run) return;
        var includeDump = CrashReport.Dump(run) != null && await AskAboutDump();
        CrashReportButton.IsEnabled = false;
        try
        {
            var target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                $"HellForge-crash-report-{DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}.zip");
            var gameLogs = paths.InstallRoot != null ? Path.Combine(paths.InstallRoot, "userdata", "logs") : null;
            var saved = await Task.Run(() => CrashReport.Save(run, gameLogs, target, includeDump));
            Append("Saved the crash report to " + target + (saved.Dump ? ", with the crash dump" : "") +
                (saved.Skipped > 0 ? $" ({saved.Skipped} files in use were left out)" : "") +
                ". Read its summary.txt, then attach the zip to a GitHub issue (Open a GitHub issue).", Green);
            CrashIssueButton.IsVisible = true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            Append("Could not save the crash report: " + error.Message, Red);
        }
        finally { CrashReportButton.IsEnabled = true; }
    }

    // true: the player wants the crash dump in the report
    async Task<bool> AskAboutDump()
    {
        if (ConfirmOverlay.IsVisible) return false;
        ConfirmTitle.Text = "Add the crash dump?";
        ConfirmText.Text = "This session wrote a crash dump. A dump is the game's memory as it crashed: HellForge " +
            "cannot scrub it, and it can hold your Steam account name and your friends' names. Add it only if you are fine with " +
            "whoever reads the issue seeing that. The rest of the report is scrubbed either way.";
        (ConfirmCancel.Content, ConfirmStop.Content) = ("Without the dump", "Add the dump");
        ConfirmButtons.IsVisible = true;
        ConfirmOverlay.IsVisible = true;
        confirm = new TaskCompletionSource<bool>();
        var answer = await confirm.Task;
        ConfirmOverlay.IsVisible = false;
        return answer;
    }
}
