using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;

namespace HellForge.LocalBackend.Multiplayer;

// How backends reach each other. A peer is named by its SteamID64, which is also the hex entity id the game uses for
// the local player (see PlayFabService), so a lobby connection string names the machine that hosts it.
public interface IPeerTransport : IDisposable
{
    ulong LocalId { get; }
    // Delivered on the transport's own thread: the sender and one encoded frame.
    event Action<ulong, byte[]>? Received;
    void Start();
    void Send(ulong peer, byte[] frame, bool reliable);
    // A Steam friend of this machine's player (strangers only reach it through the lobby directory: MultiplayerHost).
    bool IsFriend(ulong peer) => true;
}

// Friends who run HellForge, for the social tab (FriendPresence). Steam finds them by a rich presence tag; the test transports
// treat every configured peer as one.
public interface IFriendSource
{
    IReadOnlyList<FriendPresence.Friend> HellForgeFriends { get; }
}

// Backends in one process, for tests: a send is delivered on the thread pool, in order per receiver.
public sealed class InMemoryNetwork
{
    readonly ConcurrentDictionary<ulong, Endpoint> endpoints = new();
    // Machines that are not Steam friends of anyone (everyone else is everyone's friend).
    public HashSet<ulong> Strangers { get; } = [];

    public IPeerTransport Join(ulong id) => endpoints[id] = new Endpoint(this, id);

    sealed class Endpoint(InMemoryNetwork network, ulong id) : IPeerTransport, IFriendSource
    {
        public IReadOnlyList<FriendPresence.Friend> HellForgeFriends =>
            [.. network.endpoints.Keys.Where(k => k != id).Order().Select(k => new FriendPresence.Friend(k, $"Friend {k:X}"))];
        // Deliveries to this endpoint, one after another in the order they were sent: each continues the last. (A task
        // each, waiting on a semaphore, could take it in any order.)
        readonly object order = new();
        Task delivered = Task.CompletedTask;
        public ulong LocalId => id;
        public bool IsFriend(ulong peer) { lock (network.Strangers) return !network.Strangers.Contains(peer) && !network.Strangers.Contains(id); }
        public event Action<ulong, byte[]>? Received;
        public void Start() { }
        public void Send(ulong peer, byte[] frame, bool reliable)
        {
            if (!network.endpoints.TryGetValue(peer, out var target)) return; // An unknown peer is silently unreachable, like the network.
            var copy = frame.ToArray();
            lock (target.order)
                target.delivered = target.delivered.ContinueWith(_ => target.Received?.Invoke(id, copy), CancellationToken.None,
                    TaskContinuationOptions.None, TaskScheduler.Default);
        }
        public void Dispose() => network.endpoints.TryRemove(id, out _);
    }
}

// Backends on one PC over loopback UDP, for the two-backend harness: each datagram is the sender's id and one frame.
// Loopback does not lose datagrams in practice; this transport makes no delivery promise beyond that.
public sealed class UdpLoopbackTransport : IPeerTransport, IFriendSource
{
    public IReadOnlyList<FriendPresence.Friend> HellForgeFriends =>
        TreatPeersAsStrangers ? [] : [.. peers.Keys.Order().Select(k => new FriendPresence.Friend(k, $"Friend {k:X}"))];
    // Test mode (HELLFORGE_TEST_STRANGERS=1): the configured peers are strangers, not friends.
    public bool TreatPeersAsStrangers { get; init; }
    public bool IsFriend(ulong peer) => !TreatPeersAsStrangers;

    readonly UdpClient socket;
    readonly IReadOnlyDictionary<ulong, IPEndPoint> peers;
    readonly CancellationTokenSource stop = new();
    public ulong LocalId { get; }
    public event Action<ulong, byte[]>? Received;

    public UdpLoopbackTransport(ulong localId, int port, IReadOnlyDictionary<ulong, int> peerPorts)
    {
        LocalId = localId;
        socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, port));
        peers = peerPorts.ToDictionary(p => p.Key, p => new IPEndPoint(IPAddress.Loopback, p.Value));
    }

    public void Start() => _ = Task.Run(async () =>
    {
        while (!stop.IsCancellationRequested)
        {
            UdpReceiveResult datagram;
            try { datagram = await socket.ReceiveAsync(stop.Token); }
            catch (Exception error) when (error is OperationCanceledException or ObjectDisposedException) { return; }
            catch (SocketException) { continue; } // ICMP port unreachable from an absent peer.
            if (datagram.Buffer.Length < 8 || !IPAddress.IsLoopback(datagram.RemoteEndPoint.Address)) continue;
            var sender = BinaryPrimitives.ReadUInt64LittleEndian(datagram.Buffer);
            if (!peers.TryGetValue(sender, out var known) || !known.Equals(datagram.RemoteEndPoint)) continue;
            Received?.Invoke(sender, datagram.Buffer[8..]);
        }
    });

    public void Send(ulong peer, byte[] frame, bool reliable)
    {
        if (!peers.TryGetValue(peer, out var endpoint) || frame.Length > 60_000) return;
        var datagram = new byte[8 + frame.Length];
        BinaryPrimitives.WriteUInt64LittleEndian(datagram, LocalId);
        frame.CopyTo(datagram, 8);
        try { socket.Send(datagram, datagram.Length, endpoint); }
        catch (SocketException) { }
    }

    public void Dispose()
    {
        stop.Cancel();
        socket.Dispose();
    }
}
