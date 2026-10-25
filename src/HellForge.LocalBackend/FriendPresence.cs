using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The social tab's data. Arrowhead's backend keeps every player's current lobby: the game uploads it with PUT /lobby
// (players, operation, region; game.dll 0x12cb5a0, reply ignored), keeps it alive with POST /lobby/<id>/member every
// 20 s and drops it with DELETE /lobby/clear. The social tab asks POST /lobby/accounts {"ids":["<account>",...]}
// (builders 0x12cbc40 and 0x12cbdc0) for friends' lobbies and reads {"accountLobbies":[{"account","lobbyId"}],
// "lobbies":[<the uploaded fields> + lobbyId, connectionString]} (parser 0x1055b50, lobby 0x1053f60). Its friends list
// is PlayFab's Client/GetFriendsList ({"ExternalPlatformFriends":"None"}; FriendPlayFabId, TitleDisplayName).
// HellForge answers both from Steam friends who run HellForge (Multiplayer.MultiplayerHost finds them and swaps lobbies with their
// backends, with the PlayFab profile objects each game sets for itself, which the engine needs to list a friend);
// account ids are taken to be SteamID64s, like /Account/Keys.
// Lobbies the game's Galactic Map search found in the lobby directory (Multiplayer.DirectoryService) answer for their
// hosts too, after friends: the map asks /lobby/accounts for the hosts of the lobbies its search returned.
public sealed class FriendPresence
{
    public sealed record Friend(ulong SteamId, string Name);
    // Listings: the digests (DirectoryData.Digest) of the directory listings the friend's backend says it published, the
    // newest first (the one before stays good while Steam may still hold it).
    sealed record Lobby(string LobbyId, string ConnectionString, JsonObject Body, DateTime Heard, string[] Listings);
    // A lobby the directory listed: its id, connection string, the host's lobby record and its live record (or null).
    public sealed record DirectoryLobby(string LobbyId, string ConnectionString, JsonObject Body, JsonObject? Live);

    // The directory's lobbies by host account id (decimal) and by lobby id.
    public Func<string, DirectoryLobby?>? DirectoryByAccount { get; set; }
    public Func<string, DirectoryLobby?>? DirectoryByLobby { get; set; }

    // A friend's lobby is shown while their backend keeps sending it.
    public static readonly TimeSpan Fresh = TimeSpan.FromSeconds(45);

    // Fields the lobby parser (0x1053f60) requires; one missing fails the whole /lobby/accounts answer, which left
    // friends greyed out. The game's own upload lacks some (numSessionMembers, completedObjectives, planetType,
    // transitionTarget, missionId per mission), so absent ones get neutral values.
    static readonly string[] RequiredNumbers = ["completedObjectives", "crossplayMode", "difficulty", "gameModeProgress", "hostability",
        "onboardingState", "operationProgress", "operationSeed", "operationType", "planetId", "planetType", "preferenceGroup", "privacyMode",
        "race", "regionIndex", "sosBeacons", "startTime", "state", "territoryId", "transitionTarget"];
    bool loggedUploadFields;

    readonly object gate = new();
    IReadOnlyList<Friend> friends = [];
    readonly Dictionary<ulong, Lobby> friendLobbies = [];
    readonly Dictionary<ulong, JsonObject> friendObjects = [];
    JsonObject localObjects = [];
    string lastUnknown = "";
    JsonObject? localBody, localLive;
    string? localLobbyId, localListing;
    long localVersion;

    public Action<string>? Log { get; set; }

    public IReadOnlyList<Friend> Friends { get { lock (gate) return friends; } }

    public void SetFriends(IReadOnlyList<Friend> current)
    {
        lock (gate)
        {
            if (current.Select(f => f.SteamId).SequenceEqual(friends.Select(f => f.SteamId))) { friends = current; return; }
            friends = current;
        }
        Log?.Invoke(current.Count == 0 ? "No Steam friends running HellForge right now." : $"Steam friends running HellForge: {current.Count} ({string.Join(", ", current.Select(f => f.Name))}).");
    }

    // PUT /lobby: the game's current lobby (fields as the game uploads them).
    public void SetLocalLobby(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object) return;
        // bounded before friends or the directory get it (Multiplayer/RecordGuard.cs)
        if (Multiplayer.RecordGuard.Lobby(JsonNode.Parse(body.GetRawText())!.AsObject()) is not { } copy)
        {
            Log?.Invoke("The game's lobby upload is too large to share; friends and the directory keep the last one.");
            return;
        }
        bool first;
        lock (gate)
        {
            localBody = copy;
            localVersion++;
            first = !loggedUploadFields;
            loggedUploadFields = true;
        }
        if (first) Log?.Invoke("The game's lobby upload has: " + string.Join(", ", copy.Select(p => p.Key)) + ".");
    }

    // POST /lobby/<id>/member: the lobby the game is in (its PlayFab lobby id).
    public void SetLocalLobbyId(string lobbyId)
    {
        lock (gate)
        {
            if (localLobbyId == lobbyId) return;
            localLobbyId = lobbyId;
            localVersion++;
        }
        Log?.Invoke($"This game is in lobby {lobbyId}.");
    }

    // DELETE /lobby/clear.
    public void ClearLocal()
    {
        lock (gate)
        {
            localBody = null;
            localLive = null;
            localLobbyId = null;
            localVersion++;
        }
    }

    // PUT /lobby/<id>/live: the game's live mission state (missionTime, playerPositions, objective and outpost masks,
    // state), never seen from a solo host so far (docs/multiplayer.md step 7).
    public void SetLocalLive(string lobbyId, JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object) return;
        if (Multiplayer.RecordGuard.Live(JsonNode.Parse(body.GetRawText())!.AsObject()) is not { } copy)
        {
            Log?.Invoke("The game's live lobby state is too large to share.");
            return;
        }
        bool first;
        lock (gate)
        {
            first = localLive == null;
            localLive = copy;
            localVersion++;
        }
        if (first) Log?.Invoke($"The game uploads its live lobby state ({lobbyId}): " + string.Join(", ", copy.Select(p => p.Key)) + ".");
    }

    public JsonObject? LocalLive { get { lock (gate) return localLive?.DeepClone().AsObject(); } }

    // The digest of the listing this machine's directory publishes (null: none), sent to friends with the lobby so their
    // searches can tell it from a listing someone else made under this player's id (Multiplayer/DirectoryService.cs).
    public void SetLocalListing(string? digest)
    {
        lock (gate)
        {
            if (localListing == digest) return;
            localListing = digest;
            localVersion++;
        }
    }

    public string? LocalListing { get { lock (gate) return localListing; } }

    // Whether that friend's backend, over its own authenticated link, said it published a listing with this digest.
    public bool Vouches(ulong friend, string digest)
    {
        lock (gate) return friendLobbies.TryGetValue(friend, out var lobby) && DateTime.UtcNow - lobby.Heard <= Fresh && lobby.Listings.Contains(digest);
    }

    // GET /lobby/<id>: the lobby's record from this machine's own, a friend's or the directory's, completed as
    // /lobby/accounts completes it; null when unknown.
    public JsonObject? LobbyRecord(string lobbyId)
    {
        JsonObject? body = null;
        string? connection = null;
        lock (gate)
        {
            if (localLobbyId == lobbyId && localBody != null) (body, connection) = (localBody.DeepClone().AsObject(), (string?)localBody["connectionString"]);
            else if (friendLobbies.Values.FirstOrDefault(l => l.LobbyId == lobbyId && DateTime.UtcNow - l.Heard <= Fresh) is { } friend)
                (body, connection) = (friend.Body.DeepClone().AsObject(), friend.ConnectionString);
        }
        if (body == null && DirectoryByLobby?.Invoke(lobbyId) is { } listed) (body, connection) = (listed.Body.DeepClone().AsObject(), listed.ConnectionString);
        if (body == null) return null;
        body["lobbyId"] = lobbyId;
        if (connection != null) body["connectionString"] = connection;
        CompleteRequired(body);
        return body;
    }

    // GET /lobby/<id>/live: this machine's own live state, or a directory lobby's.
    public JsonObject? LiveRecord(string lobbyId)
    {
        lock (gate) if (localLobbyId == lobbyId && localLive != null) return localLive.DeepClone().AsObject();
        return DirectoryByLobby?.Invoke(lobbyId)?.Live?.DeepClone().AsObject();
    }

    public (string? LobbyId, JsonObject? Body, JsonObject Objects, long Version) Local
    {
        get { lock (gate) return (localLobbyId, localBody?.DeepClone().AsObject(), localObjects.DeepClone().AsObject(), localVersion); }
    }

    // This player's PlayFab profile objects (Object/SetObjects), shared with friends: bounded as a friend's are when they
    // arrive (Multiplayer/RecordGuard.cs), so friends are never sent a set they would drop.
    public void SetLocalObjects(JsonObject objects)
    {
        if (Multiplayer.RecordGuard.Objects(objects) is not { } copy)
        {
            Log?.Invoke("The game's profile objects are too large to share; friends keep the last ones.");
            return;
        }
        lock (gate)
        {
            localObjects = copy;
            localVersion++;
        }
    }

    public void SetFriendObjects(ulong friend, JsonObject objects)
    {
        bool first;
        lock (gate)
        {
            first = !friendObjects.ContainsKey(friend) && objects.Count > 0;
            if (objects.Count > 0 || !friendObjects.ContainsKey(friend)) friendObjects[friend] = objects;
        }
        if (first) Log?.Invoke($"Profile of {FriendName(friend) ?? friend.ToString("X", CultureInfo.InvariantCulture)} received for the friends list.");
    }

    // The game asks for its friends' profiles once, right after logging in, and leaves out a profile without objects
    // (first try: the friend was counted online but not listed). Profiles wait briefly for friends' objects still on
    // their way.
    public async Task WaitForFriendObjectsAsync(TimeSpan timeout, CancellationToken cancellation)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            lock (gate)
                if (friends.All(f => friendObjects.TryGetValue(f.SteamId, out var objects) && objects.Count > 0)) return;
            await Task.Delay(100, cancellation);
        }
        Log?.Invoke("A friend's profile had not arrived when the game asked for it; that friend may be missing from the list until the next session.");
    }

    public JsonObject? FriendObjects(ulong friend)
    {
        lock (gate) return friendObjects.TryGetValue(friend, out var objects) ? objects.DeepClone().AsObject() : null;
    }

    public string? FriendName(ulong friend)
    {
        lock (gate) return friends.FirstOrDefault(f => f.SteamId == friend)?.Name;
    }

    // listing: the digest of the friend's directory listing, or null when it lists none.
    public void SetFriendLobby(ulong friend, string lobbyId, string connectionString, JsonObject body, string? listing = null)
    {
        lock (gate)
        {
            var before = friendLobbies.TryGetValue(friend, out var old) ? old.Listings : [];
            string[] listings = listing == null ? [] : before.Length > 0 && before[0] == listing ? before : [listing, .. before.Take(1)];
            friendLobbies[friend] = new Lobby(lobbyId, connectionString, body, DateTime.UtcNow, listings);
        }
    }

    public void ClearFriendLobby(ulong friend)
    {
        lock (gate) friendLobbies.Remove(friend);
    }

    static void CompleteRequired(JsonObject body)
    {
        var members = body["players"] is JsonArray players ? Math.Max(1, players.Count) : 1;
        body["numLobbyMembers"] ??= members;
        body["numSessionMembers"] ??= body["numLobbyMembers"]!.DeepClone();
        body["warId"] ??= 801;
        foreach (var name in RequiredNumbers) body[name] ??= 0;
        if (body["operationMissionStates"] is JsonArray missions)
            foreach (var mission in missions.OfType<JsonObject>())
            {
                mission["index"] ??= 0;
                mission["type"] ??= 0;
                mission["state"] ??= 0;
                mission["missionId"] ??= "";
            }
        if (body["players"] is JsonArray list)
            foreach (var player in list.OfType<JsonObject>())
            {
                player["playerRank"] ??= 0;
                player["commendRank"] ??= 0;
            }
    }

    // POST /lobby/accounts: the lobbies of the asked-for friends that are in one.
    public JsonObject Lookup(IEnumerable<string> ids)
    {
        var accountLobbies = new JsonArray();
        var lobbies = new JsonArray();
        var listed = new HashSet<string>(StringComparer.Ordinal);
        var unknown = new List<string>();
        lock (gate)
        {
            foreach (var id in ids)
            {
                Lobby? lobby = null;
                if (ulong.TryParse(id, NumberStyles.None, CultureInfo.InvariantCulture, out var steamId) && friendLobbies.TryGetValue(steamId, out var friend) &&
                    DateTime.UtcNow - friend.Heard <= Fresh) lobby = friend;
                // a lobby the Galactic Map's search found in the directory
                else if (DirectoryByAccount?.Invoke(id) is { } found) lobby = new Lobby(found.LobbyId, found.ConnectionString, found.Body, DateTime.UtcNow, []);
                if (lobby == null)
                {
                    unknown.Add(id);
                    continue;
                }
                accountLobbies.Add((JsonNode?)new JsonObject { ["account"] = id, ["lobbyId"] = lobby.LobbyId });
                if (!listed.Add(lobby.LobbyId)) continue;
                var body = lobby.Body.DeepClone().AsObject();
                body["lobbyId"] = lobby.LobbyId;
                body["connectionString"] = lobby.ConnectionString;
                CompleteRequired(body);
                lobbies.Add((JsonNode?)body);
            }
        }
        var missing = string.Join(", ", unknown);
        lock (gate)
        {
            if (missing == lastUnknown) missing = "";
            else lastUnknown = missing;
        }
        if (missing.Length > 0) Log?.Invoke($"Asked for the lobbies of account(s) {missing}; none is a HellForge friend in a lobby or a lobby the map's search found.");
        return new JsonObject { ["accountLobbies"] = accountLobbies, ["lobbies"] = lobbies };
    }
}
