using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core;

// F.4e's named-pipe relay. A game in the overlay prefix and the loader's live editor connect to pipes in the overlay's
// wineserver; the trusted session runner serves those same names from its real prefix. The two sides speak a small
// length-prefixed protocol over loopback TCP, which both wineservers share in the session's network namespace. TCP was
// chosen over a Unix socket because the backend's HTTP loopback already proves this transport under Wine, while a PE
// process using AF_UNIX is still a new Wine contract.
//
// Every frame is u32 little-endian length, then that many bytes, at most 64 KiB. A connection's first frame is
// "<token>\n<pipe name>"; the trusted side accepts only this run's two names. Both sides apply one byte budget per relay
// and cap concurrent connections, so a compromised domain cannot spend memory or bandwidth without limit.
public static class SandboxPipeRelay
{
    public const int MaxChunkBytes = 64 * 1024;
    public const int MaxConnections = 16;
    public const int MaxBytesPerSecond = 8 * 1024 * 1024;
    public const int MaxBurstBytes = 4 * 1024 * 1024;
    internal const int HandshakeSeconds = 5;

    internal static readonly byte[] OkBytes = "ok"u8.ToArray();
    internal static readonly byte[] RefusedBytes = "no"u8.ToArray();

    // The backend's lobby/party pipe and the loader's live-edit pipe for one run id.
    public static (string Backend, string Live) Names(string runId)
    {
        if (runId.Length is 0 or > 80 || runId.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '-' or '_')))
            throw new ArgumentException("The run id must be 1-80 letters, digits, '-' or '_'.");
        return ("hellforge-" + runId, "hellforge-live-" + runId);
    }

    internal static bool IsToken(string? token) =>
        token is { Length: 32 } && token!.All(char.IsAsciiHexDigit);

    internal static bool IsPipeName(string name) =>
        name.Length is > 0 and <= 100 && name.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.');

    internal static bool FixedEquals(string? given, string expected)
    {
        if (given == null) return false;
        var left = Encoding.ASCII.GetBytes(given);
        var right = Encoding.ASCII.GetBytes(expected);
        return left.Length == right.Length && CryptographicOperations.FixedTimeEquals(left, right);
    }

    internal static async Task<byte[]?> ReadFrameAsync(Stream stream, CancellationToken cancellation)
    {
        var header = new byte[4];
        var read = await stream.ReadAsync(header, cancellation);
        if (read == 0) return null;
        while (read < header.Length)
        {
            var got = await stream.ReadAsync(header.AsMemory(read), cancellation);
            if (got == 0) throw new EndOfStreamException("The pipe relay ended inside a frame header.");
            read += got;
        }
        var length = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (length is 0 or > MaxChunkBytes) throw new InvalidDataException("The pipe relay frame length is out of bounds.");
        var payload = new byte[length];
        read = 0;
        while (read < payload.Length)
        {
            var got = await stream.ReadAsync(payload.AsMemory(read), cancellation);
            if (got == 0) throw new EndOfStreamException("The pipe relay ended inside a frame.");
            read += got;
        }
        return payload;
    }

    internal static async Task WriteFrameAsync(Stream stream, ReadOnlyMemory<byte> payload, CancellationToken cancellation)
    {
        if (payload.Length is 0 or > MaxChunkBytes) throw new InvalidDataException("The pipe relay frame length is out of bounds.");
        var header = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)payload.Length);
        await stream.WriteAsync(header, cancellation);
        await stream.WriteAsync(payload, cancellation);
        await stream.FlushAsync(cancellation);
    }

    internal static async Task PumpAsync(Stream pipe, Stream link, RelayBudget budget, CancellationToken cancellation)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        var toLink = PipeToLinkAsync(pipe, link, budget, stop.Token);
        var toPipe = LinkToPipeAsync(link, pipe, budget, stop.Token);
        try { await Task.WhenAny(toLink, toPipe); }
        finally
        {
            stop.Cancel();
            try { await Task.WhenAll(toLink, toPipe); }
            catch (Exception error) when (Expected(error)) { }
        }
    }

    static async Task PipeToLinkAsync(Stream pipe, Stream link, RelayBudget budget, CancellationToken cancellation)
    {
        var buffer = new byte[MaxChunkBytes];
        while (true)
        {
            var read = await pipe.ReadAsync(buffer, cancellation);
            if (read == 0) return;
            if (!budget.Take(read)) throw new InvalidDataException("The pipe relay's byte rate is over its limit.");
            await WriteFrameAsync(link, buffer.AsMemory(0, read), cancellation);
        }
    }

    static async Task LinkToPipeAsync(Stream link, Stream pipe, RelayBudget budget, CancellationToken cancellation)
    {
        while (await ReadFrameAsync(link, cancellation) is { } frame)
        {
            if (!budget.Take(frame.Length)) throw new InvalidDataException("The pipe relay's byte rate is over its limit.");
            await pipe.WriteAsync(frame, cancellation);
            await pipe.FlushAsync(cancellation);
        }
    }

    internal static bool Expected(Exception error) =>
        error is IOException or OperationCanceledException or SocketException or ObjectDisposedException or InvalidDataException or InvalidOperationException;

    internal static void Cancel(CancellationTokenSource source)
    {
        try { source.Cancel(); }
        catch (Exception error) when (error is ObjectDisposedException or AggregateException) { }
    }

    internal sealed class RelayBudget
    {
        readonly object gate = new();
        double bytes = MaxBurstBytes;
        long stamp = Stopwatch.GetTimestamp();

        public bool Take(int count)
        {
            if (count <= 0) return false;
            lock (gate)
            {
                var now = Stopwatch.GetTimestamp();
                bytes = Math.Min(MaxBurstBytes, bytes + (now - stamp) * (double)MaxBytesPerSecond / Stopwatch.Frequency);
                stamp = now;
                if (count > bytes) return false;
                bytes -= count;
                return true;
            }
        }
    }
}

// The trusted end, in the real prefix's session runner: it accepts the domain's loopback connections and connects each
// one to the pipe of the same name in the real wineserver.
public sealed class SandboxPipeRelayHost
{
    readonly IReadOnlyDictionary<string, string> pipes;
    readonly string token;
    readonly Action<string>? log;
    readonly TcpListener listener = new(IPAddress.Loopback, 0);
    readonly CancellationTokenSource stop = new();
    readonly SandboxPipeRelay.RelayBudget budget = new();
    readonly ConcurrentDictionary<long, Task> connections = new();
    Task? accepting;
    int active;
    long nextId;
    bool started;

    public string Token => token;
    public int Port { get; private set; }

    public SandboxPipeRelayHost(string runId, string token, Action<string>? log = null)
        : this(Identity(runId), token, log) { }

    internal SandboxPipeRelayHost(IReadOnlyDictionary<string, string> pipes, string token, Action<string>? log = null)
    {
        if (pipes.Count is < 1 or > 2 || pipes.Any(entry => !SandboxPipeRelay.IsPipeName(entry.Key) || !SandboxPipeRelay.IsPipeName(entry.Value)))
            throw new ArgumentException("The pipe relay's names are not bounded pipe names.");
        if (!SandboxPipeRelay.IsToken(token)) throw new ArgumentException("The pipe relay token is not 32 hex digits.");
        this.pipes = pipes;
        this.token = token;
        this.log = log;
    }

    static IReadOnlyDictionary<string, string> Identity(string runId)
    {
        var (backend, live) = SandboxPipeRelay.Names(runId);
        return new Dictionary<string, string>(StringComparer.Ordinal) { [backend] = backend, [live] = live };
    }

    public void Start()
    {
        if (started) throw new InvalidOperationException("The pipe relay is already started.");
        listener.Start(32);
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        started = true;
        accepting = Task.Run(AcceptLoopAsync);
        log?.Invoke($"Pipe relay listening on 127.0.0.1:{Port}.");
    }

    public void Stop()
    {
        if (!started) return;
        SandboxPipeRelay.Cancel(stop);
        try { listener.Stop(); }
        catch (Exception error) when (error is SocketException or ObjectDisposedException or InvalidOperationException) { }
    }

    async Task AcceptLoopAsync()
    {
        while (!stop.IsCancellationRequested)
        {
            TcpClient client;
            try { client = await listener.AcceptTcpClientAsync(stop.Token); }
            catch (Exception error) when (error is OperationCanceledException or SocketException or ObjectDisposedException or InvalidOperationException) { break; }
            if (Interlocked.Increment(ref active) > SandboxPipeRelay.MaxConnections)
            {
                Interlocked.Decrement(ref active);
                client.Dispose();
                log?.Invoke("Refused a pipe relay connection over the connection limit.");
                continue;
            }
            var id = Interlocked.Increment(ref nextId);
            var task = Task.Run(async () =>
            {
                try { await ServeAsync(client); }
                finally { Interlocked.Decrement(ref active); }
            });
            connections[id] = task;
            _ = task.ContinueWith(completed => connections.TryRemove(id, out _), CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }

    async Task ServeAsync(TcpClient client)
    {
        try
        {
            using (client)
            using (stop.Token.Register(static state => ((TcpClient)state!).Dispose(), client))
            {
                var link = client.GetStream();
                client.NoDelay = true;
                using var handshake = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                handshake.CancelAfter(TimeSpan.FromSeconds(SandboxPipeRelay.HandshakeSeconds));
                var hello = await SandboxPipeRelay.ReadFrameAsync(link, handshake.Token);
                if (hello == null || !budget.Take(hello.Length) || !TryHello(hello, out var name))
                {
                    await RefuseAsync(link, stop.Token);
                    log?.Invoke("Refused an unauthenticated pipe relay connection.");
                    return;
                }
                if (!pipes.TryGetValue(name, out var upstream))
                {
                    await RefuseAsync(link, stop.Token);
                    return;
                }
                using var pipe = new NamedPipeClientStream(".", upstream, PipeDirection.InOut, PipeOptions.Asynchronous);
                try { await pipe.ConnectAsync(5000, stop.Token); }
                catch (TimeoutException)
                {
                    await RefuseAsync(link, stop.Token);
                    log?.Invoke($"The real pipe {upstream} was not available.");
                    return;
                }
                await SandboxPipeRelay.WriteFrameAsync(link, SandboxPipeRelay.OkBytes, stop.Token);
                await SandboxPipeRelay.PumpAsync(pipe, link, budget, stop.Token);
            }
        }
        catch (Exception error) when (SandboxPipeRelay.Expected(error))
        {
            if (error is not OperationCanceledException) log?.Invoke("A pipe relay connection ended: " + error.Message);
        }
    }

    bool TryHello(byte[] hello, out string name)
    {
        name = "";
        var text = Encoding.UTF8.GetString(hello);
        var split = text.IndexOf('\n');
        if (split <= 0 || split + 1 >= text.Length) return false;
        name = text[(split + 1)..];
        return SandboxPipeRelay.FixedEquals(text[..split], token) && pipes.ContainsKey(name);
    }

    static async Task RefuseAsync(Stream link, CancellationToken cancellation)
    {
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            timeout.CancelAfter(TimeSpan.FromSeconds(1));
            await SandboxPipeRelay.WriteFrameAsync(link, SandboxPipeRelay.RefusedBytes, timeout.Token);
        }
        catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
    }

    public async ValueTask DisposeAsync()
    {
        Stop();
        if (accepting != null)
            try { await accepting; }
            catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
        var running = connections.Values.ToArray();
        if (running.Length > 0)
            try { await Task.WhenAll(running); }
            catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
        stop.Dispose();
    }
}

// The domain end, hosted by the domain host inside the overlay wineserver: it serves the game's and the loader's pipe
// names there and forwards each accepted client to the trusted relay over loopback TCP.
public sealed class SandboxPipeRelayDomain
{
    readonly string[] names;
    readonly string token;
    readonly int port;
    readonly Action<string>? log;
    readonly CancellationTokenSource stop = new();
    readonly SandboxPipeRelay.RelayBudget budget = new();
    readonly ConcurrentDictionary<long, Task> connections = new();
    readonly List<Task> accepting = [];
    long nextId;
    bool started;

    public SandboxPipeRelayDomain(string runId, int port, string token, Action<string>? log = null)
    {
        if (port is < 1 or > 65535) throw new ArgumentException("The pipe relay port is out of range.");
        if (!SandboxPipeRelay.IsToken(token)) throw new ArgumentException("The pipe relay token is not 32 hex digits.");
        var (backend, live) = SandboxPipeRelay.Names(runId);
        names = [backend, live];
        this.port = port;
        this.token = token;
        this.log = log;
    }

    public void Start()
    {
        if (started) throw new InvalidOperationException("The pipe relay is already started.");
        var created = new List<(string Name, NamedPipeServerStream Pipe)>();
        try
        {
            foreach (var name in names) created.Add((name, PipeServer.Create(name, SandboxPipeRelay.MaxConnections)));
        }
        catch
        {
            foreach (var (_, pipe) in created) pipe.Dispose();
            throw;
        }
        started = true;
        foreach (var (name, pipe) in created)
        {
            var task = Task.Run(() => AcceptPipeAsync(name, pipe));
            accepting.Add(task);
        }
        log?.Invoke("Serving " + string.Join(", ", names) + " in the sandbox.");
    }

    public void Stop()
    {
        if (!started) return;
        SandboxPipeRelay.Cancel(stop);
    }

    async Task AcceptPipeAsync(string name, NamedPipeServerStream first)
    {
        var pipe = first;
        while (!stop.IsCancellationRequested)
        {
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
            NamedPipeServerStream? next;
            try { next = PipeServer.Create(name, SandboxPipeRelay.MaxConnections); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                log?.Invoke($"The sandbox pipe {name} could not accept another connection: {error.Message}");
                next = null;
            }
            var id = Interlocked.Increment(ref nextId);
            var accepted = pipe;
            var task = Task.Run(() => ServePipeAsync(accepted, name));
            connections[id] = task;
            _ = task.ContinueWith(completed => connections.TryRemove(id, out _), CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            if (next == null)
            {
                await task;
                if (stop.IsCancellationRequested) return;
                try { next = PipeServer.Create(name, SandboxPipeRelay.MaxConnections); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    log?.Invoke($"The sandbox pipe {name} could not accept another connection: {error.Message}");
                    return;
                }
            }
            pipe = next;
        }
        pipe.Dispose();
    }

    async Task ServePipeAsync(NamedPipeServerStream pipe, string name)
    {
        try
        {
            using (pipe)
            using (stop.Token.Register(static state => ((IDisposable)state!).Dispose(), pipe))
            {
                using var client = new TcpClient();
                using (stop.Token.Register(static state => ((IDisposable)state!).Dispose(), client))
                {
                    using var handshake = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                    handshake.CancelAfter(TimeSpan.FromSeconds(SandboxPipeRelay.HandshakeSeconds));
                    await client.ConnectAsync(IPAddress.Loopback, port, handshake.Token);
                    client.NoDelay = true;
                    var link = client.GetStream();
                    var hello = Encoding.UTF8.GetBytes(token + "\n" + name);
                    await SandboxPipeRelay.WriteFrameAsync(link, hello, handshake.Token);
                    var answer = await SandboxPipeRelay.ReadFrameAsync(link, handshake.Token);
                    if (answer == null || !budget.Take(answer.Length) || !answer.AsSpan().SequenceEqual(SandboxPipeRelay.OkBytes))
                    {
                        log?.Invoke("The trusted pipe relay refused the sandbox connection.");
                        return;
                    }
                    await SandboxPipeRelay.PumpAsync(pipe, link, budget, stop.Token);
                }
            }
        }
        catch (Exception error) when (SandboxPipeRelay.Expected(error))
        {
            if (error is not OperationCanceledException) log?.Invoke($"A sandbox pipe connection for {name} ended: {error.Message}");
        }
    }

    public async ValueTask DisposeAsync()
    {
        Stop();
        if (accepting.Count > 0)
            try { await Task.WhenAll(accepting); }
            catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
        var running = connections.Values.ToArray();
        if (running.Length > 0)
            try { await Task.WhenAll(running); }
            catch (Exception error) when (SandboxPipeRelay.Expected(error)) { }
        stop.Dispose();
    }
}
