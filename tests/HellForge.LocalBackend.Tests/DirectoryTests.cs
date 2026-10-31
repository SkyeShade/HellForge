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
        QuickPlay(check);
        Versions(check);
        // A left-out lobby's log line: its host, the reason, the mods in the way (at most BlockersPerLine, then a count).
        var blockers = Enumerable.Range(0, 10).Select(i => new JoinBlocker(new ModSetEntry($"test.m{i}", $"M{i}", "1", new string('a', 64), 1, ModClass.Code, i),
            SyncAction.Manual, "share off")).ToList();
        var many = DirectoryService.LeftOutLine(OtherId, "it needs mods installed by hand", new JoinCheck(false, "it needs mods installed by hand", null, blockers));
        check(many.StartsWith(LobbyService.EntityId(OtherId) + ": it needs mods installed by hand: M0 1 (test.m0): share off, ", StringComparison.Ordinal) &&
            many.Contains("M7 1 (test.m7)") && !many.Contains("test.m8") && many.EndsWith(", and 2 more.", StringComparison.Ordinal), "The left-out line: " + many);
        check(DirectoryService.LeftOutLine(OtherId, "the game's filter", null) == LobbyService.EntityId(OtherId) + ": the game's filter.", "A reason without mods.");
        root = Path.Combine(root, "directory");
        await Saturated(root, check);
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
        // HellForge 0.3 can join it too (no shared bundled mod): it searches by 0.3's protocol, this one's is in hf.protocol
        check(listing.Host == HostId && listing.Data[DirectoryKeys.Protocol] == LobbyService.LegacyProtocol && listing.Data[DirectoryKeys.Feature] == LobbyService.Protocol &&
            listing.Data.ContainsKey(DirectoryKeys.ModSet) && listing.Data[DirectoryKeys.Open] == "1" &&
            listing.Data[DirectoryKeys.Wire] == LobbyService.WireLevel.ToString(CultureInfo.InvariantCulture) && listing.Data[DirectoryKeys.LobbyId] == lobbyId &&
            listing.Data[DirectoryKeys.Connection] == connection && listing.Data[DirectoryKeys.Host] == HostId.ToString(CultureInfo.InvariantCulture) &&
            listing.Data[DirectoryKeys.Members] == "1" && listing.Data[DirectoryKeys.Slots] == "3" && listing.Data["number_key3"] == "261" &&
            listing.Data[DirectoryKeys.Stats] == "0" && listing.Data[DirectoryKeys.Share] == "1" && listing.Data[DirectoryKeys.Digest] == hostSet.Digest &&
            !listing.Data.ContainsKey(DirectoryKeys.Record), "The listing: " + string.Join(", ", listing.Data.Keys));
        var wire = LobbyService.WireLevel.ToString(CultureInfo.InvariantCulture);
        check(DirectoryData.Matches(listing, new DirectoryQuery(wire, new Dictionary<string, string>())) &&
            !DirectoryData.Matches(listing, new DirectoryQuery("2", new Dictionary<string, string>())), "A search does not go by hf.wire.");
        check(DirectoryService.Valid(listing, out _, out _, out _, out _, out var listingWhy), "The current listing is not valid: " + listingWhy);
        // 0.3's listing (its protocol, no hf.protocol) is taken and known as 0.3's; 0.2's (mp-7) and a listing claiming
        // an unknown protocol are not
        DirectoryListing Speaking(string? feature, string protocol)
        {
            var data = new Dictionary<string, string>(listing.Data, StringComparer.Ordinal) { [DirectoryKeys.Protocol] = protocol };
            if (feature == null) data.Remove(DirectoryKeys.Feature);
            else data[DirectoryKeys.Feature] = feature;
            return new DirectoryListing(listing.Host, data);
        }
        var older = Speaking(null, LobbyService.LegacyProtocol);
        check(DirectoryService.Valid(older, out _, out _, out _, out _, out var olderWhy) && DirectoryService.Legacy(older) && !DirectoryService.Legacy(listing),
            "A 0.3 listing was left out or not known as one: " + olderWhy);
        check(!DirectoryService.Valid(Speaking(null, "hellforge-mp-7"), out _, out _, out _, out _, out var oldestWhy) && oldestWhy == "protocol" &&
            !DirectoryService.Valid(Speaking("hellforge-mp-99", LobbyService.LegacyProtocol), out _, out _, out _, out _, out _), "An mp-7 listing was taken: " + oldestWhy);
        check(DirectoryData.Matches(listing, new DirectoryQuery(LobbyService.LegacyProtocol, new Dictionary<string, string>(), WireKey: DirectoryKeys.Protocol)),
            "0.3's search does not find a listing it can join.");
        // this HellForge's whole set in hf.modset, 0.3's in hf.mods (the same here: no per-player or bundled mod)
        check(ModSet.Parse(DirectoryData.Unpack(DirectoryData.Get(listing.Data, DirectoryKeys.ModSet))!, local: false).Mods.Single().Id == "test.ammo" &&
            ModSet.Parse(DirectoryData.Unpack(DirectoryData.Get(listing.Data, DirectoryKeys.Mods))!, local: false) is { } legacySet &&
            legacySet.Mods.Single().Id == "test.ammo" && legacySet.Digest == hostSet.LegacyDigest, "The listing carries the mod sets.");
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
        lock (guest.Log) check(guest.Log.Any(l => l.StartsWith("Directory: left out " + LobbyService.EntityId(HostId) + ": it runs a blocked mod: ", StringComparison.Ordinal) &&
            l.Contains("(test.ammo): blocked")), "The search names the blocked mod: " + string.Join(" / ", guest.Log.TakeLast(2)));
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
        // stranger's would need a sandboxed session (else installing them by hand) and is left out, and the search log says so.
        var code = new ModSet("0123456789abcdef0123456789abcdef", [new ModSetEntry("test.code", "Code", "1", new string('c', 64), 10, ModClass.Code, 0)]);
        var otherLobby = Guid.NewGuid().ToString("D");
        var other = new Dictionary<string, string>(listings.All.Single().Data)
        {
            [DirectoryKeys.LobbyId] = otherLobby, [DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + otherLobby + ";" + LobbyService.EntityId(OtherId),
            [DirectoryKeys.Host] = OtherId.ToString(CultureInfo.InvariantCulture), [DirectoryKeys.Digest] = code.Digest
        };
        DirectoryData.Put(other, DirectoryKeys.Mods, DirectoryData.Pack(code.ToJson(local: false).ToJsonString()));
        DirectoryData.Put(other, DirectoryKeys.ModSet, DirectoryData.Pack(code.ToJson(local: false).ToJsonString()));
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
        lock (guest.Log) check(guest.Log.Any(l => l.Contains("2 listed, 1 shown (1 left out: its host's unchecked mods download only in a sandboxed session)")),
            "The search says why: " + string.Join(" / ", guest.Log.TakeLast(2)));
        var leftOutLine = "Directory: left out " + LobbyService.EntityId(OtherId) + ": its host's unchecked mods download only in a sandboxed session: Code 1 (test.code): stranger code, not sandboxed.";
        lock (guest.Log) check(guest.Log.Count(l => l == leftOutLine) == 1, "The search names the stranger's mod and why: " + string.Join(" / ", guest.Log.TakeLast(3)));
        await Task.Delay(SameSearchPause);
        await guest.Directory.SearchAsync("u1t", MapFilter, 50);
        lock (guest.Log) check(guest.Log.Count(l => l == leftOutLine) == 1, "The same left-out lobby is not logged again by the next search.");

        // The lobby browser's API: every lobby with no game filter, the stranger's with why it cannot be joined; one search
        // per MinInterval; a join hands the game an invitation, and only to a joinable lobby of a recent list.
        var browseClock = DateTime.UtcNow;
        var browse = new BrowseApi(() => browseClock);
        var unattached = await new BrowseApi().ListAsync();
        check(unattached["available"]?.GetValue<bool>() == false && new BrowseApi().Join(lobbyId).Status == 503, "Before multiplayer runs the browser is unavailable.");
        browse.Attach(guest.Directory, guest.Lobby, guest.Presence);
        await Task.Delay(SameSearchPause);
        var listTask = browse.ListAsync();
        check(ReferenceEquals(listTask, browse.ListAsync()), "Two asks within MinInterval share one search.");
        var browsed = (await listTask)["lobbies"]!.AsArray().OfType<JsonObject>().ToList();
        var open = browsed.SingleOrDefault(l => (string?)l["lobbyId"] == lobbyId);
        var closed = browsed.SingleOrDefault(l => (string?)l["lobbyId"] == otherLobby);
        check(browsed.Count == 2 && open?["joinable"]?.GetValue<bool>() == true && open["host"]?.GetValue<string>() == HostId.ToString(CultureInfo.InvariantCulture),
            "The browser lists the open lobby: " + string.Join(" | ", browsed.Select(l => l.ToJsonString())));
        check(closed?["joinable"]?.GetValue<bool>() == false && (string?)closed["verdict"] == "unjoinable" &&
            (string?)closed["reason"] == "Its host's unchecked mods download only in a sandboxed session." && (string?)closed["fix"] == JoinCheck.FixSandbox &&
            closed["mods"]?.AsArray().OfType<JsonObject>().Any(m => (string?)m["id"] == "test.code" && (string?)m["action"] == "manual" && (string?)m["why"] == "stranger code, not sandboxed") == true,
            "The browser lists the stranger's lobby with why: " + closed?.ToJsonString());
        check(browse.Join(otherLobby).Status == 409 && browse.Join(Guid.NewGuid().ToString("D")).Status == 404, "Joining an unjoinable or unknown lobby is refused.");
        var joined = browse.Join(lobbyId);
        var invite = guest.Next("lobby.inviteReceived", check);
        check(joined.Status == 202 && invite.Text(0) == lobbyId && invite.Text(1) == connection, "Joining hands the game an invitation: " + joined.Body.ToJsonString());
        await LiveBrowser(browse, guest, lobbyId, otherLobby, check);
        // With Play with strangers off, strangers' lobbies are still listed (never contacted), as ones to join after
        // turning it on; Quick Play then finds none.
        guest.Strangers = false;
        browse.Forget();
        await Task.Delay(SameSearchPause);
        var offList = (await browse.ListAsync())["lobbies"]!.AsArray().OfType<JsonObject>().ToList();
        check(offList.Count == 2 && offList.All(l => l["joinable"]?.GetValue<bool>() == false && (string?)l["fix"] == JoinCheck.FixStrangers &&
            (string?)l["reason"] == "Its host is not a Steam friend, and Play with strangers is off."),
            "Strangers off: their lobbies are listed with the setting that lets them in: " + string.Join(" | ", offList.Select(l => l.ToJsonString())));
        lock (guest.Log) check(guest.Log.Any(l => l.StartsWith("Directory: browse: 2 listed, 0 joinable (2: its host is not a Steam friend", StringComparison.Ordinal)),
            "The browse says what it left unjoinable: " + string.Join(" / ", guest.Log.TakeLast(3)));
        var none = await browse.QuickJoinAsync();
        check(none.Status == 404 && (string?)none.Body["errorCode"] == "HELLFORGE_NO_JOINABLE_LOBBY", "Quick Play with nothing joinable says so.");
        guest.Strangers = true;
        browse.Forget();
        await Task.Delay(SameSearchPause);
        var quick = await browse.QuickJoinAsync();
        var quickInvite = guest.Next("lobby.inviteReceived", check);
        check(quick.Status == 202 && quick.Outcome == "browse_quick_invited" && (string?)quick.Body["lobbyId"] == lobbyId && (string?)quick.Body["hostName"] == "Host Diver" &&
            quickInvite.Text(0) == lobbyId, "Quick Play joins the joinable lobby: " + quick.Body.ToJsonString());
        browseClock += BrowseApi.JoinableFor + TimeSpan.FromSeconds(1);
        check(browse.Join(lobbyId).Status == 404, "A lobby from an old list must be listed again first.");
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
    // The in-game browser (LiveBrowse, SDK 16) over the live-edit pipe: the editor's refresh brings the API's list as
    // browselobby lines and a browsestate; a second one says it searches first; a join answers with its outcome.
    static async Task LiveBrowser(BrowseApi browse, Node guest, string lobbyId, string otherLobby, Action<bool, string> check)
    {
        var runId = "browse-" + Guid.NewGuid().ToString("N")[..8];
        await using var live = new LiveEditServer(runId);
        var logged = new ConcurrentQueue<string>();
        var modsFolder = Directory.CreateTempSubdirectory("hf-browse-").FullName;
        var browser = new LiveBrowse(browse, live, logged.Enqueue, modsFolder);
        live.Start();
        using var pipe = new System.IO.Pipes.NamedPipeClientStream(".", LiveEditServer.PipeName(runId), System.IO.Pipes.PipeDirection.InOut,
            System.IO.Pipes.PipeOptions.Asynchronous);
        await pipe.ConnectAsync(5000);
        using var reader = new StreamReader(pipe, new System.Text.UTF8Encoding(false));
        using var writer = new StreamWriter(pipe, new System.Text.UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
        await writer.WriteLineAsync($"hello\t{LiveEditServer.Protocol}\t{StatsSchema.Current.Build}\t{StatsSchema.Fingerprint}");
        var waited = System.Diagnostics.Stopwatch.StartNew();
        while (!live.Connected && waited.Elapsed < TimeSpan.FromSeconds(5)) await Task.Delay(20);
        // the game's side: each line without its sequence number, answered as the loader answers (live.c), up to a browsestate
        // (or one in a state of until's)
        async Task<List<string>> Game(string ask, params string[] until)
        {
            await writer.WriteLineAsync(ask);
            var lines = new List<string>();
            while (await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(10)) is { } line)
            {
                var parts = line.Split('\t');
                lines.Add(string.Join('\t', parts.Where((_, i) => i != 1)));
                await writer.WriteLineAsync(parts[1] + "\tok\tnow");
                if (parts[0] == "browsestate" && parts[3] != "searching" && (until.Length == 0 || until.Contains(parts[3]))) return lines;
            }
            return lines;
        }
        var first = await Game("browseask\trefresh\t-");
        var open = first.SingleOrDefault(l => l.Contains("\t" + lobbyId + "\t"));
        var closed = first.SingleOrDefault(l => l.Contains("\t" + otherLobby + "\t"));
        check(first.Count == 3 && first[2] == "browsestate\t1\tready\t2\t" && open != null && open.StartsWith("browselobby\t1\t", StringComparison.Ordinal) &&
            open.Split('\t')[4].Contains("joinable") && closed != null && closed.Split('\t') is [_, "1", _, "unjoinable", "-", _, _, _, _, "1", "0", _, _, "Its host's unchecked mods download only in a sandboxed session.", _, "Code 1: manual", "sandbox"],
            "The game gets the list: " + string.Join(" | ", first));
        var second = await Game("browseask\trefresh\t-");
        check(second.FirstOrDefault() == "browsestate\t1\tsearching\t2\t" && second.Last() == "browsestate\t2\tready\t2\t",
            "A new search keeps the old list until the new one: " + string.Join(" | ", second));
        var joined = await Game("browseask\tjoin\t" + lobbyId);
        var invite = guest.Next("lobby.inviteReceived", check);
        check(joined.SequenceEqual(["browsestate\t2\tinvited\t2\tAccept the invitation in game to join. Other shared mods bring HellForge's MOD SYNC prompt."]) &&
            invite.Text(0) == lobbyId, "A join from the game hands it the invitation: " + string.Join(" | ", joined));
        var refused = await Game("browseask\tjoin\t" + otherLobby);
        check(refused.SequenceEqual(["browsestate\t2\tfailed\t2\tIts host's unchecked mods download only in a sandboxed session."]) && logged.Any(l => l.Contains("browse_lobby_not_joinable")),
            "An unjoinable lobby's join says why: " + string.Join(" | ", refused));
        var quick = await Game("browseask\tquick\t-", "invited", "failed");
        var quickInvite = guest.Next("lobby.inviteReceived", check);
        check(quick.Last() == "browsestate\t3\tinvited\t2\tQuick Play: joining Host Diver's lobby. Accept the invitation in game." && quickInvite.Text(0) == lobbyId &&
            logged.Any(l => l == $"Lobby browser: Quick Play: browse_quick_invited {lobbyId}."), "Quick Play from the game lists, then joins the best lobby: " + string.Join(" | ", quick));
        var shown = await Game("browseask\tallow\teditedstats");
        check(ModSettings.Read(modsFolder).ShowEditedStats && shown.Last().StartsWith("browsestate\t4\tready\t", StringComparison.Ordinal),
            "Showing edited-stats lobbies from the game saves the setting and lists again: " + string.Join(" | ", shown));
        var allowed = await Game("browseask\tallow\tstrangers");
        check(ModSettings.PlayWithStrangers(modsFolder) == true && ModSettings.Read(modsFolder).StrangersWarned && allowed.Last().StartsWith("browsestate\t5\tready\t", StringComparison.Ordinal) &&
            logged.Any(l => l.Contains("turned Play with strangers on")), "Play with strangers turned on from the game, after its warning, is saved: " + string.Join(" | ", allowed));
        check(LiveBrowse.Line(new JsonObject { ["lobbyId"] = "x", ["verdict"] = "unjoinable", ["fix"] = JoinCheck.FixStrangers }) is { Fix: "strangers" } &&
            LiveBrowse.Line(new JsonObject { ["lobbyId"] = "x", ["fix"] = JoinCheck.FixEditedStats }) is { Fix: "editedstats" } &&
            LiveBrowse.Line(new JsonObject { ["lobbyId"] = "x", ["fix"] = JoinCheck.FixSandbox }) is { Fix: "sandbox" } &&
            LiveBrowse.Line(new JsonObject { ["lobbyId"] = "x", ["fix"] = "vip" }) is { Fix: "" }, "A lobby's fix goes to the game by its word.");
        check(LiveBrowse.Line(new JsonObject { ["lobbyId"] = "has space", ["verdict"] = "ready" }) == null &&
            LiveBrowse.Line(new JsonObject { ["lobbyId"] = "x", ["members"] = 99, ["difficulty"] = 7L, ["planetId"] = -1 }) is { Members: 64, Difficulty: 7, Planet: null, Verdict: "unknown" },
            "A lobby id the loader refuses is left out; counts are kept in range.");
        GC.KeepAlive(browser);
        Directory.Delete(modsFolder, true);
    }

    // Another HellForge: whose bundled mod is older says who updates.
    static void Versions(Action<bool, string> check)
    {
        ModSetEntry Editor(string version) => new("hellforge.editor", "HellForge Editor", version, new string('e', 64), 10, ModClass.Bundled, 0);
        check(ModSync.OtherVersion(Editor("0.4.1"), Editor("0.4.2")) == "its host runs an older HellForge (HellForge Editor 0.4.1, yours 0.4.2): the host needs to update",
            "An older host is told to update.");
        check(ModSync.OtherVersion(Editor("0.5.0"), Editor("0.4.2")) == "its host runs a newer HellForge (HellForge Editor 0.5.0, yours 0.4.2): update HellForge to join",
            "A newer host: the player updates.");
        check(ModSync.OtherVersion(Editor("0.4.2"), Editor("0.4.2")) == "its host runs another HellForge version" &&
            ModSync.OtherVersion(Editor("dev"), null) == "its host runs another HellForge version", "The same or unreadable versions say neither.");
        check(DirectoryService.SquadSize(8) == 4 && DirectoryService.SquadSlots(8, 4) == 0 && DirectoryService.SquadSlots(8, 1) == 3 && DirectoryService.SquadSlots(2, 1) == 1,
            "A squad is at most 4 of the lobby's 8 places.");
    }

    // Quick Play's choice (BrowseApi.Best): joinable with a slot free, a friend's first, then the least a join needs, then
    // the fullest.
    static void QuickPlay(Action<bool, string> check)
    {
        static BrowseLobby Lobby(string id, bool friend, int members, JoinCheck join, bool locked = false, int max = 4) =>
            new(id, "c", 1, friend, "directory", members, max, locked, new Dictionary<string, string>(), new JsonObject(), join);
        var mod = new ModSetEntry("m", "M", "1", new string('a', 64), 5_000_000, ModClass.Content, 0);
        var ready = new JoinCheck(true, "", null, []);
        var download = new JoinCheck(true, "", new ModSyncPlan([new SyncItem(mod, SyncAction.Download, null)]), []);
        var no = new JoinCheck(false, "it runs a blocked mod", null, []);
        var all = new[]
        {
            Lobby("stranger-ready-2", false, 2, ready), Lobby("stranger-ready-3", false, 3, ready), Lobby("stranger-download", false, 3, download),
            Lobby("friend-download", true, 1, download), Lobby("friend-locked", true, 1, ready, locked: true), Lobby("friend-full", true, 4, ready),
            Lobby("friend-blocked", true, 1, no)
        };
        check(BrowseApi.Best(all)?.LobbyId == "friend-download", "Quick Play picks a friend's joinable lobby first.");
        check(BrowseApi.Best(all.Where(l => !l.Friend))?.LobbyId == "stranger-ready-3", "Then the same mods before a download, and the fuller lobby.");
        check(BrowseApi.Best(all.Where(l => l.LobbyId.Contains("locked") || l.LobbyId.Contains("full") || l.LobbyId.Contains("blocked"))) == null,
            "Locked, full and unjoinable lobbies are never picked.");
    }

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

    // Steam's directory in a fixed order, its cut at a query's count, every query counted.
    sealed class OrderedDirectory(List<DirectoryListing> listings) : ILobbyDirectory
    {
        public readonly List<DirectoryQuery> Queries = [];
        public void Publish(DirectoryListing? listing) { }
        public Task<IReadOnlyList<DirectoryListing>> SearchAsync(DirectoryQuery query, CancellationToken cancellation)
        {
            lock (Queries) Queries.Add(query);
            return Task.FromResult<IReadOnlyList<DirectoryListing>>([.. listings.Where(l => DirectoryData.Matches(l, query)).Take(query.Count)]);
        }
    }

    // More listings than Steam returns: the lobbies with the searcher's mods, then the ones anyone can join after a download,
    // are searched for and shown first; below the cut one search (and 0.3's) does.
    static async Task Saturated(string root, Action<bool, string> check)
    {
        var hostMods = Path.Combine(root, "saturated", "mods");
        Directory.CreateDirectory(Path.Combine(hostMods, "ammo"));
        File.WriteAllText(Path.Combine(hostMods, "ammo", ModCatalog.ManifestName),
            """{"format":1,"id":"test.ammo","name":"Ammo","version":"1","native":{"networkFieldTypes":[{"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}]}}""");
        var mods = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(hostMods).Mods));
        var network = new InMemoryNetwork();
        var memory = new MemoryLobbyDirectory();
        var host = new Node(network, memory, HostId, Path.Combine(root, "saturated", "host"), mods);
        host.Lobby.FromLocal(Create("c1", HostId, PublicSearch));
        host.Next("lobby.result", check);
        host.Directory.Publish(DateTime.UtcNow);
        var template = memory.All.Single().Data;
        DirectoryListing Listing(int n, string digest, string open)
        {
            var id = OtherId + 100 + (ulong)n;
            var lobby = Guid.NewGuid().ToString("D");
            return new DirectoryListing(id, new Dictionary<string, string>(template, StringComparer.Ordinal)
            {
                [DirectoryKeys.LobbyId] = lobby, [DirectoryKeys.Connection] = LobbyService.ConnectionPrefix + lobby + ";" + LobbyService.EntityId(id),
                [DirectoryKeys.Host] = id.ToString(CultureInfo.InvariantCulture), [DirectoryKeys.Digest] = digest, [DirectoryKeys.Open] = open
            });
        }
        var other = new string('f', 32);
        var full = Enumerable.Range(0, DirectoryQuery.MaxResults).Select(n => Listing(n, other, "0")).ToList();
        var same = Listing(98, mods.Digest, "0");
        var open = Listing(99, other, "1");
        full.AddRange([open, same]);
        var listings = new OrderedDirectory(full);
        var guest = new Node(network, memory, GuestId, Path.Combine(root, "saturated", "guest"), mods, listings);
        var found = Shown(await guest.Directory.SearchAsync("s1", MapFilter, 50));
        check(found.Count == 50 && found[0] == same.Data[DirectoryKeys.LobbyId] && found[1] == open.Data[DirectoryKeys.LobbyId],
            "Past Steam's cut, the lobby with the searcher's mods and the open one are not shown first.");
        check(listings.Queries.Count == 4 && listings.Queries.Count(q => q.Equal.GetValueOrDefault(DirectoryKeys.Digest) == mods.Digest) == 1 &&
            listings.Queries.Count(q => q.Equal.GetValueOrDefault(DirectoryKeys.Open) == "1") == 1 &&
            listings.Queries.Count(q => q.WireKey == DirectoryKeys.Protocol && q.Wire == LobbyService.LegacyProtocol) == 1,
            "The searches past the cut: " + string.Join(" / ", listings.Queries.Select(q => q.WireKey + "=" + q.Wire + " " + string.Join(",", q.Equal.Keys))));
        await Task.Delay(SameSearchPause);
        full.RemoveRange(10, DirectoryQuery.MaxResults - 10);
        listings.Queries.Clear();
        found = Shown(await guest.Directory.SearchAsync("s2", MapFilter, 50));
        check(found.Count == 12 && listings.Queries.Count == 2, $"Below the cut: {found.Count} shown after {listings.Queries.Count} searches.");
        await host.Directory.DisposeAsync();
        await guest.Directory.DisposeAsync();
        await host.Sync.DisposeAsync();
        await guest.Sync.DisposeAsync();
    }

    // The lobby ids a lobby.found answer shows, in order.    // The lobby ids a lobby.found answer shows, in order.
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
