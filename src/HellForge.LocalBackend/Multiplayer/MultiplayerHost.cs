using System.Globalization;

namespace HellForge.LocalBackend.Multiplayer;

// Wires the multiplayer services of one backend: the named pipe for the game's stand-in DLLs, the peer link to
// friends' backends, and the lobby and Party services between them. Friends sharing a lobby or network are pinged every
// 2 s; one silent for 15 s (game crashed, connection lost) is treated as gone. Logs to <evidence>/multiplayer.log.
//   link "none"                                  this machine only (hosting works, joins fail)
//   link "steam:<steam_api64.dll>"               Steam networking, friends only
//   link "udp:<id>:<port>:<peer id>=<port>,..."  loopback UDP between backends on one PC (test harness; ids in hex)
//   link "lan" or "lan:<port>"                   the local network (LanTransport; --lan-peer <ip>[,<ip>] for direct addresses)
public sealed class MultiplayerHost : IAsyncDisposable
{
    readonly PipeHost pipes;
    readonly IPeerTransport? transport;
    readonly StreamWriter logFile;
    readonly object logGate = new();
    readonly Dictionary<ulong, DateTime> lastHeard = [];
    Timer? heartbeat;
    static readonly TimeSpan PeerTimeout = TimeSpan.FromSeconds(15);

    public LobbyService Lobby { get; }
    public PartyService Party { get; }
    // The host's stats for joiners, and this game's when joined (null without a peer link or the session's mods folder).
    public StatsShare? Stats { get; private set; }

    readonly WarClock? warClock;
    AccountKeyExchange? keyExchange;
    readonly FriendPresence? presence;
    long presenceSentVersion = -1;
    DateTime presenceSent = DateTime.MinValue;
    readonly object presenceGate = new();
    readonly HashSet<ulong> presenceSentTo = [];
    static readonly TimeSpan PresenceInterval = TimeSpan.FromSeconds(10);

    MultiplayerHost(string pipeName, string link, string modDigest, string evidenceDirectory, WarClock? warClock, AccountKeys? accountKeys,
        FriendPresence? presence, string? modList, string? lanPeers)
    {
        this.presence = presence;
        this.warClock = warClock;
        logFile = new StreamWriter(Path.Combine(evidenceDirectory, "multiplayer.log"), append: true) { AutoFlush = true };
        transport = CreateTransport(link, lanPeers, Log);
        pipes = new PipeHost(pipeName, Log);
        Lobby = new LobbyService(transport, pipes.SendLobby, Log, modDigest, warClock, modList);
        // A refused join, in both games (the in-game editor shows it over the game): the joiner's says why, the host's who.
        Lobby.Refused += (peer, lines) => Stats?.Notice("error", "Couldn't join " + FriendName(peer), lines);
        Lobby.RefusedFriend += (peer, lines) => Stats?.Notice("info", FriendName(peer) + " couldn't join", lines);
        Party = new PartyService(transport, pipes.SendParty, Log, Lobby.InHostedLobby);
        pipes.Lobby = Lobby;
        pipes.Party = Party;
        if (transport != null && accountKeys != null) accountKeys.Log = Log;
        if (transport != null && presence != null) presence.Log = Log;
        if (transport != null)
            transport.Received += (peer, bytes) =>
            {
                lock (lastHeard) lastHeard[peer] = DateTime.UtcNow;
                Frame frame;
                try { frame = Frame.Decode(bytes); }
                catch (InvalidDataException) { Log("Dropped a malformed peer frame."); return; }
                keyExchange?.Heard(peer);
                if (frame.Type.StartsWith("lobby.", StringComparison.Ordinal)) Lobby.FromPeer(peer, frame);
                else if (frame.Type == "account.key") keyExchange?.FromPeer(peer, frame);
                else if (frame.Type is "presence" or "presence.hello") FromPeerPresence(peer, frame);
                else if (frame.Type.StartsWith("party.", StringComparison.Ordinal)) Party.FromPeer(peer, frame);
                else if (frame.Type.StartsWith("stats.", StringComparison.Ordinal)) Stats?.FromPeer(peer, frame);
                else if (frame.Type == "war.clock" && warClock != null && Lobby.HostsJoinedLobby(peer) && long.TryParse(frame.Text(0), out var seconds))
                {
                    warClock.Set(seconds);
                    Log($"War time set to the host's ({seconds} s).");
                }
            };
    }

    public static MultiplayerHost Start(string pipeName, string link, string modDigest, string evidenceDirectory, WarClock? warClock = null,
        AccountKeys? accountKeys = null, FriendPresence? presence = null, string? modsFolder = null, string? modList = null, string? lanPeers = null)
    {
        var host = new MultiplayerHost(pipeName, link, modDigest, evidenceDirectory, warClock, accountKeys, presence, modList, lanPeers);
        host.transport?.Start();
        // The session's live-edit pipe is "hellforge-live-" + the run id, the pipe name's own suffix ("hellforge-<run id>").
        if (host.transport != null && modsFolder != null && pipeName.StartsWith("hellforge-", StringComparison.Ordinal))
        {
            try
            {
                host.Stats = new StatsShare(host.transport, host.Lobby, pipeName["hellforge-".Length..], modsFolder, host.FriendName, host.Log);
                host.Log("Stats: co-op plays with the host's Local Stats; this game's live-edit pipe is served here.");
            }
            catch (Exception error) when (error is ArgumentException or InvalidDataException or IOException) { host.Log("Stats sharing is off: " + error.Message); }
        }
        // The transport knows this machine's SteamID only once started.
        if (host.transport != null && accountKeys != null) host.keyExchange = new AccountKeyExchange(host.transport, accountKeys, host.Log);
        host.pipes.Start();
        if (host.transport != null) host.heartbeat = new Timer(_ => host.Beat(), null, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(2));
        host.Log($"Multiplayer ready: pipe {pipeName}, link {(host.transport == null ? "none" : link.Split(':')[0])}" +
            (host.transport != null ? $", local peer {LobbyService.EntityId(host.transport.LocalId)}" : "") + ".");
        return host;
    }

    // A friend's name as Steam shows it, for the joiner's editor ("Host: <name>").
    string FriendName(ulong peer) =>
        (transport as IFriendSource)?.HellForgeFriends.FirstOrDefault(f => f.SteamId == peer)?.Name ?? "Steam friend " + LobbyService.EntityId(peer);

    static IPeerTransport? CreateTransport(string link, string? lanPeers, Action<string> log)
    {
        if (link == "none") return null;
        if (link.StartsWith("steam:", StringComparison.Ordinal)) return new SteamPeerTransport(link[6..], log);
        if (link == "lan" || link.StartsWith("lan:", StringComparison.Ordinal))
        {
            var lanPort = link.Length > 4 ? int.Parse(link[4..], CultureInfo.InvariantCulture) : LanTransport.DefaultPort;
            var lanAddresses = (lanPeers ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            return new LanTransport(lanPort, lanAddresses, log);
        }
        var parts = link.Split(':');
        if (parts is ["udp", var id, var port, var peers] && LobbyService.TryNode(id, out var localId) && int.TryParse(port, out var localPort))
        {
            var table = new Dictionary<ulong, int>();
            foreach (var entry in peers.Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                var pair = entry.Split('=');
                if (pair.Length != 2 || !LobbyService.TryNode(pair[0], out var peer) || !int.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out var peerPort))
                    throw new ArgumentException("Bad udp peer: " + entry);
                table[peer] = peerPort;
            }
            return new UdpLoopbackTransport(localId, localPort, table);
        }
        throw new ArgumentException("--link must be none, steam:<steam_api64.dll>, lan or udp:<id>:<port>:<peer>=<port>,...");
    }

    // Pings every friend this machine shares a lobby or network with; a friend unheard for PeerTimeout is gone.
    void Beat()
    {
        try
        {
            var now = DateTime.UtcNow;
            var peers = Lobby.Peers().Concat(Party.Peers()).ToHashSet();
            List<ulong> lost = [];
            lock (lastHeard)
            {
                foreach (var peer in peers)
                {
                    if (!lastHeard.TryGetValue(peer, out var heard)) lastHeard[peer] = heard = now;
                    if (now - heard > PeerTimeout) lost.Add(peer);
                }
                foreach (var peer in lastHeard.Keys.Where(p => !peers.Contains(p)).ToList()) { lastHeard.Remove(peer); keyExchange?.Forget(peer); }
                foreach (var peer in lost) { lastHeard.Remove(peer); keyExchange?.Forget(peer); }
            }
            foreach (var peer in peers.Except(lost)) transport!.Send(peer, new Frame("peer.ping").Encode(), reliable: false);
            SharePresence(now);
            foreach (var peer in lost)
            {
                Log($"Peer {LobbyService.EntityId(peer)} stopped answering; treating it as gone.");
                Lobby.PeerLost(peer);
                Party.PeerLost(peer);
            }
        }
        catch (Exception error) { Log("Heartbeat failed: " + error.Message); }
    }

    // The social tab: friends running HellForge (from the transport) get this player's profile objects and the game's current
    // lobby every 10 s and at once when either changes: "presence" [objects JSON, lobbyId, connectionString, lobby JSON as
    // the game uploaded it], without the lobby fields when in none. The first one to a friend is "presence.hello", which
    // is always answered, so two backends know each other at once, also after one restarts.
    void SharePresence(DateTime now)
    {
        if (presence == null || transport is not IFriendSource source) return;
        var friends = source.HellForgeFriends;
        presence.SetFriends(friends);
        List<ulong> hello, periodic = [];
        lock (presenceGate)
        {
            // Friends just found get it at once; everyone again when it changes or every PresenceInterval.
            presenceSentTo.IntersectWith(friends.Select(f => f.SteamId));
            hello = [.. friends.Select(f => f.SteamId).Where(id => !presenceSentTo.Contains(id))];
            presenceSentTo.UnionWith(hello);
            var version = presence.Local.Version;
            if (version != presenceSentVersion || now - presenceSent >= PresenceInterval)
            {
                presenceSentVersion = version;
                presenceSent = now;
                periodic = [.. presenceSentTo.Except(hello)];
            }
        }
        SendPresence(hello, "presence.hello");
        SendPresence(periodic, "presence");
    }

    void SendPresence(IReadOnlyCollection<ulong> targets, string type)
    {
        if (presence == null || transport == null || targets.Count == 0) return;
        var (lobbyId, body, objects, _) = presence.Local;
        var connection = lobbyId != null ? Lobby.ConnectionStringFor(lobbyId) : null;
        var frame = new Frame(type).Add(objects.ToJsonString());
        if (lobbyId != null && connection != null && body != null) frame.Add(lobbyId).Add(connection).Add(body.ToJsonString());
        var bytes = frame.Encode();
        foreach (var target in targets) transport.Send(target, bytes, reliable: true);
    }

    void FromPeerPresence(ulong peer, Frame frame)
    {
        if (presence == null || frame.Count < 1) return;
        // A friend saying hello, or one this backend has not sent to yet, gets this player's presence in reply.
        bool reply;
        lock (presenceGate) reply = presenceSentTo.Add(peer) || frame.Type == "presence.hello";
        if (reply) SendPresence([peer], "presence");
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(frame.Text(0)) is System.Text.Json.Nodes.JsonObject objects) presence.SetFriendObjects(peer, objects);
        }
        catch (System.Text.Json.JsonException) { Log("Dropped malformed profile objects from a friend."); return; }
        if (frame.Count < 4) { presence.ClearFriendLobby(peer); return; }
        var lobbyId = frame.Text(1);
        var connection = frame.Text(2);
        // Only a lobby on the sender's own machine or one it joined through HellForge can be offered.
        if (!connection.StartsWith(LobbyService.ConnectionPrefix, StringComparison.Ordinal) || connection.Split(';') is not [_, var id, _] || id != lobbyId)
        {
            Log("Dropped a malformed lobby from a friend.");
            return;
        }
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(frame.Text(3)) is System.Text.Json.Nodes.JsonObject body) presence.SetFriendLobby(peer, lobbyId, connection, body);
        }
        catch (System.Text.Json.JsonException) { Log("Dropped a malformed lobby from a friend."); }
    }

    void Log(string message)
    {
        lock (logGate) logFile.WriteLine($"{DateTime.UtcNow.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture)} {HellForge.Core.Privacy.Scrub(message)}");
    }

    public async ValueTask DisposeAsync()
    {
        if (heartbeat != null) await heartbeat.DisposeAsync();
        if (Stats != null) await Stats.DisposeAsync();
        await pipes.DisposeAsync();
        transport?.Dispose();
        Log("Multiplayer stopped.");
        logFile.Dispose();
    }
}
