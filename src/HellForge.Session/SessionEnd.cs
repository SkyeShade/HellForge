using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.Session;

// How a session's end goes into result.json, for the observation loops (PlaySession.Observe.cs and, for the Linux
// domain, PlaySession.Sandbox.cs). Nothing here touches the game or the files, so HellForge.Tests runs it (SessionEndTests).
static class SessionEnd
{
    // What the stop signal (stop-observation.signal) asked for, written to the result: the observation ended on it, and
    // "mod-sync" (the backend stopped the session for a host's mods) is the restart the launcher carries out
    // (MainWindow.TryModSyncRestart). True when it asks to end the game at once ("stop-now").
    public static bool MarkStop(JsonObject result, string signal)
    {
        result["observationStoppedBySignal"] = true;
        if (signal == "mod-sync") result["restart"] = "mod-sync";
        return signal == SessionRunner.StopNowSignal;
    }

    // Whether a sandboxed game's end was a crash: a Windows crash code (WindowsCrash.IsCrash, as the Windows path takes it),
    // the mod loader's crash line or a dump. HellForge's own stop (HostBoundedProcess.Stop ends the game with 0xB7520003)
    // and an ordinary exit code are not.
    public static bool Crashed(int? exitCode, bool crashLine, bool dump) =>
        exitCode is { } code && WindowsCrash.IsCrash(code) || crashLine || dump;
}
