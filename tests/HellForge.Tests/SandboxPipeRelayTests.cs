using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;
using HellForge.Core;

// F.4e's relay: both real pipe names cross the loopback link, many game connections work at once, either side may close,
// and an oversized frame is refused rather than allocated. The pipes are synthetic; no game, Wine or Proton is started.
static class SandboxPipeRelayTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check)
    {
        test("The sandbox pipe relay carries bytes both ways for the backend and live pipes", () => Run(async () =>
        {
            var runId = "relay" + Guid.NewGuid().ToString("N")[..12];
            var names = SandboxPipeRelay.Names(runId);
            await using var backend = new EchoPipe("real-" + names.Backend);
            await using var live = new EchoPipe("real-" + names.Live);
            backend.Start();
            live.Start();
            var pipes = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                [names.Backend] = "real-" + names.Backend, [names.Live] = "real-" + names.Live
            };
            await using var relay = RelayFixture.Start(runId, pipes);

            var first = Enumerable.Range(0, 4096).Select(i => (byte)i).ToArray();
            check((await Exchange(names.Backend, first)).SequenceEqual(first), "The backend pipe did not return the same bytes.");
            var second = Encoding.UTF8.GetBytes("hello\tfrom the live editor\n");
            check((await Exchange(names.Live, second)).SequenceEqual(second), "The live pipe did not return the same bytes.");
        }));

        test("The sandbox pipe relay refuses a domain with the wrong run token", () => Run(async () =>
        {
            var runId = "relay" + Guid.NewGuid().ToString("N")[..12];
            var names = SandboxPipeRelay.Names(runId);
            await using var backend = new EchoPipe("real-" + names.Backend);
            backend.Start();
            var pipes = new Dictionary<string, string>(StringComparer.Ordinal) { [names.Backend] = "real-" + names.Backend };
            await using var host = new SandboxPipeRelayHost(pipes, Guid.NewGuid().ToString("N"));
            host.Start();
            await using var domain = new SandboxPipeRelayDomain(runId, host.Port, Guid.NewGuid().ToString("N"));
            domain.Start();
            var refused = false;
            try { _ = await Exchange(names.Backend, [1, 2, 3]); }
            catch (Exception error) when (error is IOException or ObjectDisposedException) { refused = true; }
            check(refused, "A connection with the wrong token was served.");
        }));

        test("The sandbox pipe relay accepts many connections and a closed game's next connection", () => Run(async () =>
        {
            var runId = "relay" + Guid.NewGuid().ToString("N")[..12];
            var names = SandboxPipeRelay.Names(runId);
            await using var backend = new EchoPipe("real-" + names.Backend);
            backend.Start();
            var pipes = new Dictionary<string, string>(StringComparer.Ordinal) { [names.Backend] = "real-" + names.Backend };
            await using var relay = RelayFixture.Start(runId, pipes);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));

            var many = await Task.WhenAll(Enumerable.Range(0, 8).Select(async index =>
            {
                var payload = Encoding.UTF8.GetBytes("connection " + index + " " + new string((char)('a' + index), 2000));
                return (await Exchange(names.Backend, payload, timeout.Token)).SequenceEqual(payload);
            }));
            check(many.All(ok => ok), "Not every concurrent connection returned its own bytes.");

            var before = Encoding.UTF8.GetBytes("before closing the game side");
            check((await Exchange(names.Backend, before, timeout.Token)).SequenceEqual(before), "The first connection failed.");
            var after = Encoding.UTF8.GetBytes("after closing the game side");
            check((await Exchange(names.Backend, after, timeout.Token)).SequenceEqual(after), "A closed connection left the pipe unusable.");
        }));

        test("The sandbox pipe relay closes the game pipe when the trusted pipe side closes", () => Run(async () =>
        {
            var runId = "relay" + Guid.NewGuid().ToString("N")[..12];
            var names = SandboxPipeRelay.Names(runId);
            await using var backend = new EchoPipe("real-" + names.Backend);
            backend.Start();
            var pipes = new Dictionary<string, string>(StringComparer.Ordinal) { [names.Backend] = "real-" + names.Backend };
            await using var relay = RelayFixture.Start(runId, pipes);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            using var game = new NamedPipeClientStream(".", names.Backend, PipeDirection.InOut, PipeOptions.Asynchronous);
            await game.ConnectAsync(5000);
            var payload = Encoding.UTF8.GetBytes("trusted side closes");
            await game.WriteAsync(payload, timeout.Token);
            await game.FlushAsync(timeout.Token);
            check((await ReadExactlyAsync(game, payload.Length, timeout.Token)).SequenceEqual(payload), "The test echo failed.");

            await backend.DisposeAsync();
            var closed = false;
            try { closed = await game.ReadAsync(new byte[1], timeout.Token) == 0; }
            catch (Exception error) when (error is IOException or ObjectDisposedException) { closed = true; }
            check(closed, "The game pipe stayed open after the trusted pipe closed.");
        }));

        test("The sandbox pipe relay refuses zero and oversized frames", () =>
        {
            var oversized = new MemoryStream();
            var header = new byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)SandboxPipeRelay.MaxChunkBytes + 1u);
            oversized.Write(header);
            oversized.Position = 0;
            var refused = false;
            try { _ = SandboxPipeRelay.ReadFrameAsync(oversized, CancellationToken.None).GetAwaiter().GetResult(); }
            catch (InvalidDataException) { refused = true; }
            check(refused, "An oversized frame was accepted.");

            var zero = new MemoryStream(new byte[4]);
            refused = false;
            try { _ = SandboxPipeRelay.ReadFrameAsync(zero, CancellationToken.None).GetAwaiter().GetResult(); }
            catch (InvalidDataException) { refused = true; }
            check(refused, "A zero-length frame was accepted.");
        });
    }

    static void Run(Func<Task> action) => action().GetAwaiter().GetResult();

    static async Task<byte[]> Exchange(string pipeName, byte[] payload, CancellationToken cancellation = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        using var game = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        await game.ConnectAsync(5000);
        await game.WriteAsync(payload, timeout.Token);
        await game.FlushAsync(timeout.Token);
        return await ReadExactlyAsync(game, payload.Length, timeout.Token);
    }

    static async Task<byte[]> ReadExactlyAsync(Stream stream, int count, CancellationToken cancellation)
    {
        var bytes = new byte[count];
        var read = 0;
        while (read < bytes.Length)
        {
            var got = await stream.ReadAsync(bytes.AsMemory(read), cancellation);
            if (got == 0) throw new EndOfStreamException("The relay closed before the expected bytes arrived.");
            read += got;
        }
        return bytes;
    }

    sealed class RelayFixture : IAsyncDisposable
    {
        readonly SandboxPipeRelayHost host;
        readonly SandboxPipeRelayDomain domain;

        RelayFixture(SandboxPipeRelayHost host, SandboxPipeRelayDomain domain)
        {
            this.host = host;
            this.domain = domain;
        }

        public static RelayFixture Start(string runId, IReadOnlyDictionary<string, string> pipes)
        {
            var token = Guid.NewGuid().ToString("N");
            var host = new SandboxPipeRelayHost(pipes, token);
            host.Start();
            var domain = new SandboxPipeRelayDomain(runId, host.Port, token);
            try { domain.Start(); }
            catch
            {
                host.DisposeAsync().AsTask().GetAwaiter().GetResult();
                throw;
            }
            return new(host, domain);
        }

        public async ValueTask DisposeAsync()
        {
            await domain.DisposeAsync();
            await host.DisposeAsync();
        }
    }

    sealed class EchoPipe : IAsyncDisposable
    {
        readonly string name;
        readonly CancellationTokenSource stop = new();
        readonly List<Task> connections = [];
        Task? accepting;
        bool disposed;

        public EchoPipe(string name) => this.name = name;

        public void Start() => accepting = Task.Run(AcceptLoopAsync);

        async Task AcceptLoopAsync()
        {
            while (!stop.IsCancellationRequested)
            {
                NamedPipeServerStream pipe;
                try { pipe = PipeServer.Create(name, SandboxPipeRelay.MaxConnections); }
                catch (Exception error) when (error is IOException or InvalidOperationException or UnauthorizedAccessException) { return; }
                try
                {
                    using (stop.Token.Register(static state => ((NamedPipeServerStream)state!).Dispose(), pipe))
                        await pipe.WaitForConnectionAsync(stop.Token);
                }
                catch (Exception error) when (error is OperationCanceledException or IOException or ObjectDisposedException or InvalidOperationException)
                {
                    pipe.Dispose();
                    return;
                }
                var task = Task.Run(() => EchoAsync(pipe));
                lock (connections) connections.Add(task);
            }
        }

        async Task EchoAsync(NamedPipeServerStream pipe)
        {
            try
            {
                using (pipe)
                using (stop.Token.Register(static state => ((IDisposable)state!).Dispose(), pipe))
                {
                    var buffer = new byte[8192];
                    while (true)
                    {
                        var read = await pipe.ReadAsync(buffer, stop.Token);
                        if (read == 0) return;
                        await pipe.WriteAsync(buffer.AsMemory(0, read), stop.Token);
                        await pipe.FlushAsync(stop.Token);
                    }
                }
            }
            catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
        }

        public async ValueTask DisposeAsync()
        {
            if (disposed) return;
            disposed = true;
            stop.Cancel();
            if (accepting != null)
                try { await accepting; }
                catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
            Task[] running;
            lock (connections) running = [.. connections];
            if (running.Length > 0)
                try { await Task.WhenAll(running); }
                catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
            stop.Dispose();
        }
    }
}
