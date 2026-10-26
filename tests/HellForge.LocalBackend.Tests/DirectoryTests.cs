using System.Collections.Concurrent;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;
using HellForge.LocalBackend.Multiplayer;

// The lobby directory: the game's filter, listing values, what a host publishes and when, and what a searching backend
// shows, remembers for the map's records and leaves out; the gate for strangers. Backends over the in-memory network and
// directory; every mod synthetic.
static class DirectoryTests
{
    const ulong HostId = 76561198000000031, GuestId = 76561198000000032, OtherId = 76561198000000033;
    const string Build = "f8c9037f96047f7a";
    const string MapFilter = "string_key1 eq 'f8c9037f96047f7a' and number_key6 eq 0 and number_key3 eq 261 and number_key7 ge 5 and number_key7 le 900 " +
        "and string_key6 ne 'SA' and string_key6 ne 'AF' and lobby/memberCount le 3";

    sealed class Node
    {
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly LobbyService Lobby;
        public readonly FriendPresence Presence = new();
        public readonly ModSync Sync;
        public readonly DirectoryService Directory;
        public readonly string Mods;
        public readonly List<string> Log = [];
        public bool Edited;
        public bool ListPublic = true, Strangers = true;

        public Node(InMemoryNetwork network, MemoryLobbyDirectory listings, ulong id, string folder, ModSet own, ILobbyDirectory? directory = null)
        {
            Mods = System.IO.Directory.CreateDirectory(Path.Combine(folder, "mods")).FullName;
            var run = System.IO.Directory.CreateDirectory(Path.Combine(folder, "run", "backend")).FullName;
            var transport = network.Join(id);
            void Note(string line) { lock (Log) Log.Add(line); }
            Lobby = new LobbyService(transport, Inbox.Add, Note, own.Digest);
            Sync = new ModSync(transport, new ModSyncPaths(Mods, Path.GetDirectoryName(run)!, run, ModSyncSession.PathIn(new InstallLayout(folder))), own, _ => "Player", Note, (_, _) => false);
            Directory = new DirectoryService(directory ?? listings.For(id), Lobby, Presence, id, Inbox.Add, Note, new DirectorySettings(Sync, () => Edited, AutoPublish: false,
                ListPublic: () => ListPublic, StrangersAllowed: () => Strangers));
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("mods.", StringComparison.Ordinal)) Sync.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
        }

        public Frame Next(string type, Action<bool, string> check)
        {
            var got = Inbox.TryTake(out var frame, TimeSpan.FromSeconds(5));
            check(got && frame!.Type == type, $"expected {type}, got {(got ? frame!.Type : "nothing")}");
            return frame!;
        }
    }

    // Steam's directory as anyone can use it: a listing's host is read from its own hf.host (SteamLobbyDirectory.Read), so
    // a listing can be published under another player's id. Claims are such listings, found beside the honest ones.
    sealed class ClaimingDirectory(ILobbyDirectory honest) : ILobbyDirectory
    {
        public readonly List<Dictionary<string, string>> Claims = [];

        public void Publish(DirectoryListing? listing) => honest.Publish(listing);

        public async Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation)
        {
            var found = new List<DirectoryListing>(await honest.SearchAsync(query, cancellation));
            lock (Claims)
                foreach (var data in Claims)
                    if (ulong.TryParse(data.GetValueOrDefault(DirectoryKeys.Host), NumberStyles.None, CultureInfo.InvariantCulture, out var host) &&
                        DirectoryData.Matches(new DirectoryListing(host, data), query))
                        found.Add(new DirectoryListing(host, data));
            return found;
        }
    }

    static Frame Create(string op, ulong creator, IReadOnlyDictionary<string, string> search) => new Frame("lobby.create").Add(op).Add(LobbyService.EntityId(creator))
        .Add("title_player_account").Add(4).Add(0).Add(0).Add(0).Add(new Dictionary<string, string> { ["1"] = Build }).Add(search)
        .Add(new Dictionary<string, string> { ["crossplay_mode"] = "0" });

    static Frame SearchChange(string op, string lobby, ulong member, string key, string value) =>
        new Frame("lobby.update").Add(op).Add(lobby).Add(LobbyService.EntityId(member)).Add(0).Add("").Add(0).Add(0).Add(0).Add(0).Add(0).Add(0)
            .Add(Array.Empty<PropertyChange>()).Add(new List<PropertyChange> { new(key, value) }).Add(0).Add(Array.Empty<PropertyChange>());

    static readonly Dictionary<string, string> PublicSearch = new()
    {
        ["string_key1"] = Build, ["number_key6"] = "0", ["number_key3"] = "261", ["number_key5"] = "3", ["number_key7"] = "42", ["string_key6"] = "EU"
    };

    static string Record(ulong host) => $$"""
        {"players":[{"playerName":"Host Diver","memberAccountId":"{{host}}","isHost":true},{"playerName":"Second","memberAccountId":"76561198000000099","isHost":false}],
         "memberAccountIds":["{{host}}", 76561198000000099],"difficulty":5,"planetId":261,"connectionString":"from the game","lobbyId":"x","notInTheSchema":"leak"}
        """;

    public static async Task Run(string root, Action<bool, string> check)
    {
        root = Path.Combine(root, "directory");
        Filters(check);
        Values(check);

        // The host runs a content mod the guest lacks.
        var hostMods = Path.Combine(root, "host", "mods");
        Directory.CreateDirectory(Path.Combine(hostMods, "ammo"));
        File.WriteAllText(Path.Combine(hostMods, "ammo", ModCatalog.ManifestName),
            """{"format":1,"id":"test.ammo","name":"Ammo","version":"1","native":{"networkFieldTypes":[{"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}]}}""");
        var hostSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(hostMods).Mods));
        var network = new InMemoryNetwork();
        var listings = new MemoryLobbyDirectory();
        var host = new Node(network, listings, HostId, Path.Combine(root, "host"), hostSet);
        var claims = new ClaimingDirectory(listings.For(GuestId));
        var guest = new Node(network, listings, GuestId, Path.Combine(root, "guest"), new ModSet("none", []), claims);
        var now = DateTime.UtcNow;

        // A public lobby is listed with what a search needs; the game's record comes once it is uploaded.
        host.Lobby.FromLocal(Create("c1", HostId, PublicSearch));
        var created = host.Next("lobby.result", check);
        var (lobbyId, connection) = (created.Text(3), created.Text(4));
        host.Directory.Publish(now);
        var listing = listings.All.Single();
        check(listing.Host == HostId && listing.Data[DirectoryKeys.Protocol] == LobbyService.Protocol && listing.Data[DirectoryKeys.LobbyId] == lobbyId &&
            listing.Data[DirectoryKeys.Connection] == connection && listing.Data[DirectoryKeys.Host] == HostId.ToString(CultureInfo.InvariantCulture) &&
            listing.Data[DirectoryKeys.Members] == "1" && listing.Data[DirectoryKeys.Slots] == "3" && listing.Data["number_key3"] == "261" &&
            listing.Data[DirectoryKeys.Stats] == "0" && listing.Data[DirectoryKeys.Share] == "1" && listing.Data[DirectoryKeys.Digest] == hostSet.Digest &&
            !listing.Data.ContainsKey(DirectoryKeys.Record), "The listing: " + string.Join(", ", listing.Data.Keys));
        check(ModSet.Parse(DirectoryData.Unpack(DirectoryData.Get(listing.Data, DirectoryKeys.Mods))!, local: false).Mods.Single().Id == "test.ammo", "The listing carries the mod set.");
        host.Presence.SetLocalLobby(JsonDocument.Parse(Record(HostId)).RootElement);
        host.Directory.Publish(now + TimeSpan.FromSeconds(1));
        check(!listings.All.Single().Data.ContainsKey(DirectoryKeys.Record), "A change waits for the publish interval.");
        host.Directory.Publish(now + TimeSpan.FromSeconds(6));
        var record = JsonNode.Parse(DirectoryData.Unpack(DirectoryData.Get(listings.All.Single().Data, DirectoryKeys.Record))!)!.AsObject();
        check(record["players"]![0]!["playerName"]!.GetValue<string>() == "Host Diver" && record["players"]![1]!["playerName"]!.GetValue<string>() == "Second" &&
            record["players"]![0]!["memberAccountId"]!.GetValue<string>() == HostId.ToString(CultureInfo.InvariantCulture) &&
            record["players"]![1]!["memberAccountId"]!.GetValue<string>() == "0" && record["memberAccountIds"]![1]!.GetValue<int>() == 0 &&
            record["memberAccountIds"]![0]!.GetValue<string>() == HostId.ToString(CultureInfo.InvariantCulture) && !record.ContainsKey("connectionString") &&
            !record.ContainsKey("lobbyId") && !record.ContainsKey("notInTheSchema") && record["difficulty"]!.GetValue<int>() == 5,
            "The published record keeps names and only the host's account id: " + record.ToJsonString());

        // The guest's Galactic Map search finds it through the game's whole filter; the map's records follow for its host.
        var found = await guest.Directory.SearchAsync("s1", MapFilter, 50);
        check(found.Type == "lobby.found" && found.Text(0) == "s1" && found.Text(1) == "0" && found.Int(2) == 1 && found.Text(3) == lobbyId &&
            found.Text(4) == connection && found.Text(5) == LobbyService.EntityId(HostId) && found.Text(6) == "4" && found.Text(7) == "1" && found.Text(8) == "0",
            "The search answer: " + string.Join(" | ", Enumerable.Range(0, Math.Min(found.Count, 10)).Select(found.Text)));
        var index = 9;
        var props = found.Properties(ref index);
        check(props["number_key3"] == "261" && props["string_key1"] == Build && !props.Keys.Any(k => k.StartsWith("hf.", StringComparison.Ordinal)), "Its search properties only.");
        var accounts = guest.Presence.Lookup([HostId.ToString(CultureInfo.InvariantCulture)]);
        check(accounts["lobbies"]!.AsArray().Count == 1 && accounts["lobbies"]![0]!["lobbyId"]!.GetValue<string>() == lobbyId &&
            accounts["lobbies"]![0]!["connectionString"]!.GetValue<string>() == connection && accounts["accountLobbies"]![0]!["lobbyId"]!.GetValue<string>() == lobbyId,
            "/lobby/accounts answers for the host the search found: " + accounts.ToJsonString());
        check(guest.Presence.LobbyRecord(lobbyId)?["players"]?.AsArray().Count == 2 && guest.Directory.HostName(HostId) == "Host Diver", "GET /lobby/<id> and the host's name.");
        check((await host.Directory.SearchAsync("own", MapFilter, 50)).Int(2) == 0, "A host never finds its own lobby.");
        check((await guest.Directory.SearchAsync("s2", MapFilter.Replace("eq 261", "eq 100"), 50)).Int(2) == 0, "Another planet: not shown.");
        check((await guest.Directory.SearchAsync("s3", "lobby/memberCountRemaining gt 3", 50)).Int(2) == 0 &&
            (await guest.Directory.SearchAsync("s4", "lobby/memberCountRemaining ge 3", 50)).Int(2) == 1, "Member counts are the lobby's, not Steam's.");

        // Through the stand-in's pipe: lobby.find becomes lobby.found.
        guest.Lobby.FromLocal(new Frame("lobby.find").Add("p1").Add(LobbyService.EntityId(GuestId)).Add("title_player_account").Add(MapFilter).Add("").Add(10));
        check(guest.Next("lobby.found", check).Text(0) == "p1", "The DLL's search is answered.");

        // Edited stats: hidden unless the player shows them.
        host.Edited = true;
        host.Directory.Publish(now + TimeSpan.FromSeconds(12));
        await Task.Delay(SameSearchPause);
        check((await guest.Directory.SearchAsync("e1", MapFilter, 50)).Int(2) == 0, "A host with edited stats is hidden by default.");
        ModSettings.SetShowEditedStats(guest.Mods, true);
        check((await guest.Directory.SearchAsync("e2", MapFilter, 50)).Int(2) == 1, "Shown when the player wants them.");
        ModSettings.SetShowEditedStats(guest.Mods, false);
        host.Edited = false;
        host.Directory.Publish(now + TimeSpan.FromSeconds(18));
        await Task.Delay(SameSearchPause);

        // A blocked mod hides the lobby.
        ModSettings.SetBlocked(guest.Mods, "test.ammo", true);
        check((await guest.Directory.SearchAsync("b1", MapFilter, 50)).Int(2) == 0, "A lobby running a blocked mod is hidden.");
        ModSettings.SetBlocked(guest.Mods, "test.ammo", false);

        // Play with strangers off: this machine's lobby is not listed; a search shows friends' listings only; a join to a
        // stranger's lobby is refused at once, with why.
        host.Strangers = false;
        host.Directory.Publish(now + TimeSpan.FromSeconds(20));
        check(listings.All.Count == 0 && !host.Directory.Listing && host.Log.Any(l => l.Contains("Play with strangers is off")),
            "Strangers off: the host's lobby is not listed.");
        host.Strangers = true;
        host.Directory.Publish(now + TimeSpan.FromSeconds(21));
        check(listings.All.Count == 1 && host.Directory.Listing, "On again: listed at once.");
        await Task.Delay(SameSearchPause);
        guest.Strangers = false;
        check((await guest.Directory.SearchAsync("x1", MapFilter, 50)).Int(2) == 0 && guest.Log.Any(l => l.Contains("Play with strangers is off")),
            "Strangers off: a stranger's listing is not shown.");
        guest.Presence.SetFriends([new FriendPresence.Friend(HostId, "Host")]);
        await Task.Delay(SameSearchPause);
        check((await guest.Directory.SearchAsync("x2a", MapFilter, 50)).Int(2) == 0 && guest.Log.Any(l => l.Contains("not published by them")),
            "A friend's listing their backend has not vouched for is not shown (anyone can list under a friend's id).");
        check(host.Presence.LocalListing == DirectoryData.Digest(listings.All.Single().Data), "The host's presence carries its listing's digest.");
        guest.Presence.SetFriendLobby(HostId, lobbyId, connection, new JsonObject(), host.Presence.LocalListing);
        await Task.Delay(SameSearchPause);
        check((await guest.Directory.SearchAsync("x2", MapFilter, 50)).Int(2) == 1, "A friend's listing they vouch for is.");
        guest.Presence.SetFriends([]);
        guest.Presence.ClearFriendLobby(HostId);
        guest.Strangers = true;
        IReadOnlyList<string>? told = null;
        guest.Lobby.Refused += (_, lines) => told = lines;
        guest.Lobby.JoinRefusal = peer => peer == HostId ? ["Not a friend, and Play with strangers is off."] : null;
        guest.Lobby.FromLocal(MultiplayerTests.Join("x3", GuestId, connection));
        check(guest.Next("lobby.result", check).Text(2) == LobbyService.LobbyNotJoinable.ToString() && told?.Single().Contains("Play with strangers") == true &&
            !host.Lobby.InHostedLobby(GuestId), "A join to a stranger's lobby is refused here, with why, and never sent.");
        guest.Lobby.JoinRefusal = null;
        await Task.Delay(SameSearchPause);

        // A lobby running unverified code the guest lacks (a valid listing of its own: a GUID, its host's record): a Steam
        // friend's is shown, since joining brings the MOD SYNC prompt with TRUST (the first two-PC test's case); a
        // stranger's needs its mods installed by hand and is left out, and the search log says so.
        var code = new ModSet("0123456789abcdef0123456789abcdef", [new ModSetEntry("test.code", "Code", "1", new string('c', 64), 10, ModClass.Code, 0)]);
        var otherLobby = Guid.NewGuid().ToString("D");
        var other = new Dictionary<string, string>(listings.All.Single().Data)
        {
            [DirectoryKeys.LobbyId] = otherLobby, [DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + otherLobby + ";" + LobbyService.EntityId(OtherId),
            [DirectoryKeys.Host] = OtherId.ToString(CultureInfo.InvariantCulture), [DirectoryKeys.Digest] = code.Digest
        };
        DirectoryData.Put(other, DirectoryKeys.Mods, DirectoryData.Pack(code.ToJson(local: false).ToJsonString()));
        DirectoryData.Put(other, DirectoryKeys.Record, DirectoryData.Pack(DirectoryService.Public(JsonNode.Parse(Record(OtherId))!.AsObject(), OtherId).ToJsonString()));
        check(DirectoryService.Valid(new DirectoryListing(OtherId, other), out _, out _, out _, out _, out var otherWhy), "The other host's listing is valid: " + otherWhy);
        listings.For(OtherId).Publish(new DirectoryListing(OtherId, other));
        await Task.Delay(SameSearchPause);
        var withFriend = await guest.Directory.SearchAsync("u1", MapFilter, 50);
        check(withFriend.Int(2) == 2 && Enumerable.Range(0, withFriend.Count).Any(i => withFriend.Text(i) == otherLobby),
            "A Steam friend's lobby with unverified code the guest lacks is shown (TRUST on joining).");
        lock (network.Strangers) network.Strangers.Add(OtherId);
        await Task.Delay(SameSearchPause);
        var withStranger = await guest.Directory.SearchAsync("u1s", MapFilter, 50);
        check(withStranger.Int(2) == 1 && withStranger.Text(3) == lobbyId, "A stranger's lobby with unverified code the guest lacks is left out.");
        lock (guest.Log) check(guest.Log.Any(l => l.Contains("2 listed, 1 shown (1 left out: it needs mods installed by hand)")),
            "The search says why: " + string.Join(" / ", guest.Log.TakeLast(2)));
        lock (network.Strangers) network.Strangers.Remove(OtherId);
        await Task.Delay(SameSearchPause);
        await host.Directory.SearchAsync("own2", MapFilter, 50);
        lock (host.Log) check(host.Log.Any(l => l.Contains("left out: this machine's own lobby")), "A host's search says it left out its own lobby: " + host.Log.Last());
        other[DirectoryKeys.Digest] = guest.Sync.OwnDigest;
        other[DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + otherLobby + ";" + LobbyService.EntityId(HostId);
        listings.For(OtherId).Publish(new DirectoryListing(OtherId, other));
        await Task.Delay(SameSearchPause);
        check((await guest.Directory.SearchAsync("u2", MapFilter, 50)).Int(2) == 1, "A listing whose connection string names another host is left out.");
        listings.For(OtherId).Publish(null);

        await Hostile(listings, host, guest, check);
        await Claimed(listings, claims, network, host, guest, check);

        // The player's switch: off, the Public lobby is not listed; on again, it is.
        host.ListPublic = false;
        host.Directory.Publish(now + TimeSpan.FromSeconds(18.5));
        check(listings.All.Count == 0 && !host.Directory.Listing, "With the switch off a Public lobby is not listed.");
        host.ListPublic = true;
        host.Directory.Publish(now + TimeSpan.FromSeconds(18.7));
        check(listings.All.Count == 1 && host.Directory.Listing, "Switched on again, it is listed.");

        // A lobby that turns private, locks or fills is no longer listed, at once.
        host.Lobby.FromLocal(SearchChange("u", lobbyId, HostId, "number_key6", "1"));
        host.Next("lobby.result", check);
        host.Next("lobby.updated", check);
        host.Directory.Publish(now + TimeSpan.FromSeconds(19));
        check(listings.All.Count == 0 && host.Directory.Listing == false, "A private lobby is not listed.");

        Gate(check);
        await host.Directory.DisposeAsync();
        await guest.Directory.DisposeAsync();
        await host.Sync.DisposeAsync();
        await guest.Sync.DisposeAsync();
    }

    // Strangers' listings: anything off the schema leaves the listing out whole (or, for the live record, just that).
    static async Task Hostile(MemoryLobbyDirectory listings, Node host, Node guest, Action<bool, string> check)
    {
        var good = new Dictionary<string, string>(listings.All.Single(l => l.Host == HostId).Data);
        var lobbyId = Guid.NewGuid().ToString();
        good[DirectoryKeys.LobbyId] = lobbyId;
        good[DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + lobbyId + ";" + LobbyService.EntityId(OtherId);
        good[DirectoryKeys.Host] = OtherId.ToString(CultureInfo.InvariantCulture);
        var record = JsonNode.Parse(DirectoryData.Unpack(DirectoryData.Get(good, DirectoryKeys.Record))!)!.AsObject();
        record["players"]![0]!["memberAccountId"] = OtherId.ToString(CultureInfo.InvariantCulture);
        Dictionary<string, string> With(Action<Dictionary<string, string>> change)
        {
            var copy = new Dictionary<string, string>(good);
            DirectoryData.Put(copy, DirectoryKeys.Record, DirectoryData.Pack(record.ToJsonString()));
            change(copy);
            return copy;
        }
        Dictionary<string, string> Record(Action<JsonObject> change)
        {
            var copy = record.DeepClone().AsObject();
            change(copy);
            return With(d => DirectoryData.Put(d, DirectoryKeys.Record, DirectoryData.Pack(copy.ToJsonString())));
        }
        bool Valid(Dictionary<string, string> data) => DirectoryService.Valid(new DirectoryListing(OtherId, data), out _, out _, out _, out _, out _);
        check(Valid(With(_ => { })), "A stranger's well-formed listing is valid.");
        var bad = new Dictionary<string, Dictionary<string, string>>
        {
            ["a forged hf.host"] = With(d => d[DirectoryKeys.Host] = HostId.ToString(CultureInfo.InvariantCulture)),
            ["a lobby id that is no GUID"] = With(d => { d[DirectoryKeys.LobbyId] = "../x"; d[DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + "../x;" + LobbyService.EntityId(OtherId); }),
            ["5 members"] = With(d => d[DirectoryKeys.Members] = "5"),
            ["0 members"] = With(d => d[DirectoryKeys.Members] = "0"),
            ["9 at most"] = With(d => d[DirectoryKeys.MaxMembers] = "9"),
            ["a lock of 2"] = With(d => d[DirectoryKeys.Lock] = "2"),
            ["31 search properties"] = With(d => { for (var i = 1; i <= 30; i++) d["string_key" + i] = "x"; d["number_key1"] = "1"; }),
            ["a text number_key"] = With(d => d["number_key6"] = "zero"),
            ["string_key31"] = With(d => d["string_key31"] = "x"),
            ["a 300-character search value"] = With(d => d["string_key9"] = new string('x', 300)),
            ["a record that is no JSON"] = With(d => d[DirectoryKeys.Record] = DirectoryData.Pack("[1,2")),
            ["5 players"] = Record(r => r["players"] = new JsonArray([.. Enumerable.Range(0, 5).Select(i => (JsonNode?)new JsonObject { ["playerName"] = "P" + i })])),
            ["a 33-character name"] = Record(r => r["players"]![1]!["playerName"] = new string('n', 33)),
            ["a control character in a name"] = Record(r => r["players"]![1]!["playerName"] = "ab"),
            ["difficulty as text"] = Record(r => r["difficulty"] = "5"),
            ["a privacy of 9"] = Record(r => r["privacyMode"] = 9),
            ["an object for a number"] = Record(r => r["planetId"] = new JsonObject { ["x"] = 1 }),
            ["an account id that is no number"] = Record(r => r["memberAccountIds"]![1] = "../../x"),
            ["someone else as host"] = Record(r => r["players"]![0]!["memberAccountId"] = HostId.ToString(CultureInfo.InvariantCulture)),
            ["two hosts"] = Record(r => r["players"]![1]!["isHost"] = true),
        };
        foreach (var (what, data) in bad) check(!Valid(data), "A stranger's listing with " + what + " was taken.");

        // Searched: the hostile listing is left out (and said why), a good one is shown with its record rebuilt.
        listings.For(OtherId).Publish(new DirectoryListing(OtherId, bad["5 players"]));
        await Task.Delay(SameSearchPause);
        var found = await guest.Directory.SearchAsync("h1", MapFilter, 50);
        check(found.Int(2) == 1 && found.Text(3) != lobbyId, "A hostile listing is left out of the search.");
        lock (guest.Log) check(guest.Log.Any(l => l.Contains("malformed (lobby record)")), "The search says why a listing was left out.");
        var extra = Record(r => { r["injected"] = new JsonObject { ["script"] = "x" }; r["players"]![1]!["extraField"] = "x"; });
        DirectoryData.Put(extra, DirectoryKeys.Live, DirectoryData.Pack("""{"missionTime":12,"state":2,"playerPositions":[{"x":1,"y":2}],"bad":{"deep":{"x":1}}}"""));
        listings.For(OtherId).Publish(new DirectoryListing(OtherId, extra));
        await Task.Delay(SameSearchPause);
        found = await guest.Directory.SearchAsync("h2", MapFilter, 50);
        check(found.Int(2) == 2, "A listing with unknown fields is shown.");
        var shown = guest.Presence.LobbyRecord(lobbyId);
        check(shown != null && !shown.ContainsKey("injected") && shown["players"]![1]!.AsObject() is { } second && !second.ContainsKey("extraField") &&
            second["playerName"]!.GetValue<string>() == "Second", "Its record reaches the game rebuilt from the schema: " + shown?.ToJsonString());
        check(guest.Presence.LiveRecord(lobbyId) == null, "A live record off the schema is left out.");
        listings.For(OtherId).Publish(null);
        await Task.Delay(SameSearchPause);
        _ = host;
    }

    // Listings under another player's id, which Steam does not stop (ClaimingDirectory): a join goes to the host a listing
    // names, never to whoever made it; listings claiming one host or lobby are left out unless the host vouches for its own;
    // with Play with strangers off, a friend's listing is shown only as their backend vouches for it.
    static async Task Claimed(MemoryLobbyDirectory listings, ClaimingDirectory claims, InMemoryNetwork network, Node host, Node guest, Action<bool, string> check)
    {
        var genuine = new Dictionary<string, string>(listings.All.Single(l => l.Host == HostId).Data);
        var (lobbyId, connection) = (genuine[DirectoryKeys.LobbyId], genuine[DirectoryKeys.Connection]);

        // The digest a friend vouches with: the same for the same data in any order, another for any other value.
        var digest = DirectoryData.Digest(genuine);
        var reversed = genuine.Reverse().ToDictionary(d => d.Key, d => d.Value, StringComparer.Ordinal);
        var changed = new Dictionary<string, string>(genuine) { [DirectoryKeys.Members] = "2" };
        check(digest.Length == 64 && DirectoryData.Digest(reversed) == digest && DirectoryData.Digest(changed) != digest &&
            DirectoryData.Digest(new Dictionary<string, string> { ["ab"] = "c" }) != DirectoryData.Digest(new Dictionary<string, string> { ["a"] = "bc" }),
            "A listing's digest follows its data, not its order.");
        var presence = new Frame("presence").Add("{}").Add(lobbyId).Add(connection).Add("{}");
        check(MultiplayerHost.ListingOf(presence) == null && MultiplayerHost.ListingOf(new Frame("presence").Add("{}").Add(lobbyId).Add(connection).Add("{}").Add(digest)) == digest &&
            MultiplayerHost.ListingOf(new Frame("presence").Add("{}").Add(lobbyId).Add(connection).Add("{}").Add(digest.ToUpperInvariant())) == null &&
            MultiplayerHost.ListingOf(new Frame("presence").Add("{}").Add(lobbyId).Add(connection).Add("{}").Add("../" + digest[3..])) == null,
            "A presence frame's listing digest: 64 lowercase hex digits, or none.");

        var claimant = network.Join(OtherId);
        var heard = 0;
        claimant.Received += (_, _) => Interlocked.Increment(ref heard);
        async Task<List<string>> Search(string op)
        {
            await Task.Delay(SameSearchPause);
            return Shown(await guest.Directory.SearchAsync(op, MapFilter, 50));
        }
        void Claim(params Dictionary<string, string>[] data) { lock (claims.Claims) { claims.Claims.Clear(); claims.Claims.AddRange(data); } }
        bool Logged(string text) { lock (guest.Log) return guest.Log.Any(l => l.Contains(text)); }
        void HostLists(bool listed)
        {
            host.ListPublic = listed;
            host.Directory.Publish(DateTime.UtcNow);
        }

        // A whole listing under the host's id, with a lobby of its own: nothing in it is out of place.
        var fakeLobby = Guid.NewGuid().ToString("D");
        var fakeConnection = LobbyService.ConnectionPrefix + fakeLobby + ";" + LobbyService.EntityId(HostId);
        var fake = new Dictionary<string, string>(genuine) { [DirectoryKeys.LobbyId] = fakeLobby, [DirectoryKeys.Connection] = fakeConnection };
        check(DirectoryService.Valid(new DirectoryListing(HostId, fake), out _, out _, out _, out _, out var why), "The claimed listing is well formed: " + why);
        Claim(fake);
        var shown = await Search("c1");
        check(!shown.Contains(lobbyId) && !shown.Contains(fakeLobby) && Logged("2 left out: another listing claims its host or lobby"),
            "Beside the host's own listing, neither is shown: " + string.Join(", ", shown));

        // The host not listing: the claimed listing alone cannot be told from the host's, but a join to it goes to the host,
        // which holds no such lobby, and nothing reaches whoever made it.
        HostLists(false);
        shown = await Search("c2");
        check(shown.SequenceEqual([fakeLobby]), "Alone, the claimed listing is shown: " + string.Join(", ", shown));
        guest.Lobby.FromLocal(MultiplayerTests.Join("c3", GuestId, fakeConnection));
        var joined = guest.Next("lobby.result", check);
        check(joined.Text(1) == "join" && joined.Text(2) == LobbyService.LobbyNotFound.ToString(CultureInfo.InvariantCulture) && !host.Lobby.InHostedLobby(GuestId) &&
            Volatile.Read(ref heard) == 0, $"A join to it is refused by the host it names ({joined.Text(2)}), and its maker hears nothing ({heard} frames).");
        HostLists(true);

        // The host's own lobby with other data (another host name on the map): beside the host's, neither is shown. With
        // the host's backend vouching for its own, that one is, and the map's record is the host's.
        var record = JsonNode.Parse(Record(HostId))!.AsObject();
        record["players"]![0]!["playerName"] = "Impostor";
        var altered = new Dictionary<string, string>(genuine);
        DirectoryData.Put(altered, DirectoryKeys.Record, DirectoryData.Pack(DirectoryService.Public(record, HostId).ToJsonString()));
        Claim(altered);
        shown = await Search("c4");
        check(!shown.Contains(lobbyId), "A listing copying the host's lobby with other data hides both: " + string.Join(", ", shown));
        guest.Presence.SetFriendLobby(HostId, lobbyId, connection, new JsonObject(), host.Presence.LocalListing);
        shown = await Search("c5");
        check(shown.SequenceEqual([lobbyId]) && guest.Directory.HostName(HostId) == "Host Diver",
            $"Vouched for, the host's own is shown, with its record ({guest.Directory.HostName(HostId)}).");

        // Play with strangers off: the friend's vouched listing is shown; one under their id they did not publish is not,
        // also alone.
        guest.Strangers = false;
        guest.Presence.SetFriends([new FriendPresence.Friend(HostId, "Host")]);
        shown = await Search("c6");
        check(shown.SequenceEqual([lobbyId]), "Strangers off: the friend's own listing is shown: " + string.Join(", ", shown));
        HostLists(false);
        Claim(fake);
        shown = await Search("c7");
        check(shown.Count == 0 && Logged("not published by them"), "Strangers off: a listing under a friend's id they did not publish is not: " + string.Join(", ", shown));
        HostLists(true);
        guest.Strangers = true;
        guest.Presence.SetFriends([]);
        guest.Presence.ClearFriendLobby(HostId);

        // An exact copy of the host's listing counts once.
        Claim(new Dictionary<string, string>(genuine));
        shown = await Search("c8");
        check(shown.SequenceEqual([lobbyId]) && Logged("a copy of another listing"), "A copy of a listing counts once: " + string.Join(", ", shown));
        Claim();
        await Task.Delay(SameSearchPause);
    }

    // The lobby ids a lobby.found answer shows, in order.
    static List<string> Shown(Frame found)
    {
        var ids = new List<string>();
        var index = 3;
        for (var i = 0; i < found.Int(2); i++)
        {
            ids.Add(found.Text(index));
            index += 6;
            found.Properties(ref index);
        }
        return ids;
    }

    // Searches within DirectoryService.SameSearch share one directory search; tests that change listings wait it out.
    static readonly TimeSpan SameSearchPause = DirectoryService.SameSearch + TimeSpan.FromMilliseconds(100);

    static void Filters(Action<bool, string> check)
    {
        var filter = LobbyFilter.Parse(MapFilter);
        check(filter.Clauses.Count == 4 && filter.Ignored.Count == 4, $"Clauses kept {filter.Clauses.Count}, left out {filter.Ignored.Count}.");
        var equal = filter.Equalities();
        check(equal.Count == 3 && equal["string_key1"] == Build && equal["number_key6"] == "0" && equal["number_key3"] == "261", "The coarse filters.");
        var props = new Dictionary<string, string>(PublicSearch);
        check(filter.Matches(props, 1, 4, false) && !filter.Matches(props, 4, 4, false), "Member count clause.");
        props["number_key3"] = "260";
        check(!filter.Matches(props, 1, 4, false), "Another planet fails.");
        props.Remove("number_key3");
        check(!filter.Matches(props, 1, 4, false), "A missing key never matches.");
        var quoted = LobbyFilter.Parse("string_key2 eq 'it''s' and number_key5 ge 2.5 and lobby/membershipLock eq 'Unlocked' and lobby/amMember eq true");
        check(quoted.Clauses[0].Value == "it's" && quoted.Matches(new Dictionary<string, string> { ["string_key2"] = "it's", ["number_key5"] = "3" }, 1, 4, false) == false,
            "Quotes, decimals; amMember is never true from a search.");
        check(LobbyFilter.Parse("number_key5 ge 2.5").Matches(new Dictionary<string, string> { ["number_key5"] = "10" }, 1, 4, false), "Numbers compare as numbers (10 > 2.5).");
        check(LobbyFilter.Parse("lobby/membershipLock eq 'Unlocked'").Matches(new Dictionary<string, string>(), 1, 4, true) == false, "A locked lobby fails the lock clause.");
        check(LobbyFilter.Parse("").Clauses.Count == 0 && LobbyFilter.Parse(null).Matches(new Dictionary<string, string>(), 0, 4, false), "No filter matches everything.");
        foreach (var bad in new[] { "string_key1 eq", "string_key1 like 'x'", "string_key1 eq 'open", "a eq 1 or b eq 2", "a eq 1 and", "'k' eq 1", new string('a', 1001) })
        {
            var refused = false;
            try { LobbyFilter.Parse(bad); } catch (InvalidDataException) { refused = true; }
            check(refused, "A malformed filter was taken: " + bad[..Math.Min(40, bad.Length)]);
        }
    }

    static void Values(Action<bool, string> check)
    {
        var data = new Dictionary<string, string>();
        var big = string.Concat(Enumerable.Range(0, 9000).Select(i => (char)('a' + i % 26)));
        DirectoryData.Put(data, "k", big);
        DirectoryData.Put(data, "small", "x");
        DirectoryData.Put(data, "hash", "#not a count");
        check(data["k"] == "#3" && data["k.0"].Length == DirectoryData.PartLength && DirectoryData.Get(data, "k") == big && DirectoryData.Get(data, "small") == "x" &&
            DirectoryData.Get(data, "hash") == "#not a count", "Long values in parts, read back whole.");
        data.Remove("k.1");
        check(DirectoryData.Get(data, "k") == null && DirectoryData.Get(data, "absent") == null, "A missing part reads as nothing.");
        var json = "{\"a\":[" + string.Join(",", Enumerable.Repeat("1", 3000)) + "]}";
        check(DirectoryData.Unpack(DirectoryData.Pack(json)) == json && DirectoryData.Pack(json).Length < json.Length / 4, "JSON packs small and unpacks.");
        check(DirectoryData.Unpack("not base64!") == null && DirectoryData.Unpack(Convert.ToBase64String([1, 2, 3])) == null &&
            DirectoryData.Unpack(DirectoryData.Pack(new string('x', 2 << 20)), limit: 1 << 20) == null, "Bad or oversized packed values read as nothing.");
        var tooBig = false;
        try { DirectoryData.Put(data, "huge", new string('z', DirectoryData.PartLength * DirectoryData.MaxParts + 1)); } catch (InvalidDataException) { tooBig = true; }
        check(tooBig, "A value beyond the parts limit is refused.");
    }

    // Strangers: presence never, a join only with a public lobby listed (and not too often), the rest once related.
    static void Gate(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        network.Strangers.Add(OtherId);
        var transport = network.Join(HostId);
        var listing = false;
        var related = new HashSet<ulong>();
        // Play with strangers is on: the existing behavior.
        var gate = new StrangerGate(transport, () => listing, peer => related.Contains(peer), () => true, _ => { });
        var join = new Frame("lobby.join").Add("op");
        check(gate.Admit(GuestId, new Frame("presence")) && gate.Admit(GuestId, new Frame("party.message")), "Friends: everything.");
        check(!gate.Admit(OtherId, new Frame("presence")) && !gate.Admit(OtherId, new Frame("party.message")) && !gate.Admit(OtherId, join) &&
            gate.Admit(OtherId, new Frame("peer.ping")), "A stranger: no join without a listed lobby, only pings.");
        listing = true;
        var admitted = Enumerable.Range(0, 10).Count(_ => gate.Admit(OtherId, join));
        check(admitted == StrangerGate.JoinsPerPeer, $"Joins from one stranger are limited ({admitted}).");
        related.Add(OtherId);
        check(gate.Admit(OtherId, new Frame("party.message")) && gate.Admit(OtherId, new Frame("account.key")) && !gate.Admit(OtherId, new Frame("presence.hello")),
            "A stranger in a lobby here: its game's traffic, never presence.");

        // Play with strangers is off: all strangers refused, friends still pass.
        var closed = new StrangerGate(transport, () => true, _ => true, () => false, _ => { });
        check(closed.Admit(GuestId, new Frame("presence")), "Friends still pass with strangers off.");
        check(!closed.Admit(OtherId, join) && !closed.Admit(OtherId, new Frame("peer.ping")),
            "Strangers refused entirely when Play with strangers is off.");
    }
}
