using HellForge.Core;

namespace HellForge.Launcher;

// MainWindow's STOP (on Linux with a bounded wait, then the session runner's process tree ended), the console's line on
// whether a session is sandboxed, and the Linux card for a session's processes left running after it ended.
public partial class MainWindow
{
    void RequestStop()
    {
        if (!runner.IsRunning || stopRequested) return;
        stopRequested = true;
        stopKilled = false;
        var attempt = ++stopAttempt;
        if (runDirectory != null)
        {
            try
            {
                SessionRunner.SignalStop(runDirectory);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                stopRequested = false;
                Append("Could not write the stop signal: " + error.Message, Red);
                return;
            }
            SetStatus("Stopping: closing the game...", Phase.Busy);
            // Linux only: there a session stuck before or after Observe has nothing else to end it (H.6). Windows keeps
            // its own STOP and cleanup, including the sandbox's teardown, unchanged.
            if (OperatingSystem.IsLinux()) _ = EscalateStopAsync(attempt);
        }
        else
        {
            try { runner.Kill(); }
            catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { Append("Could not stop the session runner: " + error.Message, Red); }
            stopRequested = false;
            RenderSteps(-1, false);
            SetStatus("Cancelled before launch.", Phase.Idle, "Nothing was started.");
        }
        UpdateButton();
    }

    // STOP asks the session to close first. A runner stuck in preparation or cleanup gets a bounded wait, then its
    // whole process tree is ended so the launcher cannot remain in STOPPING and the next session can start.
    async Task EscalateStopAsync(int attempt)
    {
        await Task.Delay(TimeSpan.FromSeconds(20));
        if (attempt != stopAttempt || !stopRequested || !runner.IsRunning) return;
        try
        {
            runner.Kill();
            stopKilled = true;
            Append("The session did not stop; HellForge ended it.", Orange);
            SetStatus("The session did not stop; HellForge ended it.", Phase.Ended,
                "Its own cleanup may not have finished. Check the session logs before playing again.");
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            Append("The session did not stop and could not be ended: " + error.Message, Red);
            SetStatus("The session did not stop and could not be ended.", Phase.Failed, failure: error.Message);
        }
        finally
        {
            stopRequested = false;
            UpdateButton();
        }
    }

    // The console's line on the sandbox at PLAY: on, or on Linux why it is off (the game can then write the player's files).
    void AppendSandboxLine()
    {
        if (SandboxBox.IsChecked == true)
        {
            Append(OperatingSystem.IsLinux()
                ? "Sandboxed (Settings > Sandbox): the game runs in its own Wine prefix and a read-only filesystem view."
                : "Sandboxed (Settings > Sandbox): the game runs at low integrity, with Steam through the Steam broker.", LauncherLine);
        }
        else if (OperatingSystem.IsLinux())
        {
            var why = SessionRunner.LinuxSandboxProgramProblem(paths) ??
                (paths.InstallRoot != null ? LinuxSandboxReadiness.Problem(new InstallLayout(paths.InstallRoot)) : null) ??
                "Settings > Sandbox is off";
            Append("This session is not sandboxed (" + why + "). The game, its mods and anything they run can write your files.", Orange);
        }
    }

    bool ShowLeftoverProcesses()
    {
        if (!OperatingSystem.IsLinux() || paths.InstallRoot == null || runner.IsRunning)
        {
            LeftoverProcessesCard.IsVisible = false;
            return false;
        }
        try
        {
            var names = LinuxInstallProcesses.SessionLeftovers(paths.InstallRoot, SyncCaptureId);
            if (names.Count == 0)
            {
                LeftoverProcessesCard.IsVisible = false;
                return false;
            }
            LeftoverProcessesText.Text = "Still running: " + string.Join(", ", names) +
                ". End them before starting the next session.";
            LeftoverProcessesCard.IsVisible = true;
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            LeftoverProcessesCard.IsVisible = false;
            Append("Could not look for leftover session processes: " + error.Message, Red);
            return false;
        }
    }

    void EndLeftoverProcesses()
    {
        if (paths.InstallRoot == null) return;
        try
        {
            var ended = LinuxInstallProcesses.EndSessionLeftovers(paths.InstallRoot, SyncCaptureId);
            Append(ended.Count > 0 ? "Ended " + string.Join(", ", ended) + "." : "No leftover session processes were found.",
                ended.Count > 0 ? Green : Light);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Append("Could not end the leftover session processes: " + error.Message, Red);
        }
        ShowLeftoverProcesses();
    }
}
