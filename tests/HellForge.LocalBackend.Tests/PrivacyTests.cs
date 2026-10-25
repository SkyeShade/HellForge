using System.Collections.Concurrent;
using HellForge.LocalBackend.Multiplayer;

// Who may join a lobby (the game's privacy setting: Public, Friends only, Invite only; invitations; kicked players), the
// kick itself with its Party side, invitations between friends, the limits on other machines' lobby frames, and the
// stranger gate's rate limits and memory. Backends over the in-memory network, each with a fake stand-in DLL.
static class PrivacyTests
{
    const ulong HostId = 0x110000100000011, FriendId = 0x110000100000012, StrangerId = 0x110000100000013, InvitedId = 0x110000100000014,
        ThirdId = 0x110000100000015;

    sealed class Node
    {
        public readonly BlockingCollection<Frame> Inbox = new(), PartyInbox = new();
        public readonly LobbyService Lobby;
        public readonly PartyService Party;
        public readonly ConcurrentQueue<ulong> Kicked = new(), KickedByHost = new();
        public readonly BlockingCollection<(ulong Peer, IReadOnlyList<string> Lines)> Refused = new(), RefusedFriend = new();

        public Node(InMemoryNetwork network, ulong id)
        {
            var transport = network.Join(id);
            Lobby = new LobbyService(transport, Inbox.Add, _ => { }, "mods");
            Party = new PartyService(transport, PartyInbox.Add, _ => { }, Lobby.InHostedLobby) { Kick = Lobby.Kick };
            Lobby.Kicked += node => { Kicked.Enqueue(node); Party.Expel(node); };
            Lobby.KickedByHost += peer => KickedByHost.Enqueue(peer);
            Lobby.Refused += (peer, lines) => Refused.Add((peer, lines));
            Lobby.RefusedFriend += (peer, lines) => RefusedFriend.Add((peer, lines));
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("party.", StringComparison.Ordinal)) Party.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
        }

        public Frame Next(string type, Action<bool, string> check, string? what = null) => Take(Inbox, type, check, what);
        public Frame NextParty(string type, Action<bool, string> check) => Take(PartyInbox, type, check, null);

        static Frame Take(BlockingCollection<Frame> inbox, string type, Action<bool, string> check, string? what)
        {
            var got = inbox.TryTake(out var frame, TimeSpan.FromSeconds(5));
            check(got && frame!.Type == type, (what != null ? what + ": " : "") + $"expected {type}, got {(got ? frame!.Type + "[" + string.Join(",", Enumerable.Range(0, Math.Min(frame.Count, 4)).Select(frame.Text)) + "]" : "nothing")}");
            return frame!;
        }

        public void Quiet(Action<bool, string> check, string what)
        {
            Thread.Sleep(150);
            check(Inbox.Count == 0, what + $" (got {(Inbox.TryTake(out var extra) ? extra.Type : "")})");
        }

        public void Drain() { while (Inbox.TryTake(out _)) { } while (PartyInbox.TryTake(out _)) { } }
    }

    static Frame Create(string op, ulong creator, string? privacy) => new Frame("lobby.create").Add(op).Add(LobbyService.EntityId(creator)).Add("title_player_account")
        .Add(8).Add(0).Add(0).Add(0)
        .Add(privacy == null ? new Dictionary<string, string> { ["1"] = "f8c9037f96047f7a" } : new Dictionary<string, string> { ["1"] = "f8c9037f96047f7a", ["19"] = privacy })
        .Add(privacy == null ? new Dictionary<string, string> { ["string_key1"] = "f8c9037f96047f7a" }
            : new Dictionary<string, string> { ["string_key1"] = "f8c9037f96047f7a", ["number_key6"] = privacy })
        .Add(new Dictionary<string, string> { ["crossplay_mode"] = "0" });

    static Frame Join(string op, ulong member, string connection, IReadOnlyDictionary<string, string>? props = null) => new Frame("lobby.join").Add(op)
        .Add(LobbyService.EntityId(member)).Add("title_player_account").Add(connection).Add(props ?? new Dictionary<string, string> { ["platform_lobby"] = "2:1::" });

    internal static Frame Privacy(string op, string lobby, ulong owner, string value) =>
        new Frame("lobby.update").Add(op).Add(lobby).Add(LobbyService.EntityId(owner)).Add(0).Add("").Add(0).Add(0).Add(0).Add(0).Add(0).Add(0)
            .Add(new List<PropertyChange> { new("19", value) }).Add(new List<PropertyChange> { new("number_key6", value) }).Add(0).Add(Array.Empty<PropertyChange>());

    static Frame Kick(string op, string lobby, ulong member) => new Frame("lobby.kick").Add(op).Add(lobby).Add(LobbyService.EntityId(member)).Add(1);
    static Frame Invite(string op, string lobby, ulong sender, ulong invitee) =>
        new Frame("lobby.invite").Add(op).Add(lobby).Add(LobbyService.EntityId(sender)).Add(LobbyService.EntityId(invitee));

    // The join's answer: 0 in, else the HRESULT.
    static string Joined(Node joiner, string op, string connection, Action<bool, string> check, IReadOnlyDictionary<string, string>? props = null)
    {
        joiner.Lobby.FromLocal(Join(op, IdOf(joiner), connection, props));
        var result = joiner.Next("lobby.result", check, "join " + op);
        return result.Text(2);
    }

    static readonly Dictionary<Node, ulong> ids = [];
    static ulong IdOf(Node node) => ids[node];

    public static async Task Run(Action<bool, string> check)
    {
        Values(check);
        Matrix(check);
        KickAndInvite(check);
        Limits(check);
        Gate(check);
        await Task.CompletedTask;
    }

    static (InMemoryNetwork Network, Node Host, Node Friend, Node Stranger, Node Invited, Node Third) Machines()
    {
        var network = new InMemoryNetwork();
        // Strangers are no one's Steam friends: the stranger and the invited player (someone met in a public game).
        network.Strangers.Add(StrangerId);
        network.Strangers.Add(InvitedId);
        Node Make(ulong id) { var node = new Node(network, id); ids[node] = id; return node; }
        return (network, Make(HostId), Make(FriendId), Make(StrangerId), Make(InvitedId), Make(ThirdId));
    }

    static (string Id, string Connection) Host(Node host, string op, string? privacy, Action<bool, string> check)
    {
        host.Lobby.FromLocal(Create(op, IdOf(host), privacy));
        var created = host.Next("lobby.result", check, "create");
        return (created.Text(3), created.Text(4));
    }

    static void Values(Action<bool, string> check)
    {
        var none = new Dictionary<string, string>();
        check(LobbyService.PrivacyOf(new Dictionary<string, string> { ["number_key6"] = "0" }, none) == LobbyService.Privacy.Public &&
            LobbyService.PrivacyOf(none, new Dictionary<string, string> { ["19"] = "2" }) == LobbyService.Privacy.InviteOnly &&
            LobbyService.PrivacyOf(new Dictionary<string, string> { ["number_key6"] = "1" }, new Dictionary<string, string> { ["19"] = "0" }) == LobbyService.Privacy.FriendsOnly &&
            LobbyService.PrivacyOf(none, none) == LobbyService.Privacy.FriendsOnly &&
            LobbyService.PrivacyOf(new Dictionary<string, string> { ["number_key6"] = "7" }, none) == LobbyService.Privacy.FriendsOnly,
            "Privacy values: number_key6, else \"19\"; unknown is Friends only.");
        check(LobbyService.SearchValue("string_key1", "x") && LobbyService.SearchValue("number_key30", "-1.5") && !LobbyService.SearchValue("number_key6", "zero") &&
            !LobbyService.SearchValue("string_key31", "x") && !LobbyService.SearchValue("string_key0", "x") && !LobbyService.SearchValue("string_key01", "x") &&
            !LobbyService.SearchValue("string_key2", new string('x', 257)) && !LobbyService.SearchValue("number_key1", "NaN") && !LobbyService.SearchValue("other", "1"),
            "Search properties as PlayFab takes them.");
    }

    // Public / Friends only / Invite only, each against a Steam friend, a stranger and an invited stranger.
    static void Matrix(Action<bool, string> check)
    {
        var expected = new Dictionary<string, (bool Friend, bool Stranger, bool Invited)>
        {
            ["0"] = (true, true, true), ["1"] = (true, false, true), ["2"] = (false, false, true), ["none"] = (true, false, true)
        };
        foreach (var (privacy, allowed) in expected)
        {
            var (_, host, friend, stranger, invited, _) = Machines();
            var (lobbyId, connection) = Host(host, "c-" + privacy, privacy == "none" ? null : privacy, check);
            host.Lobby.FromLocal(Invite("i-" + privacy, lobbyId, HostId, InvitedId));
            check(host.Next("lobby.result", check, "invite").Text(2) == "0", "The host invites someone.");
            var friendIn = Joined(friend, "f-" + privacy, connection, check) == "0";
            var strangerIn = Joined(stranger, "s-" + privacy, connection, check) == "0";
            var invitedIn = Joined(invited, "v-" + privacy, connection, check) == "0";
            check((friendIn, strangerIn, invitedIn) == allowed,
                $"Privacy {privacy}: friend {friendIn}, stranger {strangerIn}, invited {invitedIn} (expected {allowed}).");
            if (!strangerIn) check(stranger.Refused.TryTake(out var told, TimeSpan.FromSeconds(2)) && told.Lines[0].Contains(privacy == "2" ? "Invite only" : "Friends only"),
                $"Privacy {privacy}: the stranger is told why.");
            if (!friendIn)
            {
                check(friend.Refused.TryTake(out var told, TimeSpan.FromSeconds(2)) && told.Lines[0] == "The host's lobby is Invite only.", "A friend refused by Invite only is told.");
                check(host.RefusedFriend.TryTake(out var hostTold, TimeSpan.FromSeconds(2)) && hostTold.Peer == FriendId && hostTold.Lines[0].Contains("invite them"),
                    "The host hears that a friend could not join an Invite only lobby.");
            }
        }

        // Privacy changes mid-session: members stay; a new stranger is refused; someone admitted before may come back.
        {
            var (_, host, friend, stranger, _, _) = Machines();
            var (lobbyId, connection) = Host(host, "c", "0", check);
            check(Joined(stranger, "s1", connection, check) == "0", "A stranger joins the Public lobby.");
            host.Drain();
            host.Lobby.FromLocal(Privacy("p", lobbyId, HostId, "2"));
            host.Next("lobby.result", check);
            check(host.Lobby.HostedLobbies().Single() is { Members: 2, Privacy: LobbyService.Privacy.InviteOnly }, "Turned Invite only, the stranger stays a member.");
            check(Joined(friend, "f1", connection, check) != "0", "A friend not invited is refused once the lobby is Invite only.");
            stranger.Drain();
            stranger.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(lobbyId).Add(""));
            stranger.Next("lobby.result", check);
            Thread.Sleep(100);
            check(host.Lobby.Welcome(StrangerId) && Joined(stranger, "s2", connection, check) == "0", "A member who left (a crash) may come back whatever the privacy.");
        }
    }

    static void KickAndInvite(Action<bool, string> check)
    {
        var (_, host, friend, stranger, invited, third) = Machines();
        var (lobbyId, connection) = Host(host, "c", "0", check);
        check(Joined(friend, "f1", connection, check) == "0" && Joined(third, "t1", connection, check) == "0", "Two friends join.");
        // The host's Party network, which the friend connects to.
        host.Party.FromLocal(new Frame("party.host").Add("net1").Add(8).Add(8).Add(1).Add(1).Add(8).Add(0));
        friend.Party.FromLocal(new Frame("party.connect").Add("pc1").Add("net1").Add(LobbyService.EntityId(HostId)));
        check(friend.NextParty("party.connected", check).Text(1) == "0", "The friend connects to the host's Party network.");
        friend.Party.FromLocal(new Frame("party.endpoint").Add("net1").Add("pe1").Add("").Add(LobbyService.EntityId(FriendId)));
        friend.NextParty("party.endpointCreated", check);
        host.Drain();
        friend.Drain();
        third.Drain();

        // Only the owner kicks, from its own machine, and never itself.
        third.Lobby.FromLocal(Kick("k0", lobbyId, FriendId));
        check(third.Next("lobby.result", check).Text(2) == LobbyService.AccessDenied.ToString(), "A member who is not the owner cannot kick.");
        host.Lobby.FromLocal(Kick("k1", lobbyId, HostId));
        check(host.Next("lobby.result", check).Text(2) == LobbyService.InvalidArgument.ToString(), "The owner cannot kick itself.");

        // The kick: the kicked game hears it was removed by the owner and the lobby is gone; the others that a member left;
        // the host's DLL gets the removal before the completion.
        host.Lobby.FromLocal(Kick("k2", lobbyId, FriendId));
        var removed = host.Next("lobby.memberRemoved", check, "host");
        check(removed.Text(1) == LobbyService.EntityId(FriendId) && removed.Text(2) == "2", "The host's game sees the member leave.");
        var done = host.Next("lobby.result", check, "kick result");
        check(done.Text(0) == "k2" && done.Text(1) == "kick" && done.Text(2) == "0", "The kick completes after the removal.");
        var gone = friend.Next("lobby.memberRemoved", check, "kicked");
        check(gone.Text(1) == LobbyService.EntityId(FriendId) && gone.Text(2) == "1", "The kicked game hears it was removed by the owner.");
        check(friend.Next("lobby.disconnected", check, "kicked").Text(1) == "0", "...and that the lobby is gone for it (no local members).");
        check(third.Next("lobby.memberRemoved", check, "third").Text(2) == "2", "The other member sees the kicked one leave.");
        Thread.Sleep(100);
        check(host.Kicked.TryDequeue(out var kicked) && kicked == FriendId && friend.KickedByHost.TryDequeue(out var by) && by == HostId,
            "Both backends know of the kick (the game's notice).");
        check(friend.NextParty("party.networkGone", check).Text(0) == "net1", "The kicked machine is out of the Party network.");
        check(!host.Lobby.InHostedLobby(FriendId) && host.Lobby.HostedLobbies().Single().Members == 2, "The kicked machine is no member.");

        // Its Party traffic is dropped, it cannot connect again, and it cannot rejoin, invited or not.
        host.Drain();
        friend.Party.FromLocal(new Frame("party.connect").Add("pc2").Add("net1").Add(LobbyService.EntityId(HostId)));
        check(friend.NextParty("party.connected", check).Text(1) == "1", "A kicked machine cannot connect to the host's network again.");
        host.Party.FromPeer(FriendId, new Frame("party.message").Add("net1").Add(4096).Add(1).Add("").Add("hello"));
        Thread.Sleep(100);
        check(host.PartyInbox.Count == 0, "A kicked machine's game traffic is dropped.");
        check(Joined(friend, "f2", connection, check) == LobbyService.LobbyNotJoinable.ToString() &&
            friend.Refused.TryTake(out var told, TimeSpan.FromSeconds(2)) && told.Lines[0] == "The host removed you from this lobby.", "A kicked machine cannot rejoin.");
        host.Lobby.FromLocal(Invite("i0", lobbyId, HostId, FriendId));
        check(host.Next("lobby.result", check).Text(2) == LobbyService.AccessDenied.ToString(), "A kicked player cannot be invited back.");
        check(!host.Lobby.Welcome(FriendId), "A kicked machine is not expected back.");

        // The game's Party kick of a member's device kicks it from the lobby too.
        host.Drain();
        host.Party.FromLocal(new Frame("party.kick").Add("net1").Add(LobbyService.EntityId(ThirdId)));
        check(host.Next("lobby.memberRemoved", check, "party kick").Text(1) == LobbyService.EntityId(ThirdId) && third.Next("lobby.memberRemoved", check).Text(2) == "1",
            "A Party kick kicks the machine from the host's lobby.");

        // Invitations: a member's goes to the invitee's backend through the inviting player's (friends only), and its game is
        // told; a stranger's backend drops one; a lobby a friend owns records it there.
        third.Next("lobby.disconnected", check, "party-kicked");
        Thread.Sleep(100);
        third.Drain();
        var (lobby2, connection2) = Host(third, "c2", "2", check);
        third.Drain();
        third.Lobby.FromLocal(Invite("i1", lobby2, ThirdId, HostId));
        var taken = third.Next("lobby.result", check);
        check(taken.Text(2) == "0", "An invitation is taken: " + string.Join(",", Enumerable.Range(0, taken.Count).Select(taken.Text)));
        var received = host.Next("lobby.inviteReceived", check, "invitee");
        check(received.Text(0) == lobby2 && received.Text(1) == connection2 && received.Text(2) == LobbyService.EntityId(ThirdId), "The invited friend's game is told.");
        third.Lobby.FromLocal(Invite("i2", lobby2, ThirdId, InvitedId));
        check(third.Next("lobby.result", check).Text(2) == "0", "Inviting a stranger is recorded.");
        invited.Quiet(check, "A stranger's backend drops an invitation");
        check(Joined(invited, "v1", connection2, check) == "0", "The invited stranger joins the Invite only lobby.");
        // the invited stranger, now a member, invites a friend of the host's into the host's lobby: its backend forwards it
        check(Joined(host, "h1", connection2, check) == "0", "The invited friend joins.");
        host.Drain();
        host.Lobby.FromLocal(Invite("i3", lobby2, HostId, FriendId));
        check(host.Next("lobby.result", check).Text(2) == "0", "A member invites through the owner.");
        check(friend.Next("lobby.inviteReceived", check, "forwarded invite").Text(0) == lobby2, "The member's invitation reaches the invitee.");
        check(Joined(friend, "f3", connection2, check) == "0", "The friend invited by a member joins.");
        _ = stranger;
    }

    // Frames from other machines keep to PlayFab's limits.
    static void Limits(Action<bool, string> check)
    {
        var (_, host, friend, _, _, _) = Machines();
        var (lobbyId, connection) = Host(host, "c", "1", check);
        var many = Enumerable.Range(0, LobbyService.MaxProperties + 1).ToDictionary(i => "k" + i, _ => "v");
        check(Joined(friend, "j1", connection, check, many) == LobbyService.InvalidArgument.ToString(), "A join with too many member properties is refused.");
        check(Joined(friend, "j2", connection, check, new Dictionary<string, string> { ["k"] = new string('x', LobbyService.MaxValueLength + 1) }) ==
            LobbyService.InvalidArgument.ToString(), "A join with an oversized member property is refused.");
        check(Joined(friend, "j3", connection, check) == "0", "A normal join is taken.");
        var update = new Frame("lobby.update").Add("u1").Add(lobbyId).Add(LobbyService.EntityId(FriendId)).Add(0).Add("").Add(0).Add(0).Add(0).Add(0).Add(0).Add(0)
            .Add(Array.Empty<PropertyChange>()).Add(Array.Empty<PropertyChange>()).Add(1)
            .Add(Enumerable.Range(0, LobbyService.MaxProperties + 1).Select(i => new PropertyChange("m" + i, "v")).ToList());
        friend.Lobby.FromLocal(update);
        check(friend.Next("lobby.result", check).Text(2) == LobbyService.InvalidArgument.ToString(), "A member update past the limit is refused.");
    }

    // The stranger gate: joins are limited also once a stranger is in a lobby here; its tables forget quiet machines.
    static void Gate(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        network.Strangers.Add(StrangerId);
        var transport = network.Join(HostId);
        var related = false;
        var welcome = new HashSet<ulong>();
        var gate = new StrangerGate(transport, () => false, _ => related, () => true, _ => { }, peer => welcome.Contains(peer));
        var join = new Frame("lobby.join").Add("op");
        var now = DateTime.UtcNow;
        check(!gate.Admit(StrangerId, join, now), "No join without a listing.");
        welcome.Add(StrangerId);
        check(gate.Admit(StrangerId, join, now), "A join from a machine a lobby here expects back.");
        welcome.Clear();
        related = true;
        var admitted = Enumerable.Range(0, 10).Count(_ => gate.Admit(StrangerId, join, now));
        check(admitted == StrangerGate.JoinsPerPeer - 1, $"A related stranger's joins are limited too ({admitted}).");
        check(gate.Admit(StrangerId, new Frame("party.message"), now) && !gate.Admit(StrangerId, new Frame("lobby.invited"), now), "Related: game traffic, never invitations.");
        check(gate.Remembered > 0, "The gate remembers the stranger for now.");
        related = false;
        gate.Admit(StrangerId, new Frame("peer.ping"), now + TimeSpan.FromMinutes(3));
        check(gate.Remembered == 0, "The gate forgets a quiet stranger.");
    }
}
