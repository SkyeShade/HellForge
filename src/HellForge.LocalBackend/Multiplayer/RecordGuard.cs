using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend.Multiplayer;

// Lobby and profile records bounded before they leave this PC or reach the game: the game's own uploads before they are
// shared with friends or published in the lobby directory (so code in the game cannot use them to carry data out), and
// friends' and strangers' records before the game reads them (it was written for records from Arrowhead's servers).
// A lobby record keeps only the fields the game uploads (docs/multiplayer.md step 7), its players only theirs; every
// value has a depth, list length and text length a game record does not need, with control characters out of text.
// Null when the result is still too large.
public static class RecordGuard
{
    public sealed record Limits(int Bytes, int Depth, int Items, int Text, int KeyLength = 64);

    public static readonly Limits LobbyLimits = new(16 << 10, 6, 64, 256), ObjectLimits = new(64 << 10, 8, 256, 4096),
        // a host's Local Stats, mod content values and arm swaps (StatsShare): up to StatsData.MaxEdits values; keys are
        // targets and "mod:" categories, which hold mod ids (up to 64 characters) and content names
        HostFileLimits = new(1 << 20, 6, 8192, 256, 256);

    static readonly HashSet<string> LobbyFields = new(StringComparer.Ordinal)
    {
        "startTime", "lobbyId", "players", "connectionString", "operationProgress", "gameModeProgress", "memberAccountIds",
        "inLoadout", "buildGameHash", "warId", "state", "difficulty", "hostability", "privacyMode", "onboardingState",
        "numLobbyMembers", "sosBeacons", "planetId", "territoryId", "race", "crossplayMode", "preferenceGroup", "regionIndex",
        "operationSeed", "operationType", "operationMissionStates", "seed"
    };
    static readonly HashSet<string> PlayerFields = new(StringComparer.Ordinal)
    {
        "memberAccountId", "playerName", "playerRank", "commendRank", "experience", "sourcePlatform", "hasInfo", "isHost"
    };
    static readonly HashSet<string> LiveFields = new(StringComparer.Ordinal)
    {
        "missionTime", "playerPositions", "objectiveStatusMask", "outpostStatusMask", "state"
    };

    // PUT /lobby's record (the game's, a friend's, a listing's).
    public static JsonObject? Lobby(JsonObject record) => Clean(record, LobbyFields, LobbyLimits, players: true, out _);

    // PUT /lobby/<id>/live's state.
    public static JsonObject? Live(JsonObject record) => Clean(record, LiveFields, LobbyLimits, players: false, out _);

    // A friend's PlayFab profile objects (presence): no known field list, so only bounded.
    public static JsonObject? Objects(JsonObject objects) => Clean(objects, null, ObjectLimits, players: false, out _);

    // Any other object: bounded only. `dropped`: how many keys and values were left out (beyond a limit).
    public static JsonObject? Bounded(JsonObject value, Limits limits, out int dropped) => Clean(value, null, limits, players: false, out dropped);

    sealed class Count { public int Dropped; }

    static JsonObject? Clean(JsonObject record, IReadOnlySet<string>? fields, Limits limits, bool players, out int dropped)
    {
        var count = new Count();
        var copy = new JsonObject();
        foreach (var (key, value) in record)
        {
            if (fields != null ? !fields.Contains(key) : !Key(key, limits, count)) continue;
            var cleaned = players && key == "players" ? Players(value, limits, count)
                : players && key == "seed" ? Seed(value, count) : Value(value, limits, 1, count);
            if (cleaned.Ok) copy[key] = cleaned.Node;
        }
        dropped = count.Dropped;
        return Encoding.UTF8.GetByteCount(copy.ToJsonString()) <= limits.Bytes ? copy : null;
    }

    static (bool Ok, JsonNode? Node) Players(JsonNode? value, Limits limits, Count count)
    {
        if (value is not JsonArray list || list.Count > limits.Items) { count.Dropped++; return (false, null); }
        var players = new JsonArray();
        foreach (var player in list.OfType<JsonObject>())
        {
            var kept = new JsonObject();
            foreach (var (key, field) in player)
                if (PlayerFields.Contains(key) && Value(field, limits, 3, count) is { Ok: true } cleaned) kept[key] = cleaned.Node;
            players.Add((JsonNode)kept);
        }
        return (true, players);
    }

    // A lobby record's selected mission (MissionSeed), longer than other text and kept only in its exact layout.
    static (bool Ok, JsonNode? Node) Seed(JsonNode? value, Count count)
    {
        if (value is JsonValue scalar && scalar.GetValueKind() == JsonValueKind.String && MissionSeed.Valid(scalar.GetValue<string>()))
            return (true, scalar.DeepClone());
        count.Dropped++;
        return (false, null);
    }

    static (bool Ok, JsonNode? Node) Value(JsonNode? value, Limits limits, int depth, Count count)
    {
        var result = Kept(value, limits, depth, count);
        if (!result.Ok) count.Dropped++;
        return result;
    }

    static (bool Ok, JsonNode? Node) Kept(JsonNode? value, Limits limits, int depth, Count count)
    {
        switch (value)
        {
            case null: return (true, null);
            case JsonValue scalar: return Scalar(scalar, limits);
            case JsonArray list when depth < limits.Depth && list.Count <= limits.Items: return (true, List(list, limits, depth, count));
            case JsonObject node when depth < limits.Depth && node.Count <= limits.Items: return (true, Object(node, limits, depth, count));
            default: return (false, null);
        }
    }

    // A text within limits.Text (its control characters dropped), a finite number, a boolean or null.
    static (bool Ok, JsonNode? Node) Scalar(JsonValue scalar, Limits limits) => scalar.GetValueKind() switch
    {
        JsonValueKind.String => scalar.GetValue<string>() is var text && text.Length <= limits.Text
            ? (true, JsonValue.Create(new string([.. text.Where(c => !char.IsControl(c))]))) : (false, null),
        JsonValueKind.Number => double.TryParse(scalar.ToJsonString(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var number) &&
            double.IsFinite(number) ? (true, scalar.DeepClone()) : (false, null),
        JsonValueKind.True or JsonValueKind.False or JsonValueKind.Null => (true, scalar.DeepClone()),
        _ => (false, null)
    };

    static JsonArray List(JsonArray list, Limits limits, int depth, Count count)
    {
        var copy = new JsonArray();
        foreach (var item in list)
            if (Value(item, limits, depth + 1, count) is { Ok: true } cleaned) copy.Add(cleaned.Node);
        return copy;
    }

    static JsonObject Object(JsonObject node, Limits limits, int depth, Count count)
    {
        var copy = new JsonObject();
        foreach (var (key, item) in node)
            if (Key(key, limits, count) && Value(item, limits, depth + 1, count) is { Ok: true } cleaned) copy[key] = cleaned.Node;
        return copy;
    }

    static bool Key(string key, Limits limits, Count count)
    {
        if (key.Length > 0 && key.Length <= limits.KeyLength && !key.Any(char.IsControl)) return true;
        count.Dropped++;
        return false;
    }
}
