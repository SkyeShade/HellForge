using HellForge.Core;

// A session that stops before its run folder exists: the reason the launcher and the CLI show comes from the logs
// written for that start, never from an older one.
static class EarlySessionFailureTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("An early session failure shows the session runner's own reason, and only the one written for this start", () =>
        {
            var logs = Directory.CreateDirectory(Path.Combine(root, "early-failure", "logs")).FullName;
            var log = Path.Combine(logs, EarlySessionFailure.RunnerLog);
            var started = DateTime.UtcNow;
            check(EarlySessionFailure.Reason(logs, started) == null, "A missing log gave a reason.");

            // As HellForge.Session writes it: the local time, a space, the scrubbed message.
            File.WriteAllText(log, "2026-11-09 18:02:11 Not a HellForge installation: C:\\old\n" +
                "2026-11-09 19:40:03 " + GameCatalog.SyncFirst + "\n");
            check(EarlySessionFailure.Reason(logs, started) == GameCatalog.SyncFirst,
                "The newest entry was not shown without its time: " + EarlySessionFailure.Reason(logs, started));

            // The same file, last written before this start: its reason belongs to an earlier one.
            File.SetLastWriteTimeUtc(log, started.AddMinutes(-5));
            check(EarlySessionFailure.Reason(logs, started) == null, "An older start's reason was shown for this one.");
            // A card that keeps times to two seconds may date the write just before the start.
            File.SetLastWriteTimeUtc(log, started.AddSeconds(-1.5));
            check(EarlySessionFailure.Reason(logs, started) == GameCatalog.SyncFirst, "A two-second file time hid the reason.");

            File.WriteAllText(log, "2026-11-09 19:40:03 The game copy is incomplete:\r\n  data/game is missing\r\n\r\n");
            check(EarlySessionFailure.Reason(logs, started) == "The game copy is incomplete:   data/game is missing",
                "A reason with line breaks was cut to its last line: " + EarlySessionFailure.Reason(logs, started));
            File.WriteAllText(log, "a line without a time\n");
            check(EarlySessionFailure.Reason(logs, started) == "a line without a time", "A line without a time was dropped.");
            File.WriteAllText(log, "\n  \n");
            check(EarlySessionFailure.Reason(logs, started) == null, "An empty log gave a reason.");
        });

        test("On Linux the wrapper's last line stands in when the session runner never ran", () =>
        {
            var logs = Directory.CreateDirectory(Path.Combine(root, "early-failure-wrapper", "logs")).FullName;
            var log = Path.Combine(logs, EarlySessionFailure.LinuxWrapperLog);
            var started = DateTime.UtcNow;
            check(EarlySessionFailure.WrapperLastLine(logs, started) == null, "A missing wrapper log gave a line.");
            File.WriteAllText(log, "HellForge: starting\nHellForge: Proton was not found at /x/proton\n\n");
            check(EarlySessionFailure.WrapperLastLine(logs, started) == "HellForge: Proton was not found at /x/proton",
                "The wrapper's last line was not read: " + EarlySessionFailure.WrapperLastLine(logs, started));
            File.SetLastWriteTimeUtc(log, started.AddMinutes(-5));
            check(EarlySessionFailure.WrapperLastLine(logs, started) == null, "An older run's wrapper line was shown.");
        });
    }
}
