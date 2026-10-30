using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// The lobby browser's API on the backend's loopback HTTP (docs/external-launchers.md "Lobby browser API"), for the
// in-game browser and launchers of one's own (Echelon):
//   GET  /hellforge/v1/lobbies              every HellForge lobby the directory has, with no planet or difficulty filter,
//                                           plus friends' lobbies heard through presence, each with its join check;
//   POST /hellforge/v1/lobbies/<id>/join    hands this machine's game an invitation to a lobby of the last list
//                                           (LobbyService.InviteLocal); the game joins only when the player accepts it.
// Both sit behind the backend's only-this-PC check. A list is searched at most once per MinInterval whoever asks: callers
// in that time share it. The multiplayer services start after the HTTP server, so they are attached later (Attach);
// until then, and in a session without multiplayer, the list says it is unavailable.
public sealed class BrowseApi
{
    public const int Version = 1;
    public static readonly TimeSpan MinInterval = TimeSpan.FromSeconds(2);
    // How long a listed lobby can still be joined by its id after the list that showed it.
    public static readonly TimeSpan JoinableFor = TimeSpan.FromMinutes(2);
    public const int MaxModsListed = 64;

    readonly object gate = new();
    readonly Func<DateTime> now;
    DirectoryService? directory;
    LobbyService? lobby;
    FriendPresence? presence;
    (DateTime At, Task<JsonObject> List)? last;
    readonly Dictionary<string, (BrowseLobby Lobby, DateTime At)> listed = new(StringComparer.Ordinal);

    public BrowseApi(Func<DateTime>? clock = null) => now = clock ?? (() => DateTime.UtcNow);

    // The multiplayer services, once they run (directory null: a session without the lobby directory).
    public void Attach(DirectoryService? directory, LobbyService lobby, FriendPresence presence)
    {
        lock (gate) (this.directory, this.lobby, this.presence) = (directory, lobby, presence);
    }

    // GET /hellforge/v1/lobbies: the list, searched again only after MinInterval.
    public Task<JsonObject> ListAsync()
    {
        lock (gate)
        {
            var at = now();
            if (last is { } previous && at - previous.At < MinInterval && !previous.List.IsFaulted) return previous.List;
            var list = SearchAsync();
            last = (at, list);
            return list;
        }
    }

    async Task<JsonObject> SearchAsync()
    {
        DirectoryService? directory;
        FriendPresence? presence;
        lock (gate) (directory, presence) = (this.directory, this.presence);
        if (presence == null) return Unavailable("Multiplayer is off for this session.");
        var lobbies = directory != null ? [.. await directory.BrowseAsync()] : new List<BrowseLobby>();
        // friends' lobbies the directory does not list (Friends only, or not listed), from their presence
        foreach (var (friend, lobbyId, connection, body) in presence.FriendLobbies())
        {
            if (lobbies.Any(l => l.LobbyId == lobbyId)) continue;
            var members = body["numLobbyMembers"] is JsonValue count && count.TryGetValue<int>(out var n) ? n : 1;
            lobbies.Add(new BrowseLobby(lobbyId, connection, friend, true, "friend", members, 4, false,
                new Dictionary<string, string>(StringComparer.Ordinal), body, new JoinCheck(true, "", null, [])));
        }
        var at = now();
        lock (gate)
        {
            foreach (var entry in lobbies) listed[entry.LobbyId] = (entry, at);
            foreach (var old in listed.Where(l => at - l.Value.At > JoinableFor).Select(l => l.Key).ToList()) listed.Remove(old);
        }
        return new JsonObject
        {
            ["version"] = Version, ["available"] = true, ["searchedUtc"] = at.ToString("O", CultureInfo.InvariantCulture),
            ["lobbies"] = new JsonArray([.. lobbies.Select(l => (JsonNode?)Describe(l, presence))])
        };
    }

    static JsonObject Unavailable(string reason) => new() { ["version"] = Version, ["available"] = false, ["reason"] = reason, ["lobbies"] = new JsonArray() };

    // One lobby as the API shows it.
    public static JsonObject Describe(BrowseLobby lobby, FriendPresence? presence)
    {
        var record = lobby.Record;
        var players = (record["players"] as JsonArray)?.OfType<JsonObject>().ToList() ?? [];
        var hostName = players.Where(p => p["isHost"]?.GetValueKind() == JsonValueKind.True).Select(p => Text(p["playerName"])).FirstOrDefault(n => n != null)
            ?? presence?.FriendName(lobby.Host);
        var join = lobby.Join;
        var item = new JsonObject
        {
            ["lobbyId"] = lobby.LobbyId, ["host"] = lobby.Host.ToString(CultureInfo.InvariantCulture), ["hostName"] = hostName,
            ["friend"] = lobby.Friend, ["source"] = lobby.Source, ["members"] = lobby.Members, ["maxMembers"] = lobby.MaxMembers,
            ["locked"] = lobby.Locked, ["privacy"] = Number(record["privacyMode"]) ?? SearchNumber(lobby, "number_key6"),
            ["difficulty"] = Number(record["difficulty"]), ["planetId"] = Number(record["planetId"]), ["race"] = Number(record["race"]),
            ["inMission"] = record["seed"] is JsonValue,
            ["players"] = new JsonArray([.. players.Select(p => (JsonNode?)Text(p["playerName"])).Where(n => n != null)]),
            ["joinable"] = join.Joinable && !lobby.Locked && lobby.Members < lobby.MaxMembers,
            ["verdict"] = Verdict(lobby), ["reason"] = Reason(lobby),
            ["downloadBytes"] = join.Plan?.DownloadBytes ?? 0
        };
        item["mods"] = Mods(join);
        return item;
    }

    // ready, download, trust, unjoinable, blocked, full, locked, or unknown (a friend's lobby: its mods are known on joining).
    static string Verdict(BrowseLobby lobby)
    {
        if (lobby.Locked) return "locked";
        if (lobby.Members >= lobby.MaxMembers) return "full";
        if (lobby.Join.Plan is { } plan) return plan.Verdict.ToString().ToLowerInvariant();
        if (!lobby.Join.Joinable) return "unjoinable";
        return lobby.Source == "friend" ? "unknown" : "ready";
    }

    static string Reason(BrowseLobby lobby) =>
        lobby.Locked ? "The host's lobby is locked."
        : lobby.Members >= lobby.MaxMembers ? "The host's lobby is full."
        : lobby.Join.Joinable ? ""
        : lobby.Join.Why.Length > 0 ? char.ToUpperInvariant(lobby.Join.Why[0]) + lobby.Join.Why[1..] + "." : "";

    // The host's mods, shared ones with what a join does to each (the plan's action, and why for one in the way), then
    // per-player ones, which never need to match.
    static JsonArray Mods(JoinCheck join)
    {
        var mods = new JsonArray();
        if (join.Set is not { } set) return mods;
        var items = join.Plan?.Items ?? [];
        foreach (var mod in set.Mods.Take(MaxModsListed))
        {
            var item = new JsonObject
            {
                ["id"] = mod.Id, ["name"] = mod.Name, ["version"] = mod.Version, ["tier"] = ModSets.TierName(mod.Tier),
                ["class"] = ModSets.ClassName(mod.Class)
            };
            if (mod.Tier == CoopTier.Shared && items.FirstOrDefault(i => i.Mod.Id == mod.Id) is { } planned)
            {
                item["action"] = ModSyncPlan.ActionName(planned.Action);
                if (join.Blocking.FirstOrDefault(b => b.Mod.Id == mod.Id) is { } blocker) item["why"] = blocker.Why;
            }
            if (mod.Url != null) item["url"] = mod.Url;
            mods.Add((JsonNode?)item);
        }
        return mods;
    }

    // POST /hellforge/v1/lobbies/<id>/join: an invitation for this machine's game to a lobby of a recent list.
    public (int Status, string Outcome, JsonObject Body) Join(string lobbyId)
    {
        LobbyService? lobby;
        BrowseLobby? entry = null;
        lock (gate)
        {
            lobby = this.lobby;
            if (listed.TryGetValue(lobbyId, out var found) && now() - found.At <= JoinableFor) entry = found.Lobby;
        }
        if (lobby == null) return (503, "browse_unavailable", Error("HELLFORGE_MULTIPLAYER_OFF", "Multiplayer is off for this session."));
        if (entry == null) return (404, "browse_lobby_unknown", Error("HELLFORGE_LOBBY_UNKNOWN", "That lobby is not in a recent list: list the lobbies again."));
        if (entry.Locked || entry.Members >= entry.MaxMembers || !entry.Join.Joinable)
            return (409, "browse_lobby_not_joinable", Error("HELLFORGE_LOBBY_NOT_JOINABLE", Reason(entry)));
        lobby.InviteLocal(entry.LobbyId, entry.ConnectionString, LobbyService.EntityId(entry.Host), "chosen in the lobby browser");
        return (202, "browse_invited", new JsonObject
        {
            ["version"] = Version, ["lobbyId"] = entry.LobbyId, ["state"] = "invited",
            ["message"] = "Accept the invitation in game to join. Other shared mods bring HellForge's MOD SYNC prompt."
        });
    }

    static JsonObject Error(string code, string message) => new() { ["errorCode"] = code, ["message"] = message };

    static string? Text(JsonNode? node) => node is JsonValue value && value.TryGetValue<string>(out var text) && !string.IsNullOrWhiteSpace(text) ? text : null;

    static long? Number(JsonNode? node) => node is JsonValue value && value.TryGetValue<long>(out var number) ? number : null;

    static long? SearchNumber(BrowseLobby lobby, string key) =>
        lobby.SearchProperties.TryGetValue(key, out var text) && long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n) ? n : null;
}
