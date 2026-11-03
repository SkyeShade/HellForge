using HellForge.Core;

// Sync Inventory's end of the sync game (CaptureGameEnd, InventorySync.GameAfterCapture): once the capture is sealed the
// capture closes the game it opened, politely first, so Easy Anti-Cheat never shows "Could not connect"; a game it could
// not end is the player's to close. Stand-in processes only.
static class CaptureGameEndTests
{
    sealed class FakeGame(int id, DateTime? started, bool closes, bool killable = true) : CaptureGameEnd.IGame
    {
        public readonly List<string> Calls = [];
        bool exited;
        public int Id => id;
        public DateTime? StartedUtc => started;
        public bool HasExited => exited;

        public bool RequestClose()
        {
            Calls.Add("close");
            if (closes) exited = true;
            return true;
        }

        public void Kill()
        {
            Calls.Add("kill");
            if (!killable) throw new System.ComponentModel.Win32Exception(5, "Access is denied.");
            exited = true;
        }

        public bool WaitForExit(TimeSpan timeout)
        {
            Calls.Add("wait");
            return exited;
        }
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var capture = new DateTime(2026, 10, 30, 12, 0, 0, DateTimeKind.Utc);

        test("Sync Inventory ends only the game started after its capture, or one whose start can't be read", () =>
        {
            var before = new FakeGame(1, capture.AddMinutes(-5), closes: true);
            var after = new FakeGame(2, capture.AddSeconds(10), closes: true);
            var unknown = new FakeGame(3, null, closes: true);
            var ours = CaptureGameEnd.Ours([before, after, unknown], capture);
            check(ours.Count == 2 && ours.Contains(after) && ours.Contains(unknown), "Picked the wrong games.");
            check(CaptureGameEnd.End([], TimeSpan.Zero, TimeSpan.Zero, _ => { }) == CaptureGameEnd.Outcome.NotRunning, "No game is not NotRunning.");
        });

        test("Sync Inventory closes its game politely first and never ends one that closed", () =>
        {
            var game = new FakeGame(2, capture, closes: true);
            var log = new List<string>();
            check(CaptureGameEnd.End([game], TimeSpan.Zero, TimeSpan.Zero, log.Add) == CaptureGameEnd.Outcome.Closed, "A closed game is not Closed.");
            check(string.Join(",", game.Calls) == "close,wait", "Wrong order: " + string.Join(",", game.Calls));
            check(log.Count == 1 && log[0].Contains("pid 2", StringComparison.Ordinal), "The close is not logged.");
        });

        test("Sync Inventory ends a game that ignores the close, and says when it can't", () =>
        {
            var stubborn = new FakeGame(2, capture, closes: false);
            check(CaptureGameEnd.End([stubborn], TimeSpan.Zero, TimeSpan.Zero, _ => { }) == CaptureGameEnd.Outcome.Ended, "An ended game is not Ended.");
            check(string.Join(",", stubborn.Calls) == "close,wait,kill,wait", "Wrong order: " + string.Join(",", stubborn.Calls));
            var protectedGame = new FakeGame(3, capture, closes: false, killable: false);
            var log = new List<string>();
            check(CaptureGameEnd.End([protectedGame], TimeSpan.Zero, TimeSpan.Zero, log.Add) == CaptureGameEnd.Outcome.Failed, "A game left running is not Failed.");
            check(log.Any(line => line.Contains("could not end", StringComparison.Ordinal)), "The refused end is not logged.");
            check(CaptureGameEnd.Describe(CaptureGameEnd.Outcome.Failed).Contains("forwarding", StringComparison.Ordinal), "Failed does not say the capture keeps forwarding.");
        });

        test("Sync Inventory asks the player to close the game only when the capture could not, well after it was taken", () =>
        {
            check(InventorySync.GameAfterCapture(capture, capture.AddMinutes(5), gameRunning: false) == null, "Warned about a closed game.");
            check(InventorySync.GameAfterCapture(capture, capture + InventorySync.CloseGameWait - TimeSpan.FromSeconds(1), gameRunning: true) == null,
                "Warned before the capture had time to close the game.");
            var warning = InventorySync.GameAfterCapture(capture, capture + InventorySync.CloseGameWait, gameRunning: true);
            check(warning != null && warning.Contains("close it yourself", StringComparison.Ordinal), "No warning for a game still open.");
            check(InventorySync.CloseGameWait > CaptureGameEnd.Grace + CaptureGameEnd.KillWait, "The launcher warns before the capture can have ended the game.");
        });
    }
}
