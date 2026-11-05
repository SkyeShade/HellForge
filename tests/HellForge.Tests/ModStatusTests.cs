using System.Runtime.Versioning;
using HellForge.Core;

// SDK 16 on HellForge's side: the statuses mods set (ModStatus, the mod loader's mod-status.tsv) and the session's end
// the session runner tells the game (SessionEndSignal, answered here as the mod loader answers it).
static class ModStatusTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Mod statuses are read from the run, or from its game folder when sandboxed, scrubbed", () =>
        {
            var run = Directory.CreateDirectory(Path.Combine(root, "mod-status", new string('a', 32))).FullName;
            check(ModStatus.FileIn(run) == null && ModStatus.Read(run)!.Count == 0 && ModStatus.WrittenUtc(run) == DateTime.MinValue, "No file: no statuses.");
            var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            File.WriteAllText(Path.Combine(run, ModStatus.FileName), $"a.mod\twaiting for a mission\nb.mod\toff: {profile}\\x missing\nno tab\n\tno id\nc.mod\t\n");
            var statuses = ModStatus.Read(run)!;
            check(statuses.Count == 2 && statuses["a.mod"] == "waiting for a mission", "Statuses: " + string.Join(" | ", statuses));
            check(statuses["b.mod"] == $"off: {Privacy.ProfileMark}\\x missing", "Not scrubbed: " + statuses["b.mod"]);
            check(ModStatus.WrittenUtc(run) > DateTime.MinValue, "No time for the file.");
            var sandboxed = Directory.CreateDirectory(Path.Combine(root, "mod-status", new string('b', 32), "game")).FullName;
            File.WriteAllText(Path.Combine(sandboxed, ModStatus.FileName), "a.mod\tfrom the game folder\n");
            check(ModStatus.Read(Path.GetDirectoryName(sandboxed)!)!["a.mod"] == "from the game folder", "A sandboxed session's statuses.");
            using (new FileStream(Path.Combine(run, ModStatus.FileName), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
                check(ModStatus.Read(run) == null, "A file being replaced reads as not now, not as no statuses.");
        });

        test("The session's end: the loader's answer is waited for, and a game that does not answer is not", () =>
        {
            if (OperatingSystem.IsWindows()) SessionEnd(check); // named events: Windows only
        });
    }

    [SupportedOSPlatform("windows")]
    static void SessionEnd(Action<bool, string> check)
    {
        var processId = Random.Shared.Next(1_000_000, 2_000_000); // no real process: the names only need to be its own
        using var signal = SessionEndSignal.Create(processId)!;
        var loader = Task.Run(() => AnswerAsTheLoader(processId));
        check(signal.Signal(TimeSpan.FromSeconds(10)) && loader.Wait(TimeSpan.FromSeconds(10)), "The answer was not seen.");
        using var silent = SessionEndSignal.Create(processId + 1)!;
        var clock = System.Diagnostics.Stopwatch.StartNew();
        check(!silent.Signal(TimeSpan.FromMilliseconds(200)) && clock.ElapsedMilliseconds < 5000, "A game without the loader held the stop.");
    }

    // What native/mod-loader/mod_status.c does: wait for the end, then answer.
    [SupportedOSPlatform("windows")]
    static void AnswerAsTheLoader(int processId)
    {
        using var end = EventWaitHandle.OpenExisting(SessionEndSignal.EndName(processId));
        using var ended = EventWaitHandle.OpenExisting(SessionEndSignal.EndedName(processId));
        if (end.WaitOne(TimeSpan.FromSeconds(10))) ended.Set();
    }
}
