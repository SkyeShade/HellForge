using System.ComponentModel;
using System.Diagnostics;
using HellForge.Core;

// The capture's end of the sync's game (--close-game): the running helldivers2 processes, as CaptureGameEnd sees them.
// A file of its own so a stand-in test host can compile and run exactly this against real processes.
sealed class ProcessGame(Process process) : CaptureGameEnd.IGame
{
    public const string GameProcess = "helldivers2";

    // Ends the games started after the capture did, and says how it went.
    public static CaptureGameEnd.Outcome EndSyncGame(DateTime captureStartedUtc, Action<string> log)
    {
        var running = Process.GetProcessesByName(GameProcess);
        try
        {
            var games = CaptureGameEnd.Ours(running.Select(process => (CaptureGameEnd.IGame)new ProcessGame(process)), captureStartedUtc);
            return CaptureGameEnd.End(games, CaptureGameEnd.Grace, CaptureGameEnd.KillWait, log);
        }
        finally
        {
            foreach (var process in running) process.Dispose();
        }
    }

    public int Id => process.Id;

    public DateTime? StartedUtc
    {
        get
        {
            try { return process.StartTime.ToUniversalTime(); }
            catch (Exception error) when (error is InvalidOperationException or Win32Exception or NotSupportedException) { return null; }
        }
    }

    public bool HasExited
    {
        get
        {
            try { return process.HasExited; }
            catch (Exception error) when (error is InvalidOperationException or Win32Exception) { return false; }
        }
    }

    public bool RequestClose()
    {
        try { return process.CloseMainWindow(); }
        catch (InvalidOperationException) { return false; }
    }

    public void Kill() => process.Kill();

    public bool WaitForExit(TimeSpan timeout)
    {
        try { return process.WaitForExit(timeout); }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception) { return false; }
    }
}
