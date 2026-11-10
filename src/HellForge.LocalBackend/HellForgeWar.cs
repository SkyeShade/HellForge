using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// A war the game is served (StartupBodies): the live war (LiveWar) or the HellForge war (HellForgeWar).
public interface IServedWar
{
    bool HasWar { get; }
    // the audit's name for a body it served
    string Source { get; }
    bool TryGet(string path, out JsonElement body);
}

// The HellForge war (backend --war hellforge): one galactic war shared by HellForge players, run by the HellForge war
// server (HellForge.Core.WarServer: its own service, https://war.hf-mod.com unless another is chosen). docs/hellforge-war.md.
//   GET  {server}/v1/snapshot          {schema: 1, warId: 801, sequence, generatedUtc, warInfo, status, galacticWarEffects,
//                                      assignments, news}: the game's own bodies, served unchanged
//   POST {server}/v1/mission/start     {installId, planetIndex, difficulty 1..11, race} -> {ticket}
//   POST {server}/v1/mission/heartbeat {ticket} -> 204, every 2 minutes while the mission runs (players per planet)
//   POST {server}/v1/mission/end       {ticket, success} -> {impact, capped}
// The snapshot is fetched before the game starts and every minute after (30 s after a failure), and kept in a cache file
// (WarServer.CachePath: one per server) with the time it was fetched; without the server the cached war is served, and
// without either the frozen war. Its warinfo, Status and GalacticWarEffects take the startup bodies' place; its major
// orders (/v2/Assignment/War/801) and dispatches (/NewsFeed/801) are WarServices'. The client's parsers want every key
// and integers as integers, so the bodies are kept as the server's bytes (JsonElement), never rebuilt.
// Missions are reported from the game's own requests (MissionService): Operation/Create names each operation's planet,
// difficulty (11 for the mod loader's level, native/mod-loader/difficulty.c) and enemy; Mission/Start starts a mission
// of one; Mission/end ends it. Each report goes out in the background with a short timeout: the game's request is
// answered as before, whether the server answers or not. The impact the server answers is logged, not put in the
// Mission/end reply (the client wants an integer there, and what its end screen does with it is not known).
// Where the session has no network (Linux), the snapshot fetched before the namespace is served (cacheOnly) and no
// mission is reported.
public sealed class HellForgeWar : IServedWar, IAsyncDisposable
{
    public const string Mode = "hellforge";
    public const int Schema = 1, WarId = LiveWar.WarId;
    public static readonly TimeSpan RefreshInterval = TimeSpan.FromMinutes(1), HeartbeatInterval = TimeSpan.FromMinutes(2);
    static readonly TimeSpan RetryInterval = TimeSpan.FromSeconds(30), FetchTimeout = TimeSpan.FromSeconds(8), ReportTimeout = TimeSpan.FromSeconds(5);
    const int MaxNews = 16;

    readonly string cachePath, server;
    readonly WarClock clock;
    readonly Action<string> log;
    readonly HttpClient http;
    readonly object gate = new();
    readonly CancellationTokenSource stop = new();
    readonly TimeSpan heartbeat;
    JsonElement? warInfo, status, effects, assignments, news;
    long sequence;
    Task? refreshing;
    bool lastFailed;

    HellForgeWar(string cachePath, string server, WarClock clock, Action<string> log, HttpMessageHandler? handler, TimeSpan heartbeat)
    {
        this.cachePath = cachePath;
        this.server = server;
        this.clock = clock;
        this.log = log;
        this.heartbeat = heartbeat;
        http = new HttpClient(handler ?? new HttpClientHandler(), disposeHandler: true) { BaseAddress = new Uri(server + "/"), Timeout = FetchTimeout };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HellForge/" + (typeof(HellForgeWar).Assembly.GetName().Version?.ToString(3) ?? "0"));
        http.DefaultRequestHeaders.Accept.ParseAdd("application/json");
    }

    // How a war is started: Handler (tests: a fake server), its refresh and heartbeat intervals, and CacheOnly: the cached
    // copy alone, no request to the server.
    public sealed record Options(HttpMessageHandler? Handler = null, TimeSpan? RefreshInterval = null, bool CacheOnly = false, TimeSpan? HeartbeatInterval = null);

    // Loads the cached copy, fetches the snapshot once before returning (the game starts after the backend is ready), and
    // keeps refreshing in the background.
    public static async Task<HellForgeWar> StartAsync(string cachePath, string server, Guid installId, WarClock clock, Action<string> log, Options? options = null)
    {
        var (handler, refreshInterval, cacheOnly, heartbeatInterval) = options ?? new Options();
        var normalized = HellForge.Core.WarServer.Normalize(server) ?? throw new ArgumentException($"Not a war server address: {server} (https, or http to this PC).");
        var war = new HellForgeWar(Path.GetFullPath(cachePath), normalized, clock, log, handler, heartbeatInterval ?? HeartbeatInterval)
            { InstallId = installId, Reports = !cacheOnly };
        war.LoadCache();
        if (cacheOnly)
        {
            log(war.HasWar ? "Serving the HellForge war fetched before the session; it is not refreshed and missions are not reported (no network in this session)."
                : "No HellForge war was fetched before the session (no network in this session): the built-in war is served.");
            return war;
        }
        await war.RefreshAsync(war.stop.Token);
        var interval = refreshInterval ?? RefreshInterval;
        war.refreshing = Task.Run(async () =>
        {
            try
            {
                while (true)
                {
                    await Task.Delay(war.lastFailed && RetryInterval < interval ? RetryInterval : interval, war.stop.Token);
                    await war.RefreshAsync(war.stop.Token);
                }
            }
            catch (OperationCanceledException) { }
        });
        return war;
    }

    // Fetches the snapshot once into the cache file, for a session that will have no network (true when it was fetched).
    public static async Task<bool> FetchOnceAsync(string cachePath, string server, Action<string> log, HttpMessageHandler? handler = null)
    {
        var normalized = HellForge.Core.WarServer.Normalize(server) ?? throw new ArgumentException($"Not a war server address: {server} (https, or http to this PC).");
        await using var war = new HellForgeWar(Path.GetFullPath(cachePath), normalized, new WarClock(), log, handler, HeartbeatInterval) { Reports = false };
        await war.RefreshAsync(war.stop.Token);
        return !war.lastFailed && war.HasWar;
    }

    public Guid InstallId { get; private init; }
    bool Reports { get; init; }
    public string Server => server;
    public string Source => "hellforge_war";
    public bool HasWar { get { lock (gate) return status != null; } }
    public long Sequence { get { lock (gate) return sequence; } }

    // The snapshot's body for a startup path; false leaves it to the frozen war.
    public bool TryGet(string path, out JsonElement body)
    {
        lock (gate)
        {
            JsonElement? found = path == $"/WarSeason/{WarId}/warinfo" ? warInfo : path == $"/WarSeason/{WarId}/Status" ? status
                : path == "/WarSeason/GalacticWarEffects" ? effects : null;
            body = found ?? default;
            return found != null;
        }
    }

    // The snapshot's major orders (/v2/Assignment/War/801), or null without a snapshot.
    public JsonElement? Assignments { get { lock (gate) return assignments; } }

    // The snapshot's dispatches published after fromTimeStamp, at most maxEntries (16), in the server's order; null without
    // a snapshot.
    public IEnumerable<JsonElement>? News(long fromTimeStamp, int maxEntries = MaxNews)
    {
        JsonElement? all;
        lock (gate) all = news;
        if (all is not { } list) return null;
        return list.EnumerateArray().Where(d => !d.TryGetProperty("published", out var p) || !p.TryGetInt64(out var at) || at > fromTimeStamp)
            .Take(Math.Clamp(maxEntries, 0, MaxNews)).ToList();
    }

    async Task RefreshAsync(CancellationToken cancellation)
    {
        try
        {
            using var response = await http.GetAsync("v1/snapshot", cancellation);
            if (!response.IsSuccessStatusCode) throw new HttpRequestException($"snapshot: HTTP {(int)response.StatusCode}");
            await using var stream = await response.Content.ReadAsStreamAsync(cancellation);
            using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellation);
            var snapshot = document.RootElement.Clone();
            Validate(snapshot);
            var fetched = DateTimeOffset.UtcNow;
            Apply(snapshot, TimeSpan.Zero);
            SaveCache(snapshot, fetched);
            lastFailed = false;
            log($"HellForge war fetched from {http.BaseAddress!.Authority}: sequence {snapshot.GetProperty("sequence").GetInt64()}, war time " +
                $"{snapshot.GetProperty("status").GetProperty("time").GetInt64()} s, {snapshot.GetProperty("assignments").GetArrayLength()} major orders, " +
                $"{snapshot.GetProperty("news").GetArrayLength()} dispatches.");
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException or JsonException or InvalidDataException or IOException or UnauthorizedAccessException)
        {
            lastFailed = true;
            log($"Could not fetch the HellForge war ({error.Message}); keeping the {(HasWar ? "last fetched" : "built-in")} war.");
        }
    }

    // A snapshot the game's parsers can take: schema 1, war 801, a war time, planets, and every list there (anything else
    // keeps the war served before).
    public static void Validate(JsonElement snapshot)
    {
        static bool Array(JsonElement body, string name) => body.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Array;
        if (snapshot.ValueKind != JsonValueKind.Object) throw new InvalidDataException("the snapshot is not a JSON object");
        if (!snapshot.TryGetProperty("schema", out var schema) || !schema.TryGetInt32(out var version) || version != Schema)
            throw new InvalidDataException($"the snapshot is not schema {Schema}");
        if (!snapshot.TryGetProperty("warId", out var war) || !war.TryGetInt32(out var id) || id != WarId) throw new InvalidDataException($"the snapshot is not war {WarId}");
        if (!snapshot.TryGetProperty("sequence", out var sequence) || !sequence.TryGetInt64(out _)) throw new InvalidDataException("the snapshot has no sequence");
        if (!snapshot.TryGetProperty("warInfo", out var info) || !snapshot.TryGetProperty("status", out var live)) throw new InvalidDataException("the snapshot has no warInfo or status");
        LiveWar.Validate(info, live);
        if (!Array(snapshot, "galacticWarEffects") || !Array(snapshot, "assignments") || !Array(snapshot, "news"))
            throw new InvalidDataException("the snapshot has no galacticWarEffects, assignments or news");
    }

    // A cached copy's war time is advanced by the time since it was fetched: the war runs in real time.
    void Apply(JsonElement snapshot, TimeSpan sinceFetch)
    {
        var live = snapshot.GetProperty("status");
        lock (gate)
        {
            warInfo = snapshot.GetProperty("warInfo");
            status = live;
            effects = snapshot.GetProperty("galacticWarEffects");
            assignments = snapshot.GetProperty("assignments");
            news = snapshot.GetProperty("news");
            sequence = snapshot.GetProperty("sequence").GetInt64();
        }
        clock.Set(live.GetProperty("time").GetInt64() + (long)Math.Max(0, sinceFetch.TotalSeconds));
    }

    void LoadCache()
    {
        if (!File.Exists(cachePath)) return;
        try
        {
            using var cache = JsonDocument.Parse(File.ReadAllText(cachePath));
            var root = cache.RootElement;
            // a cache is one server's (WarServer.CachePath); a copied or renamed file from another is not used
            if (root.GetProperty("server").GetString() != server) throw new InvalidDataException("it is another server's war");
            var snapshot = root.GetProperty("snapshot").Clone();
            Validate(snapshot);
            var fetched = DateTimeOffset.Parse(root.GetProperty("fetchedUtc").GetString()!, CultureInfo.InvariantCulture);
            Apply(snapshot, DateTimeOffset.UtcNow - fetched);
            log($"Loaded the HellForge war fetched {fetched:u} (sequence {Sequence}) from {cachePath}.");
        }
        catch (Exception error) when (error is JsonException or InvalidDataException or KeyNotFoundException or InvalidOperationException or FormatException or IOException or UnauthorizedAccessException)
        {
            log($"Ignored an unreadable HellForge war cache ({error.Message}).");
        }
    }

    // {server, fetchedUtc, snapshot}, the snapshot as the server's bytes; written beside, then moved over the old one.
    void SaveCache(JsonElement snapshot, DateTimeOffset fetched)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(cachePath)!);
        var temporary = cachePath + ".tmp";
        using (var file = File.Create(temporary))
        using (var writer = new Utf8JsonWriter(file))
        {
            writer.WriteStartObject();
            writer.WriteString("server", server);
            writer.WriteString("fetchedUtc", fetched.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture));
            writer.WritePropertyName("snapshot");
            snapshot.WriteTo(writer);
            writer.WriteEndObject();
        }
        File.Move(temporary, cachePath, overwrite: true);
    }

    // ---- Mission reports ----

    sealed record Operation(int Planet, int Difficulty, int Race);
    // A started mission: its ticket (null until or unless the server gave one) and its heartbeat.
    sealed class Running(long operation, string key)
    {
        public readonly long Operation = operation;
        public readonly string Key = key;
        public Task<string?> Ticket = Task.FromResult<string?>(null);
        public readonly CancellationTokenSource Beat = new();
    }

    readonly ConcurrentDictionary<long, Operation> operations = new();
    readonly ConcurrentDictionary<string, Running> running = new(StringComparer.Ordinal);
    readonly ConcurrentDictionary<Task, byte> pending = new();

    // POST /Operation/Create: each operation's planet, difficulty and enemy, for its missions' reports.
    public void OperationsCreated(JsonElement body)
    {
        if (!Reports) return;
        try
        {
            if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty("operations", out var list) || list.ValueKind != JsonValueKind.Array) return;
            foreach (var operation in list.EnumerateArray())
                if (Long(operation, "id") is { } id && Long(operation, "planetIndex") is { } planet && Long(operation, "difficulty") is { } difficulty)
                    operations[id] = new Operation((int)planet, (int)difficulty, (int)(Long(operation, "race") ?? 0));
        }
        catch (Exception error) when (error is InvalidOperationException or FormatException or OverflowException) { log($"An operation was not read for mission reports ({error.Message})."); }
    }

    // POST /Operation/Mission/Start, answered with missionId: the server is told a mission started, and heartbeats follow.
    public void MissionStarted(JsonElement body, string missionId)
    {
        if (!Reports) return;
        try
        {
            var operationId = Long(body, "operationId") ?? -1;
            operations.TryGetValue(operationId, out var operation);
            var planet = Long(body, "planetIndex") ?? operation?.Planet;
            var difficulty = Long(body, "difficulty") ?? Long(body, "difficultyType") ?? operation?.Difficulty;
            var race = Long(body, "race") ?? operation?.Race ?? 0;
            if (planet is not { } p || p < 0 || difficulty is not { } d || d is < 1 or > 11)
            {
                log($"Mission not reported: its planet or difficulty is not known (operation {operationId}).");
                return;
            }
            var mission = new Running(operationId, operationId + "/" + (Long(body, "missionIndex") ?? 0).ToString(CultureInfo.InvariantCulture));
            var start = new JsonObject { ["installId"] = InstallId.ToString("D"), ["planetIndex"] = p, ["difficulty"] = d, ["race"] = race };
            mission.Ticket = Track(async () =>
            {
                using var reply = await PostAsync("v1/mission/start", start, CancellationToken.None);
                var ticket = reply?.RootElement.TryGetProperty("ticket", out var t) == true && t.ValueKind == JsonValueKind.String ? t.GetString() : null;
                if (reply != null && ticket == null) log("The war server's mission start had no ticket.");
                if (ticket != null)
                {
                    log($"Mission start reported (planet {p}, difficulty {d}).");
                    _ = Track(() => HeartbeatAsync(ticket, mission.Beat.Token));
                }
                return ticket;
            });
            running[missionId] = mission;
            running[mission.Key] = mission;
        }
        catch (Exception error) when (error is InvalidOperationException or FormatException or OverflowException) { log($"A mission start was not reported ({error.Message})."); }
    }

    // POST /Operation/Mission/end: the mission's end and whether it was won; the heartbeat stops.
    public void MissionEnded(JsonElement body)
    {
        if (!Reports) return;
        try
        {
            var mission = (body.TryGetProperty("missionId", out var id) && id.ValueKind == JsonValueKind.String && running.TryGetValue(id.GetString()!, out var byId) ? byId : null)
                ?? (running.TryGetValue((Long(body, "operationId") ?? -1) + "/" + (Long(body, "missionIndex") ?? 0).ToString(CultureInfo.InvariantCulture), out var byIndex) ? byIndex : null);
            if (mission == null) return;
            var success = body.TryGetProperty("missionSuccess", out var won) && won.ValueKind == JsonValueKind.True;
            End(mission, success, success ? "won" : "lost");
        }
        catch (Exception error) when (error is InvalidOperationException or FormatException or OverflowException) { log($"A mission end was not reported ({error.Message})."); }
    }

    // POST /Operation/Abandon: its running missions end as lost, so the server stops counting the player at once (a
    // ticket nobody ends only lapses after its heartbeats stop).
    public void OperationAbandoned(JsonElement body)
    {
        if (!Reports) return;
        var operationId = body.ValueKind == JsonValueKind.Object ? Long(body, "operationId") ?? Long(body, "id") : null;
        foreach (var mission in running.Values.Distinct().Where(m => operationId == null || m.Operation == operationId).ToList())
            End(mission, false, "abandoned");
    }

    // The mission's end, reported in the background; its heartbeat stops.
    void End(Running mission, bool success, string how)
    {
        Forget(mission);
        _ = Track(async () =>
        {
            if (await mission.Ticket is not { } ticket) return 0;
            using var reply = await PostAsync("v1/mission/end", new JsonObject { ["ticket"] = ticket, ["success"] = success }, CancellationToken.None);
            if (reply != null)
                log($"Mission end reported ({how}): impact {(reply.RootElement.TryGetProperty("impact", out var impact) ? impact.GetRawText() : "none")}" +
                    (reply.RootElement.TryGetProperty("capped", out var capped) && capped.ValueKind == JsonValueKind.True ? ", capped." : "."));
            return 0;
        });
    }

    void Forget(Running mission)
    {
        foreach (var key in running.Where(e => e.Value == mission).Select(e => e.Key).ToList()) running.TryRemove(key, out _);
        mission.Beat.Cancel();
    }

    async Task<int> HeartbeatAsync(string ticket, CancellationToken stopped)
    {
        using var both = CancellationTokenSource.CreateLinkedTokenSource(stopped, stop.Token);
        try
        {
            while (true)
            {
                await Task.Delay(heartbeat, both.Token);
                (await PostAsync("v1/mission/heartbeat", new JsonObject { ["ticket"] = ticket }, both.Token))?.Dispose();
            }
        }
        catch (OperationCanceledException) { }
        return 0;
    }

    // A report: the reply's JSON (empty for 204), or null when the server could not be reached or refused it (logged).
    async Task<JsonDocument?> PostAsync(string path, JsonObject body, CancellationToken cancellation)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(ReportTimeout);
        try
        {
            using var content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json");
            using var response = await http.PostAsync(path, content, timeout.Token);
            if (!response.IsSuccessStatusCode)
            {
                log($"The war server refused {path}: HTTP {(int)response.StatusCode}.");
                return null;
            }
            var text = await response.Content.ReadAsStringAsync(timeout.Token);
            return JsonDocument.Parse(string.IsNullOrWhiteSpace(text) ? "{}" : text);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException or JsonException or IOException)
        {
            log($"Could not reach the war server for {path} ({(error is OperationCanceledException ? "timed out" : error.Message)}).");
            return null;
        }
    }

    // A background report, awaited (briefly) when the backend stops.
    Task<T> Track<T>(Func<Task<T>> report)
    {
        var task = Task.Run(async () =>
        {
            try { return await report(); }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                log($"A mission report failed ({error.Message}).");
                return default!;
            }
        });
        pending[task] = 0;
        task.ContinueWith(t => pending.TryRemove(t, out _), TaskScheduler.Default);
        return task;
    }

    static long? Long(JsonElement owner, string name) =>
        owner.ValueKind == JsonValueKind.Object && owner.TryGetProperty(name, out var value) ? value.ValueKind switch
        {
            JsonValueKind.Number when value.TryGetInt64(out var number) => number,
            JsonValueKind.String when long.TryParse(value.GetString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var text) => text,
            _ => null
        } : null;

    public async ValueTask DisposeAsync()
    {
        stop.Cancel();
        if (refreshing != null) await refreshing;
        // an end reported just before the session closed still goes out, within a few seconds
        try { await Task.WhenAll(pending.Keys).WaitAsync(TimeSpan.FromSeconds(3)); }
        catch (TimeoutException) { log("Stopped with mission reports still unanswered."); }
        foreach (var mission in running.Values.Distinct()) mission.Beat.Cancel();
        http.Dispose();
        stop.Dispose();
    }
}
