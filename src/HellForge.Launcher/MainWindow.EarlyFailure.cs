using System.Globalization;
using HellForge.Core;

namespace HellForge.Launcher;

// MainWindow's words for a start that stopped before its session began, and Save logs to Desktop, which also saves
// that start's logs.
public partial class MainWindow
{
    void ShowEarlyFailure(int? exitCode)
    {
        // The wrapper refuses before Steam and Proton when HellForge was started inside Flatpak, Snap or Steam's
        // runtime container. It writes the reason for this line; without it, keep the generic early-exit advice.
        var containerRefusal = paths.InstallRoot == null ? null : SessionRunner.LinuxContainerRefusal(paths.InstallRoot);
        if (containerRefusal != null)
        {
            SetStatus("HellForge cannot start inside this container.", Phase.Failed, failure: containerRefusal);
            return;
        }
        var failureLogs = paths.InstallRoot == null ? "Sessions run in installations made with HellForge-Setup."
            : OperatingSystem.IsLinux()
                ? "Check " + Path.Combine(paths.InstallRoot, "logs", "linux-session-wrapper.log") +
                    " for Bash/Proton output and " + Path.Combine(paths.InstallRoot, "logs", "session-runner.log") +
                    " for errors after the Windows session runner starts."
                : "See " + Path.Combine(paths.InstallRoot, "logs", "session-runner.log") + " for why.";
        var exit = exitCode is { } code ? $"The launch process exited with code {code}." : null;
        var reason = EarlyFailureReason();
        if (reason != null) Append("The session runner exited before starting: " + reason, Red);
        SetStatus("The session runner exited before starting.", Phase.Failed, failureLogs,
            reason == null ? exit : reason + (exit == null ? "" : " (" + exit + ")"));
    }

    // What the session runner (or, on Linux, the wrapper before it) wrote for this start, so the player does not have
    // to find and open a log for it.
    string? EarlyFailureReason()
    {
        if (paths.InstallRoot == null) return null;
        var logs = Path.Combine(paths.InstallRoot, "logs");
        if (HellForge.Core.EarlySessionFailure.Reason(logs, runnerStartedUtc) is { } reason) return reason;
        return OperatingSystem.IsLinux() ? HellForge.Core.EarlySessionFailure.WrapperReason(logs, runnerStartedUtc) : null;
    }

    // Zips the latest session folder to the Desktop, for sending to whoever helps test (it names Steam accounts by id).
    void SaveLogsToDesktop()
    {
        try
        {
            var run = runDirectory ?? (Directory.Exists(paths.RunsFolder)
                ? new DirectoryInfo(paths.RunsFolder).GetDirectories().OrderByDescending(d => d.LastWriteTimeUtc).FirstOrDefault()?.FullName : null);
            var target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                $"HellForge-logs-{DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}.zip");
            // A start that stopped before its session folder existed left its reason in the installation's logs folder.
            var installLogs = paths.InstallRoot != null ? Path.Combine(paths.InstallRoot, "logs") : null;
            if (run == null)
            {
                if (installLogs != null && HellForge.Core.SessionLogs.ExportStart(installLogs, target) is { } start)
                    Append("No session has run yet: saved the logs of the start that stopped to " + target +
                        (start.Skipped > 0 ? $" ({start.Skipped} files in use were left out)" : ""), Green);
                else Append("No session logs yet: play a session first.", Orange);
                return;
            }
            // the same zip as HellForge.Cli logs --export: scrubbed of the PC's names (SessionLogs.Export)
            var gameLogs = paths.InstallRoot != null ? Path.Combine(paths.InstallRoot, "userdata", "logs") : null;
            var skipped = HellForge.Core.SessionLogs.Export(run, gameLogs, target, installLogs).Skipped;
            Append("Saved the latest session's logs to " + target + (skipped > 0 ? $" ({skipped} files in use were left out)" : ""), Green);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Append("Could not save the logs: " + error.Message, Red);
        }
    }
}
