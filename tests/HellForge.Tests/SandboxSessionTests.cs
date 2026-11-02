using HellForge.Core;

// A sandboxed session as the runner reads it: the Steam broker's log before the game runs (SteamBrokerLog), in the lines
// hellforge_steam_broker.c writes, and what the sandbox refused the game (SandboxRefusals).
static class SandboxSessionTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A broker that has Steam is ready when it waits for the game, and connected when the game is", () =>
        {
            // a sandboxed session that played (#94's crash report)
            string[] played = ["Setting breakpad minidump AppID = 553850", "broker: Steam ready (pipe 1)", "broker: Play with strangers off",
                "broker: waiting for the game (17188)", "broker: game connected", "broker: shutdown; 0 callback(s) dropped, 1 call(s) refused"];
            check(played.Count(SteamBrokerLog.IsReady) == 1 && SteamBrokerLog.IsReady(played[3]), "Ready at \"waiting for the game\" only.");
            check(played.Count(SteamBrokerLog.IsConnected) == 1 && SteamBrokerLog.IsConnected(played[4]), "Connected at \"game connected\" only.");
            check(!SteamBrokerLog.IsReady("broker: the game exited before connecting"), "An end line is not ready.");
            check(SteamBrokerLog.EndedWithGame(played), "The game's SteamAPI_Shutdown ends the broker with the game.");
            check(SteamBrokerLog.EndedWithGame(["broker: game connected", "broker: the game exited; 0 callback(s) dropped, 0 call(s) refused"]), "So does the game's exit.");
            check(!SteamBrokerLog.EndedWithGame(["broker: game connected", "broker: the pipe closed; 0 callback(s) dropped, 0 call(s) refused"]),
                "A pipe that closed under the game did not end with it.");
        });

        test("A broker that ended before the game says why, in the player's words", () =>
        {
            var init = SteamBrokerLog.Failure(["broker: SteamAPI_Init -> false", "broker: Steam did not start"], 1);
            check(init.Contains("Steam did not answer") && init.Contains("SteamAPI_Init -> false") && init.Contains("signed in") && init.Contains("administrator"),
                "Steam's init: " + init);
            var flat = SteamBrokerLog.Failure(["broker: SteamAPI_InitFlat -> 2 (Steam is not running)", "broker: Steam did not start"], 1);
            check(flat.Contains("SteamAPI_InitFlat -> 2 (Steam is not running)"), "A newer SDK's reason is kept: " + flat);
            var load = SteamBrokerLog.Failure(["broker: cannot load Valve's steam_api64.dll (126)"], 1);
            check(load.Contains("steam_api64.dll (Windows error 126)") && load.Contains("Check and repair"), "Valve's DLL: " + load);
            check(SteamBrokerLog.Failure(["broker: Steam ready (pipe 1)", "broker: Valve's DLL lacks manual dispatch"], 1).Contains("not the one"), "Manual dispatch.");
            var channel = SteamBrokerLog.Failure(["broker: cannot create the channel (231)"], 1);
            check(channel.Contains("channel to the game (Windows error 231)") && channel.Contains("other HellForge session"), "The channel: " + channel);
            var other = SteamBrokerLog.Failure(["broker: game process 4242 not found (87)"], 1);
            check(other.Contains("(exit code 1): game process 4242 not found (87)") && other.Contains("Settings > Sandbox"), "Anything else: " + other);
            var silent = SteamBrokerLog.Failure([], null);
            check(silent.StartsWith("HellForge's Steam broker stopped before the game started. ", StringComparison.Ordinal), "No log, no exit code: " + silent);
            var late = SteamBrokerLog.NotReady();
            check(late.Contains("60 s") && late.Contains("hellforge_steam_broker.exe") && late.Contains("antivirus"), "Not ready in time: " + late);
        });

        test("What the sandbox refused the game is said by the overlay that asked, then the other modules", () =>
        {
            check(SandboxRefusals.Describe([]) == null, "Nothing refused: no line.");
            check(SandboxRefusals.ModuleOf("nvspcap64.dll+0x1a2b") == "nvspcap64.dll" && SandboxRefusals.ModuleOf("?+0x10") == "?", "The module of a caller.");
            check(SandboxRefusals.OverlayOf("GameOverlayRenderer64.dll") == "Steam's overlay" && SandboxRefusals.OverlayOf("game.dll") == null,
                "Known overlays by name, whatever the case.");
            List<SandboxRefusals.Refusal> refused =
            [
                new("Local\\SteamOverlayShm_1", "GameOverlayRenderer64.dll+0x100", 2),
                new("C:\\Users\\%USERPROFILE%\\AppData\\Local\\NVIDIA\\DXCache\\a.bin", "nvwgf2umx.dll+0x20", 30),
                new("\\\\.\\pipe\\nvsp-1", "nvspcap64.dll+0x10", 5),
                new("Local\\nvsp-2", "nvspcap64.dll+0x14", 1),
                new("Local\\nvsp-3", "nvspcap64.dll+0x18", 1),
                new("Local\\nvsp-4", "nvspcap64.dll+0x1c", 1),
            ];
            var line = SandboxRefusals.Describe(refused, 120)!;
            check(line.StartsWith("The sandbox refused the game 120 time(s)", StringComparison.Ordinal), "The census's own total: " + line);
            check(line.IndexOf("NVIDIA's overlay (nvspcap64.dll) 8x", StringComparison.Ordinal) < line.IndexOf("Steam's overlay (GameOverlayRenderer64.dll) 2x", StringComparison.Ordinal) &&
                line.IndexOf("Steam's overlay", StringComparison.Ordinal) < line.IndexOf("nvwgf2umx.dll 30x", StringComparison.Ordinal),
                "Overlays first, the most refused first, then other modules: " + line);
            check(line.Contains("\\\\.\\pipe\\nvsp-1, Local\\nvsp-2, Local\\nvsp-3 and 1 more"), "Three names a module, then how many more: " + line);
            check(SandboxRefusals.Describe(refused)!.Contains(" 40 time(s)"), "Without a total, the listed ones are added up.");
        });
    }
}
