using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.Session;

// How a session's end goes into result.json (src/HellForge.Session/SessionEnd.cs). A Linux sandboxed session that a mod sync
// stops must still name the restart the launcher carries out, although the sandbox supervisor stops the game on the same
// signal before the session runner reads it; and HellForge's own stop is not a crash.
static class SessionEndTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Session end: a stop signal is recorded, a mod sync's as the launcher's restart, stop-now as ending the game at once", () =>
        {
            var sync = new JsonObject();
            check(!SessionEnd.MarkStop(sync, "mod-sync"), "A mod sync's stop asked to end the game at once.");
            check(sync["restart"]?.GetValue<string>() == "mod-sync" && sync["observationStoppedBySignal"]?.GetValue<bool>() == true,
                "A mod sync's stop was not recorded as the launcher's restart: " + sync.ToJsonString());

            var stop = new JsonObject();
            check(!SessionEnd.MarkStop(stop, "stop") && stop["restart"] == null && stop["observationStoppedBySignal"]?.GetValue<bool>() == true,
                "The launcher's STOP was recorded wrong: " + stop.ToJsonString());

            var now = new JsonObject();
            check(SessionEnd.MarkStop(now, SessionRunner.StopNowSignal) && now["restart"] == null, "stop-now did not end the game at once: " + now.ToJsonString());
        });

        test("Session end: HellForge's own stop (0xB7520003) and an ordinary exit are no crash; a crash code, crash line or dump are", () =>
        {
            check(!SessionEnd.Crashed(unchecked((int)0xB7520003), false, false), "HellForge's own stop counted as a crash.");
            check(!SessionEnd.Crashed(0, false, false) && !SessionEnd.Crashed(1, false, false) && !SessionEnd.Crashed(null, false, false),
                "An ordinary exit counted as a crash.");
            check(SessionEnd.Crashed(unchecked((int)0xC0000005), false, false), "An access violation did not count as a crash.");
            check(SessionEnd.Crashed(unchecked((int)0xB7520003), true, false) && SessionEnd.Crashed(0, false, true),
                "The mod loader's crash line or a dump did not count as a crash.");
        });
    }
}
