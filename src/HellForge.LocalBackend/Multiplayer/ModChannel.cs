using System.Globalization;
using System.Threading.Channels;
using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// The mod channel in co-op (SDK 16; docs/multiplayer.md "The mod channel in co-op", docs/modding/concepts.md "Co-op"):
// what the same code mod on every machine of a lobby sends each other, and the host's shared table. Shaped like
// AssetShare: the game's live pipe carries its mods' lines (native/mod-loader/channel.c), the host relays messages, and
// the host keeps a versioned table and sends each member what it lacks, so a late joiner gets every row.
//   "mods.msg"      [mod id, from hex, target ("all", "host" or a peer id), data]   joiner to its host, then relayed
//   "mods.share"    [version, mod id, key hex, value]                                host to every member: one row (no
//                                                                                   value: removed)
//   "mods.sharesnap" [version, count, (mod id, key hex, value) * count]              host to one member whose version is
//                                                                                   behind: the whole table
// Messages are at most MaxMessage bytes and 64 a second from each machine; a row's value at most MaxValue bytes; a mod's
// table at most MaxRows rows. The host enforces all of them, so a peer that skips its loader cannot grow the table. Rows
// go when the lobby ends or this machine leaves it, and the game is sent "sharereset" then. A game that connected with
// live protocol 1 is never sent these lines; an older backend ignores the frames (MultiplayerHost routes them here).
public sealed class ModChannel : IAsyncDisposable
{
    public const int MaxMessage = 1024, MaxValue = 64, MaxRows = 512, MaxModId = 80;
    public const int MessagesPerSecond = 64;
    const int MaxChanges = 512;

    readonly IPeerTransport transport;
    readonly LobbyService lobby;
    readonly LiveEditServer live;
    readonly Action<string> log;
    readonly object gate = new();
    readonly Timer timer;
    // the game's lines, one after another (LiveEditServer answers one request at a time)
    readonly Channel<Delivery> deliveries = Channel.CreateBounded<Delivery>(new BoundedChannelOptions(4096)
        { SingleReader = true });
    readonly Task delivering;
    // host: each mod's rows, the table's version and the changes since it started; the version each friend has
    readonly Dictionary<string, SortedDictionary<ulong, byte[]>> table = new(StringComparer.Ordinal);
    readonly List<Change> changes = [];
    readonly Dictionary<ulong, long> sent = [];
    long version;
    // joiner: the host this table came from, and its last version
    ulong hostPeer;
    long hostVersion = -1;
    bool wasInLobby; /* under gate: the lobby's rows were held at the last tick */
    // hosting: each friend's messages this second
    readonly Dictionary<ulong, Bucket> rates = [];
    DateTime queueLogged = DateTime.MinValue, shareRefused = DateTime.MinValue;

    readonly record struct Change(long Version, string Mod, ulong Key, byte[]? Value);
    readonly record struct Delivery(string Kind, string Mod, ulong From, ulong Key, byte[]? Value);
    sealed class Bucket
    {
        public double Tokens = MessagesPerSecond;
        public DateTime At = DateTime.UtcNow;
        public DateTime Logged = DateTime.MinValue;
    }

    public ModChannel(IPeerTransport transport, LobbyService lobby, LiveEditServer live, Action<string> log)
    {
        this.transport = transport;
        this.lobby = lobby;
        this.live = live;
        this.log = log;
        live.Received += FromGame;
        live.StateChanged += GameStateChanged;
        timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
        delivering = Task.Run(DeliverAsync);
    }

    // The game connected again (or for the first time): it holds no rows, so it is sent the table this backend holds.
    void GameStateChanged()
    {
        if (!live.Connected || live.GameProtocol < 2) return;
        EnqueueSnapshot();
    }

    void EnqueueSnapshot()
    {
        Enqueue(new Delivery("reset", "", 0, 0, null));
        List<Delivery> rows = [];
        lock (gate)
            foreach (var (mod, set) in table.OrderBy(p => p.Key, StringComparer.Ordinal))
                foreach (var (key, value) in set)
                    rows.Add(new Delivery("share", mod, 0, key, value));
        foreach (var row in rows) Enqueue(row);
        if (rows.Count > 0) log($"Mod channel: the game is sent the table again ({rows.Count} row(s)).");
    }

    void Enqueue(Delivery line)
    {
        if (deliveries.Writer.TryWrite(line)) return;
        bool logIt;
        lock (gate) { logIt = DateTime.UtcNow - queueLogged > TimeSpan.FromSeconds(30); if (logIt) queueLogged = DateTime.UtcNow; }
        if (logIt) log("Mod channel: the game's queue is full; a line was dropped.");
    }

    // The game's lines, one at a time and in order; a game that is gone loses them (its backend sends the table again
    // when it connects, and a message is not kept).
    async Task DeliverAsync()
    {
        await foreach (var line in deliveries.Reader.ReadAllAsync())
        {
            if (!live.Connected || live.Mismatched || live.GameProtocol < 2) continue;
            _ = line.Kind switch
            {
                "message" => await live.MessageAsync(line.Mod, line.From, line.Value ?? Array.Empty<byte>()),
                "share" => await live.ShareAsync(line.Mod, line.Key, line.Value),
                _ => await live.ShareResetAsync(),
            };
        }
    }

    // A line the game sent on its own (LiveEditServer.Received).
    void FromGame(string[] parts)
    {
        if (parts.Length < 2) return;
        if (parts[0] == "send") FromGameSend(parts);
        else if (parts[0] == "share") FromGameShare(parts);
    }

    // send <mod id> <all|host|peer hex> <data hex>: this game's mod sends a message.
    void FromGameSend(string[] parts)
    {
        if (parts.Length != 4) { log("Mod channel: the game sent a malformed message."); return; }
        string mod;
        byte[] data;
        try { mod = ModId(parts[1]); data = Hex(parts[3], MaxMessage); }
        catch (InvalidDataException error) { log("Mod channel: " + error.Message); return; }
        if (parts[2] is not ("all" or "host") && !TryPeer(parts[2], out _)) { log("Mod channel: the game named no machine; the message is dropped."); return; }
        var hosts = lobby.JoinedHosts();
        if (hosts.Count > 0)
        {
            transport.Send(hosts.Min(), new Frame("mods.msg").Add(mod).Add(LocalText()).Add(parts[2]).Add(data).Encode(), reliable: true);
            return;
        }
        if (lobby.HostedLobbies().Count == 0) { log("Mod channel: a message in solo has no other machine; it is dropped."); return; }
        var hosted = lobby.HostedFriends();
        if (parts[2] == "host") return; /* the host is this machine: no other machine's mod is here */
        if (parts[2] == "all")
        {
            foreach (var friend in hosted) SendMessage(friend, transport.LocalId, "all", mod, data);
            return;
        }
        if (TryPeer(parts[2], out var target) && hosted.Contains(target)) SendMessage(target, transport.LocalId, KeyText(target), mod, data);
        else log($"Mod channel: no machine {parts[2]} in this lobby; the message is dropped.");
    }

    // share <mod id> <key hex> <value hex or ->: a row the host's mod set.
    void FromGameShare(string[] parts)
    {
        if (lobby.JoinedHosts().Count > 0) { log("Mod channel: only the host sets the shared table; a joiner's row is dropped."); return; }
        string mod;
        ulong key;
        byte[]? value;
        try { (mod, key, value) = ParseShare(parts); }
        catch (InvalidDataException error) { log("Mod channel: " + error.Message); return; }
        var (changed, full) = ApplyShare(mod, key, value);
        if (full) { log($"Mod channel: {mod}'s table holds {MaxRows} rows; the row is dropped."); return; }
        if (changed) Tick();
    }

    // A share line's fields: its mod, key and value (null: removed).
    static (string Mod, ulong Key, byte[]? Value) ParseShare(string[] parts)
    {
        if (parts.Length != 4) throw new InvalidDataException("a share needs its mod, key and value.");
        return (ModId(parts[1]), Key(parts[2]), parts[3] == "-" ? null : Hex(parts[3], MaxValue));
    }

    // One row into this machine's table: changed, or full (a new row past the cap). Gate held.
    (bool Changed, bool Full) ApplyShare(string mod, ulong key, byte[]? value)
    {
        lock (gate)
        {
            if (!table.TryGetValue(mod, out var rows)) table[mod] = rows = new SortedDictionary<ulong, byte[]>();
            if (value == null)
            {
                var changed = rows.Remove(key);
                if (changed) { changes.Add(new Change(++version, mod, key, null)); TrimChanges(); }
                if (rows.Count == 0) table.Remove(mod);
                return (changed, false);
            }
            if (rows.TryGetValue(key, out var had) && had.AsSpan().SequenceEqual(value)) return (false, false);
            if (!rows.ContainsKey(key) && rows.Count >= MaxRows) return (false, true);
            rows[key] = value;
            changes.Add(new Change(++version, mod, key, value));
            TrimChanges();
            return (true, false);
        }
    }

    // Keeps the change log a member behind a snapshot can be caught up from. Gate held.
    void TrimChanges()
    {
        if (changes.Count > MaxChanges) changes.RemoveRange(0, changes.Count - MaxChanges);
    }

    // Every second and on every change: the table to a friend that is behind (the whole of it, or the changes since its
    // version), and the lobby's end (every row goes).
    void Tick()
    {
        try
        {
            var hosted = lobby.HostedFriends();
            var hosts = lobby.JoinedHosts();
            var hostedLobbies = lobby.HostedLobbies().Count;
            List<(ulong To, Frame Frame, string What)> sends = [];
            bool reset = false;
            lock (gate)
            {
                reset = UpdateLobby(hosted, hosts, hostedLobbies);
                sends = PlanSends(hosted, hosts, hostedLobbies);
            }
            foreach (var (to, frame, what) in sends)
            {
                transport.Send(to, frame.Encode(), reliable: true);
                log($"Mod channel: sent {what} to {LobbyService.EntityId(to)}.");
            }
            if (reset) Enqueue(new Delivery("reset", "", 0, 0, null));
        }
        catch (Exception error)
        {
            lock (gate) sent.Clear(); // a friend marked sent may not have got it: the next tick sends it again
            log("Mod channel: sharing failed: " + error.Message);
        }
    }

    // Under gate: the lobby's members changed; drop stale tracking, follow the host, and clear the table when the lobby
    // ends. Returns whether the game should be told every row goes.
    bool UpdateLobby(IReadOnlyCollection<ulong> hosted, IReadOnlyCollection<ulong> hosts, int hostedLobbies)
    {
        bool reset = false;
        foreach (var gone in sent.Keys.Where(p => !hosted.Contains(p)).ToList()) sent.Remove(gone);
        foreach (var gone in rates.Keys.Where(p => !hosted.Contains(p)).ToList()) rates.Remove(gone);
        if (hosts.Count > 0)
        {
            var host = hosts.Min();
            if (host != hostPeer)
            {
                if (hostPeer != 0 || table.Count > 0) reset = true;
                (hostPeer, hostVersion) = (host, -1);
                table.Clear(); changes.Clear(); sent.Clear(); version = 0;
            }
        }
        else if (hostPeer != 0)
        {
            (hostPeer, hostVersion) = (0, -1);
            table.Clear(); changes.Clear(); sent.Clear(); version = 0;
            reset = true;
        }
        var inLobby = hosts.Count > 0 || hostedLobbies > 0;
        if (!inLobby && wasInLobby)
        {
            table.Clear(); changes.Clear(); sent.Clear(); rates.Clear(); version = 0;
            reset = true;
        }
        wasInLobby = inLobby;
        return reset;
    }

    // Under gate: what the host sends the friends that are behind.
    List<(ulong To, Frame Frame, string What)> PlanSends(IReadOnlyCollection<ulong> hosted, IReadOnlyCollection<ulong> hosts, int hostedLobbies)
    {
        List<(ulong To, Frame Frame, string What)> sends = [];
        if (hosts.Count != 0 || hostedLobbies == 0) return sends;
        foreach (var friend in hosted)
        {
            var known = sent.TryGetValue(friend, out var had) ? had : -1;
            if (known == version) continue;
            if (known >= 0 && changes.Count > 0 && changes[0].Version <= known + 1)
                foreach (var change in changes.Where(c => c.Version > known))
                    sends.Add((friend, Delta(change), $"a row ({change.Mod})"));
            else sends.Add((friend, Snapshot(), $"the table (version {version})"));
            sent[friend] = version;
        }
        return sends;
    }

    // The whole table at its version; called with gate held.
    Frame Snapshot()
    {
        var frame = new Frame("mods.sharesnap").Add(version).Add(table.Sum(r => r.Value.Count));
        foreach (var (mod, set) in table.OrderBy(p => p.Key, StringComparer.Ordinal))
            foreach (var (key, value) in set)
                frame.Add(mod).Add(KeyText(key)).Add(value);
        return frame;
    }

    // One row at its version; called with gate held.
    static Frame Delta(Change change) =>
        new Frame("mods.share").Add(change.Version).Add(change.Mod).Add(KeyText(change.Key)).Add(change.Value ?? Array.Empty<byte>());

    // A frame from a peer's backend.
    public void FromPeer(ulong peer, Frame frame)
    {
        try
        {
            if (frame.Type == "mods.msg")
            {
                if (lobby.InHostedLobby(peer)) MessageFromJoiner(peer, frame);
                else if (lobby.HostsJoinedLobby(peer)) MessageFromHost(peer, frame);
            }
            else if (frame.Type is "mods.share" or "mods.sharesnap")
            {
                if (lobby.HostsJoinedLobby(peer))
                {
                    if (frame.Type == "mods.share") DeltaFromHost(peer, frame);
                    else SnapshotFromHost(peer, frame);
                }
                else if (lobby.InHostedLobby(peer)) RefuseShare(peer);
            }
        }
        catch (InvalidDataException error) { log($"Mod channel: bad {frame.Type} from {LobbyService.EntityId(peer)}: {error.Message}"); }
    }

    // A joiner's message: the host delivers it to its own game when it names the host or every machine, and relays it to
    // the members it names.
    void MessageFromJoiner(ulong peer, Frame frame)
    {
        if (frame.Count != 4) throw new InvalidDataException("mods.msg needs its mod, sender, target and data.");
        var mod = ModId(frame.Text(0));
        var target = frame.Text(2);
        var data = frame.Bytes(3);
        if (data.Length > MaxMessage) throw new InvalidDataException($"a message is at most {MaxMessage} bytes.");
        if (!TakeMessage(peer)) return;
        var hosted = lobby.HostedFriends();
        if (target == "host")
        {
            Enqueue(new Delivery("message", mod, peer, 0, data));
            return;
        }
        if (target == "all")
        {
            Enqueue(new Delivery("message", mod, peer, 0, data));
            foreach (var friend in hosted.Where(f => f != peer)) SendMessage(friend, peer, "all", mod, data);
            return;
        }
        if (!TryPeer(target, out var to)) throw new InvalidDataException("mods.msg's target is not a peer.");
        if (to == transport.LocalId) Enqueue(new Delivery("message", mod, peer, 0, data));
        else if (hosted.Contains(to)) SendMessage(to, peer, KeyText(to), mod, data);
        else log($"Mod channel: {LobbyService.EntityId(peer)} named {target}, which is not in this lobby; the message is dropped.");
    }

    // The host's message, or another member's the host relayed: only one this machine is named in.
    void MessageFromHost(ulong peer, Frame frame)
    {
        if (frame.Count != 4) throw new InvalidDataException("mods.msg needs its mod, sender, target and data.");
        var mod = ModId(frame.Text(0));
        var from = TryPeer(frame.Text(1), out var sender) ? sender : peer;
        var target = frame.Text(2);
        var data = frame.Bytes(3);
        if (data.Length > MaxMessage) throw new InvalidDataException($"a message is at most {MaxMessage} bytes.");
        if (target != "all" && !(TryPeer(target, out var to) && to == transport.LocalId)) return;
        Enqueue(new Delivery("message", mod, from, 0, data));
    }

    // One row of the host's table at a new version.
    void DeltaFromHost(ulong peer, Frame frame)
    {
        if (frame.Count != 4) throw new InvalidDataException("mods.share needs its version, mod, key and value.");
        var received = Version(frame.Text(0));
        var mod = ModId(frame.Text(1));
        var key = Key(frame.Text(2));
        var value = frame.Bytes(3);
        if (value.Length > MaxValue) throw new InvalidDataException($"a row's value is at most {MaxValue} bytes.");
        bool applied = false;
        lock (gate)
        {
            if (peer == hostPeer && received <= hostVersion) return;
            if (peer == hostPeer && received != hostVersion + 1) log($"Mod channel: a row's version {received} follows {hostVersion}; the host sends the table again when the game reconnects.");
            (hostPeer, hostVersion) = (peer, received);
            if (value.Length == 0) applied = Remove(mod, key);
            else applied = Set(mod, key, value);
        }
        if (applied) Enqueue(new Delivery("share", mod, 0, key, value.Length == 0 ? null : value));
    }

    // The whole table, after a lobby change or for a late joiner.
    void SnapshotFromHost(ulong peer, Frame frame)
    {
        if (frame.Count < 2) throw new InvalidDataException("mods.sharesnap needs its version and row count.");
        var received = Version(frame.Text(0));
        var count = frame.Int(1);
        if (count < 0 || count > (frame.Count - 2) / 3 || frame.Count != 2 + count * 3) throw new InvalidDataException("mods.sharesnap's row count does not match its fields.");
        Dictionary<string, SortedDictionary<ulong, byte[]>> rows = new(StringComparer.Ordinal);
        var at = 2;
        for (var i = 0; i < count; i++)
        {
            var mod = ModId(frame.Text(at++));
            var key = Key(frame.Text(at++));
            var value = frame.Bytes(at++);
            if (value.Length == 0 || value.Length > MaxValue) throw new InvalidDataException($"a row's value is 1 to {MaxValue} bytes.");
            if (!rows.TryGetValue(mod, out var set)) rows[mod] = set = new SortedDictionary<ulong, byte[]>();
            if (set.Count >= MaxRows) throw new InvalidDataException($"a mod's table is at most {MaxRows} rows.");
            set[key] = value;
        }
        lock (gate)
        {
            // A snapshot is the joined host's table, whole: it replaces this machine's even when its version is lower
            // (the host restarted, or a game reconnected and its own table went).
            (hostPeer, hostVersion) = (peer, received);
            table.Clear();
            foreach (var (mod, set) in rows) table[mod] = set;
        }
        Enqueue(new Delivery("reset", "", 0, 0, null));
        foreach (var (mod, set) in rows.OrderBy(p => p.Key, StringComparer.Ordinal))
            foreach (var (key, value) in set)
                Enqueue(new Delivery("share", mod, 0, key, value));
        log($"Mod channel: the lobby's table (version {received}, {count} row(s)) from {LobbyService.EntityId(peer)}.");
    }

    // A joiner's share frame: only the host's rows are accepted.
    void RefuseShare(ulong peer)
    {
        bool logIt;
        lock (gate) { logIt = DateTime.UtcNow - shareRefused > TimeSpan.FromSeconds(30); if (logIt) shareRefused = DateTime.UtcNow; }
        if (logIt) log($"Mod channel: {LobbyService.EntityId(peer)} sent the lobby's table; only the host's rows are accepted (dropped).");
    }

    // 64 messages a second from one machine, so a peer that skips its loader cannot flood the lobby.
    bool TakeMessage(ulong peer)
    {
        bool logIt = false, taken;
        lock (gate)
        {
            if (!rates.TryGetValue(peer, out var bucket)) rates[peer] = bucket = new Bucket();
            var now = DateTime.UtcNow;
            bucket.Tokens = Math.Min(MessagesPerSecond, bucket.Tokens + (now - bucket.At).TotalSeconds * MessagesPerSecond);
            bucket.At = now;
            taken = bucket.Tokens >= 1;
            if (taken) bucket.Tokens--;
            else if (now - bucket.Logged > TimeSpan.FromSeconds(30)) { bucket.Logged = now; logIt = true; }
        }
        if (logIt) log($"Mod channel: {LobbyService.EntityId(peer)} sent more than {MessagesPerSecond} messages a second; the rest are dropped.");
        return taken;
    }

    // A row into this machine's table; called with gate held. A new row past the cap is dropped.
    bool Set(string mod, ulong key, byte[] value)
    {
        if (!table.TryGetValue(mod, out var rows)) table[mod] = rows = new SortedDictionary<ulong, byte[]>();
        if (!rows.ContainsKey(key) && rows.Count >= MaxRows) return false;
        rows[key] = value;
        return true;
    }

    bool Remove(string mod, ulong key)
    {
        if (!table.TryGetValue(mod, out var rows) || !rows.Remove(key)) return false;
        if (rows.Count == 0) table.Remove(mod);
        return true;
    }

    void SendMessage(ulong to, ulong from, string target, string mod, byte[] data) =>
        transport.Send(to, new Frame("mods.msg").Add(mod).Add(KeyText(from)).Add(target).Add(data).Encode(), reliable: true);

    string LocalText() => KeyText(transport.LocalId);

    static string ModId(string text) =>
        text.Length is > 0 and <= MaxModId && !text.Any(char.IsControl) ? text : throw new InvalidDataException("a mod id is 1 to 80 characters without control characters.");

    static ulong Key(string text) =>
        text.Length is > 0 and <= 16 && ulong.TryParse(text, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var key) ? key :
            throw new InvalidDataException("a row's key is not hex.");

    static long Version(string text) =>
        long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var version) && version >= 0 ? version :
            throw new InvalidDataException("a table's version is not a number.");

    static string KeyText(ulong key) => key.ToString("x16", CultureInfo.InvariantCulture);

    static bool TryPeer(string text, out ulong peer)
    {
        peer = 0;
        return text.Length is > 0 and <= 16 && ulong.TryParse(text, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out peer);
    }

    // A field of hex digits (two to a byte), at most max bytes; "" is no bytes.
    static byte[] Hex(string text, int max)
    {
        if (text.Length % 2 != 0 || text.Length / 2 > max) throw new InvalidDataException($"a hex value is at most {max} bytes.");
        var bytes = new byte[text.Length / 2];
        for (var i = 0; i < bytes.Length; i++)
            if (!byte.TryParse(text.AsSpan(i * 2, 2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out bytes[i]))
                throw new InvalidDataException("a hex value has a digit that is not hex.");
        return bytes;
    }

    public async ValueTask DisposeAsync()
    {
        await timer.DisposeAsync();
        live.Received -= FromGame;
        live.StateChanged -= GameStateChanged;
        deliveries.Writer.TryComplete();
        await delivering;
    }
}
