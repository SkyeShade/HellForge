using System.Text.Json.Nodes;
using HellForge.LocalBackend;

// The game's idle kick (alone on the ship, 15 minutes without input) is served off; nothing else in the body changes.
static class IdleKickTests
{
    public static void Run(Action<bool, string> check)
    {
        // The client finds an override by the mix of its path's id32s (game.dll 0xADB437): feature 3050214232, then 900631814.
        check(Mix(3050214232, 900631814) == IdleKick.Override, "The idle kick's override is not the client's key.");

        using var live = LocalBackend.LoadResource("game-client-config.json");
        var before = Overrides(JsonNode.Parse(live.RootElement.GetProperty("gameClient").GetRawText())!);
        var served = Overrides(JsonNode.Parse(LocalBackend.LoadStartupFixtures()["/Configuration/GameClient"].GetRawText())!);
        static bool IsIdleKick(JsonObject o) => (uint)o["mixId"]! == IdleKick.Override;

        check(before.Count(IsIdleKick) == 1 && (string)before.Single(IsIdleKick)["data"]! == "true",
            "The live snapshot no longer turns the idle kick on: check the override's key.");
        var idle = served.Single(IsIdleKick);
        check((string)idle["data"]! == "false" && (bool)idle["enabled"]! && (int)idle["dataType"]! == 7,
            "The idle kick is not served off as an enabled bool.");
        check(served.Count == before.Count && served.Zip(before).All(p => IsIdleKick(p.First) || JsonNode.DeepEquals(p.First, p.Second)),
            "Turning off the idle kick changed another override.");
    }

    static List<JsonObject> Overrides(JsonNode client) => [.. client["onlineOverrideConfiguration"]!.AsArray().OfType<JsonObject>()];

    static uint Mix(uint first, uint second)
    {
        const uint M = 0x5BD1E995;
        var e = second * M;
        return first * M ^ (e ^ e >> 24) * M;
    }
}
