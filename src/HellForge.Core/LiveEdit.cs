using System.Globalization;
using System.IO.Pipes;
using System.Text;

namespace HellForge.Core;

// When a stats field's change takes effect in a running game: now, respawn or mission (mappings/components.json), and
// whether that is confirmed. Null for fields live editing does not reach (attachment deltas: from the next session).
public sealed record LiveApplies(string When, bool Verified)
{
    public static LiveApplies? Of(StatsField field) =>
        field.Kind == "EntityDelta" ? null :
        SdkMappings.Current.FieldAt(field.Kind == "component" ? field.Component : field.Kind, field.Offset, field.Storage) is { } mapped
            ? new LiveApplies(mapped.Applies, mapped.AppliesVerified) : null;

    public string Text => When switch
    {
        "now" => "applies now",
        "respawn" => "applies on respawn",
        "mission" => "applies next mission",
        _ => When
    } + (Verified ? "" : " (expected)");
}

public sealed record LiveResult(bool Ok, string Detail, uint? Bits = null)
{
    public static readonly LiveResult NotConnected = new(false, "the game is not connected");
}

// The launcher's end of live editing (native/mod-loader/live.c): a named pipe, \\.\pipe\hellforge-live-<run id>, that
// the session's mod loader connects to; current user only, one game at a time. Each request names a stats field as
// the stats schema does and the loader applies it with set_live, as an edit of the Local Stats mod. Solo sessions only:
// the caller decides (a friend's game would not have the change).
public sealed class LiveEditServer : IAsyncDisposable
{
    public const int Protocol = 1;
    readonly string name, build;
    readonly CancellationTokenSource stop = new();
    readonly SemaphoreSlim sending = new(1, 1);
    readonly Dictionary<int, TaskCompletionSource<string>> pending = [];
    NamedPipeServerStream? pipe;
    StreamWriter? writer;
    Task? serving;
    int sequence;

    public static string PipeName(string runId) => "hellforge-live-" + runId;

    // The game's hello: its build and the stats schema it was planned with; empty until a game connects.
    public string GameBuild { get; private set; } = "";
    public string GameSchema { get; private set; } = "";
    public bool Connected { get; private set; }
    // The connected game runs another build or schema than this HellForge: requests are not sent.
    public bool Mismatched => Connected && (GameBuild != build || GameSchema != "-" && GameSchema != StatsSchema.Fingerprint);
    public event Action? StateChanged;
    // A line the game sent on its own (not a reply), split at its tabs: mod sync's "syncanswer", and "packages" (this
    // game's own packages, for friends: AssetShare) (native/mod-loader/live.c).
    public event Action<string[]>? Received;

    // build: the game build the session must run (the schema's; a test harness passes its own).
    public LiveEditServer(string runId, string? build = null)
    {
        this.build = build ?? StatsSchema.Current.Build;
        if (runId.Length is 0 or > 80 || runId.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '-' or '_')))
            throw new ArgumentException("The run id must be 1-80 letters, digits, '-' or '_'.");
        name = PipeName(runId);
    }

    public void Start() => serving ??= Task.Run(ServeAsync);

    async Task ServeAsync()
    {
        while (!stop.IsCancellationRequested)
        {
            var server = PipeServer.Create(name, 1);
            try
            {
                await server.WaitForConnectionAsync(stop.Token);
                pipe = server;
                writer = new StreamWriter(server, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
                using var reader = new StreamReader(server, new UTF8Encoding(false));
                while (await reader.ReadLineAsync(stop.Token) is { } line)
                {
                    var parts = line.Split('\t');
                    if (parts[0] == "hello" && parts.Length == 4 && parts[1] == Protocol.ToString(CultureInfo.InvariantCulture))
                    {
                        (GameBuild, GameSchema, Connected) = (parts[2], parts[3], true);
                        StateChanged?.Invoke();
                    }
                    else if (int.TryParse(parts[0], CultureInfo.InvariantCulture, out var seq))
                    {
                        lock (pending) if (pending.Remove(seq, out var waiter)) waiter.TrySetResult(line);
                    }
                    else if (parts[0].Length > 0 && parts[0].All(char.IsAsciiLetterLower)) Received?.Invoke(parts);
                }
            }
            catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
            finally
            {
                Connected = false;
                writer = null;
                pipe = null;
                lock (pending) { foreach (var waiter in pending.Values) waiter.TrySetResult("-\trefused\tthe game disconnected"); pending.Clear(); }
                await server.DisposeAsync();
                if (!stop.IsCancellationRequested) StateChanged?.Invoke();
            }
        }
    }

    // A stats field as a request names it: kind, component, resource, record, offset, storage.
    static string Identity(StatsField field) => string.Join('\t', field.Kind, field.Component.Length == 0 ? "-" : field.Component,
        Convert.ToUInt64(field.Resource[2..], 16).ToString("x16", CultureInfo.InvariantCulture),
        field.Record.ToString(CultureInfo.InvariantCulture), field.Offset.ToString(CultureInfo.InvariantCulture), field.Storage);

    public Task<LiveResult> SetAsync(StatsField field, uint bits, bool shared) =>
        SendAsync("set", Identity(field) + "\t" + bits.ToString("x8", CultureInfo.InvariantCulture) + "\t" + (shared ? "1" : "0"));
    // Undoes the editor's live edit of the field: its value from before it.
    public Task<LiveResult> ResetAsync(StatsField field) => SendAsync("reset", Identity(field));
    // The field at its base: without the Local Stats mod's edits, live or from the session start (a co-op joiner's own
    // values give way to the host's).
    public Task<LiveResult> BaseAsync(StatsField field) => SendAsync("base", Identity(field));
    // The session as the in-game editor shows it (HfApi session, stats_source): "solo", "host" or "joiner", whose stats
    // the game plays with ("own", or "host": joined to a host who shares its stats; the editor is read-only), and the
    // host's name.
    public Task<LiveResult> SessionAsync(string role, string host, string stats = "own") =>
        SendAsync("session", role + "\tstats=" + (stats == "host" ? "host" : "own") + "\t" + string.Concat(host.Where(c => !char.IsControl(c)).Take(60)));
    // The lobby's packages for friends' changes (native/mod-loader/packages.c): the game asks for each as it does for its
    // own edits'. Ids as the loader's "packages" line gives them; at most 256.
    public Task<LiveResult> PreloadAsync(IEnumerable<ulong> packages)
    {
        var ids = packages.Take(256).Select(id => id.ToString("x16", CultureInfo.InvariantCulture)).ToList();
        return SendAsync("preload", ids.Count == 0 ? "-" : string.Join(',', ids));
    }

    // Each player's projectile swaps, which the game's copies of their shots fire (native/mod-loader/shots.c):
    // "<peer hex>:<weapon resource hex>=<type>" comma separated, "-" none.
    public Task<LiveResult> ShotsAsync(string rules) => SendAsync("shots", rules);

    // Each player's changes of projectile and damage rows, which the game's copies of their shots fly and hit with
    // (native/mod-loader/rounds.c): "<peer hex>=<entry>;<entry>..." comma separated, "-" none (AssetShare.Rounds).
    public Task<LiveResult> RoundsAsync(string list) => SendAsync("rounds", list);

    // A "packages" or "preload" list: package ids, 16 hex digits each, comma separated, "-" none. Null when malformed.
    public static SortedSet<ulong>? Packages(string list, int max)
    {
        var ids = new SortedSet<ulong>();
        if (list is "-" or "") return ids;
        foreach (var part in list.Split(','))
        {
            if (part.Length != 16 || !part.All(char.IsAsciiHexDigit) || ids.Count == max) return null;
            ids.Add(ulong.Parse(part, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture));
        }
        return ids;
    }
    // A message for the player, which the in-game editor shows over the game (HfApi notice): why a friend's lobby refused
    // this game's join, say. kind "error" or "info"; a title and up to 8 lines. A loader from before notices refuses it.
    public Task<LiveResult> NoticeAsync(string kind, string title, IEnumerable<string> lines) =>
        SendAsync("notice", string.Join('\t', new[] { kind, title }.Concat(lines.Take(8)).Select(t => string.Concat(t.Where(c => !char.IsControl(c)).Take(200)))));
    // The field's value in the running game now.
    public Task<LiveResult> GetAsync(StatsField field) => SendAsync("get", Identity(field));

    // Mod sync (HfApi sync_state / sync_mod): an offer's mods, index 0 up, then its state, which publishes them in the
    // game. A progress update is the state alone, with the same version. Words, not numbers, for actions, classes and states.
    // url: where players get the mod (ModCatalog.WebAddress, at most 200 characters), or "-".
    public Task<LiveResult> SyncModAsync(int version, int index, string action, string modClass, long bytes, string id, string name, string modVersion, string? url = null) =>
        SendAsync("syncmod", string.Join('\t', Invariant(version), Invariant(index), action, modClass, Invariant(bytes), Field(id), Field(name), Field(modVersion),
            ModCatalog.WebAddress(url) ?? "-"));
    public Task<LiveResult> SyncStateAsync(int version, string state, string host, long bytes, long done, int mods, string message) =>
        SendAsync("syncstate", string.Join('\t', Invariant(version), state, Field(host), Invariant(bytes), Invariant(done), Invariant(mods), Field(message)));

    static string Invariant(long number) => number.ToString(CultureInfo.InvariantCulture);
    // Text inside one field: no tabs, line breaks or other control characters, at most 200 characters.
    static string Field(string text) => string.Concat(text.Where(c => !char.IsControl(c)).Take(200));

    async Task<LiveResult> SendAsync(string verb, string rest)
    {
        if (!Connected || writer == null) return LiveResult.NotConnected;
        if (Mismatched) return new(false, $"the game runs build {GameBuild} with another stats schema; restart the session");
        var waiter = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        int seq;
        lock (pending) { seq = ++sequence; pending[seq] = waiter; }
        await sending.WaitAsync();
        try { await writer.WriteLineAsync($"{verb}\t{seq.ToString(CultureInfo.InvariantCulture)}\t{rest}"); }
        catch (Exception error) when (error is IOException or ObjectDisposedException or NullReferenceException)
        {
            lock (pending) pending.Remove(seq);
            return new(false, "the game disconnected");
        }
        finally { sending.Release(); }
        var reply = await waiter.Task.WaitAsync(TimeSpan.FromSeconds(5)).ContinueWith(t => t.IsCompletedSuccessfully ? t.Result : "-\trefused\tthe game did not answer");
        var parts = reply.Split('\t');
        if (parts.Length >= 3 && parts[1] == "ok")
            return new(true, parts[2], parts.Length >= 4 && uint.TryParse(parts[3], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var bits) ? bits : null);
        return new(false, parts.Length >= 3 ? parts[2] : "malformed reply");
    }

    public async ValueTask DisposeAsync()
    {
        stop.Cancel();
        if (serving != null) await serving;
        stop.Dispose();
        sending.Dispose();
    }
}
