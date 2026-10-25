namespace HellForge.LocalBackend.Multiplayer;

// The PlayFab Party service for HellForge: the network that carries the game's traffic. A network lives in the backend of the
// player who created it (its host); that backend knows which machines are in it and every endpoint (unique id, machine,
// player), assigns endpoint ids to friends' endpoints, and routes the game's messages between machines. Only players in
// one of the host's lobbies may connect, so the lobby's modlist check guards the game traffic too. The Party stand-in DLL
// of every machine mirrors the network from the events below. Frames are the same on the DLL hop and the peer hop.
//
// Requests (to the host's backend):
//   party.host        [networkId, maxUsers, maxDevices, maxUsersPerDevice, maxDevicesPerUser, maxEndpointsPerDevice, peerOptions]
//                     (this machine's DLL created a network)
//   party.connect     [op, networkId, hostEntityId]
//   party.endpoint    [networkId, op, uid ("" = assign one), entityId]
//   party.endpointGone [networkId, uid]
//   party.leave       [networkId]
//   party.message     [networkId, senderUid, options, targets ("" = everyone, else comma-separated uids), payload]
//   party.kick        [networkId, node] (this machine's DLL, hosting: the game kicked a device or user; Kick)
// Events (from the host's backend):
//   party.connected   [op, result (0 = ok), networkId, the six configuration numbers, endpointBase, endpointCount,
//                     (uid, node, entityId)...]; endpointBase starts the block of EndpointBlock ids the joining machine
//                     gives its own endpoints, so they exist at once (a joining game sends as soon as it sees the host)
//   party.endpointCreated [networkId, op ("" = someone else's), uid, node, entityId]
//   party.endpointDestroyed [networkId, uid]
//   party.deviceJoined [networkId, node]
//   party.deviceLeft  [networkId, node, "left" | "lost"]  (a machine that left on purpose, or stopped answering)
//   party.networkGone [networkId]
//   party.message     (as above, delivered to the machines owning the targets)
// Nodes are SteamID64s in hex (a machine's entity id); a frame's node is never trusted, the transport's sender is.
public sealed class PartyService
{
    public const int GuaranteedDelivery = 1;
    const ushort FirstRemoteEndpoint = 1000; // The host DLL numbers its own endpoints from 1; friends' get 1000 and up.
    public const ushort EndpointBlock = 64;
    static readonly HashSet<string> Requests = ["party.connect", "party.endpoint", "party.endpointGone", "party.leave"];
    static readonly HashSet<string> Events = ["party.connected", "party.endpointCreated", "party.endpointDestroyed", "party.deviceJoined", "party.deviceLeft", "party.networkGone"];

    readonly object gate = new();
    readonly IPeerTransport? transport;
    readonly Action<Frame> toLocal;
    readonly Action<string> log;
    readonly Func<ulong, bool> inHostedLobby;
    readonly Dictionary<string, HostedNetwork> hosted = new(StringComparer.Ordinal);
    // Friends' networks this machine's DLL is in or connecting to: network id -> host node.
    readonly Dictionary<string, ulong> remote = new(StringComparer.Ordinal);

    public PartyService(IPeerTransport? transport, Action<Frame> toLocal, Action<string> log, Func<ulong, bool> inHostedLobby)
    {
        this.transport = transport;
        this.toLocal = toLocal;
        this.log = log;
        this.inHostedLobby = inHostedLobby;
    }

    ulong LocalId => transport?.LocalId ?? 0;
    ulong Absolute(ulong node) => node == LobbyService.LocalNode ? LocalId : node;
    static string Hex(ulong node) => LobbyService.EntityId(node);

    // Machines this backend shares a network with, for the peer heartbeat.
    public IReadOnlyCollection<ulong> Peers()
    {
        lock (gate) return hosted.Values.SelectMany(n => n.Nodes).Concat(remote.Values).Where(n => n != LobbyService.LocalNode).ToHashSet();
    }

    public void FromLocal(Frame frame)
    {
        lock (gate)
        {
            try { Route(frame); }
            catch (InvalidDataException error) { log($"Bad {frame.Type} from the Party DLL: {error.Message}"); }
        }
    }

    public void FromPeer(ulong peer, Frame frame)
    {
        lock (gate)
        {
            try
            {
                var networkId = frame.Type == "party.connected" ? frame.Text(2) : frame.Text(frame.Type == "party.connect" ? 1 : 0);
                if (hosted.ContainsKey(networkId) && (Requests.Contains(frame.Type) || frame.Type == "party.message")) Handle(peer, frame);
                else if (frame.Type == "party.connect") Answer(peer, frame.Text(0), networkId, refused: true);
                else if ((Events.Contains(frame.Type) || frame.Type == "party.message") && remote.TryGetValue(networkId, out var host) && host == peer)
                {
                    if (frame.Type == "party.networkGone" || frame.Type == "party.connected" && frame.Text(1) != "0") remote.Remove(networkId);
                    if (frame.Type == "party.message") CountRemote(networkId, "received");
                    toLocal(frame);
                }
            }
            catch (InvalidDataException error) { log($"Bad {frame.Type} from a peer: {error.Message}"); }
        }
    }

    // This machine's game closed: networks it hosted end for everyone; it leaves friends' networks.
    public void LocalDisconnected()
    {
        lock (gate)
        {
            foreach (var network in hosted.Values.ToList()) Close(network);
            foreach (var (networkId, host) in remote.ToList()) Send(host, new Frame("party.leave").Add(networkId), true);
            remote.Clear();
        }
    }

    // The host kicked a machine from its lobby (LobbyService.Kicked): it leaves every network hosted here as if lost, the
    // others are told, and the kicked machine hears that each network is gone. Whatever it sends afterwards is dropped (it
    // has no endpoints and is no network's node), and it cannot connect again (no longer in a lobby here).
    public void Expel(ulong node)
    {
        if (node == LobbyService.LocalNode) return;
        lock (gate)
            foreach (var network in hosted.Values.Where(n => n.Nodes.Contains(node)).ToList())
            {
                RemoveNode(network, node);
                Send(node, new Frame("party.networkGone").Add(network.Id), true);
                log($"Removed a kicked machine from Party network {network.Id}.");
            }
    }

    // The game's own Party kick (PartyNetworkKickDevice/KickUser from the Party stand-in: party.kick [networkId, entity id
    // or node]): the host's lobby kicks that machine (Kick, LobbyService), which removes it here too (Expel).
    public Func<ulong, bool>? Kick { get; set; }

    // A friend's machine stopped answering: it leaves the networks hosted here, and networks it hosted are gone.
    public void PeerLost(ulong peer)
    {
        lock (gate)
        {
            foreach (var network in hosted.Values.ToList())
                if (network.Nodes.Contains(peer)) RemoveNode(network, peer, lost: true);
            foreach (var (networkId, host) in remote.Where(r => r.Value == peer).ToList())
            {
                remote.Remove(networkId);
                toLocal(new Frame("party.networkGone").Add(networkId));
            }
        }
    }

    readonly Dictionary<string, (long Sent, long Received, DateTime Reported)> remoteTraffic = new(StringComparer.Ordinal);

    void CountRemote(string networkId, string direction)
    {
        var (sent, received, reported) = remoteTraffic.GetValueOrDefault(networkId, (0, 0, DateTime.UtcNow));
        if (direction == "sent") sent++; else received++;
        if (DateTime.UtcNow - reported >= TimeSpan.FromSeconds(10))
        {
            log($"Party network {networkId} (a friend's) messages so far: {sent} sent, {received} received.");
            reported = DateTime.UtcNow;
        }
        remoteTraffic[networkId] = (sent, received, reported);
    }

    void Route(Frame frame)
    {
        if (frame.Type == "party.kick")
        {
            // under this lock the lobby service's is taken (as inHostedLobby does), and its Kicked handler comes back to Expel
            // on this thread once the lobby's lock is released
            if (hosted.ContainsKey(frame.Text(0)) && LobbyService.TryNode(frame.Text(1), out var kicked) && kicked != LocalId && Kick?.Invoke(kicked) == false)
                log("The game kicked a Party device that is in no lobby here.");
            return;
        }
        if (frame.Type == "party.host")
        {
            var network = new HostedNetwork(frame.Text(0)) { Configuration = Enumerable.Range(1, 6).Select(frame.Text).ToArray() };
            network.Nodes.Add(LobbyService.LocalNode);
            hosted[network.Id] = network;
            log($"Party network {network.Id} hosted here.");
            return;
        }
        if (frame.Type == "party.connect")
        {
            var networkId = frame.Text(1);
            if (hosted.ContainsKey(networkId)) { Handle(LobbyService.LocalNode, frame); return; }
            if (!LobbyService.TryNode(frame.Text(2), out var host) || transport == null || host == LocalId) { Answer(LobbyService.LocalNode, frame.Text(0), networkId, refused: true); return; }
            remote[networkId] = host;
            Send(host, frame, true);
            return;
        }
        var id = frame.Text(0);
        if (hosted.ContainsKey(id)) { Handle(LobbyService.LocalNode, frame); return; }
        if (!remote.TryGetValue(id, out var owner)) return;
        if (frame.Type == "party.leave") remote.Remove(id);
        if (frame.Type == "party.message") CountRemote(id, "sent");
        Send(owner, frame, frame.Type != "party.message" || (frame.Int(2) & GuaranteedDelivery) != 0);
    }

    // Requests for a network hosted here, from this machine's DLL (LocalNode) or a friend's backend.
    void Handle(ulong node, Frame frame)
    {
        switch (frame.Type)
        {
            case "party.connect":
            {
                var network = hosted[frame.Text(1)];
                // Only players in one of this machine's lobbies (which checked their mods) may join its network.
                if (node != LobbyService.LocalNode && !inHostedLobby(node)) { log("Refused a Party connection from a player outside the lobby."); Answer(node, frame.Text(0), network.Id, refused: true); return; }
                // A friend's machine connecting again while still in the network (its game crashed and came back): its earlier
                // endpoints go first, or the new game's endpoints, numbered from the same block, would be ignored as taken.
                if (node != LobbyService.LocalNode && network.Nodes.Contains(node))
                {
                    log($"A friend connected to Party network {network.Id} again; their earlier endpoints were removed first.");
                    RemoveNode(network, node);
                }
                var joining = network.Nodes.Add(node);
                if (node != LobbyService.LocalNode && !network.Blocks.ContainsKey(node) && network.NextBlock <= ushort.MaxValue - EndpointBlock)
                {
                    network.Blocks[node] = (ushort)network.NextBlock;
                    network.NextBlock += EndpointBlock;
                }
                Answer(node, frame.Text(0), network.Id, refused: false);
                if (joining)
                {
                    log($"A friend joined Party network {network.Id} ({network.Nodes.Count} machines).");
                    Broadcast(network, new Frame("party.deviceJoined").Add(network.Id).Add(Hex(Absolute(node))), except: node);
                }
                return;
            }
            case "party.endpoint":
            {
                var network = hosted[frame.Text(0)];
                if (!network.Nodes.Contains(node)) return;
                var op = frame.Text(1);
                ushort uid;
                if (frame.Text(2).Length > 0)
                {
                    // An id the machine chose: the host's own below FirstRemoteEndpoint, a friend's inside its block.
                    if (!ushort.TryParse(frame.Text(2), out uid) || uid == 0 || network.Endpoints.ContainsKey(uid)) return;
                    var allowed = node == LobbyService.LocalNode ? uid < FirstRemoteEndpoint
                        : network.Blocks.TryGetValue(node, out var block) && uid >= block && uid < block + EndpointBlock;
                    if (!allowed) return;
                }
                else
                {
                    uid = network.NextRemote++;
                    if (uid < FirstRemoteEndpoint) return;
                }
                var endpoint = new PartyEndpoint(uid, node, frame.Text(3));
                network.Endpoints[uid] = endpoint;
                var created = Created(network, endpoint, "");
                if (op.Length > 0) Send(node, Created(network, endpoint, op), true);
                Broadcast(network, created, except: node);
                return;
            }
            case "party.endpointGone":
            {
                var network = hosted[frame.Text(0)];
                if (!ushort.TryParse(frame.Text(1), out var uid) || !network.Endpoints.TryGetValue(uid, out var endpoint) || endpoint.Node != node) return;
                network.Endpoints.Remove(uid);
                Broadcast(network, new Frame("party.endpointDestroyed").Add(network.Id).Add(uid), except: node);
                return;
            }
            case "party.leave":
            {
                var network = hosted[frame.Text(0)];
                if (node == LobbyService.LocalNode) Close(network);
                else if (network.Nodes.Contains(node)) RemoveNode(network, node);
                return;
            }
            case "party.message":
            {
                var network = hosted[frame.Text(0)];
                if (!ushort.TryParse(frame.Text(1), out var sender) || !network.Endpoints.TryGetValue(sender, out var from) || from.Node != node) return;
                var reliable = (frame.Int(2) & GuaranteedDelivery) != 0;
                network.Count(node == LobbyService.LocalNode ? "host" : "friend", log);
                var targets = frame.Text(3);
                IEnumerable<ulong> nodes = targets.Length == 0
                    ? network.Nodes
                    : targets.Split(',').Select(t => ushort.TryParse(t, out var uid) && network.Endpoints.TryGetValue(uid, out var e) ? e.Node : ulong.MaxValue)
                        .Where(n => n != ulong.MaxValue).Distinct();
                foreach (var target in nodes.Where(n => n != node).ToList()) Send(target, frame, reliable);
                return;
            }
        }
    }

    Frame Created(HostedNetwork network, PartyEndpoint endpoint, string op) =>
        new Frame("party.endpointCreated").Add(network.Id).Add(op).Add(endpoint.Uid).Add(Hex(Absolute(endpoint.Node))).Add(endpoint.EntityId);

    void Answer(ulong node, string op, string networkId, bool refused)
    {
        var frame = new Frame("party.connected").Add(op).Add(refused ? 1 : 0).Add(networkId);
        if (!refused && hosted.TryGetValue(networkId, out var network))
        {
            foreach (var value in network.Configuration) frame.Add(value);
            frame.Add(network.Blocks.TryGetValue(node, out var block) ? block : 0);
            frame.Add(network.Endpoints.Count);
            foreach (var endpoint in network.Endpoints.Values) frame.Add(endpoint.Uid).Add(Hex(Absolute(endpoint.Node))).Add(endpoint.EntityId);
        }
        Send(node, frame, true);
    }

    // A machine that leaves on purpose is reported as left (the SDK's PARTY_DESTROYED_REASON_REQUESTED), a lost one as
    // disconnected. The game never reads the reason (co-op research 2026-10-21: the exe
    // logs it and nothing else). What kept a friend from joining back was the engine's dead connection object
    // for that peer id, which it keeps while a lobby lists the peer; the loader closes it once the game session has let
    // the peer go (native/mod-loader/netreap.c).
    void RemoveNode(HostedNetwork network, ulong node, bool lost = false)
    {
        foreach (var endpoint in network.Endpoints.Values.Where(e => e.Node == node).ToList())
        {
            network.Endpoints.Remove(endpoint.Uid);
            Broadcast(network, new Frame("party.endpointDestroyed").Add(network.Id).Add(endpoint.Uid), except: node);
        }
        network.Nodes.Remove(node);
        log($"A friend left Party network {network.Id}.");
        Broadcast(network, new Frame("party.deviceLeft").Add(network.Id).Add(Hex(Absolute(node))).Add(lost ? "lost" : "left"), except: node);
    }

    // The host's game left its network: its machine leaves first (on purpose), then the network goes.
    void Close(HostedNetwork network)
    {
        hosted.Remove(network.Id);
        log($"Party network {network.Id} closed.");
        Broadcast(network, new Frame("party.deviceLeft").Add(network.Id).Add(Hex(Absolute(LobbyService.LocalNode))).Add("left"), except: LobbyService.LocalNode);
        Broadcast(network, new Frame("party.networkGone").Add(network.Id), except: LobbyService.LocalNode);
    }

    void Broadcast(HostedNetwork network, Frame frame, ulong except)
    {
        foreach (var node in network.Nodes.Where(n => n != except).ToList()) Send(node, frame, true);
    }

    void Send(ulong node, Frame frame, bool reliable)
    {
        if (node == LobbyService.LocalNode) toLocal(frame);
        else transport?.Send(node, frame.Encode(), reliable);
    }

    sealed class HostedNetwork(string id)
    {
        public string Id { get; } = id;
        public string[] Configuration = [];
        public HashSet<ulong> Nodes { get; } = [];
        public Dictionary<ushort, PartyEndpoint> Endpoints { get; } = [];
        public ushort NextRemote = FirstRemoteEndpoint;
        // Endpoint id blocks of friends' machines; the assign-on-request range above stays below the first block.
        public Dictionary<ulong, ushort> Blocks { get; } = [];
        public int NextBlock = 4096;
        readonly Dictionary<string, long> messages = new(StringComparer.Ordinal);
        DateTime lastReport = DateTime.UtcNow;

        // Counts game messages by sender and logs the totals every 10 s, so a log shows whether traffic flows both ways.
        public void Count(string from, Action<string> log)
        {
            messages[from] = messages.GetValueOrDefault(from) + 1;
            if (DateTime.UtcNow - lastReport < TimeSpan.FromSeconds(10)) return;
            lastReport = DateTime.UtcNow;
            log($"Party network {Id} messages so far: " + string.Join(", ", messages.Select(m => $"{m.Value} from {m.Key}")));
        }
    }

    sealed record PartyEndpoint(ushort Uid, ulong Node, string EntityId);
}
