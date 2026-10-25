using System.IO.Pipes;
using System.Threading.Channels;

namespace HellForge.LocalBackend.Multiplayer;

// The named pipe the game's stand-in DLLs connect to (\\.\pipe\<name>, current user only). Each DLL opens its own
// connection and introduces itself with hello [component, process id]: "lobby" frames go to the lobby service, "party"
// frames to the Party service, and each service's frames for this machine go back over its component's connection.
// Writes are queued and done by the connection's own task, so a busy game never blocks a service. When a connection
// ends (the game closed), the service hears that this machine left.
public sealed class PipeHost : IAsyncDisposable
{
    readonly string name;
    readonly Action<string> log;
    readonly CancellationTokenSource stop = new();
    readonly List<Task> connections = [];
    readonly Dictionary<string, Channel<byte[]>> outboxes = new(StringComparer.Ordinal);
    Task? accepting;

    public LobbyService? Lobby { get; set; }
    public PartyService? Party { get; set; }

    public PipeHost(string name, Action<string> log)
    {
        if (name.Length is 0 or > 100 || name.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.')))
            throw new ArgumentException("The pipe name must be 1-100 letters, digits, '-', '_' or '.'.");
        this.name = name;
        this.log = log;
    }

    public void Start() => accepting = Task.Run(AcceptLoop);

    async Task AcceptLoop()
    {
        while (!stop.IsCancellationRequested)
        {
            var pipe = HellForge.Core.PipeServer.Create(name, NamedPipeServerStream.MaxAllowedServerInstances);
            try { await pipe.WaitForConnectionAsync(stop.Token); }
            catch (OperationCanceledException) { await pipe.DisposeAsync(); return; }
            lock (connections) connections.Add(Task.Run(() => Serve(pipe)));
        }
    }

    async Task Serve(NamedPipeServerStream pipe)
    {
        var component = "";
        Channel<byte[]>? outbox = null;
        try
        {
            var hello = await Frame.ReadAsync(pipe, stop.Token);
            if (hello is not { Type: "hello" } || hello.Count < 1 || hello.Text(0) is not ("lobby" or "party")) return;
            component = hello.Text(0);
            log($"Stand-in connected: {component}.");
            outbox = Channel.CreateUnbounded<byte[]>(new UnboundedChannelOptions { SingleReader = true });
            lock (outboxes) outboxes[component] = outbox;
            var writer = outbox;
            _ = Task.Run(async () =>
            {
                try { await foreach (var bytes in writer.Reader.ReadAllAsync(stop.Token)) { await pipe.WriteAsync(bytes, stop.Token); await pipe.FlushAsync(stop.Token); } }
                catch (Exception error) when (error is IOException or ObjectDisposedException or OperationCanceledException or InvalidOperationException) { }
            });
            var prefix = component + ".";
            while (await Frame.ReadAsync(pipe, stop.Token) is { } frame)
            {
                if (!frame.Type.StartsWith(prefix, StringComparison.Ordinal)) continue;
                if (component == "lobby") Lobby?.FromLocal(frame);
                else Party?.FromLocal(frame);
            }
        }
        catch (Exception error) when (error is IOException or InvalidDataException or OperationCanceledException or ObjectDisposedException)
        {
            if (error is InvalidDataException) log($"Stand-in {component} sent a bad frame: {error.Message}");
        }
        finally
        {
            if (outbox != null)
            {
                outbox.Writer.TryComplete();
                lock (outboxes) if (outboxes.TryGetValue(component, out var current) && current == outbox) outboxes.Remove(component);
                if (component == "lobby") Lobby?.LocalDisconnected();
                else Party?.LocalDisconnected();
                log($"Stand-in disconnected: {component}.");
            }
            await pipe.DisposeAsync();
        }
    }

    // Frames for a local stand-in; dropped when it is not connected.
    public void Send(string component, Frame frame)
    {
        Channel<byte[]>? outbox;
        lock (outboxes) outboxes.TryGetValue(component, out outbox);
        outbox?.Writer.TryWrite(frame.Encode());
    }

    public void SendLobby(Frame frame) => Send("lobby", frame);
    public void SendParty(Frame frame) => Send("party", frame);

    public async ValueTask DisposeAsync()
    {
        stop.Cancel();
        if (accepting != null) await accepting;
        Task[] running;
        lock (connections) running = connections.ToArray();
        await Task.WhenAll(running);
    }
}
