using System.Collections.Concurrent;
using System.IO.Pipes;
using HellForge.LocalBackend;
using HellForge.LocalBackend.Multiplayer;

// Two (and three) backends' lobby services wired together, each with a fake stand-in DLL that records its frames.
static class MultiplayerTests
{
    const ulong HostId = 0x110000100000001, GuestId = 0x110000100000002, StrangerId = 0x110000100000003;

    sealed class Node
    {
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly BlockingCollection<Frame> PartyInbox = new();
        public readonly LobbyService Lobby;
        public readonly PartyService Party;
        public readonly WarClock Clock = new();
        // What the game is told of refused joins: ("refused" by a host | "friend" refused here, the other machine, lines).
        public readonly BlockingCollection<(string Kind, ulong Peer, IReadOnlyList<string> Lines)> Notices = new();
        public Node(InMemoryNetwork network, ulong id, string digest, string? modList = null)
        {
            var transport = network.Join(id);
            Lobby = new LobbyService(transport, Inbox.Add, _ => { }, digest, Clock, modList);
            Lobby.Refused += (peer, lines) => Notices.Add(("refused", peer, lines));
            Lobby.RefusedFriend += (peer, lines) => Notices.Add(("friend", peer, lines));
            Party = new PartyService(transport, PartyInbox.Add, _ => { }, Lobby.InHostedLobby);
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("party.", StringComparison.Ordinal)) Party.FromPeer(peer, frame);
                else if (frame.Type == "war.clock") { if (Lobby.HostsJoinedLobby(peer)) Clock.Set(long.Parse(frame.Text(0))); }
                else Lobby.FromPeer(peer, frame);
            };
        }

        public Frame Next(string type, Action<bool, string> check) => Take(Inbox, type, check);
        public Frame NextParty(string type, Action<bool, string> check) => Take(PartyInbox, type, check);

        static Frame Take(BlockingCollection<Frame> inbox, string type, Action<bool, string> check)
        {
            var got = inbox.TryTake(out var frame, TimeSpan.FromSeconds(5));
            check(got && frame!.Type == type, $"expected {type}, got {(got ? frame!.Type : "nothing")}");
            return frame!;
        }

        public void PartyQuiet(Action<bool, string> check, string what)
        {
            Thread.Sleep(150);
            check(PartyInbox.Count == 0, what + $" (got {(PartyInbox.TryTake(out var extra) ? extra.Type : "")})");
        }

        public void Quiet(Action<bool, string> check, string what)
        {
            Thread.Sleep(150);
            check(Inbox.Count == 0, what + $" (got {(Inbox.TryTake(out var extra) ? extra.Type : "")})");
        }
    }

    internal static Frame Create(string op, ulong creator) => new Frame("lobby.create").Add(op).Add(LobbyService.EntityId(creator)).Add("title_player_account")
        .Add(4).Add(0).Add(0).Add(0)
        .Add(new Dictionary<string, string> { ["1"] = "f8c9037f96047f7a", ["5"] = "1" })
        .Add(new Dictionary<string, string> { ["string_key1"] = "f8c9037f96047f7a" })
        .Add(new Dictionary<string, string> { ["crossplay_mode"] = "0" });

    internal static Frame Join(string op, ulong member, string connection) => new Frame("lobby.join").Add(op).Add(LobbyService.EntityId(member))
        .Add("title_player_account").Add(connection).Add(new Dictionary<string, string> { ["platform_lobby"] = "2:1::" });

    static Frame Update(string op, string lobby, ulong member, IReadOnlyList<PropertyChange> lobbyChanges, IReadOnlyList<PropertyChange>? memberChanges) =>
        new Frame("lobby.update").Add(op).Add(lobby).Add(LobbyService.EntityId(member)).Add(0).Add("").Add(0).Add(0).Add(0).Add(0).Add(0).Add(0)
            .Add(lobbyChanges).Add(Array.Empty<PropertyChange>()).Add(memberChanges != null ? 1 : 0).Add(memberChanges ?? []);

    public static async Task Run(Action<bool, string> check)
    {
        // Frames survive encoding.
        var sample = Create("op", HostId);
        var decoded = Frame.Decode(sample.Encode());
        check(decoded.Type == "lobby.create" && decoded.Count == sample.Count && decoded.Text(1) == LobbyService.EntityId(HostId), "Frame round trip");
        foreach (var bad in new[] { new byte[] { 1, 0, 0, 0 }, new byte[] { 2, 0, 0, 0, 0, 0 }, new byte[] { 9, 0, 0, 0, 1, 0, 9, 0, 0, 0, 1 } })
        {
            var rejected = false;
            try { Frame.Decode(bad); } catch (InvalidDataException) { rejected = true; }
            check(rejected, "A malformed frame decodes");
        }

        var network = new InMemoryNetwork();
        // The host's and the stranger's mods one by one (ModlistDiff's form): A at another version, B only the host's, C
        // only the stranger's. The guest's backend has no list (one from before modlists).
        var host = new Node(network, HostId, "mods-a", "a.mod\tMod A\t1.0\taaaa\nb.mod\tMod B\t2.0\tbbbb");
        var guest = new Node(network, GuestId, "mods-a");
        var stranger = new Node(network, StrangerId, "mods-b", "a.mod\tMod A\t1.1\taaa2\nc.mod\tMod C\t1.0\tcccc");

        // The host creates a lobby; its connection string names the host.
        host.Lobby.FromLocal(Create("c1", HostId));
        var created = host.Next("lobby.result", check);
        check(created.Text(0) == "c1" && created.Text(1) == "create" && created.Text(2) == "0", "Create succeeds");
        var lobbyId = created.Text(3);
        var connection = created.Text(4);
        check(connection == $"hellforge1-lobby;{lobbyId};{LobbyService.EntityId(HostId)}" && created.Text(5) == LobbyService.EntityId(HostId), "Connection string and owner");

        // A friend joins through the connection string: their DLL gets the whole lobby, the host's DLL the new member,
        // and their backend takes the host's war time.
        host.Clock.Set(5_000_000);
        stranger.Clock.Set(42);
        // the host's backend lists the joiner for a sandboxed game's Steam P2P before its game hears of the joiner
        // (MultiplayerHost.ListPeer): the host's game sends to a joiner the moment it does
        var admitted = new System.Collections.Concurrent.ConcurrentQueue<(ulong Peer, bool GameKnew)>();
        host.Lobby.PeerAdmitted += peer => admitted.Enqueue((peer, host.Inbox.Any(f => f.Type == "lobby.memberAdded")));
        guest.Lobby.FromLocal(Join("j1", GuestId, connection));
        var joined = guest.Next("lobby.result", check);
        check(joined.Text(0) == "j1" && joined.Text(1) == "join" && joined.Text(2) == "0" && joined.Text(3) == lobbyId, "Join succeeds");
        var index = 10;
        _ = joined.Properties(ref index);
        _ = joined.Properties(ref index);
        check(joined.Int(index) == 2 && joined.Text(index + 1) == LobbyService.EntityId(HostId), "The joiner sees both members");
        var added = host.Next("lobby.memberAdded", check);
        check(admitted.TryPeek(out var listedFirst) && listedFirst.Peer == GuestId && !listedFirst.GameKnew,
            "The joiner is admitted as a P2P peer before the host's game hears of it");
        Thread.Sleep(100);
        check(Math.Abs(guest.Clock.Seconds - 5_000_000) <= 1, "The friend's war time follows the host's");
        network.Join(StrangerId + 20).Send(GuestId, new Frame("war.clock").Add(7).Encode(), true);
        Thread.Sleep(100);
        check(guest.Clock.Seconds >= 5_000_000, "War time from a machine that hosts no joined lobby is ignored");
        check(added.Text(0) == lobbyId && added.Text(1) == LobbyService.EntityId(GuestId), "The host sees the joiner");

        // The friend's game crashes and comes back before the host notices it was gone: it joins again while still listed,
        // and its earlier membership leaves first.
        guest.Lobby.FromLocal(Join("j1b", GuestId, connection));
        check(guest.Next("lobby.result", check) is var rejoined && rejoined.Text(0) == "j1b" && rejoined.Text(2) == "0", "A friend still listed joins again");
        check(host.Next("lobby.memberRemoved", check).Text(1) == LobbyService.EntityId(GuestId) && host.Next("lobby.memberAdded", check).Text(1) == LobbyService.EntityId(GuestId),
            "The host sees the earlier membership leave and the friend join again");

        // Member properties propagate to everyone; only the owner may change the lobby itself.
        guest.Lobby.FromLocal(Update("u1", lobbyId, GuestId, [], [new("platform_lobby", "2:2::")]));
        check(guest.Next("lobby.result", check).Text(2) == "0", "A member updates itself");
        foreach (var node in new[] { host, guest })
        {
            var updated = node.Next("lobby.updated", check);
            var at = 5;
            _ = updated.Changes(ref at);
            _ = updated.Changes(ref at);
            check(updated.Int(at) == 1 && updated.Text(at + 1) == LobbyService.EntityId(GuestId), "Member update reaches every member");
        }
        guest.Lobby.FromLocal(Update("u2", lobbyId, GuestId, [new("5", "2")], null));
        check(guest.Next("lobby.result", check).Text(2) == LobbyService.AccessDenied.ToString(), "A member may not change the lobby");
        host.Quiet(check, "A refused update is not broadcast");
        host.Lobby.FromLocal(Update("u3", lobbyId, HostId, [new("5", "2"), new("10", null)], null));
        check(host.Next("lobby.result", check).Text(2) == "0", "The owner changes the lobby");
        host.Next("lobby.updated", check);
        guest.Next("lobby.updated", check);

        // A friend with other gameplay mods is refused; nobody can join as someone else.
        stranger.Lobby.FromLocal(Join("j2", StrangerId, connection));
        check(stranger.Next("lobby.result", check).Text(2) == LobbyService.ModlistMismatch.ToString(), "Different mods are refused");
        // Both games are told which mods differ, each from its own side.
        check(stranger.Notices.TryTake(out var refusal, TimeSpan.FromSeconds(5)) && refusal.Kind == "refused" && refusal.Peer == HostId &&
            refusal.Lines.SequenceEqual(["Your gameplay mods don't match the host's.", "Mod A: you have 1.1, the host has 1.0",
                "Missing: Mod B 2.0 (the host has it)", "Not on the host: Mod C 1.0",
                "Turn on the same gameplay mods on the launcher's MODS page, then start a new session."]),
            "The refused joiner is told which mods differ: " + string.Join(" | ", refusal.Lines ?? []));
        check(host.Notices.TryTake(out var told, TimeSpan.FromSeconds(5)) && told.Kind == "friend" && told.Peer == StrangerId &&
            told.Lines.SequenceEqual(["Their gameplay mods don't match yours.", "Mod A: they have 1.1, you have 1.0", "They don't have Mod B 2.0",
                "They have Mod C 1.0, which you don't"]),
            "The host is told which of a friend's mods differ: " + string.Join(" | ", told.Lines ?? []));
        var impostor = new Node(network, 0x110000100000004, "mods-a");
        impostor.Lobby.FromLocal(Join("j3", GuestId, connection));
        check(impostor.Next("lobby.result", check).Text(2) == LobbyService.AccessDenied.ToString(), "Joining as another player is refused");
        // An older HellForge forwards the join without a protocol version, and is refused even with the same mods.
        var outdated = network.Join(0x110000100000005);
        var outdatedAnswers = new BlockingCollection<Frame>();
        outdated.Received += (_, bytes) => outdatedAnswers.Add(Frame.Decode(bytes));
        outdated.Send(HostId, Join("j4", 0x110000100000005, connection).Add("mods-a").Encode(), reliable: true);
        // The reason comes first ("lobby.refused"), then the join fails as not joinable; the test transport may reorder them.
        Frame? outdatedResult = null, outdatedReason = null;
        while ((outdatedResult == null || outdatedReason == null) && outdatedAnswers.TryTake(out var answer, TimeSpan.FromSeconds(5)))
            if (answer.Type == "lobby.result") outdatedResult = answer;
            else if (answer.Type == "lobby.refused") outdatedReason = answer;
        check(outdatedResult?.Text(2) == LobbyService.LobbyNotJoinable.ToString(), "An older HellForge was let in");
        check(outdatedReason?.Text(0).Contains("HellForge versions", StringComparison.Ordinal) == true, "The refusal did not say why");
        check(outdatedReason?.Count > 1 && outdatedReason.Text(1).Contains("HellForge version", StringComparison.Ordinal), "The refusal has no line for the joiner's game");
        check(host.Notices.TryTake(out var outdatedTold, TimeSpan.FromSeconds(5)) && outdatedTold.Peer == 0x110000100000005, "The host is not told of an older HellForge's join");
        // A friend's backend from before modlists (the guest's: no list) is refused with the reason alone, which its game shows.
        var listless = new Node(network, StrangerId + 30, "mods-c");
        listless.Lobby.FromLocal(Join("j6", StrangerId + 30, connection));
        listless.Next("lobby.result", check);
        check(listless.Notices.TryTake(out var plain, TimeSpan.FromSeconds(5)) &&
            plain.Lines.SequenceEqual(["Your gameplay mods don't match the host's.", "Turn on the same gameplay mods on the launcher's MODS page, then start a new session."]),
            "Without a modlist the joiner is told what to do: " + string.Join(" | ", plain.Lines ?? []));
        check(host.Notices.TryTake(out var plainTold, TimeSpan.FromSeconds(5)) && plainTold.Lines.SequenceEqual(["Their gameplay mods don't match yours."]), "The host's line without a modlist");
        host.Quiet(check, "Refused joins are not announced");

        // Events about a lobby only count from its owner.
        network.Join(StrangerId + 10).Send(GuestId, new Frame("lobby.updated").Add(lobbyId).Add("").Add("").Add("").Add("")
            .Add(Array.Empty<PropertyChange>()).Add(Array.Empty<PropertyChange>()).Add(0).Encode(), true);
        guest.Quiet(check, "A forged event from another peer is dropped");

        // The friend leaves; then the host closes the lobby when its game goes away.
        guest.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(lobbyId).Add(LobbyService.EntityId(GuestId)));
        check(guest.Next("lobby.result", check).Text(2) == "0", "Leave succeeds");
        var removed = host.Next("lobby.memberRemoved", check);
        check(removed.Text(1) == LobbyService.EntityId(GuestId), "The host sees the friend leave");
        guest.Lobby.FromLocal(Join("j4", GuestId, connection));
        guest.Next("lobby.result", check);
        host.Next("lobby.memberAdded", check);
        host.Lobby.LocalDisconnected();
        check(guest.Next("lobby.disconnected", check).Text(0) == lobbyId, "Friends are told when the host's lobby closes");
        guest.Lobby.FromLocal(Join("j5", GuestId, connection));
        check(guest.Next("lobby.result", check).Text(2) == LobbyService.LobbyNotFound.ToString(), "A closed lobby cannot be joined");

        PartyFlow(check);
        await KeyExchange(check);

        // The real pipe: a stand-in connects, says hello, creates a lobby and reads the answer.
        var pipeName = "hellforge-test-" + Guid.NewGuid().ToString("N");
        await using (var pipes = new PipeHost(pipeName, _ => { }))
        {
            var service = new LobbyService(null, pipes.SendLobby, _ => { }, "mods-a");
            pipes.Lobby = service;
            pipes.Start();
            await using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await client.ConnectAsync(5000);
            await client.WriteAsync(new Frame("hello").Add("lobby").Add(Environment.ProcessId).Encode());
            await client.WriteAsync(Create("p1", HostId).Encode());
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            var answer = await Frame.ReadAsync(client, timeout.Token);
            check(answer is { Type: "lobby.result" } && answer.Text(2) == "0" && answer.Text(4).EndsWith(LobbyService.EntityId(HostId)),
                "A stand-in creates a lobby over the named pipe");
        }

        // Two backends on one PC over loopback UDP (the harness transport).
        var ports = (FreePort(), FreePort());
        using var first = new UdpLoopbackTransport(HostId, ports.Item1, new Dictionary<ulong, int> { [GuestId] = ports.Item2 });
        using var second = new UdpLoopbackTransport(GuestId, ports.Item2, new Dictionary<ulong, int> { [HostId] = ports.Item1 });
        var arrived = new TaskCompletionSource<(ulong, Frame)>();
        second.Received += (from, bytes) => arrived.TrySetResult((from, Frame.Decode(bytes)));
        first.Start();
        second.Start();
        first.Send(GuestId, new Frame("ping").Add("udp").Encode(), true);
        var delivered = await Task.WhenAny(arrived.Task, Task.Delay(5000)) == arrived.Task;
        check(delivered && arrived.Task.Result.Item1 == HostId && arrived.Task.Result.Item2.Text(0) == "udp", "Loopback UDP delivers a frame with its sender");
    }

    // The Party network: only lobby members connect; endpoints get ids; messages reach the machines that own the targets.
    // Players' public keys reach friends' backends, follow a new login, and are refused when malformed.
    static async Task KeyExchange(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        (AccountKeys Keys, AccountKeyExchange Exchange, IPeerTransport Transport) Backend(ulong id)
        {
            var transport = network.Join(id);
            var keys = new AccountKeys();
            var exchange = new AccountKeyExchange(transport, keys, _ => { });
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                exchange.Heard(peer);
                if (frame.Type == "account.key") exchange.FromPeer(peer, frame);
            };
            return (keys, exchange, transport);
        }
        var host = Backend(HostId);
        var guest = Backend(GuestId);
        // The game's key text is kept verbatim (base64 in practice, so case and padding matter).
        string hostKey = new('a', 64), guestKey = "q83vEjRWeJA+/q83vEjRWeJA+/q83vEjRWeJA+/q83vE=", guestRelogin = guestKey.ToUpperInvariant();
        host.Keys.SetLocal(hostKey);
        guest.Keys.SetLocal(guestKey);
        check(host.Keys.Find(HostId) == hostKey && host.Keys.Find(GuestId) == null, "A key was known before any contact.");
        // The guest's first frame (its join) makes each side send its key.
        guest.Transport.Send(HostId, new Frame("lobby.join").Encode(), reliable: true);
        check(await host.Keys.WaitAsync(GuestId, TimeSpan.FromSeconds(5), default) == guestKey, "The host did not receive the guest's key.");
        check(await guest.Keys.WaitAsync(HostId, TimeSpan.FromSeconds(5), default) == hostKey, "The guest did not receive the host's key.");
        guest.Keys.SetLocal(guestRelogin);
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (host.Keys.Find(GuestId) != guestRelogin && DateTime.UtcNow < deadline) await Task.Delay(20);
        check(host.Keys.Find(GuestId) == guestRelogin, "A new login's key did not reach the host.");
        host.Exchange.FromPeer(GuestId, new Frame("account.key").Add("not a key"));
        check(host.Keys.Find(GuestId) == guestRelogin, "A malformed key replaced a friend's key.");
        check(await host.Keys.WaitAsync(StrangerId, TimeSpan.FromMilliseconds(200), default) == null, "An unknown player has a key.");
        var rejected = false;
        try { host.Keys.SetLocal("has spaces"); } catch (ArgumentException) { rejected = true; }
        check(rejected && host.Keys.Local == hostKey, "A malformed local key was accepted.");
    }

    static void PartyFlow(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var host = new Node(network, HostId, "mods-a");
        var guest = new Node(network, GuestId, "mods-a");
        const string Net = "b75b75b7-0001-0001-0001-000000000001";
        host.Party.FromLocal(new Frame("party.host").Add(Net).Add(4).Add(4).Add(1).Add(1).Add(8).Add(0));
        host.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("").Add(1).Add(LobbyService.EntityId(HostId)));

        guest.Party.FromLocal(new Frame("party.connect").Add("c1").Add(Net).Add(LobbyService.EntityId(HostId)));
        check(guest.NextParty("party.connected", check).Text(1) == "1", "A player outside the lobby connects to the network");

        host.Lobby.FromLocal(Create("lc", HostId));
        var connection = host.Next("lobby.result", check).Text(4);
        guest.Lobby.FromLocal(Join("lj", GuestId, connection));
        guest.Next("lobby.result", check);
        host.Next("lobby.memberAdded", check);

        guest.Party.FromLocal(new Frame("party.connect").Add("c2").Add(Net).Add(LobbyService.EntityId(HostId)));
        var connected = guest.NextParty("party.connected", check);
        check(connected.Text(0) == "c2" && connected.Text(1) == "0" && connected.Text(2) == Net && connected.Int(9) >= 4096 && connected.Text(10) == "1" &&
            connected.Text(11) == "1" && connected.Text(12) == LobbyService.EntityId(HostId), "A lobby member connects, gets an endpoint block and sees the host's endpoint");
        var block = connected.Int(9);
        check(host.NextParty("party.deviceJoined", check).Text(1) == LobbyService.EntityId(GuestId), "The host sees the friend's machine join");

        guest.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("e1").Add("").Add(LobbyService.EntityId(GuestId)));
        var mine = guest.NextParty("party.endpointCreated", check);
        check(mine.Text(1) == "e1" && mine.Text(2) == "1000" && mine.Text(3) == LobbyService.EntityId(GuestId), "The friend's endpoint gets an id from the host");
        var theirs = host.NextParty("party.endpointCreated", check);
        check(theirs.Text(1) == "" && theirs.Text(2) == "1000", "The host sees the friend's endpoint");
        guest.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("").Add(block).Add(LobbyService.EntityId(GuestId)));
        check(host.NextParty("party.endpointCreated", check).Text(2) == block.ToString(), "A friend's endpoint can take an id from its block");
        guest.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("").Add(5).Add(LobbyService.EntityId(GuestId)));
        host.PartyQuiet(check, "A friend cannot take an id outside its block");

        var payload = Enumerable.Range(0, 300).Select(i => (byte)i).ToArray();
        guest.Party.FromLocal(new Frame("party.message").Add(Net).Add(1000).Add(PartyService.GuaranteedDelivery).Add("1").Add(payload));
        var received = host.NextParty("party.message", check);
        check(received.Text(1) == "1000" && received.Bytes(4).SequenceEqual(payload), "The friend's message reaches the host intact");
        host.Party.FromLocal(new Frame("party.message").Add(Net).Add(1).Add(0).Add("").Add(new byte[] { 7, 7, 7 }));
        check(guest.NextParty("party.message", check).Bytes(4).SequenceEqual(new byte[] { 7, 7, 7 }), "The host's broadcast reaches the friend");
        host.PartyQuiet(check, "A broadcast is not echoed to its sender");
        guest.Party.FromLocal(new Frame("party.message").Add(Net).Add(1).Add(0).Add("").Add(new byte[] { 1 }));
        host.PartyQuiet(check, "A message from someone else's endpoint is dropped");

        guest.Party.FromLocal(new Frame("party.leave").Add(Net));
        check(host.NextParty("party.endpointDestroyed", check).Text(1) == "1000" && host.NextParty("party.endpointDestroyed", check).Text(1) == block.ToString() &&
            host.NextParty("party.deviceLeft", check) is var left && left.Text(1) == LobbyService.EntityId(GuestId) && left.Text(2) == "left",
            "The host sees the friend leave on purpose");

        guest.Party.FromLocal(new Frame("party.connect").Add("c3").Add(Net).Add(LobbyService.EntityId(HostId)));
        guest.NextParty("party.connected", check);
        host.NextParty("party.deviceJoined", check);
        // The friend's game crashes and comes back: it connects again while still in the network; its earlier endpoints go
        // first, so the new game can number its own from the same block.
        guest.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("").Add(block).Add(LobbyService.EntityId(GuestId)));
        host.NextParty("party.endpointCreated", check);
        guest.Party.FromLocal(new Frame("party.connect").Add("c3b").Add(Net).Add(LobbyService.EntityId(HostId)));
        check(guest.NextParty("party.connected", check).Text(1) == "0", "A friend still in the network connects again");
        check(host.NextParty("party.endpointDestroyed", check).Text(1) == block.ToString() && host.NextParty("party.deviceLeft", check).Text(1) == LobbyService.EntityId(GuestId) &&
            host.NextParty("party.deviceJoined", check).Text(1) == LobbyService.EntityId(GuestId), "Its earlier endpoints and machine leave, and it joins again");
        guest.Party.FromLocal(new Frame("party.endpoint").Add(Net).Add("").Add(block).Add(LobbyService.EntityId(GuestId)));
        check(host.NextParty("party.endpointCreated", check).Text(2) == block.ToString(), "The returning game takes its block's ids again");
        guest.Party.FromLocal(new Frame("party.endpointGone").Add(Net).Add(block));
        host.NextParty("party.endpointDestroyed", check);
        host.Party.PeerLost(GuestId);
        check(host.NextParty("party.deviceLeft", check) is var lost && lost.Text(1) == LobbyService.EntityId(GuestId) && lost.Text(2) == "lost", "A silent friend is lost from the network");
        guest.Party.PeerLost(HostId);
        check(guest.NextParty("party.networkGone", check).Text(0) == Net, "A silent host's network is gone for the friend");

        guest.Party.FromLocal(new Frame("party.connect").Add("c4").Add(Net).Add(LobbyService.EntityId(HostId)));
        guest.NextParty("party.connected", check);
        host.NextParty("party.deviceJoined", check);
        host.Party.LocalDisconnected();
        // The host's machine leaves on purpose before its network goes (HD2 will not rejoin a machine that "disconnected").
        check(guest.NextParty("party.deviceLeft", check) is var hostLeft && hostLeft.Text(1) == LobbyService.EntityId(HostId) && hostLeft.Text(2) == "left",
            "The host's machine leaves on purpose");
        check(guest.NextParty("party.networkGone", check).Text(0) == Net, "The network ends for friends when the host's game closes");
    }

    static int FreePort()
    {
        using var probe = new System.Net.Sockets.UdpClient(new System.Net.IPEndPoint(System.Net.IPAddress.Loopback, 0));
        return ((System.Net.IPEndPoint)probe.Client.LocalEndPoint!).Port;
    }
}
