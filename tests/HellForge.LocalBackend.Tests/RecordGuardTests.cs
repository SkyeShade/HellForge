using System.Text.Json.Nodes;
using HellForge.LocalBackend.Multiplayer;

// Lobby and profile records bounded before they are shared or reach the game (RecordGuard): only the known fields, every
// value within its limits, control characters out of text, and nothing over the size limit.
static class RecordGuardTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        var record = new JsonObject
        {
            ["planetId"] = 261, ["difficulty"] = 5, ["buildGameHash"] = new string('a', 214), ["stolen"] = "C:/Users/someone/secret.txt",
            ["players"] = new JsonArray(new JsonObject { ["playerName"] = "Name\u0007\nHere", ["isHost"] = true, ["extra"] = "x" }),
            ["sosBeacons"] = new JsonArray(Enumerable.Range(0, 65).Select(i => (JsonNode?)JsonValue.Create(i)).ToArray()),
            ["operationProgress"] = Deep(10), ["state"] = new string('s', 300), ["startTime"] = 1.5
        };
        var clean = RecordGuard.Lobby(record);
        check(clean != null && !clean.ContainsKey("stolen") && clean["planetId"]!.GetValue<int>() == 261 && clean["buildGameHash"]!.GetValue<string>().Length == 214,
            "A lobby record keeps its known fields only: " + clean?.ToJsonString());
        var player = clean!["players"]![0]!.AsObject();
        check(player["playerName"]!.GetValue<string>() == "NameHere" && player["isHost"]!.GetValue<bool>() && !player.ContainsKey("extra"),
            "A player keeps its known fields, with control characters out of text.");
        check(!clean.ContainsKey("sosBeacons") && !clean.ContainsKey("state") && clean["startTime"]!.GetValue<double>() == 1.5,
            "A list or text past its limit is left out.");
        check(Depth(clean["operationProgress"]) < RecordGuard.LobbyLimits.Depth, "Nesting stops at the depth limit.");
        var huge = new JsonObject { ["players"] = new JsonArray(Enumerable.Range(0, 64).Select(_ => (JsonNode?)new JsonObject { ["playerName"] = new string('n', 256) }).ToArray()) };
        check(RecordGuard.Lobby(huge) == null, "A record over the size limit is refused.");
        // The selected mission ("seed", MissionSeed): kept in its exact layout, whatever its length; a count past its
        // slot in the game's 0xE8-byte struct, a wrong length, another version or uppercase is left out.
        const string hostSeed = "f3fb5deebd23f97b279d04011f43e0c8d5003621037ec6ffee01890da150010205359da6d95501d50301020800000300010000";
        var full = Seed(32, 8, 3, 2, 4, 8);
        check(MissionSeed.Valid(hostSeed) && MissionSeed.Valid(full) && full.Length == MissionSeed.MaxLength && MissionSeed.Valid(Seed(0, 0, 0, 0, 0, 0)),
            $"A host's seed and seeds with empty and full arrays are valid ({full.Length} digits).");
        check(!MissionSeed.Valid(Seed(33, 0, 0, 0, 0, 0)) && !MissionSeed.Valid(Seed(0, 9, 0, 0, 0, 0)) && !MissionSeed.Valid(Seed(0, 0, 4, 0, 0, 0)) &&
              !MissionSeed.Valid(Seed(0, 0, 0, 3, 0, 0)) && !MissionSeed.Valid(Seed(0, 0, 0, 0, 5, 0)) && !MissionSeed.Valid(Seed(0, 0, 0, 0, 0, 9)),
            "A count past its slot is refused.");
        check(!MissionSeed.Valid(hostSeed + "00") && !MissionSeed.Valid(hostSeed[..^2]) && !MissionSeed.Valid(hostSeed.ToUpperInvariant()) &&
              !MissionSeed.Valid("f3fa" + hostSeed[4..]) && !MissionSeed.Valid("") && !MissionSeed.Valid(null),
            "A wrong length, uppercase, another version or no seed is refused.");
        var seeded = RecordGuard.Lobby(new JsonObject { ["planetId"] = 7, ["seed"] = full });
        var forged = RecordGuard.Lobby(new JsonObject { ["planetId"] = 7, ["seed"] = Seed(40, 0, 0, 0, 0, 0) });
        var numeric = RecordGuard.Lobby(new JsonObject { ["planetId"] = 7, ["seed"] = 5 });
        check(seeded?["seed"]?.GetValue<string>() == full && forged != null && !forged.ContainsKey("seed") && numeric != null && !numeric.ContainsKey("seed"),
            "A lobby record keeps a valid seed past the text limit and drops a forged or non-text one.");
        // The lobby directory's schema (LobbyRecords): published with its seed; a stranger's listing with a forged seed is
        // refused, and this machine's own record leaves a forged one out.
        static JsonObject Listing(string seed) => JsonNode.Parse($"{{\"planetId\":7,\"seed\":\"{seed}\"}}")!.AsObject();
        var listed = LobbyRecords.Clean(Listing(hostSeed), strict: true);
        var forgedListing = LobbyRecords.Clean(Listing(Seed(0, 0, 0, 0, 0, 9)), strict: true);
        var ownForged = LobbyRecords.Clean(Listing(hostSeed + "0"), strict: false);
        check(listed?["seed"]?.GetValue<string>() == hostSeed && forgedListing == null && ownForged != null && !ownForged.ContainsKey("seed"),
            "The directory keeps a valid seed, refuses a stranger's listing with a forged one and leaves a forged one out of its own.");
        var live = RecordGuard.Live(new JsonObject { ["missionTime"] = 30, ["playerPositions"] = new JsonArray(1, 2), ["other"] = 1 });
        check(live != null && live.Count == 2 && !live.ContainsKey("other"), "A live state keeps its known fields.");
        var objects = RecordGuard.Objects(new JsonObject { ["Title"] = new JsonObject { ["DataObject"] = "abc" }, ["bad\u0001key"] = 1 });
        check(objects != null && objects.ContainsKey("Title") && objects.Count == 1, "Profile objects are bounded, keys with control characters left out.");
        // A host's files: keys up to 256 characters ("mod:" categories hold mod ids), and what was left out is counted.
        var host = RecordGuard.Bounded(new JsonObject { ["mod:" + new string('m', 64) + "/silo"] = new JsonObject { ["v"] = 1 }, [new string('k', 257)] = 1 },
            RecordGuard.HostFileLimits, out var dropped);
        check(host != null && host.Count == 1 && host.ContainsKey("mod:" + new string('m', 64) + "/silo") && dropped == 1, $"Host file keys: {host?.ToJsonString()} ({dropped} dropped).");
        RecordGuard.Bounded(new JsonObject { ["a"] = new string('t', 300), ["b"] = 1 }, RecordGuard.HostFileLimits, out dropped);
        check(dropped == 1, $"A value past its limit is counted ({dropped}).");

        // The game's own profile objects: bounded before friends get them; too large, friends keep the last ones.
        var presence = new HellForge.LocalBackend.FriendPresence();
        var logged = new List<string>();
        presence.Log = logged.Add;
        presence.SetLocalObjects(new JsonObject { ["name"] = new JsonObject { ["EscapedDataObject"] = "\"Diver\"" }, ["bad\u0001"] = 1 });
        var first = presence.Local;
        check(first.Objects.Count == 1 && first.Objects.ContainsKey("name"), "The game's objects are bounded before they are shared: " + first.Objects.ToJsonString());
        presence.SetLocalObjects(new JsonObject(Enumerable.Range(0, 40).Select(i => KeyValuePair.Create("o" + i, (JsonNode?)new string('x', 4000)))));
        check(presence.Local.Objects.ToJsonString() == first.Objects.ToJsonString() && presence.Local.Version == first.Version && logged.Any(l => l.Contains("too large")),
            "Objects over the size limit are not shared; the last ones stay.");

        // Play with strangers, read for every stranger's frame: from the file once, again only when it changed.
        var mods = Directory.CreateDirectory(Path.Combine(root, "strangers-setting")).FullName;
        var clock = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        var setting = new StrangerSetting(mods, () => clock);
        check(!setting.Allowed && !new StrangerSetting(null).Allowed, "Play with strangers is off by default (and without a mods folder).");
        HellForge.Core.ModSettings.SetPlayWithStrangers(mods, true);
        var read = setting.Reads;
        for (var i = 0; i < 1000; i++) _ = setting.Allowed;
        check(setting.Reads == read && !setting.Allowed, "Within Recheck the file is not looked at again.");
        clock += StrangerSetting.Recheck;
        check(setting.Allowed && setting.Reads == read + 1, "After Recheck a changed file is read again.");
        clock += StrangerSetting.Recheck;
        check(setting.Allowed && setting.Reads == read + 1, "An unchanged file is not read again.");
        Console.WriteLine("PASS record guard: known fields, limits, control characters, size; the selected mission's seed; host file keys; the game's objects; the strangers setting");
    }

    // A seed in the game's layout (MissionSeed) with the given array counts, every entry filled.
    static string Seed(int words, int bytes, int few, int pair, int fourWords, int eightWords)
    {
        static string Count(int n, int digits) => n.ToString("x2") + string.Concat(Enumerable.Repeat(new string('1', digits), n));
        return MissionSeed.Version + new string('0', 58) + Count(words, 8) + Count(bytes, 2) + Count(few, 2) + Count(pair, 2) + "03" +
            Count(fourWords, 8) + "0100" + Count(eightWords, 8);
    }

    static JsonObject Deep(int levels)
    {
        var top = new JsonObject();
        var at = top;
        for (var i = 0; i < levels; i++) { var next = new JsonObject(); at["n"] = next; at = next; }
        at["leaf"] = 1;
        return top;
    }

    static int Depth(JsonNode? node) => node is JsonObject o && o["n"] is { } next ? 1 + Depth(next) : 0;
}
