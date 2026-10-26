using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Local stand-in for the PlayFab web API the engine calls (stingray playfab_web_api.cpp, base URL redirected to
// "<backend>/playfab" by scripts/LocalRoutingPatch.cs). Nothing is forwarded to PlayFab. Identities are derived from
// the Steam ID inside the client's own session ticket, so every HellForge backend gives a friend the same IDs without
// exchanging anything: title player account (the game's peer id) = SteamID64 in hex. The ticket itself is never stored.
// Request/response shapes follow PlayFab's public REST API; the engine reads SessionTicket, PlayFabId, EntityToken
// {EntityToken, TokenExpiration, Entity}, InfoResultPayload (AccountInfo, TitleInfo, SteamInfo, PlayerProfile), Data,
// TitlePlayerAccounts, Profiles (Lineage ids copied into 21-byte buffers; Objects read through EscapedDataObject only)
// and errorCode/errorMessage. Profiles of players this backend has no objects for carry an empty Objects map, which the
// engine skips.
public sealed class PlayFabService
{
    public const string Prefix = "/playfab";
    const ulong MasterOffset = 0x1000000000000000;

    // Objects set by players through Object/SetObjects (testament_platform_user_name etc.), keyed by entity id. Bounded:
    // the game sets a few small objects for its own entity, and what it sets is shared with friends.
    public const int MaxEntities = 16, MaxObjectsPerEntity = 32, MaxObjectBytes = 16 << 10;
    readonly ConcurrentDictionary<string, JsonObject> objects = new(StringComparer.OrdinalIgnoreCase);
    readonly ConcurrentDictionary<string, string> displayNames = new(StringComparer.OrdinalIgnoreCase);
    readonly FriendPresence? presence;
    ulong? localSteamId;

    // With presence, Steam friends who run HellForge are the friends list, with the profile objects their backends shared.
    public PlayFabService(FriendPresence? presence = null) => this.presence = presence;

    public static bool Handles(string path) => path.StartsWith(Prefix + "/", StringComparison.Ordinal);

    public static string TitlePlayerId(ulong steamId) => steamId.ToString("X", CultureInfo.InvariantCulture);
    public static string MasterId(ulong steamId) => (steamId + MasterOffset).ToString("X", CultureInfo.InvariantCulture);

    // Steam session tickets start with the GC token block: u32 length (20), u64 token, u64 SteamID, u32 time.
    public static ulong? SteamIdFromTicket(string? hex)
    {
        if (hex is null || hex.Length < 40 || hex.Length % 2 != 0) return null;
        byte[] raw;
        try { raw = Convert.FromHexString(hex); }
        catch (FormatException) { return null; }
        if (BinaryPrimitives.ReadUInt32LittleEndian(raw) != 20) return null;
        var id = BinaryPrimitives.ReadUInt64LittleEndian(raw.AsSpan(12));
        // Public universe (1), individual account (1).
        return id >> 56 == 1 && (id >> 52 & 0xF) == 1 ? id : null;
    }

    public (int Status, JsonObject Reply, string Outcome) Handle(string path, JsonElement body)
    {
        var call = path[Prefix.Length..];
        return call switch
        {
            "/Client/LoginWithSteam" => Login(body),
            "/Client/GetPlayerCombinedInfo" => localSteamId is { } me ? Ok(new JsonObject { ["PlayFabId"] = MasterId(me), ["InfoResultPayload"] = Payload(me) }, "pf_combined_info") : NotLoggedIn(),
            "/Client/GetPlayFabIDsFromSteamIDs" => SteamIds(body),
            "/Profile/GetTitlePlayersFromMasterPlayerAccountIds" => TitlePlayers(body),
            "/Profile/GetProfile" => Profiles(body, single: true),
            "/Profile/GetProfiles" => Profiles(body, single: false),
            "/Object/SetObjects" => SetObjects(body),
            "/Client/UpdateUserTitleDisplayName" => DisplayName(body),
            "/Client/GetFriendsList" => FriendsList(),
            "/Client/ReportPlayer" => Ok(new JsonObject { ["SubmissionsRemaining"] = 5 }, "pf_report_ignored"),
            _ => Error(400, "NotImplemented", 1000, "HellForge local PlayFab does not implement " + call, "pf_unsupported")
        };
    }

    // Several installations on one PC share a Steam account: HELLFORGE_IDENTITY (a SteamID64, set with HELLFORGE_MULTI_INSTANCE)
    // logs this installation's game in as another player, the id its backend has on the loopback link (docs/multi-instance.md).
    public static ulong? IdentityOverride(Func<string, string?> environment)
    {
        if (environment("HELLFORGE_MULTI_INSTANCE") != "1" || !ulong.TryParse(environment("HELLFORGE_IDENTITY"), NumberStyles.None, CultureInfo.InvariantCulture, out var id)) return null;
        return id >> 56 == 1 && (id >> 52 & 0xF) == 1 ? id : null;
    }

    static readonly ulong? identity = IdentityOverride(Environment.GetEnvironmentVariable);

    (int, JsonObject, string) Login(JsonElement body)
    {
        var steamId = SteamIdFromTicket(body.ValueKind == JsonValueKind.Object && body.TryGetProperty("SteamTicket", out var t) ? t.GetString() : null);
        if (steamId is not { } ticketId) return Error(400, "InvalidSteamTicket", 1010, "Steam ticket not recognised.", "pf_login_ticket_unrecognised");
        var id = identity ?? ticketId;
        localSteamId = id;
        var now = DateTime.UtcNow;
        return Ok(new JsonObject
        {
            ["SessionTicket"] = "hellforge-pf-" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16)),
            ["PlayFabId"] = MasterId(id),
            ["NewlyCreated"] = false,
            ["LastLoginTime"] = now.ToString("o"),
            ["EntityToken"] = new JsonObject
            {
                ["EntityToken"] = "hellforge-entity-" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16)),
                ["TokenExpiration"] = now.AddDays(1).ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
                ["Entity"] = Entity(id)
            },
            ["InfoResultPayload"] = Payload(id)
        }, "pf_login_local");
    }

    static JsonObject Entity(ulong id) => new() { ["Id"] = TitlePlayerId(id), ["Type"] = "title_player_account", ["TypeString"] = "title_player_account" };

    JsonObject Payload(ulong id)
    {
        var name = displayNames.GetValueOrDefault(TitlePlayerId(id));
        var created = new DateTime(2024, 2, 8, 0, 0, 0, DateTimeKind.Utc).ToString("o");
        return new JsonObject
        {
            ["AccountInfo"] = new JsonObject
            {
                ["PlayFabId"] = MasterId(id), ["Created"] = created,
                ["TitleInfo"] = new JsonObject
                {
                    ["DisplayName"] = name, ["Origination"] = "Steam", ["Created"] = created, ["FirstLogin"] = created,
                    ["LastLogin"] = DateTime.UtcNow.ToString("o"), ["isBanned"] = false,
                    ["TitlePlayerAccount"] = new JsonObject { ["Id"] = TitlePlayerId(id), ["Type"] = "title_player_account", ["TypeString"] = "title_player_account" }
                },
                ["SteamInfo"] = new JsonObject { ["SteamId"] = id.ToString(CultureInfo.InvariantCulture), ["SteamCountry"] = "", ["SteamCurrency"] = "", ["SteamActivationStatus"] = "Activated" }
            },
            ["PlayerProfile"] = new JsonObject { ["PlayerId"] = MasterId(id), ["DisplayName"] = name, ["Locations"] = new JsonArray() }
        };
    }

    // {"SteamStringIDs": ["7656..."]} -> Data [{SteamStringId, PlayFabId}]
    static (int, JsonObject, string) SteamIds(JsonElement body)
    {
        var data = new JsonArray();
        foreach (var value in Strings(body, "SteamStringIDs"))
            if (ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var id))
                data.Add((JsonNode?)new JsonObject { ["SteamStringId"] = value, ["PlayFabId"] = MasterId(id) });
        return Ok(new JsonObject { ["Data"] = data }, "pf_steam_ids");
    }

    static (int, JsonObject, string) TitlePlayers(JsonElement body)
    {
        var accounts = new JsonObject();
        foreach (var master in Strings(body, "MasterPlayerAccountIds"))
            if (ulong.TryParse(master, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value) && value >= MasterOffset)
                accounts[master] = new JsonObject { ["Id"] = TitlePlayerId(value - MasterOffset), ["Type"] = "title_player_account" };
        return Ok(new JsonObject { ["TitleId"] = "974A9", ["TitlePlayerAccounts"] = accounts }, "pf_title_players");
    }

    (int, JsonObject, string) FriendsList()
    {
        var friends = new JsonArray();
        foreach (var friend in presence?.Friends ?? [])
            friends.Add((JsonNode?)new JsonObject
            {
                ["FriendPlayFabId"] = MasterId(friend.SteamId), ["TitleDisplayName"] = friend.Name, ["Username"] = friend.Name,
                ["SteamInfo"] = new JsonObject { ["SteamId"] = friend.SteamId.ToString(CultureInfo.InvariantCulture) }, ["Tags"] = new JsonArray()
            });
        return Ok(new JsonObject { ["Friends"] = friends }, friends.Count > 0 ? "pf_friends_hellforge" : "pf_friends_empty");
    }

    // The entity ids a GetProfile (its Entity) or GetProfiles (its Entities) body asks for.
    static List<string> ProfileIds(JsonElement body, bool single)
    {
        var ids = new List<string>();
        if (single && body.ValueKind == JsonValueKind.Object && body.TryGetProperty("Entity", out var entity) && entity.TryGetProperty("Id", out var one))
            ids.Add(one.GetString() ?? "");
        if (!single && body.ValueKind == JsonValueKind.Object && body.TryGetProperty("Entities", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var e in list.EnumerateArray())
                if (e.ValueKind == JsonValueKind.Object && e.TryGetProperty("Id", out var id)) ids.Add(id.GetString() ?? "");
        return ids;
    }

    (int, JsonObject, string) Profiles(JsonElement body, bool single)
    {
        var ids = ProfileIds(body, single);
        var profiles = new JsonArray();
        foreach (var id in ids)
        {
            if (!ulong.TryParse(id, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var steamId)) continue;
            profiles.Add((JsonNode?)new JsonObject
            {
                ["Entity"] = new JsonObject { ["Id"] = id, ["Type"] = "title_player_account" },
                ["EntityChain"] = "title_player_account!" + id, ["VersionNumber"] = 1,
                ["Objects"] = objects.TryGetValue(id, out var stored) ? stored.DeepClone() : presence?.FriendObjects(steamId) ?? new JsonObject(),
                ["DisplayName"] = displayNames.GetValueOrDefault(id) ?? presence?.FriendName(steamId),
                ["Lineage"] = new JsonObject { ["MasterPlayerAccountId"] = MasterId(steamId), ["TitlePlayerAccountId"] = id, ["TitleId"] = "974A9" }
            });
        }
        return single
            ? Ok(new JsonObject { ["Profile"] = profiles.Count > 0 ? profiles[0]!.DeepClone() : null }, "pf_profile")
            : Ok(new JsonObject { ["Profiles"] = profiles }, "pf_profiles");
    }

    (int, JsonObject, string) SetObjects(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty("Entity", out var entity) || !entity.TryGetProperty("Id", out var idElement) ||
            idElement.GetString() is not { Length: > 0 } id || !body.TryGetProperty("Objects", out var list) || list.ValueKind != JsonValueKind.Array)
            return Error(400, "InvalidParams", 1000, "Entity and Objects are required.", "pf_set_objects_rejected");
        if (!objects.ContainsKey(id) && objects.Count >= MaxEntities)
            return Error(400, "InvalidParams", 1000, "Too many entities.", "pf_set_objects_rejected");
        var stored = objects.GetOrAdd(id, _ => new JsonObject());
        var results = new JsonArray();
        lock (stored)
            foreach (var item in list.EnumerateArray())
                if (SetObject(stored, item) is { } result) results.Add((JsonNode?)result);
        if (presence != null && localSteamId is { } me && string.Equals(id, TitlePlayerId(me), StringComparison.OrdinalIgnoreCase))
            lock (stored) presence.SetLocalObjects(stored);
        return Ok(new JsonObject { ["ProfileVersion"] = 1, ["SetResults"] = results }, "pf_objects_set");
    }

    // One object of a SetObjects body into the entity's stored objects, or rejected over the limits; its set result, or
    // null for an item that names no object or carries no DataObject.
    static JsonObject? SetObject(JsonObject stored, JsonElement item)
    {
        if (item.ValueKind != JsonValueKind.Object || !item.TryGetProperty("ObjectName", out var nameElement) || nameElement.GetString() is not { Length: > 0 and <= 64 } name) return null;
        if (!item.TryGetProperty("DataObject", out var value)) return null;
        if (value.GetRawText().Length > MaxObjectBytes || (!stored.ContainsKey(name) && stored.Count >= MaxObjectsPerEntity))
            return new JsonObject { ["ObjectName"] = name, ["OperationReason"] = "Rejected", ["SetResult"] = "None" };
        // The engine reads only EscapedDataObject (the object's JSON text) and dereferences it unchecked
        // (exe 0x8D4710): a profile object without it crashes the client.
        stored[name] = new JsonObject
        {
            ["ObjectName"] = name, ["DataObject"] = JsonNode.Parse(value.GetRawText()), ["EscapedDataObject"] = value.GetRawText()
        };
        return new JsonObject { ["ObjectName"] = name, ["OperationReason"] = "Updated", ["SetResult"] = "Updated" };
    }

    (int, JsonObject, string) DisplayName(JsonElement body)
    {
        var name = body.ValueKind == JsonValueKind.Object && body.TryGetProperty("DisplayName", out var n) ? n.GetString() : null;
        if (localSteamId is not { } me || string.IsNullOrEmpty(name)) return Error(400, "InvalidParams", 1000, "DisplayName required.", "pf_display_name_rejected");
        displayNames[TitlePlayerId(me)] = name;
        return Ok(new JsonObject { ["DisplayName"] = name }, "pf_display_name_set");
    }

    (int, JsonObject, string) NotLoggedIn() => Error(401, "NotAuthenticated", 1074, "Log in first.", "pf_not_logged_in");

    static IEnumerable<string> Strings(JsonElement body, string key)
    {
        if (body.ValueKind == JsonValueKind.Object && body.TryGetProperty(key, out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var item in list.EnumerateArray())
                if (item.ValueKind == JsonValueKind.String) yield return item.GetString()!;
    }

    static (int, JsonObject, string) Ok(JsonObject data, string outcome) =>
        (200, new JsonObject { ["code"] = 200, ["status"] = "OK", ["data"] = data }, outcome);

    static (int, JsonObject, string) Error(int code, string error, int errorCode, string message, string outcome) =>
        (code, new JsonObject { ["code"] = code, ["status"] = code == 401 ? "Unauthorized" : "BadRequest", ["error"] = error, ["errorCode"] = errorCode, ["errorMessage"] = message }, outcome);
}
