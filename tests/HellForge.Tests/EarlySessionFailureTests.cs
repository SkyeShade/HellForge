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

        test("On Linux the wrapper's reason stands in when the session runner never ran", () =>
        {
            var logs = Directory.CreateDirectory(Path.Combine(root, "early-failure-wrapper", "logs")).FullName;
            var log = Path.Combine(logs, EarlySessionFailure.LinuxWrapperLog);
            var started = DateTime.UtcNow;
            string? reason() => EarlySessionFailure.WrapperReason(logs, started);
            check(reason() == null, "A missing wrapper log gave a reason.");
            // The wrapper's exit trap writes after the reason, so the log never ends on it.
            const string start = "HellForge: /x with /p/proton (prefix /c)\nSteam compatibility app ID: 553850\n";
            const string bridge = "Steam's local ports could not be bridged: is Steam running and signed in?";
            File.WriteAllText(log, start + bridge + "\nHellForge wrapper exiting with code 1.\n");
            check(reason() == bridge, "The wrapper's failure line was not the reason: " + reason());
            const string appArmor = "This system only lets allowed programs make the network namespace HellForge runs " +
                "in. Run this once, then start HellForge again:";
            File.WriteAllText(log, start + appArmor + "\n  sudo bash /x/app/linux/allow-namespace.sh   (docs/linux.md)\n");
            check(reason() == appArmor + " sudo bash /x/app/linux/allow-namespace.sh   (docs/linux.md)",
                "A failure line ending in a colon lost its command: " + reason());
            File.WriteAllText(log, start + "Entering the offline network namespace.\nwine: could not load kernel32.dll\n" +
                "Offline namespace command exited with code 53.\nHellForge is running inside the container \"box\"; if " +
                "this namespace step failed, the container may not allow it.\nSteam bridge did not stop after 5 seconds; " +
                "killing it.\nHellForge wrapper exiting with code 53.\n");
            check(reason() == "The last line of linux-session-wrapper.log before the wrapper stopped: wine: could not " +
                "load kernel32.dll", "The wrapper's closing lines were not passed over: " + reason());
            File.WriteAllText(log, "HellForge wrapper received SIGTERM.\nHellForge wrapper exiting with code 143.\n");
            check(reason() == null, "A log of closing lines alone gave a reason: " + reason());
            File.WriteAllText(log, start + bridge + "\n");
            File.SetLastWriteTimeUtc(log, started.AddMinutes(-5));
            check(reason() == null, "An older run's wrapper reason was shown.");
        });

        test("The wrapper's closing lines the launcher passes over are the ones the Linux scripts write", () =>
        {
            if (RepositoryRoot() is not { } repository) { Console.WriteLine("  (skipped: no repository root above the test)"); return; }
            var scripts = File.ReadAllText(Path.Combine(repository, "linux", "hellforge-linux.sh")) +
                File.ReadAllText(Path.Combine(repository, "linux", "hf-container.sh"));
            foreach (var closing in EarlySessionFailure.WrapperClosingLines)
                check(scripts.Contains(closing, StringComparison.Ordinal), "No Linux script writes: " + closing);
            // Everything the exit trap and the failed-namespace branch echo must be on the list.
            var wrapper = File.ReadAllText(Path.Combine(repository, "linux", "hellforge-linux.sh"));
            var trap = wrapper[wrapper.IndexOf("cleanup() {", StringComparison.Ordinal)..wrapper.IndexOf("trap cleanup EXIT", StringComparison.Ordinal)];
            var failed = wrapper[wrapper.IndexOf("Offline namespace command completed.", StringComparison.Ordinal)..];
            foreach (System.Text.RegularExpressions.Match echo in
                System.Text.RegularExpressions.Regex.Matches(trap + failed, "echo \"([^\"$]+)"))
                check(EarlySessionFailure.WrapperClosingLines.Any(closing => echo.Groups[1].Value.Contains(closing, StringComparison.Ordinal)),
                    "The wrapper writes a closing line the launcher would show as the reason: " + echo.Groups[1].Value);
        });
    }

    static string? RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "linux", "hellforge-linux.sh"))) return folder.FullName;
        return null;
    }
}
