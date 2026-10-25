using System.Collections.Concurrent;
using System.Diagnostics;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// Mod sync's speed (docs/multiplayer.md "Mod sync speed"): the joiner's window, its pipelining across files and the host's
// pacing over a simulated path with a round trip, and over a slow link whose queue game traffic shares. Steam's own
// send rate is not simulated (SteamPeerTransport.SendRate sets it); every mod is synthetic.
static partial class ModSyncTests
{
    // The window: grows while chunks come back quickly, takes a quarter off (once a round trip) when they queue, never
    // below MinWindow or above MaxWindow, and halves its ceiling for a host that refuses requests for being too many.
    static void Window(Action<bool, string> check)
    {
        var window = new DownloadWindow();
        var quick = TimeSpan.FromMilliseconds(150);
        check(window.Size == ModSync.Window, "A download starts with Window chunks asked for.");
        for (var i = 0; i < 40; i++) window.Answered(quick);
        check(window.Size == ModSync.MaxWindow, $"Chunks that come back quickly open the window to MaxWindow ({window.Size}).");
        var late = quick + ModSync.TargetDelay + TimeSpan.FromMilliseconds(50);
        window.Answered(late);
        check(window.Size == ModSync.MaxWindow * 3 / 4, $"A chunk that queued takes a quarter off ({window.Size}).");
        for (var i = 0; i < window.Size - 2; i++) window.Answered(late);
        check(window.Size == ModSync.MaxWindow * 3 / 4, $"Once a round trip: the chunks behind it waited in the same queue ({window.Size}).");
        for (var i = 0; i < 200; i++) window.Answered(late);
        check(window.Size == ModSync.MinWindow, $"Never below MinWindow ({window.Size}).");
        window.Answered(quick);
        check(window.Size == ModSync.MinWindow, $"Once cut, it grows by about one a round trip, not one a chunk ({window.Size}).");
        for (var i = 0; i < 6; i++) window.Answered(quick);
        check(window.Size > ModSync.MinWindow, $"... but it grows ({window.Size}).");

        var crowded = new DownloadWindow();
        for (var i = 0; i < 40; i++) crowded.Answered(quick);
        crowded.Crowded();
        crowded.Crowded();
        check(crowded.Size == ModSync.MaxWindow / 2 && crowded.Ceiling == ModSync.MaxWindow / 2,
            $"A host that refuses requests for being too many halves the ceiling once for one crowd ({crowded.Size}, ceiling {crowded.Ceiling}).");
        for (var i = 0; i < 400; i++) crowded.Answered(quick);
        check(crowded.Size == ModSync.MaxWindow / 2, $"The window stays under that ceiling ({crowded.Size}).");
    }

    // Two downloads of a 6 MB mod of 63 files: over a 120 ms round trip with no bottleneck, and over a 2 MiB/s link
    // (80 ms round trip) while the host sends a small game frame every 50 ms down the same link.
    static async Task Speed(string root, Action<bool, string> check)
    {
        root = Path.Combine(root, "mod-sync-speed");
        var hostMods = Path.Combine(root, "host", "mods");
        Write(Path.Combine(hostMods, "big"), ModCatalog.ManifestName, """{"format":1,"id":"test.big","name":"Big","version":"1"}""");
        Write(Path.Combine(hostMods, "big"), StatsData.FileName, Stats);
        var big = new byte[6 << 20];
        new Random(5).NextBytes(big);
        Directory.CreateDirectory(Path.Combine(hostMods, "big", "assets"));
        File.WriteAllBytes(Path.Combine(hostMods, "big", "assets", "big.bin"), big);
        for (var i = 0; i < 60; i++) Write(Path.Combine(hostMods, "big"), $"notes/note{i:00}.txt", new string((char)('a' + i % 26), 900 + i));
        var hostSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(hostMods).Mods));
        var bytes = hostSet.Mods.Single().Bytes;
        check(hostSet.Mods.Single().Class == ModClass.Content && hostSet.Mods.Single().Files.Count == 63, "The speed fixture is one content mod of 63 files.");

        // Round trips: the old design (4 chunks, one file at a time, 1 MiB/s) took about 6 s for the bytes and 7 s for
        // 63 files' round trips.
        var (took, _) = await Download(root, "rtt", TimeSpan.FromMilliseconds(60), double.PositiveInfinity, hostSet, bytes, probe: false, check);
        check(took < TimeSpan.FromSeconds(4), $"6 MB in 63 files over a 120 ms round trip downloads in {took.TotalSeconds:0.00} s (at most 4 s).");

        // Next door (2 ms round trip, no bottleneck): the host's pacing is the limit, PeerRate a machine, never more.
        var (near, _) = await Download(root, "near", TimeSpan.FromMilliseconds(1), double.PositiveInfinity, hostSet, bytes, probe: false, check);
        var cap = TimeSpan.FromSeconds((double)bytes / ModSync.PeerRate) - ModSync.PaceBurst;
        check(near >= cap && near < cap * 2 + TimeSpan.FromSeconds(0.5), $"Next door it downloads at about PeerRate: {near.TotalSeconds:0.00} s (PeerRate allows {cap.TotalSeconds:0.00} s).");

        // A slow link: the window fills it without a long queue ahead of game traffic (a fixed MaxWindow would queue
        // 2 MiB there, a second at this rate).
        const double Rate = 2 << 20;
        var (slow, probes) = await Download(root, "slow", TimeSpan.FromMilliseconds(40), Rate, hostSet, bytes, probe: true, check);
        var floor = TimeSpan.FromSeconds(bytes / Rate);
        probes.Sort();
        var (median, worst) = (probes[probes.Count / 2], probes[^1]);
        Console.WriteLine(FormattableString.Invariant($"mod sync speed, 6 MB in 63 files: next door {near.TotalSeconds:0.00} s (PeerRate allows {cap.TotalSeconds:0.00} s); ") +
            FormattableString.Invariant($"120 ms round trip {took.TotalSeconds:0.00} s; 2 MiB/s link {slow.TotalSeconds:0.00} s (the link alone {floor.TotalSeconds:0.00} s), ") +
            FormattableString.Invariant($"game frames on it median {median.TotalMilliseconds:0} ms, worst {worst.TotalMilliseconds:0} ms (40 ms one way)"));
        check(slow < floor * 2, $"Over a 2 MiB/s link it uses the link: {slow.TotalSeconds:0.00} s (the link alone takes {floor.TotalSeconds:0.00} s).");
        check(probes.Count > 10 && median < TimeSpan.FromMilliseconds(300) && worst < TimeSpan.FromMilliseconds(700),
            $"Game frames on the same link wait little: {probes.Count} frames, median {median.TotalMilliseconds:0} ms, worst {worst.TotalMilliseconds:0} ms (40 ms one way).");
    }

    // One download from a fresh host to a fresh joiner over a simulated path: how long from accept to ready, and (probe) how
    // long the host's small frames took to arrive meanwhile.
    static async Task<(TimeSpan Took, List<TimeSpan> Probes)> Download(string root, string name, TimeSpan oneWay, double rate, ModSet hostSet, long bytes, bool probe,
        Action<bool, string> check)
    {
        using var network = new SimulatedNetwork(oneWay, rate);
        var hostLink = network.Join(HostId);
        var guestLink = network.Join(GuestId);
        var probes = new List<TimeSpan>();
        guestLink.Received += (_, frame) =>
        {
            var decoded = Frame.Decode(frame);
            if (decoded.Type == "test.probe") lock (probes) probes.Add(network.Clock.Elapsed - TimeSpan.FromTicks(long.Parse(decoded.Text(0))));
        };
        var host = new Node(hostLink, Path.Combine(root, name, "host"), hostSet, (_, _) => false);
        var guest = new Node(guestLink, Path.Combine(root, name, "guest"), new ModSet("none", []), (_, _) => false);
        var offer = Refused(host, guest, check, name, "offer");
        check(offer.Bytes == bytes, $"{name}: the offer is the whole mod ({offer.Bytes} of {bytes}).");
        using var stopProbes = new CancellationTokenSource();
        var probing = probe ? Task.Run(async () =>
        {
            while (!stopProbes.IsCancellationRequested)
            {
                hostLink.Send(GuestId, new Frame("test.probe").Add(network.Clock.Elapsed.Ticks).Encode(), reliable: false);
                try { await Task.Delay(50, stopProbes.Token); } catch (OperationCanceledException) { }
            }
        }) : Task.CompletedTask;
        var clock = Stopwatch.StartNew();
        guest.Sync.Answer(["syncanswer", offer.Version.ToString(), "accept"]);
        var ready = guest.Shown.Next(check, "ready", "failed");
        var took = clock.Elapsed;
        stopProbes.Cancel();
        await probing;
        check(ready.State == "ready" && ready.Done == bytes, $"{name}: downloaded ({ready.State}: {ready.Message}).");
        await guest.Sync.DisposeAsync();
        await host.Sync.DisposeAsync();
        lock (probes) return (took, [.. probes]);
    }
}

// Backends over a simulated path, for the speed checks: each direction between two machines is a link that sends rate
// bytes a second, one frame after another (a frame waits for those before it: the bottleneck's queue), and delivers it
// oneWay later, in order. Every machine is every other's Steam friend.
sealed class SimulatedNetwork(TimeSpan oneWay, double rate) : IDisposable
{
    readonly ConcurrentDictionary<ulong, Endpoint> endpoints = new();
    readonly Dictionary<(ulong From, ulong To), TimeSpan> free = [];
    public Stopwatch Clock { get; } = Stopwatch.StartNew();

    public IPeerTransport Join(ulong id) => endpoints[id] = new Endpoint(this, id);

    void Carry(ulong from, ulong to, byte[] frame)
    {
        if (!endpoints.TryGetValue(to, out var target)) return;
        TimeSpan arrival;
        lock (free)
        {
            var now = Clock.Elapsed;
            var start = free.TryGetValue((from, to), out var at) && at > now ? at : now;
            var sent = start + TimeSpan.FromSeconds(frame.Length / rate);
            free[(from, to)] = sent;
            arrival = sent + oneWay;
        }
        target.Arrive(arrival, from, frame);
    }

    public void Dispose()
    {
        foreach (var endpoint in endpoints.Values) endpoint.Dispose();
    }

    sealed class Endpoint : IPeerTransport
    {
        readonly SimulatedNetwork network;
        readonly PriorityQueue<(ulong From, byte[] Frame), (TimeSpan At, long Order)> arriving = new();
        long order;
        bool stopped;

        public Endpoint(SimulatedNetwork network, ulong id)
        {
            this.network = network;
            LocalId = id;
            new Thread(Deliver) { IsBackground = true, Name = "Simulated link " + id.ToString("X") }.Start();
        }

        public ulong LocalId { get; }
        public event Action<ulong, byte[]>? Received;
        public void Start() { }
        public void Send(ulong peer, byte[] frame, bool reliable) => network.Carry(LocalId, peer, frame.ToArray());

        public void Arrive(TimeSpan at, ulong from, byte[] frame)
        {
            lock (arriving)
            {
                arriving.Enqueue((from, frame), (at, order++));
                Monitor.Pulse(arriving);
            }
        }

        void Deliver()
        {
            while (true)
            {
                (ulong From, byte[] Frame) next;
                lock (arriving)
                    while (true)
                    {
                        if (stopped) return;
                        if (!arriving.TryPeek(out _, out var when)) { Monitor.Wait(arriving); continue; }
                        var wait = when.At - network.Clock.Elapsed;
                        if (wait <= TimeSpan.Zero) { next = arriving.Dequeue(); break; }
                        Monitor.Wait(arriving, wait);
                    }
                try { Received?.Invoke(next.From, next.Frame); }
                catch (Exception error) { Console.Error.WriteLine("Simulated link: a handler failed: " + error); }
            }
        }

        public void Dispose()
        {
            lock (arriving)
            {
                stopped = true;
                Monitor.Pulse(arriving);
            }
        }
    }
}
