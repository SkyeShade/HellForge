using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Follows the live galactic war read-only through the community API mirror (api.helldivers2.dev), never Arrowhead's
// servers: GET /raw/api/WarSeason/801/WarInfo and /Status, the public bodies war-data.json was imported from. The backend
// fetches both before the game starts and every 5 minutes after, and serves them for /WarSeason/801/warinfo and /Status
// (the game re-polls Status). The last good copy is kept in a cache file (war-data.json's shape) and used when the mirror
// cannot be reached; with neither, the embedded frozen war is served. The mirror asks each client to name itself and a
// contact (X-Super-Client, X-Super-Contact).
// Where a session has no way out at all (Linux: the game, the launcher and this backend run in a network namespace with
// loopback only), the war is fetched before the namespace is entered (FetchOnceAsync, "fetch-live-war"), and the
// session serves that copy without trying the mirror (cacheOnly): the war does not move until the next launch.
public sealed class LiveWar : IServedWar, IAsyncDisposable
{
    public const string Mirror = "https://api.helldivers2.dev";
    public const int WarId = 801;
    public const string Client = "HellForge", Contact = "https://github.com/SkyeShade";
    public static readonly TimeSpan RefreshInterval = TimeSpan.FromMinutes(5);
    // After a failed fetch (the mirror was down for a while on 2026-10-15) the next try comes sooner.
    static readonly TimeSpan RetryInterval = TimeSpan.FromMinutes(1);
    static readonly TimeSpan FetchTimeout = TimeSpan.FromSeconds(8);

    readonly string cachePath;
    readonly WarClock clock;
    readonly Action<string> log;
    readonly HttpClient http;
    readonly object gate = new();
    readonly CancellationTokenSource stop = new();
    JsonElement? warInfo, status;
    Task? refreshing;
    bool lastFailed;

    LiveWar(string cachePath, WarClock clock, Action<string> log, HttpMessageHandler? handler, string mirror)
    {
        this.cachePath = cachePath;
        this.clock = clock;
        this.log = log;
        http = new HttpClient(handler ?? new HttpClientHandler(), disposeHandler: true) { BaseAddress = new Uri(mirror), Timeout = FetchTimeout };
        http.DefaultRequestHeaders.Add("X-Super-Client", Client);
        http.DefaultRequestHeaders.Add("X-Super-Contact", Contact);
        http.DefaultRequestHeaders.Accept.ParseAdd("application/json");
    }

    // Loads the cached copy, then fetches the live war once before returning (the game starts after the backend is ready),
    // and keeps refreshing in the background. cacheOnly: the cached copy alone, no request to the mirror.
    public static async Task<LiveWar> StartAsync(string cachePath, WarClock clock, Action<string> log, HttpMessageHandler? handler = null,
        string mirror = Mirror, TimeSpan? refreshInterval = null, bool cacheOnly = false)
    {
        var war = new LiveWar(Path.GetFullPath(cachePath), clock, log, handler, mirror);
        war.LoadCache();
        if (cacheOnly)
        {
            log(war.HasWar ? "Serving the live war fetched before the session; it is not refreshed (no network in this session)."
                : "No live war was fetched before the session (no network in this session): the built-in war is served.");
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

    public bool HasWar { get { lock (gate) return status != null; } }
    public string Source => "live_war";

    // Fetches the live war once into the cache file, for a session that will have no network (true when it was fetched).
    public static async Task<bool> FetchOnceAsync(string cachePath, Action<string> log, HttpMessageHandler? handler = null, string mirror = Mirror)
    {
        await using var war = new LiveWar(Path.GetFullPath(cachePath), new WarClock(), log, handler, mirror);
        await war.RefreshAsync(war.stop.Token);
        return !war.lastFailed && war.HasWar;
    }

    // The live or cached body for the game's war requests; false leaves them to the embedded frozen war.
    public bool TryGet(string path, out JsonElement body)
    {
        lock (gate)
        {
            JsonElement? found = string.Equals(path, $"/WarSeason/{WarId}/warinfo", StringComparison.OrdinalIgnoreCase) ? warInfo
                : string.Equals(path, $"/WarSeason/{WarId}/Status", StringComparison.OrdinalIgnoreCase) ? status : null;
            body = found ?? default;
            return found != null;
        }
    }

    async Task RefreshAsync(CancellationToken cancellation)
    {
        try
        {
            var info = await FetchAsync("WarInfo", cancellation);
            var live = await FetchAsync("Status", cancellation);
            Validate(info, live);
            var fetched = DateTimeOffset.UtcNow;
            Apply(info, live, fetched, sinceFetch: TimeSpan.Zero);
            SaveCache(info, live, fetched);
            lastFailed = false;
            log($"Live war {WarId} fetched from {http.BaseAddress!.Host}: war time {live.GetProperty("time").GetInt64()} s, " +
                $"{live.GetProperty("planetStatus").GetArrayLength()} planets, {live.GetProperty("campaigns").GetArrayLength()} campaigns.");
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException or JsonException or InvalidDataException or IOException or UnauthorizedAccessException)
        {
            lastFailed = true;
            log($"Could not fetch the live war ({error.Message}); keeping the {(HasWar ? "last fetched" : "built-in")} war.");
        }
    }

    async Task<JsonElement> FetchAsync(string name, CancellationToken cancellation)
    {
        using var response = await http.GetAsync($"/raw/api/WarSeason/{WarId}/{name}", cancellation);
        if (!response.IsSuccessStatusCode) throw new HttpRequestException($"{name}: HTTP {(int)response.StatusCode}");
        await using var stream = await response.Content.ReadAsStreamAsync(cancellation);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellation);
        return document.RootElement.Clone();
    }

    // The game's parsers expect the shapes war-data.json was checked with; anything else keeps the previous war.
    // (HellForgeWar checks its snapshot's warInfo and status the same way.)
    internal static void Validate(JsonElement info, JsonElement live)
    {
        static bool Array(JsonElement body, string name) => body.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Array;
        if (info.ValueKind != JsonValueKind.Object || live.ValueKind != JsonValueKind.Object) throw new InvalidDataException("not JSON objects");
        if (!info.TryGetProperty("warId", out var infoWar) || !infoWar.TryGetInt32(out var a) || a != WarId ||
            !live.TryGetProperty("warId", out var liveWar) || !liveWar.TryGetInt32(out var b) || b != WarId)
            throw new InvalidDataException($"the live war is not war {WarId}");
        if (!live.TryGetProperty("time", out var time) || !time.TryGetInt64(out var seconds) || seconds <= 0)
            throw new InvalidDataException("no war time");
        if (!Array(info, "planetInfos") || info.GetProperty("planetInfos").GetArrayLength() == 0 || !Array(live, "planetStatus") || !Array(live, "campaigns"))
            throw new InvalidDataException("missing planets or campaigns");
    }

    // A cached copy's war time is advanced by the time since it was fetched: the war runs in real time.
    void Apply(JsonElement info, JsonElement live, DateTimeOffset fetched, TimeSpan sinceFetch)
    {
        lock (gate)
        {
            warInfo = info;
            status = live;
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
            var info = root.GetProperty("warInfo").Clone();
            var live = root.GetProperty("status").Clone();
            Validate(info, live);
            var fetched = DateTimeOffset.Parse(root.GetProperty("source").GetProperty("fetchedUtc").GetString()!, CultureInfo.InvariantCulture);
            Apply(info, live, fetched, DateTimeOffset.UtcNow - fetched);
            log($"Loaded the live war fetched {fetched:u} from {cachePath}.");
        }
        catch (Exception error) when (error is JsonException or InvalidDataException or KeyNotFoundException or InvalidOperationException or FormatException or IOException or UnauthorizedAccessException)
        {
            log($"Ignored an unreadable live war cache ({error.Message}).");
        }
    }

    void SaveCache(JsonElement info, JsonElement live, DateTimeOffset fetched)
    {
        var cache = new JsonObject
        {
            ["scope"] = $"Public galactic war data (war {WarId}) fetched read-only from the community API mirror {Mirror} raw endpoints by the HellForge backend; refreshed every session.",
            ["source"] = new JsonObject
            {
                ["warInfo"] = $"/raw/api/WarSeason/{WarId}/WarInfo",
                ["status"] = $"/raw/api/WarSeason/{WarId}/Status",
                ["fetchedUtc"] = fetched.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture)
            },
            ["warId"] = WarId,
            ["warInfo"] = JsonNode.Parse(info.GetRawText()),
            ["status"] = JsonNode.Parse(live.GetRawText())
        };
        Directory.CreateDirectory(Path.GetDirectoryName(cachePath)!);
        var temporary = cachePath + ".tmp";
        File.WriteAllText(temporary, cache.ToJsonString());
        File.Move(temporary, cachePath, overwrite: true);
    }

    public async ValueTask DisposeAsync()
    {
        stop.Cancel();
        if (refreshing != null) await refreshing;
        http.Dispose();
        stop.Dispose();
    }
}
