using System.Collections.Concurrent;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// Rejoining by itself after a mod sync (AutoRejoin, LobbyService's lobby.rejoin): the rejoin file's checks, the host
// keeping a machine refused for its mods expected back (whatever the privacy then, never when kicked, not for ever), the
// host's answers, and the joiner's whole path over the in-memory network: its game on the ship, the ask, the invitation
// its game receives, the join; and each way it gives up once. The joiner's clock is a test clock.
static class RejoinTests
{
    const ulong HostId = 0x110000100000031, FriendId = 0x110000100000032, StrangerId = 0x110000100000033, ThirdId = 0x110000100000034;
    const string HostMods = "host-mods", OwnMods = "own-mods";

    sealed class Node
    {
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly LobbyService Lobby;
        public readonly ulong Id;
        public readonly IPeerTransport Transport;
        public readonly BlockingCollection<(ulong Peer, IReadOnlyList<string> Lines)> Refused = new();

        public Node(InMemoryNetwork network, ulong id, string digest)
        {
            Id = id;
            var transport = Transport = network.Join(id);
            Lobby = new LobbyService(transport, Inbox.Add, _ => { }, digest);
            Lobby.Refused += (peer, lines) => Refused.Add((peer, lines));
            transport.Received += (peer, bytes) => Lobby.FromPeer(peer, Frame.Decode(bytes));
        }

        public Frame Next(string type, Action<bool, string> check, string what)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
            while (Inbox.TryTake(out var frame, deadline - DateTime.UtcNow > TimeSpan.Zero ? deadline - DateTime.UtcNow : TimeSpan.Zero))
                if (frame.Type == type) return frame;
            check(false, $"{what}: expected {type}, got nothing");
            return new Frame(type);
        }

        public void Drain() { while (Inbox.TryTake(out _)) { } }

        // The game creates its own lobby (on the ship); its id and connection string.
        public (string Id, string Connection) Create(string op, string? privacy, Action<bool, string> check, uint max = 4)
        {
            var props = new Dictionary<string, string> { ["1"] = "f8c9037f96047f7a" };
            var search = new Dictionary<string, string> { ["string_key1"] = "f8c9037f96047f7a" };
            if (privacy != null) { props["19"] = privacy; search["number_key6"] = privacy; }
            Lobby.FromLocal(new Frame("lobby.create").Add(op).Add(LobbyService.EntityId(Id)).Add("title_player_account").Add(max).Add(0).Add(0).Add(0)
                .Add(props).Add(search).Add(new Dictionary<string, string> { ["crossplay_mode"] = "0" }));
            var created = Next("lobby.result", check, "create " + op);
            return (created.Text(3), created.Text(4));
        }

        // The game joins a lobby with its connection string: the answer, 0 in, else the HRESULT.
        public string Join(string op, string connection, Action<bool, string> check)
        {
            Lobby.FromLocal(new Frame("lobby.join").Add(op).Add(LobbyService.EntityId(Id)).Add("title_player_account").Add(connection)
                .Add(new Dictionary<string, string> { ["platform_lobby"] = "2:1::" }));
            return Next("lobby.result", check, "join " + op).Text(2);
        }
    }

    // The joiner's next session: its lobby service with the host's mods, and the rejoin on a test clock.
    sealed class Returning
    {
        public readonly Node Node;
        public readonly AutoRejoin Rejoin;
        public readonly ConcurrentQueue<(string Kind, string Title, IReadOnlyList<string> Lines)> Notices = new();
        public readonly ConcurrentQueue<Frame> Sent = new();
        public DateTime Now = new(2026, 10, 23, 12, 0, 0, DateTimeKind.Utc);

        public Returning(InMemoryNetwork network, ulong id, string lobbyId, string digest = HostMods, ulong host = HostId)
        {
            Node = new Node(network, id, digest);
            var transport = Node.Transport;
            Rejoin = new AutoRejoin(new AutoRejoin.Target(host, "Host", lobbyId, HostMods), digest,
                (peer, frame) => { Sent.Enqueue(frame); transport.Send(peer, frame.Encode(), reliable: true); }, Node.Lobby.InviteLocal,
                (kind, title, lines) => Notices.Enqueue((kind, title, lines)), _ => { }, () => Now);
            Rejoin.Attach(Node.Lobby);
        }

        public void Advance(TimeSpan time) { Now += time; Rejoin.Tick(); }

        // The game reaches the ship (creates its lobby), then the rejoin asks the host once Settle has passed.
        public void OnShip(Action<bool, string> check)
        {
            Node.Create("own", "1", check);
            Wait(() => Rejoin.Current == AutoRejoin.Stage.Settling, check, "The game on the ship starts the rejoin");
            Advance(AutoRejoin.Settle - TimeSpan.FromSeconds(1));
            check(Rejoin.Current == AutoRejoin.Stage.Settling && Sent.IsEmpty, "The rejoin waits for the ship's screens.");
            Advance(TimeSpan.FromSeconds(2));
            check(Rejoin.Current == AutoRejoin.Stage.Asking && Sent.TryPeek(out var ask) && ask.Type == "lobby.rejoin", "Then it asks the host.");
        }

        public (string Kind, string Title, IReadOnlyList<string> Lines) Notice(Action<bool, string> check, string what)
        {
            Wait(() => !Notices.IsEmpty, check, what);
            Notices.TryDequeue(out var notice);
            return notice;
        }
    }

    static void Wait(Func<bool> condition, Action<bool, string> check, string what)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (!condition() && DateTime.UtcNow < deadline) Thread.Sleep(10);
        check(condition(), what);
    }

    public static async Task Run(string root, Action<bool, string> check)
    {
        ReadFile(Path.Combine(root, "rejoin-files"), check);
        ExpectedBack(check);
        Answers(check);
        Rejoined(check);
        GivingUp(check);
        await Task.CompletedTask;
    }

    // The rejoin file: the host and lobby from its connection string, only while fresh, only when this session plays the
    // host's mods (its digest is the host's), never this player's own lobby.
    static void ReadFile(string folder, Action<bool, string> check)
    {
        Directory.CreateDirectory(folder);
        var now = DateTime.UtcNow;
        string Write(string name, string host, string connection, string digest)
        {
            var path = Path.Combine(folder, name);
            new ModSyncSession(host, "Host", connection, digest, []).Write(path);
            return path;
        }
        var lobby = Guid.NewGuid().ToString();
        var good = Write("good.json", LobbyService.EntityId(HostId), $"hellforge1-lobby;{lobby};{LobbyService.EntityId(HostId)}", HostMods);
        var (target, reason) = AutoRejoin.Read(good, FriendId, HostMods, now);
        check(target is { Host: HostId, HostName: "Host", Digest: HostMods } && target.LobbyId == lobby && reason == null, "A fresh rejoin file names the host and lobby: " + reason);
        check(AutoRejoin.Read(good, FriendId, OwnMods, now).Reason?.Contains("its own mods") == true, "No rejoin when the session plays its own mods (the host's were not all found).");
        check(AutoRejoin.Read(good, HostId, HostMods, now).Target == null, "No rejoin into this player's own lobby.");
        check(AutoRejoin.Read(good, FriendId, HostMods, now + AutoRejoin.MaxAge + TimeSpan.FromMinutes(1)).Reason?.Contains("older") == true, "No rejoin from an old mod sync.");
        var other = Write("other.json", LobbyService.EntityId(HostId), $"hellforge1-lobby;{lobby};{LobbyService.EntityId(ThirdId)}", HostMods);
        check(AutoRejoin.Read(other, FriendId, HostMods, now).Target == null, "No rejoin when the connection string names another machine than the host.");
        var bad = Write("bad.json", LobbyService.EntityId(HostId), "vanilla-connection-string", HostMods);
        check(AutoRejoin.Read(bad, FriendId, HostMods, now).Target == null, "No rejoin into a lobby that is not HellForge's.");
        check(AutoRejoin.Read(Path.Combine(folder, "missing.json"), FriendId, HostMods, now).Target == null, "No rejoin without the file.");
        File.WriteAllText(Path.Combine(folder, "broken.json"), "{");
        check(AutoRejoin.Read(Path.Combine(folder, "broken.json"), FriendId, HostMods, now).Target == null, "No rejoin from a broken file.");
    }

    // A machine refused for its gameplay mods is expected back with the host's: invited players stay invited, a stranger
    // refused from a Public lobby gets back in after it turned Friends only, never after a kick, and not after ExpectedBack.
    static void ExpectedBack(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        network.Strangers.Add(StrangerId);
        var clock = DateTime.UtcNow;
        var host = new Node(network, HostId, HostMods);
        host.Lobby.Clock = () => clock;

        // Invite only: the invited friend refused for its mods is still invited, and comes back with the host's.
        var (inviteOnly, inviteConnection) = host.Create("c1", "2", check);
        host.Lobby.FromLocal(new Frame("lobby.invite").Add("i1").Add(inviteOnly).Add(LobbyService.EntityId(HostId)).Add(LobbyService.EntityId(FriendId)));
        check(host.Next("lobby.result", check, "invite").Text(2) == "0", "The host invites the friend.");
        var friend = new Node(network, FriendId, OwnMods);
        check(friend.Join("f1", inviteConnection, check) == LobbyService.LobbyNotJoinable.ToString() && friend.Refused.TryTake(out var told, TimeSpan.FromSeconds(2)) &&
            told.Lines[0].Contains("gameplay mods"), "The invited friend is refused for its mods.");
        var back = new Node(network, FriendId, HostMods);
        check(back.Join("f2", inviteConnection, check) == "0", "With the host's mods it joins the Invite only lobby: still invited.");
        host.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(inviteOnly).Add(""));
        host.Drain();

        // Public, then Friends only: the stranger refused for its mods (it passed Public) comes back after the change.
        var (open, openConnection) = host.Create("c2", "0", check);
        var stranger = new Node(network, StrangerId, OwnMods);
        check(stranger.Join("s1", openConnection, check) == LobbyService.LobbyNotJoinable.ToString(), "A stranger with other mods is refused from the Public lobby.");
        check(host.Lobby.Welcome(StrangerId), "The host expects it back (its Steam session and join pass the stranger gate).");
        host.Lobby.FromLocal(PrivacyTests.Privacy("p1", open, HostId, "1"));
        host.Next("lobby.result", check, "privacy");
        var third = new Node(network, ThirdId, HostMods);
        network.Strangers.Add(ThirdId);
        check(third.Join("t1", openConnection, check) != "0", "A new stranger is refused once the lobby is Friends only.");
        var strangerBack = new Node(network, StrangerId, HostMods);
        check(strangerBack.Join("s2", openConnection, check) == "0", "The stranger back with the host's mods is let in although the lobby turned Friends only.");

        // Not for ever: a stranger refused for its mods is no longer expected after ExpectedBack.
        var (later, laterConnection) = host.Create("c3", "0", check);
        var late = new Node(network, ThirdId, OwnMods);
        check(late.Join("t2", laterConnection, check) != "0", "Another stranger is refused for its mods.");
        host.Lobby.FromLocal(PrivacyTests.Privacy("p2", later, HostId, "1"));
        host.Next("lobby.result", check, "privacy 2");
        clock += LobbyService.ExpectedBack + TimeSpan.FromMinutes(1);
        check(!host.Lobby.Welcome(ThirdId), "After ExpectedBack the host no longer expects it.");
        var lateBack = new Node(network, ThirdId, HostMods);
        check(lateBack.Join("t3", laterConnection, check) != "0" && lateBack.Refused.TryTake(out var lateTold, TimeSpan.FromSeconds(2)) &&
            lateTold.Lines[0].Contains("Friends only"), "Too late, it meets the lobby's privacy.");
        clock -= LobbyService.ExpectedBack + TimeSpan.FromMinutes(1);

        // A kicked machine is never expected back.
        host.Drain();
        host.Lobby.FromLocal(new Frame("lobby.kick").Add("k1").Add(open).Add(LobbyService.EntityId(StrangerId)).Add(1));
        host.Next("lobby.result", check, "kick");
        check(!host.Lobby.Welcome(StrangerId), "A kicked machine is not expected back.");
        _ = back;
    }

    // The host's answers to lobby.rejoin: ok (the lobby asked, or the host's lobby now), or why not.
    static void Answers(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        network.Strangers.Add(StrangerId);
        var host = new Node(network, HostId, HostMods);
        var asker = network.Join(FriendId);
        var answers = new BlockingCollection<Frame>();
        asker.Received += (_, bytes) => { var frame = Frame.Decode(bytes); if (frame.Type == "lobby.rejoined") answers.Add(frame); };
        Frame Ask(string lobbyId, string digest = HostMods, string protocol = LobbyService.Protocol)
        {
            asker.Send(HostId, new Frame("lobby.rejoin").Add(lobbyId).Add(digest).Add(protocol).Encode(), reliable: true);
            check(answers.TryTake(out var answer, TimeSpan.FromSeconds(5)) && answer.Text(0) == lobbyId, "The host answers a rejoin ask.");
            return answer ?? new Frame("none").Add("").Add("");
        }

        var (lobby, connection) = host.Create("c1", "1", check, max: 2);
        var ok = Ask(lobby);
        check(ok.Text(1) == "ok" && ok.Text(2) == lobby && ok.Text(3) == connection, "A friend may rejoin the Friends only lobby it asked for.");
        check(Ask(lobby, digest: OwnMods).Text(1) == "mods", "The host's mods changed: \"mods\".");
        check(Ask(lobby, protocol: "hellforge-mp-1").Text(1) == "version", "Another HellForge: \"version\".");
        var third = new Node(network, ThirdId, HostMods);
        check(third.Join("t1", connection, check) == "0", "A third player fills the lobby of two.");
        check(Ask(lobby).Text(1) == "full", "A full lobby: \"full\".");
        host.Lobby.FromLocal(new Frame("lobby.update").Add("u1").Add(lobby).Add(LobbyService.EntityId(HostId)).Add(0).Add("").Add(0).Add(0).Add(0).Add(0).Add(1).Add(1)
            .Add(Array.Empty<PropertyChange>()).Add(Array.Empty<PropertyChange>()).Add(0).Add(Array.Empty<PropertyChange>()));
        host.Next("lobby.result", check, "lock");
        check(Ask(lobby).Text(1) == "locked", "A locked lobby: \"locked\".");

        // The host left that lobby and hosts a new one: the answer names it.
        host.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(lobby).Add(""));
        host.Drain();
        check(Ask(lobby).Text(1) == "gone", "No lobby hosted here: \"gone\".");
        var (moved, movedConnection) = host.Create("c2", "1", check);
        var follow = Ask(lobby);
        check(follow.Text(1) == "ok" && follow.Text(2) == moved && follow.Text(3) == movedConnection, "The host's new lobby is named when the one asked has closed.");

        // A stranger: the new lobby is Friends only, and it was never expected there.
        var stranger = network.Join(StrangerId);
        var strangerAnswers = new BlockingCollection<Frame>();
        stranger.Received += (_, bytes) => strangerAnswers.Add(Frame.Decode(bytes));
        stranger.Send(HostId, new Frame("lobby.rejoin").Add(lobby).Add(HostMods).Add(LobbyService.Protocol).Encode(), reliable: true);
        check(strangerAnswers.TryTake(out var refused, TimeSpan.FromSeconds(5)) && refused.Text(1) == "refused" && refused.Text(2).Contains("Friends only"),
            "A stranger the host does not expect is refused by the lobby's privacy.");
        _ = movedConnection;
    }

    // The whole path: the game reaches the ship, the rejoin asks the host, the game receives the host's invitation and joins.
    static void Rejoined(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        network.Strangers.Add(StrangerId);
        var host = new Node(network, HostId, HostMods);
        var (lobby, connection) = host.Create("c1", "0", check);
        // The stranger was refused for its mods from the Public lobby, which then turned Invite only.
        var before = new Node(network, StrangerId, OwnMods);
        check(before.Join("s1", connection, check) != "0", "The stranger is refused for its mods.");
        host.Lobby.FromLocal(PrivacyTests.Privacy("p1", lobby, HostId, "2"));
        host.Next("lobby.result", check, "privacy");

        var returning = new Returning(network, StrangerId, lobby);
        returning.Advance(TimeSpan.FromSeconds(30));
        check(returning.Rejoin.Current == AutoRejoin.Stage.Waiting && returning.Sent.IsEmpty, "Nothing is asked before the game is on the ship.");
        returning.OnShip(check);
        var invited = returning.Node.Next("lobby.inviteReceived", check, "invitation");
        check(invited.Text(0) == lobby && invited.Text(1) == connection && invited.Text(2) == LobbyService.EntityId(HostId), "The game receives the host's invitation into its lobby.");
        var shown = returning.Notice(check, "notice");
        check(shown.Kind == "info" && shown.Title == "Rejoining Host" && returning.Rejoin.Current == AutoRejoin.Stage.Invited, "The player is told the invitation waits.");
        check(returning.Rejoin.Related(HostId), "The host's frames are taken from the stranger while rejoining.");
        check(returning.Node.Join("s2", connection, check) == "0", "The game takes the invitation: the join is admitted although the lobby is Invite only.");
        Wait(() => returning.Rejoin.Current == AutoRejoin.Stage.Done, check, "The rejoin is done once the game is in.");
        check(host.Lobby.HostedLobbies().Single().Members == 2, "The host's lobby has the player back.");
        returning.Advance(AutoRejoin.AcceptWindow + TimeSpan.FromMinutes(1));
        check(returning.Notices.IsEmpty && returning.Sent.Count == 1, "Nothing more is sent or shown.");

        // The player joins from the social tab before the invitation comes: no ask, no invitation.
        var network2 = new InMemoryNetwork();
        var host2 = new Node(network2, HostId, HostMods);
        var (lobby2, connection2) = host2.Create("c2", "1", check);
        var early = new Returning(network2, FriendId, lobby2);
        early.Node.Create("own", "1", check);
        Wait(() => early.Rejoin.Current == AutoRejoin.Stage.Settling, check, "On the ship");
        check(early.Node.Join("f1", connection2, check) == "0", "The player joins by hand.");
        Wait(() => early.Rejoin.Current == AutoRejoin.Stage.Done, check, "A join by hand ends the rejoin.");
        early.Advance(TimeSpan.FromMinutes(1));
        check(early.Sent.IsEmpty && early.Notices.IsEmpty, "Nothing is asked once the player joined by hand.");

        // The game joins another lobby: the rejoin ends.
        var third = new Node(network2, ThirdId, HostMods);
        var (_, thirdConnection) = third.Create("c3", "1", check);
        var elsewhere = new Returning(network2, StrangerId + 100, lobby2);
        check(elsewhere.Node.Join("e1", thirdConnection, check) == "0" && elsewhere.Rejoin.Current == AutoRejoin.Stage.Done, "Joining another lobby ends the rejoin.");
        elsewhere.Node.Create("own", "1", check);
        elsewhere.Advance(TimeSpan.FromMinutes(1));
        check(elsewhere.Sent.IsEmpty, "...and nothing is asked later.");
    }

    // Each way it gives up: once, with a notice saying why, and nothing tried again.
    static void GivingUp(Action<bool, string> check)
    {
        // The game never reaches the ship.
        {
            var network = new InMemoryNetwork();
            var returning = new Returning(network, FriendId, Guid.NewGuid().ToString());
            returning.Advance(AutoRejoin.ReadyTimeout + TimeSpan.FromSeconds(1));
            var notice = returning.Notice(check, "ready timeout");
            check(returning.Rejoin.Current == AutoRejoin.Stage.GaveUp && notice.Kind == "error" && notice.Lines[0].Contains("ship"), "Not on the ship in time: given up, with a notice.");
            returning.Node.Create("own", "1", check);
            returning.Advance(TimeSpan.FromMinutes(1));
            check(returning.Sent.IsEmpty && returning.Rejoin.Current == AutoRejoin.Stage.GaveUp, "A late ship asks nothing.");
        }
        // The host does not answer (its game closed, or a backend from before lobby.rejoin).
        {
            var network = new InMemoryNetwork();
            var returning = new Returning(network, FriendId, Guid.NewGuid().ToString());
            returning.OnShip(check);
            returning.Advance(AutoRejoin.AnswerTimeout + TimeSpan.FromSeconds(1));
            var notice = returning.Notice(check, "no answer");
            check(returning.Rejoin.Current == AutoRejoin.Stage.GaveUp && notice.Lines[0] == "Host did not answer.", "No answer: given up, with a notice.");
        }
        // The host's lobby has closed and it hosts none; full; the host's answer naming another machine.
        foreach (var (status, line) in new[] { ("gone", "closed"), ("full", "full") })
        {
            var network = new InMemoryNetwork();
            var host = new Node(network, HostId, HostMods);
            var (lobby, connection) = host.Create("c1", "1", check, max: 2);
            if (status == "full") check(new Node(network, ThirdId, HostMods).Join("t1", connection, check) == "0", "A third player fills the lobby.");
            else { host.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(lobby).Add("")); host.Drain(); }
            var returning = new Returning(network, FriendId, lobby);
            returning.OnShip(check);
            var notice = returning.Notice(check, status);
            check(returning.Rejoin.Current == AutoRejoin.Stage.GaveUp && notice.Title == "Couldn't rejoin Host" && notice.Lines[0].Contains(line) &&
                notice.Lines[^1].Contains("social tab"), $"\"{status}\": given up, the notice says why and how to join by hand.");
            check(returning.Node.Inbox.All(f => f.Type != "lobby.inviteReceived"), $"\"{status}\": no invitation reaches the game.");
            returning.Advance(TimeSpan.FromMinutes(5));
            check(returning.Sent.Count == 1, $"\"{status}\": asked once only.");
        }
        // The invitation is not taken: the rejoin ends quietly.
        {
            var network = new InMemoryNetwork();
            var host = new Node(network, HostId, HostMods);
            var (lobby, _) = host.Create("c1", "1", check);
            var returning = new Returning(network, FriendId, lobby);
            returning.OnShip(check);
            returning.Node.Next("lobby.inviteReceived", check, "invitation");
            returning.Notice(check, "rejoining");
            returning.Advance(AutoRejoin.AcceptWindow + TimeSpan.FromSeconds(1));
            check(returning.Rejoin.Current == AutoRejoin.Stage.Done && returning.Notices.IsEmpty && !returning.Rejoin.Related(HostId),
                "An invitation not taken ends the rejoin without a notice.");
        }
        // An answer from someone else, or for another lobby, is ignored.
        {
            var network = new InMemoryNetwork();
            var returning = new Returning(network, FriendId, "lobby-a");
            returning.OnShip(check);
            returning.Rejoin.Answered(ThirdId, new Frame("lobby.rejoined").Add("lobby-a").Add("ok").Add("lobby-a").Add("hellforge1-lobby;lobby-a;" + LobbyService.EntityId(ThirdId)));
            returning.Rejoin.Answered(HostId, new Frame("lobby.rejoined").Add("lobby-b").Add("gone"));
            check(returning.Rejoin.Current == AutoRejoin.Stage.Asking, "Answers from another machine or about another lobby are ignored.");
            returning.Rejoin.Answered(HostId, new Frame("lobby.rejoined").Add("lobby-a").Add("ok").Add("lobby-a").Add("hellforge1-lobby;lobby-a;" + LobbyService.EntityId(ThirdId)));
            check(returning.Rejoin.Current == AutoRejoin.Stage.GaveUp && returning.Node.Inbox.All(f => f.Type != "lobby.inviteReceived"),
                "An \"ok\" naming another machine's lobby is not passed to the game.");
        }
    }
}
