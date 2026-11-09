using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Text;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// The mod channel end to end: a host and two joiners over the in-memory network, each game a fake mod loader on its
// live-edit pipe that sends the channel's lines and records what the backend sends. The host relays HF_TO_ALL, HF_TO_HOST
// and a peer's messages; a late joiner gets the table; a reconnect sends the table again; the host caps messages and rows;
// the lobby's end clears the table; a live protocol 1 game gets none of the lines.
static class ModChannelTests
{
    const ulong HostId = 0x110000100000041, FirstId = 0x110000100000042, SecondId = 0x110000100000043, OldId = 0x110000100000044;
    const string Champ = "test.champ";

    sealed class Game : IAsyncDisposable
    {
        // What the fake loader has loaded: a message or row for another mod is dropped, as native/mod-loader/channel.c does.
        static readonly HashSet<string> LoadedMods = [Champ];
        public readonly BlockingCollection<string> Lines = new();
        readonly BlockingCollection<string> outgoing = new();
        readonly CancellationTokenSource stop = new();
        readonly Task running;
        readonly int protocol;
        readonly object pipeGate = new();
        CancellationTokenSource? connection;

        public Game(string runId, int protocol)
        {
            this.protocol = protocol;
            running = Task.Run(() => Serve(runId));
        }

        // A line the game sends on its own (send, share): the loader's, without its newline.
        public void Send(string line) => outgoing.Add(line);
        // Ends this connection from the game's side without closing the pipe under Serve's writer: Serve ends its
        // writer, reader and client in that order.
        public void Drop() { lock (pipeGate) connection?.Cancel(); }
        public void Drain() { while (Lines.TryTake(out _, 50)) { } }

        async Task Serve(string runId)
        {
            while (!stop.IsCancellationRequested)
            {
                using var client = new NamedPipeClientStream(".", LiveEditServer.PipeName(runId), PipeDirection.InOut, PipeOptions.Asynchronous);
                try { await client.ConnectAsync(200, stop.Token); }
                catch (TimeoutException) { continue; }
                catch (OperationCanceledException) { return; }
                using var connected = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                lock (pipeGate) connection = connected;
                using var reader = new StreamReader(client, new UTF8Encoding(false));
                // Disposed by hand below: its final flush meets a pipe the backend may already have closed.
                var writer = new StreamWriter(client, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
                var writing = new SemaphoreSlim(1, 1);
                async Task Write(string text)
                {
                    await writing.WaitAsync();
                    try { await writer.WriteLineAsync(text); }
                    catch (Exception error) when (error is IOException or ObjectDisposedException) { }
                    finally { writing.Release(); }
                }
                await Write($"hello\t{protocol}\t{StatsSchema.Current.Build}\t{StatsSchema.Fingerprint}");
                var pushing = Task.Run(async () =>
                {
                    try { while (outgoing.TryTake(out var line, -1, connected.Token)) await Write(line); }
                    catch (Exception error) when (error is OperationCanceledException or IOException or ObjectDisposedException) { }
                });
                try
                {
                    while (await reader.ReadLineAsync(connected.Token) is { } line)
                    {
                        var parts = line.Split('\t');
                        var channel = parts[0] is "message" or "share";
                        if (!channel || parts.Length >= 3 && LoadedMods.Contains(parts[2])) Lines.Add(line);
                        if (parts.Length >= 2) await Write(parts[1] + "\tok\tnow");
                    }
                }
                catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
                connected.Cancel();
                try { await pushing; } catch (OperationCanceledException) { }
                try { writer.Dispose(); }
                catch (Exception error) when (error is IOException or ObjectDisposedException) { }
                lock (pipeGate) if (connection == connected) connection = null;
            }
        }

        // The matching lines among the next ones, waiting up to ten seconds for count of them.
        public List<string[]> WaitFor(Func<string[], bool> match, int count)
        {
            var found = new List<string[]>();
            for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until && found.Count < count;)
            {
                if (!Lines.TryTake(out var line, TimeSpan.FromMilliseconds(100))) continue;
                var parts = line.Split('\t');
                if (match(parts)) found.Add(parts);
            }
            return found;
        }

        // How many of the lines within the time match (the others are dropped).
        public int Count(Func<string[], bool> match, TimeSpan within)
        {
            var count = 0;
            for (var until = DateTime.UtcNow + within; DateTime.UtcNow < until;)
                if (Lines.TryTake(out var line, TimeSpan.FromMilliseconds(100)) && match(line.Split('\t'))) count++;
            return count;
        }

        public async ValueTask DisposeAsync()
        {
            stop.Cancel();
            await running;
        }
    }

    sealed class Machine : IAsyncDisposable
    {
        public readonly LobbyService Lobby;
        public readonly IPeerTransport Link;
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly LiveEditServer Live;
        public readonly ModChannel Channel;
        public readonly Game Game;

        public Machine(InMemoryNetwork network, ulong id, string name, int protocol, ConcurrentQueue<string> log)
        {
            Link = network.Join(id);
            Lobby = new LobbyService(Link, Inbox.Add, _ => { }, "mods");
            var runId = "channel-" + name + "-" + Guid.NewGuid().ToString("N")[..8];
            Live = new LiveEditServer(runId);
            Live.Start();
            Channel = new ModChannel(Link, Lobby, Live, line => log.Enqueue(name + ": " + line));
            Link.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type is "mods.msg" or "mods.share" or "mods.sharesnap") Channel.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
            Game = new Game(runId, protocol);
        }

        public async ValueTask DisposeAsync()
        {
            await Game.DisposeAsync();
            await Channel.DisposeAsync();
            await Live.DisposeAsync();
        }
    }

    static bool WaitConnected(LiveEditServer live)
    {
        for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until;)
        {
            if (live.Connected) return true;
            Thread.Sleep(10);
        }
        return false;
    }

    static bool WaitHosts(LobbyService lobby, ulong peer)
    {
        for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until;)
        {
            if (lobby.InHostedLobby(peer)) return true;
            Thread.Sleep(10);
        }
        return false;
    }

    static string Hex(ulong id) => id.ToString("x16", System.Globalization.CultureInfo.InvariantCulture);
    static bool Is(string[] parts, string type) => parts.Length > 0 && parts[0] == type;

    public static async Task Run(string root, Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var log = new ConcurrentQueue<string>();
        await using var host = new Machine(network, HostId, "host", 2, log);
        await using var first = new Machine(network, FirstId, "first", 2, log);
        await using var late = new Machine(network, SecondId, "late", 2, log);
        check(WaitConnected(host.Live) && WaitConnected(first.Live) && WaitConnected(late.Live), "The games' live pipes connect with protocol 2.");

        // The host creates a lobby; the first joiner joins. The late joiner comes after the first row.
        host.Lobby.FromLocal(MultiplayerTests.Create("c1", HostId));
        host.Inbox.TryTake(out var created, TimeSpan.FromSeconds(5));
        var connection = created!.Text(4);
        first.Lobby.FromLocal(MultiplayerTests.Join("j1", FirstId, connection));
        check(WaitHosts(host.Lobby, FirstId), "The host sees the first joiner.");

        // The host's mod sets a row: the joiner's game gets it as a delta.
        host.Game.Send("share\t" + Champ + "\t0000000000000001\t01020304");
        var delta = first.Game.WaitFor(parts => Is(parts, "share") && parts.Length == 5 && parts[2] == Champ, 1);
        check(delta.Count == 1 && delta[0][3] == "0000000000000001" && delta[0][4] == "01020304", "A host row reaches the joiner's game: " + string.Join("|", delta.SelectMany(p => p)));

        // A late joiner gets the whole table: a reset and its rows.
        late.Lobby.FromLocal(MultiplayerTests.Join("j2", SecondId, connection));
        check(WaitHosts(host.Lobby, SecondId), "The host sees the late joiner.");
        var snapshot = late.Game.WaitFor(parts => Is(parts, "sharereset") || Is(parts, "share"), 3);
        check(snapshot.Count(parts => Is(parts, "sharereset")) >= 1 && snapshot.Count(parts => Is(parts, "share") && parts[2] == Champ && parts[3] == "0000000000000001" && parts[4] == "01020304") == 1,
            "A late joiner gets the table: " + string.Join("|", snapshot.Select(p => string.Join(",", p))));

        // A second row changes the table for everyone.
        host.Game.Send("share\t" + Champ + "\t0000000000000002\t05");
        check(first.Game.WaitFor(parts => Is(parts, "share") && parts.Length == 5 && parts[3] == "0000000000000002", 1).Count == 1 &&
            late.Game.WaitFor(parts => Is(parts, "share") && parts.Length == 5 && parts[3] == "0000000000000002", 1).Count == 1,
            "A host row reaches both joiners.");

        // The host's message to every other machine: both joiners get it, the host's own game does not.
        host.Game.Send("send\t" + Champ + "\tall\tdeadbeef");
        var firstMessage = first.Game.WaitFor(parts => Is(parts, "message"), 1);
        var lateMessage = late.Game.WaitFor(parts => Is(parts, "message"), 1);
        check(firstMessage.Count == 1 && firstMessage[0].Length == 5 && firstMessage[0][2] == Champ && firstMessage[0][3] == Hex(HostId) && firstMessage[0][4] == "deadbeef" &&
            lateMessage.Count == 1 && lateMessage[0][2] == Champ && lateMessage[0][3] == Hex(HostId),
            "The host's HF_TO_ALL reaches both joiners: " + string.Join("|", firstMessage.SelectMany(p => p)));
        check(host.Game.Count(parts => Is(parts, "message"), TimeSpan.FromMilliseconds(500)) == 0, "The host's own HF_TO_ALL does not come back to it.");

        // A joiner's message to the host: only the host's game.
        first.Game.Send("send\t" + Champ + "\thost\t0102");
        var toHost = host.Game.WaitFor(parts => Is(parts, "message"), 1);
        check(toHost.Count == 1 && toHost[0][2] == Champ && toHost[0][3] == Hex(FirstId) && toHost[0][4] == "0102", "A joiner's HF_TO_HOST reaches the host: " + string.Join("|", toHost.SelectMany(p => p)));

        // A joiner's message to one peer: only that peer.
        first.Game.Send("send\t" + Champ + "\t" + Hex(SecondId) + "\t03");
        var toPeer = late.Game.WaitFor(parts => Is(parts, "message"), 1);
        check(toPeer.Count == 1 && toPeer[0][2] == Champ && toPeer[0][3] == Hex(FirstId) && toPeer[0][4] == "03", "A joiner's message reaches the peer it names: " + string.Join("|", toPeer.SelectMany(p => p)));
        check(host.Game.Count(parts => Is(parts, "message"), TimeSpan.FromMilliseconds(500)) == 0, "A peer's message does not reach the host.");

        // A joiner's message to every other machine: the host and the other joiner, not itself.
        first.Game.Send("send\t" + Champ + "\tall\t04");
        var hostAll = host.Game.WaitFor(parts => Is(parts, "message"), 1);
        var lateAll = late.Game.WaitFor(parts => Is(parts, "message"), 1);
        check(hostAll.Count == 1 && hostAll[0][4] == "04" && lateAll.Count == 1 && lateAll[0][4] == "04", "A joiner's HF_TO_ALL reaches the host and the other joiner.");
        check(first.Game.Count(parts => Is(parts, "message"), TimeSpan.FromMilliseconds(500)) == 0, "A joiner's HF_TO_ALL does not come back to it.");

        // A message for a mod the games do not load: the backend relays it, and each game's loader drops it (channel.c).
        host.Game.Send("send\tno.such.mod\tall\t01");
        check(first.Game.Count(parts => Is(parts, "message") && parts[2] == "no.such.mod", TimeSpan.FromSeconds(1)) == 0 &&
            late.Game.Count(parts => Is(parts, "message") && parts[2] == "no.such.mod", TimeSpan.FromSeconds(1)) == 0,
            "A message for a mod the games do not load is dropped.");

        // The game reconnects: the backend sends its table again.
        host.Game.Drain();
        host.Game.Drop();
        var again = host.Game.WaitFor(parts => Is(parts, "sharereset") || Is(parts, "share"), 3);
        check(again.Count(parts => Is(parts, "sharereset")) == 1 && again.Count(parts => Is(parts, "share") && parts.Length == 5 && parts[2] == Champ) == 2,
            "Reconnected, the host's game is sent the table again: " + string.Join("|", again.Select(p => string.Join(",", p))));

        // A live protocol 1 game gets none of the channel's lines.
        await using var old = new Machine(network, OldId, "old", 1, log);
        check(WaitConnected(old.Live), "The protocol 1 game's live pipe connects.");
        old.Lobby.FromLocal(MultiplayerTests.Join("j3", OldId, connection));
        check(WaitHosts(host.Lobby, OldId), "The host sees the protocol 1 joiner.");
        host.Game.Send("send\t" + Champ + "\t" + Hex(OldId) + "\tbeef");
        check(old.Game.Count(parts => Is(parts, "message") || Is(parts, "share") || Is(parts, "sharereset"), TimeSpan.FromSeconds(2)) == 0,
            "A protocol 1 game gets no message, share or sharereset lines.");

        // The caps: a message over 1024 bytes and more than 64 a second are dropped by the host.
        first.Game.Send("send\t" + Champ + "\tall\t" + new string('a', (1024 + 1) * 2));
        check(late.Game.Count(parts => Is(parts, "message"), TimeSpan.FromSeconds(1)) == 0, "A message over 1024 bytes is dropped.");
        for (var i = 0; i < 200; i++) first.Game.Send("send\t" + Champ + "\tall\t01");
        var relayed = late.Game.Count(parts => Is(parts, "message"), TimeSpan.FromSeconds(2));
        check(relayed is > 0 and < 200 && log.Any(l => l.Contains("more than 64 messages a second")), $"The host caps a sender's messages: {relayed} of 200 relayed.");

        // The row cap: a 513th row is dropped.
        for (var i = 0; i < 513; i++) host.Game.Send($"share\t{Champ}\t{(0x1000 + i):x16}\t01");
        for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until && !log.Any(l => l.Contains("table holds 512 rows"));) Thread.Sleep(50);
        check(log.Any(l => l.Contains("table holds 512 rows")), "A row past 512 is dropped.");
        host.Game.Send("share\t" + Champ + "\t0000000000000abc\t" + new string('a', 65 * 2));
        for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until && !log.Any(l => l.Contains("at most 64 bytes"));) Thread.Sleep(50);
        check(log.Any(l => l.Contains("at most 64 bytes")), "A value over 64 bytes is dropped.");

        // A joiner's share frame is not the host's: it cannot grow the table.
        first.Link.Send(HostId, new Frame("mods.share").Add(1000).Add("no.such.mod").Add("0000000000000001").Add(new byte[] { 1 }).Encode(), reliable: true);
        for (var until = DateTime.UtcNow.AddSeconds(5); DateTime.UtcNow < until && !log.Any(l => l.Contains("only the host's rows are accepted"));) Thread.Sleep(50);
        check(log.Any(l => l.Contains("only the host's rows are accepted")), "A joiner cannot set the table (an unknown mod id's row included).");

        // The lobby ends: the game is told every row goes.
        host.Game.Drain();
        host.Lobby.LocalDisconnected();
        var cleared = host.Game.WaitFor(parts => Is(parts, "sharereset"), 1);
        check(cleared.Count == 1, "The lobby's end clears the table in the game.");
    }
}
