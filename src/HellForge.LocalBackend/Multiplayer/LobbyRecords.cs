using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend.Multiplayer;

// The game's lobby record (PUT /lobby, FriendPresence) and live record (PUT /lobby/<id>/live) as the lobby directory
// carries them. Both directions rebuild a record from a fixed schema instead of passing JSON through:
//   publishing (DirectoryService.Public): only the fields below, wrong kinds dropped, every account id but the host's 0,
//            player names kept (decided 2026-10-22: the map's hover card shows them);
//   reading a stranger's listing (DirectoryService.Valid): a known field of the wrong kind, a value out of range or a list
//            too long refuses the whole listing; unknown fields are dropped. Nothing else reaches the game.
// The fields are those the game's record builder writes (game.dll 0x12CB190.., PUT /lobby; strings, integers and booleans
// told apart by the JSON writer's slots +0xE8, +0xF8, +0x108 and 0x1749B10 for startTime) and those FriendPresence
// completes for the parser (0x1053F60): numSessionMembers, completedObjectives, planetType, transitionTarget, missionId.
// "seed" is written only once a mission is selected, and is kept only in its exact layout (MissionSeed).
public static class LobbyRecords
{
    public const int MaxPlayers = 4, MaxNameLength = 32, MaxMissions = 16;
    enum Kind { Integer, Long, Bool, Text, Hash, Seed, AccountIds, Players, Missions }

    static readonly Dictionary<string, (Kind Kind, long Min, long Max)> Fields = new(StringComparer.Ordinal)
    {
        // seed: the selected mission, which the map draws a scanned lobby's marker from (MissionSeed)
        ["buildGameHash"] = (Kind.Hash, 0, 64), ["seed"] = (Kind.Seed, 0, MissionSeed.MaxLength),
        ["numLobbyMembers"] = (Kind.Integer, 0, 8), ["numSessionMembers"] = (Kind.Integer, 0, 8), ["privacyMode"] = (Kind.Integer, 0, 3),
        ["difficulty"] = (Kind.Integer, 0, 20), ["state"] = (Kind.Integer, int.MinValue, int.MaxValue), ["sosBeacons"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["gameModeProgress"] = (Kind.Integer, int.MinValue, int.MaxValue), ["operationProgress"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["warId"] = (Kind.Integer, int.MinValue, int.MaxValue), ["planetId"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["territoryId"] = (Kind.Integer, int.MinValue, int.MaxValue), ["race"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["hostability"] = (Kind.Integer, int.MinValue, int.MaxValue), ["crossplayMode"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["preferenceGroup"] = (Kind.Integer, int.MinValue, int.MaxValue), ["onboardingState"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["operationSeed"] = (Kind.Integer, uint.MinValue, uint.MaxValue), ["operationType"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["regionIndex"] = (Kind.Integer, int.MinValue, int.MaxValue), ["completedObjectives"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["planetType"] = (Kind.Integer, int.MinValue, int.MaxValue), ["transitionTarget"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["startTime"] = (Kind.Long, 0, long.MaxValue), ["inLoadout"] = (Kind.Bool, 0, 0),
        ["memberAccountIds"] = (Kind.AccountIds, 0, MaxPlayers), ["players"] = (Kind.Players, 0, MaxPlayers),
        ["operationMissionStates"] = (Kind.Missions, 0, MaxMissions)
    };

    static readonly Dictionary<string, (Kind Kind, long Min, long Max)> PlayerFields = new(StringComparer.Ordinal)
    {
        ["playerName"] = (Kind.Text, 0, MaxNameLength), ["memberAccountId"] = (Kind.AccountIds, 0, 0), ["isHost"] = (Kind.Bool, 0, 0),
        ["hasInfo"] = (Kind.Bool, 0, 0), ["sourcePlatform"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["experience"] = (Kind.Integer, int.MinValue, int.MaxValue), ["playerRank"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["commendRank"] = (Kind.Integer, int.MinValue, int.MaxValue)
    };

    static readonly Dictionary<string, (Kind Kind, long Min, long Max)> MissionFields = new(StringComparer.Ordinal)
    {
        ["index"] = (Kind.Integer, int.MinValue, int.MaxValue), ["type"] = (Kind.Integer, int.MinValue, int.MaxValue),
        ["state"] = (Kind.Integer, int.MinValue, int.MaxValue), ["missionId"] = (Kind.Text, 0, 64)
    };

    // The record rebuilt from the schema. strict: null when a known field is wrong (a stranger's listing); otherwise wrong
    // fields are left out (this machine's own record, published). host: every account id but this one becomes 0 (0: kept).
    public static JsonObject? Clean(JsonObject record, bool strict, ulong host = 0) => Object(record, Fields, strict, host);

    static JsonObject? Object(JsonObject source, Dictionary<string, (Kind Kind, long Min, long Max)> fields, bool strict, ulong host)
    {
        var clean = new JsonObject();
        foreach (var (key, value) in source)
        {
            if (!fields.TryGetValue(key, out var field)) continue;
            var kept = Value(value, field.Kind, field.Min, field.Max, strict, host);
            if (kept != null) clean[key] = kept;
            else if (strict) return null;
        }
        return clean;
    }

    static JsonNode? Value(JsonNode? value, Kind kind, long min, long max, bool strict, ulong host)
    {
        switch (kind)
        {
            case Kind.Integer or Kind.Long: return Whole(value, min, max);
            case Kind.Bool: return value?.GetValueKind() is JsonValueKind.True or JsonValueKind.False ? JsonValue.Create(value.GetValue<bool>()) : null;
            case Kind.Text:
            case Kind.Hash: return Text(value, kind, max);
            case Kind.Seed:
                return value?.GetValueKind() == JsonValueKind.String && value.GetValue<string>() is var seed && MissionSeed.Valid(seed) ? JsonValue.Create(seed) : null;
            case Kind.AccountIds when max == 0: return Account(value, host);
            case Kind.AccountIds: return Accounts(value, max, strict, host);
            case Kind.Players or Kind.Missions: return Objects(value, kind == Kind.Players ? PlayerFields : MissionFields, max, strict, host);
        }
        return null;
    }

    static JsonNode? Whole(JsonNode? value, long min, long max) =>
        value is JsonValue number && number.GetValueKind() == JsonValueKind.Number && number.TryGetValue<long>(out var whole) && whole >= min && whole <= max
            ? JsonValue.Create(whole) : null;

    // A text of at most max characters, no control characters; a hash only letters and digits.
    static JsonNode? Text(JsonNode? value, Kind kind, long max)
    {
        if (value?.GetValueKind() != JsonValueKind.String) return null;
        var text = value.GetValue<string>();
        if (text.Length > max || text.Any(char.IsControl) || kind == Kind.Hash && !text.All(char.IsAsciiLetterOrDigit)) return null;
        return JsonValue.Create(text);
    }

    // A list of at most max account ids (Account); strict: null when one is not one, else it is left out.
    static JsonNode? Accounts(JsonNode? value, long max, bool strict, ulong host)
    {
        if (value is not JsonArray ids || ids.Count > max) return null;
        var clean = new JsonArray();
        foreach (var id in ids)
            if (Account(id, host) is { } kept) clean.Add((JsonNode?)kept);
            else if (strict) return null;
        return clean;
    }

    // A list of at most max objects of these fields (players, missions); strict: null when one is wrong, else it is left out.
    static JsonNode? Objects(JsonNode? value, Dictionary<string, (Kind Kind, long Min, long Max)> fields, long max, bool strict, ulong host)
    {
        if (value is not JsonArray list || list.Count > max) return null;
        var clean = new JsonArray();
        foreach (var item in list)
            if (item is JsonObject entry && Object(entry, fields, strict, host) is { } kept) clean.Add((JsonNode?)kept);
            else if (strict) return null;
        return clean;
    }

    // An account id: the SteamID64 in decimal, as text ("%llu") or a number; others than host become 0 of the same kind.
    static JsonNode? Account(JsonNode? value, ulong host)
    {
        ulong id;
        bool text;
        if (value?.GetValueKind() == JsonValueKind.String && value.GetValue<string>() is { Length: > 0 and <= 20 } digits && digits.All(char.IsAsciiDigit) &&
            ulong.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out id)) text = true;
        else if (value is JsonValue number && number.GetValueKind() == JsonValueKind.Number && number.TryGetValue(out id)) text = false;
        else return null;
        if (host != 0 && id != host) id = 0;
        return text ? JsonValue.Create(id.ToString(CultureInfo.InvariantCulture)) : JsonValue.Create(id);
    }

    // The live record (missionTime, playerPositions, objective and outpost masks, state; never seen from a solo host): at
    // most 32 fields named like the game's, each a number, a boolean, a short text, or a list of at most 16 numbers,
    // booleans or flat objects of those. Null when anything else is in it: the listing stays, without its live record.
    public static JsonObject? CleanLive(JsonObject live)
    {
        if (live.Count > 32) return null;
        var clean = new JsonObject();
        foreach (var (key, value) in live)
        {
            if (key.Length is 0 or > 32 || !char.IsAsciiLetter(key[0]) || !key.All(char.IsAsciiLetterOrDigit)) return null;
            var kept = Flat(value) ?? (value is JsonArray list && list.Count <= 16 ? List(list) : null);
            if (kept == null) return null;
            clean[key] = kept;
        }
        return clean;

        static JsonNode? Flat(JsonNode? value) => value?.GetValueKind() switch
        {
            JsonValueKind.Number when value.AsValue().TryGetValue<double>(out var number) && double.IsFinite(number) => JsonValue.Create(number),
            JsonValueKind.True or JsonValueKind.False => JsonValue.Create(value.GetValue<bool>()),
            JsonValueKind.String when value.GetValue<string>() is { Length: <= 64 } text && !text.Any(char.IsControl) => JsonValue.Create(text),
            _ => null
        };
        static JsonArray? List(JsonArray list)
        {
            var clean = new JsonArray();
            foreach (var item in list)
            {
                if (Flat(item) is { } flat) { clean.Add((JsonNode?)flat); continue; }
                if (item is not JsonObject entry || entry.Count > 8) return null;
                var copy = new JsonObject();
                foreach (var (key, value) in entry)
                    if (key.Length is > 0 and <= 32 && key.All(char.IsAsciiLetterOrDigit) && Flat(value) is { } kept && kept.GetValueKind() != JsonValueKind.String) copy[key] = kept;
                    else return null;
                clean.Add((JsonNode?)copy);
            }
            return clean;
        }
    }
}
