using System.Collections.Concurrent;
using HellForge.LocalBackend.Multiplayer;

// A squad of three and four: players joining a lobby of two one after another and at the same moment. Nobody already in
// the lobby is removed or replaced, every game sees every member, and each joiner's backend lists the squad's other
// joiners as co-op peers (LobbyService.LobbyMates, MateAdded) before its game hears of them, as a sandboxed game reaches
// only listed peers. The FAQ's known issue for 0.3.0-beta: a third player joining a lobby of two replaced the second.
static class SquadTests
{
    const ulong HostId = 0x110000100000101, SecondId = 0x110000100000102, ThirdId = 0x110000100000103, FourthId = 0x110000100000104,
        FifthId = 0x110000100000105;

    sealed class Player
    {
        public readonly ulong Id;
        public readonly ConcurrentQueue<Frame> Game = new();
        public readonly LobbyService Lobby;
        // The co-op peers the backend listed ahead of its game (MateAdded), each with whether its game already knew of them.
        public readonly ConcurrentQueue<(ulong Peer, bool GameKnew)> Listed = new();
        // The co-op peers it took out again (MateRemoved).
        public readonly ConcurrentQueue<ulong> Unlisted = new();

        public Player(InMemoryNetwork network, ulong id)
        {
            Id = id;
            var transport = network.Join(id);
            Lobby = new LobbyService(transport, Game.Enqueue, _ => { }, "mods-a");
            Lobby.MateAdded += peer => Listed.Enqueue((peer, GameKnows(peer)));
            Lobby.MateRemoved += Unlisted.Enqueue;
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("lobby.", StringComparison.Ordinal)) Lobby.FromPeer(peer, frame);
            };
        }

        // Whether this game was told of that member: in a join's snapshot or a lobby.memberAdded.
        public bool GameKnows(ulong peer)
        {
            var id = LobbyService.EntityId(peer);
            return Game.Any(f => f.Type == "lobby.memberAdded" && f.Text(1) == id || f.Type == "lobby.result" && f.Text(1) == "join" &&
                f.Text(2) == "0" && Members(f).Contains(id));
        }

        public Frame? Result(string op) => Game.FirstOrDefault(f => f.Type == "lobby.result" && f.Text(0) == op);

        // The members this game sees in the lobby: its join's snapshot (or the host's create), then added and removed.
        public HashSet<string> Sees()
        {
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var frame in Game.ToArray())
            {
                if (frame.Type == "lobby.result" && frame.Text(2) == "0" && frame.Text(1) is "join" or "create") { seen.Clear(); seen.UnionWith(Members(frame)); }
                else if (frame.Type == "lobby.memberAdded") seen.Add(frame.Text(1));
                else if (frame.Type == "lobby.memberRemoved") seen.Remove(frame.Text(1));
                else if (frame.Type == "lobby.disconnected") seen.Clear();
            }
            return seen;
        }

        public bool Removed() => Game.Any(f => f.Type is "lobby.disconnected" || f.Type == "lobby.memberRemoved" && f.Text(1) == LobbyService.EntityId(Id));
    }

    // A create or join answer's members (the snapshot after the lobby's fields and its two property maps).
    static List<string> Members(Frame result)
    {
        var index = 10;
        _ = result.Properties(ref index);
        _ = result.Properties(ref index);
        var count = result.Int(index++);
        var members = new List<string>();
        for (var i = 0; i < count; i++)
        {
            members.Add(result.Text(index));
            index += 2;
            _ = result.Properties(ref index);
        }
        return members;
    }

    public static Task Run(Action<bool, string> check)
    {
        InSequence(check);
        AtOnce(check);
        return Task.CompletedTask;
    }

    // Host, second and third join one after another, then a fourth, then a fifth into the full squad.
    static void InSequence(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var (host, second, third, fourth, fifth) = (new Player(network, HostId), new Player(network, SecondId), new Player(network, ThirdId),
            new Player(network, FourthId), new Player(network, FifthId));
        var connection = Open(host, check);
        var everyone = new[] { host, second, third, fourth };

        Join(second, connection, "s1", check);
        check(Poll.Until(() => host.Sees().Count == 2), "The host sees a lobby of two.");
        check(second.Lobby.LobbyMates().Count == 0, "In a lobby of two, the joiner's only co-op peer is the host (no mates).");

        Join(third, connection, "t1", check);
        var ids = Ids(host, second, third);
        check(Poll.Until(() => new[] { host, second, third }.All(p => p.Sees().SetEquals(ids))), "A third player joins a lobby of two: all three games see all three members.");
        check(!second.Removed() && !host.Removed(), "The third player's join removed nobody (the second player stays).");
        check(Poll.Until(() => Mates(second, [ThirdId]) && Mates(third, [SecondId])),
            "The second and third players' backends list each other as co-op peers.");
        check(second.Listed.Any(l => l.Peer == ThirdId && !l.GameKnew), "The second player's backend lists the third before its game hears of them.");
        check(third.Listed.Any(l => l.Peer == SecondId && !l.GameKnew), "The third player's backend lists the second before its game sees the lobby.");
        check(!second.Listed.Any(l => l.Peer == HostId) && !third.Listed.Any(l => l.Peer == HostId || l.Peer == ThirdId),
            "Mates are other joiners only (not the owner, not this machine).");

        Join(fourth, connection, "f1", check);
        ids = Ids(everyone);
        check(Poll.Until(() => everyone.All(p => p.Sees().SetEquals(ids))), "A fourth player fills the squad: all four games see all four.");
        check(everyone.All(p => !p.Removed()), "Nobody was removed when the squad filled.");
        check(Poll.Until(() => Mates(second, [ThirdId, FourthId]) && Mates(fourth, [SecondId, ThirdId])),
            "Each joiner lists the two others.");

        fifth.Lobby.FromLocal(MultiplayerTests.Join("x1", FifthId, connection));
        check(Poll.Until(() => fifth.Result("x1") is { } refused && refused.Text(2) == LobbyService.LobbyNotJoinable.ToString()),
            "A fifth player is refused: the lobby is full.");
        Thread.Sleep(150);
        check(everyone.All(p => !p.Removed() && p.Sees().SetEquals(ids)), "The refused fifth player took nobody's place.");

        // The third player leaves; the others keep the squad, and their backends drop the third as a mate.
        third.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(LobbyId(connection)).Add(LobbyService.EntityId(ThirdId)));
        check(Poll.Until(() => Mates(second, [FourthId]) && Mates(fourth, [SecondId])),
            "A joiner who left is no one's mate any more.");
        check(third.Lobby.LobbyMates().Count == 0, "A machine out of the lobby lists no mates.");
        check(Poll.Until(() => second.Unlisted.Contains(ThirdId) && fourth.Unlisted.Contains(ThirdId)),
            "The others' backends take the third out of their co-op peers at once (MateRemoved).");
        check(third.Unlisted.Contains(SecondId) && third.Unlisted.Contains(FourthId), "The machine that left takes out its own mates.");
        check(!second.Unlisted.Contains(FourthId) && !fourth.Unlisted.Contains(SecondId), "Mates still in the lobby stay listed.");
        check(!second.Removed() && !fourth.Removed(), "The others stay when one leaves.");
    }

    // Two players join a lobby of one at the same moment, then two more at once into a lobby of three (one place left).
    static void AtOnce(Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var (host, second, third, fourth, fifth) = (new Player(network, HostId), new Player(network, SecondId), new Player(network, ThirdId),
            new Player(network, FourthId), new Player(network, FifthId));
        var connection = Open(host, check);

        Parallel.Invoke(() => second.Lobby.FromLocal(MultiplayerTests.Join("s1", SecondId, connection)),
            () => third.Lobby.FromLocal(MultiplayerTests.Join("t1", ThirdId, connection)));
        check(Poll.Until(() => second.Result("s1")?.Text(2) == "0" && third.Result("t1")?.Text(2) == "0"), "Two players joining at once both get in.");
        var ids = Ids(host, second, third);
        check(Poll.Until(() => new[] { host, second, third }.All(p => p.Sees().SetEquals(ids))), "Joined at once: all three games see all three.");
        check(!second.Removed() && !third.Removed(), "Joined at once: nobody was removed.");
        check(Poll.Until(() => Mates(second, [ThirdId]) && Mates(third, [SecondId])),
            "Joined at once: each lists the other as a co-op peer, whichever the host took first.");

        // one place left, two at once: exactly one gets in, the other is refused, and the three already in stay
        Parallel.Invoke(() => fourth.Lobby.FromLocal(MultiplayerTests.Join("f1", FourthId, connection)),
            () => fifth.Lobby.FromLocal(MultiplayerTests.Join("x1", FifthId, connection)));
        check(Poll.Until(() => fourth.Result("f1") != null && fifth.Result("x1") != null), "Both late joiners are answered.");
        var winner = fourth.Result("f1")!.Text(2) == "0" ? fourth : fifth;
        var loser = winner == fourth ? fifth : fourth;
        check(winner.Result(winner == fourth ? "f1" : "x1")!.Text(2) == "0" &&
            loser.Result(loser == fourth ? "f1" : "x1")!.Text(2) == LobbyService.LobbyNotJoinable.ToString(), "Exactly one of two at once takes the last place.");
        ids = Ids(host, second, third, winner);
        check(Poll.Until(() => new[] { host, second, third, winner }.All(p => p.Sees().SetEquals(ids))), "The full squad: all four see all four.");
        check(new[] { host, second, third, winner }.All(p => !p.Removed()), "The race for the last place removed nobody.");
        check(Poll.Until(() => Mates(second, [ThirdId, winner.Id])) && loser.Lobby.LobbyMates().Count == 0,
            "The refused player is no one's mate and lists none.");
    }

    static string Open(Player host, Action<bool, string> check)
    {
        host.Lobby.FromLocal(MultiplayerTests.Create("c1", HostId));
        check(Poll.Until(() => host.Result("c1") != null), "The host's lobby is created.");
        return host.Result("c1")!.Text(4);
    }

    static void Join(Player player, string connection, string op, Action<bool, string> check)
    {
        player.Lobby.FromLocal(MultiplayerTests.Join(op, player.Id, connection));
        check(Poll.Until(() => player.Result(op)?.Text(2) == "0"), $"{LobbyService.EntityId(player.Id)} joins.");
    }

    // Whether exactly those machines are the player's mates (LobbyService.LobbyMates).
    static bool Mates(Player player, IEnumerable<ulong> expected) => player.Lobby.LobbyMates().ToHashSet().SetEquals(expected);

    static string LobbyId(string connection) => connection.Split(';')[1];

    static HashSet<string> Ids(params Player[] players) => players.Select(p => LobbyService.EntityId(p.Id)).ToHashSet(StringComparer.Ordinal);
}
