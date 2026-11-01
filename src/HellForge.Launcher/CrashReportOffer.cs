using System.Globalization;
using HellForge.Core;

namespace HellForge.Launcher;

// Save crash report (CrashReport): offered on the Play page once a session has ended in a crash. One zip on the Desktop,
// for the player to attach to a GitHub issue; then Open a GitHub issue opens the repository's new-issue page, with
// nothing in its address. Nothing is uploaded. The crash dump goes in only when the player says so, after being told
// that a dump cannot be scrubbed.
public partial class MainWindow
{
    string? crashRun;

    void OfferCrashReport(string? run)
    {
        if (run == null || !CrashReport.Crashed(run)) return;
        crashRun = run;
        CrashReportPanel.IsVisible = true;
        CrashReportButton.IsEnabled = true;
        CrashIssueButton.IsVisible = false;
        Append("The game crashed. Save crash report (on the Play page) puts this session's logs, scrubbed for a public bug report, " +
            "in one zip on your Desktop to attach to a GitHub issue.", Orange);
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
        ConfirmText.Text = "This session wrote a crash dump (Settings > Crash dump). A dump is the game's memory as it crashed: HellForge " +
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
