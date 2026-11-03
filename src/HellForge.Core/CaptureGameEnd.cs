namespace HellForge.Core;

// Ends the normal game the sync opened, once the capture is sealed. The game has done its job by then (the capture reads
// the inventory with the login's own session, before the game gets the login's answer), and left running it showed
// "Could not connect to Easy Anti Cheat network" and closed itself, whether the redirect stayed or ended (live runs,
// 2026-10-26), so the player had to close it. Ending it first means the game never gets that far and the capture removes
// its hosts line and CA at once (it stops when the game exits).
// First a polite close (the window's close message, like Alt+F4), then, after the grace, an end. Only games started after
// the capture did: the launcher refuses a sync while the game runs, so any other is not the sync's.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureGameEnd
{
    public enum Outcome { NotRunning, Closed, Ended, Failed }

    public interface IGame
    {
        int Id { get; }
        // Null when it cannot be read.
        DateTime? StartedUtc { get; }
        bool HasExited { get; }
        // False when the game has no window to close.
        bool RequestClose();
        void Kill();
        bool WaitForExit(TimeSpan timeout);
    }

    public static readonly TimeSpan Grace = TimeSpan.FromSeconds(5), KillWait = TimeSpan.FromSeconds(5);

    // The sync's games: those started after the capture (or whose start cannot be read) and still running.
    public static List<IGame> Ours(IEnumerable<IGame> running, DateTime captureStartedUtc) =>
        [.. running.Where(game => !game.HasExited && (game.StartedUtc is not { } started || started >= captureStartedUtc))];

    public static Outcome End(IReadOnlyList<IGame> games, TimeSpan grace, TimeSpan killWait, Action<string> log)
    {
        if (games.Count == 0) return Outcome.NotRunning;
        foreach (var game in games)
            log(game.RequestClose() ? $"asked the game (pid {game.Id}) to close" : $"the game (pid {game.Id}) has no window to close");
        var left = Remaining(games, grace);
        if (left.Count == 0) return Outcome.Closed;
        foreach (var game in left) Kill(game, log);
        return Remaining(left, killWait).Count == 0 ? Outcome.Ended : Outcome.Failed;
    }

    // For proxy.log.
    public static string Describe(Outcome outcome) => outcome switch
    {
        Outcome.NotRunning => "the game was not running",
        Outcome.Closed => "the game closed",
        Outcome.Ended => "the game was ended",
        _ => "the game is still running; forwarding until it exits"
    };

    static void Kill(IGame game, Action<string> log)
    {
        try
        {
            game.Kill();
            log($"the game (pid {game.Id}) did not close within the grace: ended it");
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            log($"could not end the game (pid {game.Id}): {error.Message}");
        }
    }

    // The games still running after waiting up to timeout in all.
    static List<IGame> Remaining(IReadOnlyList<IGame> games, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        var left = new List<IGame>();
        foreach (var game in games)
        {
            var wait = deadline - DateTime.UtcNow;
            if (!game.WaitForExit(wait > TimeSpan.Zero ? wait : TimeSpan.Zero)) left.Add(game);
        }
        return left;
    }
}
