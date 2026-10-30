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

// The lobby browser in the game (HfApi browse_state, browse_lobby and browse_ask, SDK 16): an editor's asks over the
// session's live-edit pipe ("browseask refresh|join <lobby id>", native/mod-loader/live_browse.c), answered with the API's
// own list (BrowseApi.ListAsync) as one "browselobby" line a lobby and then "browsestate", which publishes them, and a
// join through the API's (BrowseApi.Join). Asks are answered one at a time, in order.
public sealed class LiveBrowse
{
    // The loader's limit (live_browse.c BROWSE_MAX_LOBBIES): past it the message says how many more there are.
    public const int MaxLobbies = 128;

    readonly BrowseApi api;
    readonly LiveEditServer live;
    readonly Action<string> log;
    readonly SemaphoreSlim order = new(1, 1);
    int attempts;
    // the list the game shows: its version and how many lobbies (0, 0: none yet)
    (int Version, int Lobbies) shown;

    public LiveBrowse(BrowseApi api, LiveEditServer live, Action<string> log)
    {
        (this.api, this.live, this.log) = (api, live, log);
        live.Received += parts => { if (parts[0] == "browseask") _ = AskAsync(parts); };
        live.StateChanged += () => { if (!live.Connected) shown = (0, 0); }; // the loader forgets the list with the pipe
    }

    // "browseask" refresh - | join <lobby id>.
    public async Task AskAsync(IReadOnlyList<string> parts)
    {
        if (parts.Count != 3 || parts[0] != "browseask") return;
        await order.WaitAsync();
        try
        {
            if (parts[1] == "refresh") await RefreshAsync();
            else if (parts[1] == "join" && parts[2] != "-") await JoinAsync(parts[2]);
        }
        catch (Exception error) { log("Lobby browser: the game's ask failed: " + error.Message); }
        finally { order.Release(); }
    }

    async Task RefreshAsync()
    {
        if (shown.Version > 0) await live.BrowseStateAsync(shown.Version, "searching", shown.Lobbies, "");
        var list = await api.ListAsync();
        var version = ++attempts;
        if (list["available"]?.GetValueKind() != JsonValueKind.True)
        {
            if ((await live.BrowseStateAsync(version, "unavailable", 0, list["reason"]?.GetValue<string>() ?? "")).Ok) shown = (version, 0);
            return;
        }
        var all = (list["lobbies"] as JsonArray)?.OfType<JsonObject>().Select(Line).OfType<LiveLobby>().ToList() ?? [];
        var lobbies = all.Take(MaxLobbies).ToList();
        for (var i = 0; i < lobbies.Count; i++)
            if (await live.BrowseLobbyAsync(version, i, lobbies[i]) is { Ok: false } refused) { log("Lobby browser: the game refused a lobby: " + refused.Detail); return; }
        var message = all.Count > MaxLobbies ? $"({all.Count - MaxLobbies} more lobbies are not listed.)" : "";
        if ((await live.BrowseStateAsync(version, "ready", lobbies.Count, message)).Ok) shown = (version, lobbies.Count);
    }

    async Task JoinAsync(string lobbyId)
    {
        var (status, outcome, body) = api.Join(lobbyId);
        log($"Lobby browser: the game asked to join {lobbyId}: {outcome}.");
        await live.BrowseStateAsync(shown.Version, status == 202 ? "invited" : "failed", shown.Lobbies, body["message"]?.GetValue<string>() ?? "");
    }

    // A lobby of the API's list as the loader reads it; null for one it could not take (a lobby id it refuses).
    public static LiveLobby? Line(JsonObject lobby)
    {
        var id = lobby["lobbyId"]?.GetValue<string>() ?? "";
        if (id.Length is 0 or > 64 || id.Any(c => c <= ' ' || c >= 0x7F)) return null;
        var flags = new List<string>();
        if (lobby["friend"]?.GetValueKind() == JsonValueKind.True) flags.Add("friend");
        if (lobby["inMission"]?.GetValueKind() == JsonValueKind.True) flags.Add("mission");
        if (lobby["joinable"]?.GetValueKind() == JsonValueKind.True) flags.Add("joinable");
        int Count(string key) => (int)Math.Clamp(Integer(lobby[key]) ?? 0, 0, 64);
        long? Optional(string key, long max) => Integer(lobby[key]) is { } n && n >= 0 && n <= max ? n : null;
        var shared = (lobby["mods"] as JsonArray)?.OfType<JsonObject>().Count(m => m["tier"]?.GetValue<string>() == ModSets.TierName(CoopTier.Shared)) ?? 0;
        return new LiveLobby(lobby["verdict"]?.GetValue<string>() ?? "unknown", flags, Count("members"), Count("maxMembers"), Optional("difficulty", 100),
            Optional("planetId", 100000), shared, Math.Max(0, Integer(lobby["downloadBytes"]) ?? 0), id, lobby["hostName"]?.GetValue<string>() ?? "",
            lobby["reason"]?.GetValue<string>() ?? "");
    }

    // A whole number the list holds (an int or a long, as Describe put it, or parsed JSON), else null.
    static long? Integer(JsonNode? node) =>
        node is not JsonValue value ? null : value.TryGetValue<long>(out var wide) ? wide : value.TryGetValue<int>(out var narrow) ? narrow : null;
}
