using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using Microsoft.Win32;

namespace HellForge.LocalBackend.Multiplayer;

// Backend-to-backend messages over the local network, for co-op without Steam friends (players on the same network).
// Every backend on the subnet announces itself (UDP broadcast on one discovery port, plus unicast to any address a
// player typed) and pairs with the others over TCP: one connection per peer, every frame framed with its length, so
// frames arrive ordered and whole as the Steam link's reliable messages do. The backend is not in the network block, so
// only this program needs an inbound Windows Firewall rule (the session places it once, with one Windows prompt), and
// only for the local subnet.
// Peers are known by their SteamID64, read from the running Steam client's own registry key: the backend has to know the
// id the game logs in as (PlayFab accounts and lobby member ids are SteamIDs), so a LAN friend looks to the game
// exactly like a Steam friend. Nothing here authenticates a peer beyond "it announced itself on the
// LAN": unlike the Steam link, which accepts only the signed-in account's friends.
public sealed class LanTransport : IPeerTransport, IFriendSource
{
    public const int DefaultPort = 47991;
    const string BeaconMagic = "hellforge-lan1";
    const int MaxFrame = 60_000;
    static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);

    readonly Action<string> log;
    readonly int port;
    readonly List<IPEndPoint> manual = [];
    readonly CancellationTokenSource stop = new();
    readonly ConcurrentDictionary<ulong, Connection> connections = new();
    readonly ConcurrentDictionary<ulong, byte> dialing = new();
    byte[] announcement = [];
    UdpClient? discovery;
    TcpListener? server;
    Thread? beaconThread, receiveThread, acceptThread;

    public ulong LocalId { get; }
    public event Action<ulong, byte[]>? Received;
    // The peers this machine is connected to, as the game's social tab lists friends (SteamID + name).
    public IReadOnlyList<FriendPresence.Friend> HellForgeFriends =>
        [.. connections.Keys.Order().Select(id => new FriendPresence.Friend(id, Label(id)))];

    // --link lan: this machine's SteamID from the running client.
    public LanTransport(int port, IReadOnlyList<string> manualPeers, Action<string> log)
        : this(SteamId(), port, manualPeers, log) { }

    public LanTransport(ulong localId, int port, IReadOnlyList<string> manualPeers, Action<string> log)
    {
        LocalId = localId;
        this.port = port;
        this.log = log;
        foreach (var peer in manualPeers)
        {
            if (!IPEndPoint.TryParse(peer.Contains(':') ? peer : peer + ":" + port.ToString(CultureInfo.InvariantCulture), out var endpoint) ||
                endpoint.AddressFamily != AddressFamily.InterNetwork)
                throw new ArgumentException($"Bad LAN peer address: {peer} (an IPv4 address, optionally with :port)");
            manual.Add(endpoint);
        }
    }

    // The id the game logs in as: Steam's active account, as the Steam client names it under HKCU.
    static ulong SteamId()
    {
        if (!OperatingSystem.IsWindows())
            throw new IOException("LAN co-op is a Windows feature.");
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess");
        var account = key?.GetValue("ActiveUser");
        if (account == null || Convert.ToUInt32(account, CultureInfo.InvariantCulture) is 0 or uint.MaxValue)
            throw new IOException("LAN co-op needs Steam running and signed in on this PC (its account was not found).");
        return 76561197960265728UL + Convert.ToUInt32(account, CultureInfo.InvariantCulture);
    }

    static string Label(ulong id)
    {
        var hex = LobbyService.EntityId(id);
        return "Player " + hex[^Math.Min(4, hex.Length)..];
    }

    public void Start()
    {
        try
        {
            discovery = new UdpClient(AddressFamily.InterNetwork) { EnableBroadcast = true };
            discovery.Client.Bind(new IPEndPoint(IPAddress.Any, port));
            // TCP shares the number with UDP discovery: an announcement is all a player needs to type the port once.
            server = new TcpListener(IPAddress.Any, port);
            server.Start();
        }
        catch (SocketException error)
        {
            discovery?.Dispose();
            server?.Stop();
            throw new IOException($"LAN co-op could not listen on port {port} (another program may hold it): {error.Message}");
        }
        announcement = Encoding.UTF8.GetBytes(string.Join('|', BeaconMagic, LobbyService.EntityId(LocalId), port.ToString(CultureInfo.InvariantCulture)));
        log($"LAN co-op listening on UDP and TCP {port}.");
        beaconThread = new Thread(Beacons) { IsBackground = true, Name = "HellForge LAN beacons" };
        beaconThread.Start();
        receiveThread = new Thread(Receive) { IsBackground = true, Name = "HellForge LAN discovery" };
        receiveThread.Start();
        acceptThread = new Thread(Accept) { IsBackground = true, Name = "HellForge LAN peers" };
        acceptThread.Start();
    }

    // Announces this backend every two seconds: to each interface's subnet broadcast, to 255.255.255.255, and to the
    // addresses a player typed (for networks that drop broadcasts). The announcement carries this backend's id and
    // port; the backend that hears it connects back, so one announcement is enough to pair, and a player only has to
    // type the address on one of the two PCs.
    void Beacons()
    {
        var targets = Broadcasts().Concat(manual).ToList();
        while (!stop.IsCancellationRequested)
        {
            foreach (var target in targets)
            {
                try { discovery!.Send(announcement, announcement.Length, target); }
                catch (SocketException) { }
                catch (ObjectDisposedException) { return; }
                catch (InvalidOperationException) { return; }
            }
            if (stop.Token.WaitHandle.WaitOne(TimeSpan.FromSeconds(2))) return;
        }
    }

    IEnumerable<IPEndPoint> Broadcasts()
    {
        yield return new IPEndPoint(IPAddress.Broadcast, port);
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var unicast in nic.GetIPProperties().UnicastAddresses)
            {
                if (unicast.Address.AddressFamily != AddressFamily.InterNetwork || unicast.IPv4Mask is not { } mask) continue;
                var address = unicast.Address.GetAddressBytes();
                var netmask = mask.GetAddressBytes();
                var broadcast = new byte[4];
                for (var i = 0; i < 4; i++) broadcast[i] = (byte)(address[i] | ~netmask[i]);
                yield return new IPEndPoint(new IPAddress(broadcast), port);
            }
        }
    }

    void Receive()
    {
        IPEndPoint remote = new(IPAddress.Any, 0);
        while (!stop.IsCancellationRequested)
        {
            byte[] datagram;
            try { datagram = discovery!.Receive(ref remote); }
            catch (SocketException)
            {
                if (stop.IsCancellationRequested) return;
                Thread.Sleep(100);
                continue;
            }
            catch (ObjectDisposedException) { return; }
            if (!Parse(datagram, out var peer, out var dataPort)) continue;
            // One side dials, always the same one: the peer with the smaller SteamID. The larger answers an announcement
            // with its own, so a network that drops broadcasts still pairs when either player typed the other's address.
            if (LocalId > peer) Answer(remote);
            else if (!connections.ContainsKey(peer)) Dial(peer, new IPEndPoint(remote.Address, dataPort));
        }
    }

    // Another backend's announcement: the SteamID it logs in as and the port its TCP link listens on.
    bool Parse(byte[] datagram, out ulong peer, out int dataPort)
    {
        peer = 0;
        dataPort = 0;
        var parts = Encoding.UTF8.GetString(datagram).Split('|');
        return parts.Length >= 3 && parts[0] == BeaconMagic && LobbyService.TryNode(parts[1], out peer) && peer != LocalId &&
               int.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out dataPort) && dataPort is >= 1 and <= 65535;
    }

    void Answer(IPEndPoint remote)
    {
        try { discovery!.Send(announcement, announcement.Length, remote); }
        catch (SocketException) { }
        catch (ObjectDisposedException) { return; }
    }

    // One dial per peer at a time: the smaller SteamID's side dials, and the larger side only ever accepts.
    void Dial(ulong peer, IPEndPoint endpoint)
    {
        if (!dialing.TryAdd(peer, 0)) return;
        _ = Task.Run(() =>
        {
            TcpClient? client = null;
            try
            {
                client = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                timeout.CancelAfter(ConnectTimeout);
                client.ConnectAsync(endpoint, timeout.Token).AsTask().GetAwaiter().GetResult();
                var stream = client.GetStream();
                var handshake = new byte[8];
                BitConverter.TryWriteBytes(handshake, LocalId);
                stream.Write(handshake);
                if (ReadId(stream) is not { } remote || remote == LocalId) return;
                var connection = new Connection(client, stream, PeerId: remote);
                if (connections.TryAdd(remote, connection))
                {
                    client = null;
                    log($"LAN peer {LobbyService.EntityId(remote)} connected ({Label(remote)}) from {endpoint}.");
                    Read(connection);
                }
            }
            catch (Exception error) when (error is SocketException or IOException or OperationCanceledException or ObjectDisposedException)
            {
                if (!stop.IsCancellationRequested) log($"LAN peer {LobbyService.EntityId(peer)} did not connect: {error.Message}");
            }
            finally
            {
                client?.Dispose();
                dialing.TryRemove(peer, out _);
            }
        });
    }

    void Accept()
    {
        while (!stop.IsCancellationRequested)
        {
            TcpClient client;
            try { client = server!.AcceptTcpClient(); }
            catch (SocketException) { if (!stop.IsCancellationRequested) Thread.Sleep(50); continue; }
            catch (ObjectDisposedException) { return; }
            _ = Task.Run(() =>
            {
                try
                {
                    client.NoDelay = true;
                    var stream = client.GetStream();
                    // Each side names itself, then reads the other; the dialer wrote before it read, so this writes too.
                    var hello = new byte[8];
                    BitConverter.TryWriteBytes(hello, LocalId);
                    stream.Write(hello);
                    if (ReadId(stream) is not { } remote || remote == LocalId) { client.Dispose(); return; }
                    var connection = new Connection(client, stream, PeerId: remote);
                    if (connections.TryAdd(remote, connection))
                    {
                        log($"LAN peer {LobbyService.EntityId(remote)} connected ({Label(remote)}) from {client.Client.RemoteEndPoint}.");
                        Read(connection);
                    }
                    else client.Dispose();
                }
                catch (Exception error) when (error is IOException or SocketException or ObjectDisposedException) { client.Dispose(); }
            });
        }
    }

    void Read(Connection connection)
    {
        _ = Task.Run(() =>
        {
            try
            {
                var header = new byte[4];
                while (!stop.IsCancellationRequested)
                {
                    if (!ReadExactly(connection.Stream, header, 4)) break;
                    var length = BitConverter.ToInt32(header);
                    if (length is < 1 or > MaxFrame) { log("Dropped an oversized LAN frame."); break; }
                    var frame = new byte[length];
                    if (!ReadExactly(connection.Stream, frame, length)) break;
                    if (connections.TryGetValue(connection.PeerId, out var current) && ReferenceEquals(current, connection))
                        Received?.Invoke(connection.PeerId, frame);
                }
            }
            catch (Exception error) when (error is IOException or SocketException or ObjectDisposedException) { }
            finally
            {
                Close(connection);
                connections.TryRemove(connection.PeerId, out var removed);
                if (ReferenceEquals(removed, connection) && !stop.IsCancellationRequested)
                    log($"LAN peer {LobbyService.EntityId(connection.PeerId)} disconnected.");
            }
        });
    }

    static bool ReadExactly(NetworkStream stream, byte[] buffer, int count)
    {
        var read = 0;
        while (read < count)
        {
            var got = stream.Read(buffer, read, count - read);
            if (got <= 0) return false;
            read += got;
        }
        return true;
    }

    static ulong? ReadId(NetworkStream stream)
    {
        var handshake = new byte[8];
        stream.ReadTimeout = (int)ConnectTimeout.TotalMilliseconds;
        try
        {
            if (!ReadExactly(stream, handshake, 8)) return null;
        }
        finally { stream.ReadTimeout = Timeout.Infinite; }
        return BitConverter.ToUInt64(handshake) is var id && id != 0 ? id : null;
    }

    public void Send(ulong peer, byte[] frame, bool reliable)
    {
        if (frame.Length > MaxFrame) { log($"Dropped a {frame.Length}-byte frame for LAN peer {LobbyService.EntityId(peer)}: too long."); return; }
        if (!connections.TryGetValue(peer, out var connection)) { log($"Dropped a frame for LAN peer {LobbyService.EntityId(peer)}: not connected."); return; }
        var packet = new byte[4 + frame.Length];
        BitConverter.TryWriteBytes(packet, frame.Length);
        frame.CopyTo(packet, 4);
        try
        {
            lock (connection.WriteGate) connection.Stream.Write(packet);
        }
        catch (Exception error) when (error is IOException or ObjectDisposedException) { Close(connection); }
    }

    static void Close(Connection connection)
    {
        try { connection.Client.Dispose(); } catch (ObjectDisposedException) { }
    }

    public void Dispose()
    {
        stop.Cancel();
        try { discovery?.Dispose(); } catch (ObjectDisposedException) { }
        try { server?.Stop(); } catch (ObjectDisposedException) { }
        foreach (var connection in connections.Values) Close(connection);
        foreach (var thread in new[] { beaconThread, receiveThread, acceptThread }) if (thread is { IsAlive: true }) thread.Join(TimeSpan.FromSeconds(2));
        stop.Dispose();
    }

    sealed class Connection(TcpClient client, NetworkStream stream, ulong PeerId)
    {
        public TcpClient Client { get; } = client;
        public NetworkStream Stream { get; } = stream;
        public ulong PeerId { get; } = PeerId;
        public object WriteGate { get; } = new();
    }
}
