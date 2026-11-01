namespace HellForge.LocalBackend.Multiplayer;

// What a backend takes from someone who is not a Steam friend (docs/multiplayer.md step 7). Nothing at all unless the
// player has turned on "Play with strangers" (off by default, ModSettings.PlayWithStrangers), whatever a lobby's privacy.
// Then strangers only come through the lobby directory, or come back to a lobby here after a mod sync:
//   - a join (lobby.join) while a public lobby is listed here, from a machine this one deals with already (below), or
//     from one a lobby here expects back (welcome: admitted before, invited, or back after a mod sync, LobbyService.Welcome),
//     and a rejoin request after a mod sync (lobby.rejoin) by the same rule,
//     at most JoinsPerPeer a minute from one machine and JoinsInAll from everyone, whoever they are: a member rejoining
//     counts too. Whether the join is taken is the lobby's own rule (LobbyService: privacy, kicked players);
//   - everything else that follows once this machine deals with them (Related: a member of a lobby hosted here, the host
//     of a lobby joined or being joined from here, a machine downloading this one's mods or this one theirs, the host this
//     machine is rejoining after a mod sync);
//   - pings.
// Presence (the social tab) and invitations stay between friends. The tables of strangers forget a machine after
// Window of silence, and are pruned at most every PruneInterval.
public sealed class StrangerGate(IPeerTransport transport, Func<bool> listing, Func<ulong, bool> related, Func<bool> strangersAllowed, Action<string> log,
    Func<ulong, bool>? welcome = null)
{
    public const int JoinsPerPeer = 6, JoinsInAll = 60;
    static readonly TimeSpan Window = TimeSpan.FromMinutes(1), PruneInterval = TimeSpan.FromSeconds(10);

    readonly Dictionary<ulong, Queue<DateTime>> joins = [];
    readonly Queue<DateTime> allJoins = new();
    readonly Dictionary<ulong, DateTime> logged = [];
    DateTime pruned = DateTime.MinValue;

    // How many strangers the tables remember now (tests).
    public int Remembered { get { lock (joins) return joins.Count + logged.Count; } }

    public bool Admit(ulong peer, Frame frame) => Admit(peer, frame, DateTime.UtcNow);

    public bool Admit(ulong peer, Frame frame, DateTime now)
    {
        if (transport.IsFriend(peer)) return true;
        Prune(now);
        if (!strangersAllowed()) return Refuse(peer, frame.Type + " (Play with strangers is off)", now);
        if (frame.Type is "presence" or "presence.hello" or "lobby.invite" or "lobby.invited") return Refuse(peer, frame.Type, now);
        if (frame.Type == "peer.ping") return true;
        if (frame.Type is "lobby.join" or "lobby.rejoin")
        {
            if (!listing() && !related(peer) && welcome?.Invoke(peer) != true) return Refuse(peer, "a join with no public lobby listed here", now);
            return CountJoin(peer, now);
        }
        return related(peer) || Refuse(peer, frame.Type, now);
    }

    // A join within JoinsPerPeer a minute from this machine and JoinsInAll from everyone, counted.
    bool CountJoin(ulong peer, DateTime now)
    {
        lock (joins)
        {
            if (!joins.TryGetValue(peer, out var times)) joins[peer] = times = new Queue<DateTime>();
            while (times.Count > 0 && now - times.Peek() > Window) times.Dequeue();
            while (allJoins.Count > 0 && now - allJoins.Peek() > Window) allJoins.Dequeue();
            if (times.Count >= JoinsPerPeer || allJoins.Count >= JoinsInAll) return Refuse(peer, "too many joins", now);
            times.Enqueue(now);
            allJoins.Enqueue(now);
            return true;
        }
    }

    // Machines silent for Window leave both tables.
    void Prune(DateTime now)
    {
        lock (joins)
        {
            if (now - pruned < PruneInterval) return;
            pruned = now;
            foreach (var (peer, times) in joins.ToList())
            {
                while (times.Count > 0 && now - times.Peek() > Window) times.Dequeue();
                if (times.Count == 0) joins.Remove(peer);
            }
        }
        lock (logged)
            foreach (var peer in logged.Where(l => now - l.Value > Window).Select(l => l.Key).ToList()) logged.Remove(peer);
    }

    // Logged once a minute per machine.
    bool Refuse(ulong peer, string what, DateTime now)
    {
        lock (logged)
        {
            if (logged.TryGetValue(peer, out var last) && now - last < Window) return false;
            logged[peer] = now;
        }
        log($"Dropped {what} from {LobbyService.EntityId(peer)}, who is not a Steam friend.");
        return false;
    }
}
