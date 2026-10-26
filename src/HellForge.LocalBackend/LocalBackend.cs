using System.Diagnostics;
using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public enum LoginMode { LocalSession, RejectGmi }

// What LocalBackend.Create serves. The caller owns a supplied profile. Without one, fixtures use an unsaved default
// profile. ItemCatalog (development) replaces the embedded catalogue with a JSON file's "items" array; FixtureOverride
// (development) is a JSON object mapping endpoint paths to replacement bodies.
public sealed record BackendSettings(int Port, string EvidenceDirectory, LoginMode Mode)
{
    public bool StartupFixtures { get; init; }
    public ProfileStore? Profile { get; init; }
    public string? ItemCatalog { get; init; }
    public string? FixtureOverride { get; init; }
    public bool CaptureMissionBodies { get; init; }
    public WarClock? WarClock { get; init; }
    public AccountKeys? AccountKeys { get; init; }
    public LiveWar? LiveWar { get; init; }
    public FriendPresence? Presence { get; init; }
    public RegisteredItems? RegisteredItems { get; init; }
    public bool OpenWar { get; init; }
    public uint WarSeed { get; init; }
    // the all-planets war deals its subfactions and SEAF allies from the seed (true) or keeps the frozen war's
    public bool DealSubfactions { get; init; } = true;
    public bool DealSeaf { get; init; } = true;
}

// A synthetic backend contract probe, not a Steam ownership verifier or a game launcher.
public static partial class LocalBackend
{
    public const int MaximumBodyBytes = 1024 * 1024;

    // The error bodies the backend answers with ({"errorCode": "HELLFORGE_..."}).
    static JsonObject Error(string code) => new() { ["errorCode"] = code };

    // A request from a program on this PC, not from a web page in the player's browser: the Host is the loopback address
    // the backend listens on (a DNS-rebinding page sends its own name), and none of a browser's marks is there (Origin,
    // Referer, Sec-Fetch-*). The game, the session runner and the launcher send none of them.
    public static bool FromLocalProgram(HttpRequest request) =>
        request.Host.Host is "127.0.0.1" or "localhost" or "[::1]" &&
        !request.Headers.ContainsKey("Origin") && !request.Headers.ContainsKey("Referer") &&
        !request.Headers.Keys.Any(k => k.StartsWith("Sec-Fetch-", StringComparison.OrdinalIgnoreCase));

    // A response body: a JSON node or element, written compactly as the serializer writes them.
    static byte[] Utf8(object response)
    {
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            switch (response)
            {
                case JsonNode node: node.WriteTo(writer); break;
                case JsonElement element: element.WriteTo(writer); break;
                default: throw new InvalidOperationException("A response must be JSON: " + response.GetType().Name);
            }
        }
        return buffer.ToArray();
    }

    // The client's JSON decoder sizes its node buffer from the body length, so short or dense
    // bodies such as {"id":1} fail with "out of memory" (shown as error 0, status 200).
    // Trailing whitespace adds capacity without adding nodes; see scripts/ProbeSavedJsonDecoder.py.
    public static byte[] PadForClientDecoder(byte[] json)
    {
        var padded = new byte[json.Length + Math.Max(json.Length, 16)];
        json.CopyTo(padded, 0);
        padded.AsSpan(json.Length).Fill((byte)' ');
        return padded;
    }

    public static JsonDocument LoadResource(string name)
    {
        using var stream = typeof(LocalBackend).Assembly.GetManifestResourceStream("HellForge.LocalBackend." + name)
            ?? throw new InvalidOperationException("Missing embedded resource " + name);
        return JsonDocument.Parse(stream);
    }

    // A "bodySource" names an embedded data file and optional "#key" (default "items") whose value is the
    // body: the item catalogue and progression data rebuilt from the user's own snapshot by
    // scripts/BuildItemCatalog.py, and the game client's feature switches read from it (game-client-config.json).
    public static Dictionary<string, JsonElement> LoadStartupFixtures()
    {
        using var document = LoadResource("startup-fixtures.json");
        var fixtures = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (var endpoint in document.RootElement.GetProperty("endpoints").EnumerateArray())
        {
            var body = endpoint.GetProperty("body").Clone();
            if (endpoint.TryGetProperty("bodySource", out var source))
            {
                var parts = source.GetString()!.Split('#');
                using var data = LoadResource(parts[0]);
                body = data.RootElement.GetProperty(parts.Length > 1 ? parts[1] : "items").Clone();
            }
            fixtures.Add(endpoint.GetProperty("path").GetString()!, body);
        }
        return fixtures;
    }

    public static WebApplication Create(BackendSettings settings)
    {
        CheckSettings(settings);
        var fixtures = Fixtures(settings);
        // --war open: the all-planets war (OpenWar) replaces the live war.
        var liveWar = settings.OpenWar ? null : settings.LiveWar;
        var profile = settings.Profile;
        if (settings.StartupFixtures) profile ??= ProfileStore.Open(null);
        var missions = profile != null ? new MissionService(profile) : null;
        // Friends' lobbies and profiles for the social tab, filled by multiplayer when a caller supplies it.
        var presence = settings.Presence ?? new FriendPresence();
        var playFab = new PlayFabService(presence);
        string? captureBodies = settings.CaptureMissionBodies ? Path.Combine(settings.EvidenceDirectory, "mission-bodies") : null;
        if (captureBodies != null) Directory.CreateDirectory(captureBodies);
        var seasonPassDetails = settings.StartupFixtures ? SeasonPassDetails() : new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        // The war clock continues from the imported war's time; the client re-polls it (parser 0x10817b0). A caller's
        // clock is shared with multiplayer, which aligns it with a host's.
        var warClock = settings.WarClock ?? new WarClock();
        // A live war (LiveWar) has set the clock from its own Status; otherwise it continues from the embedded war.
        // Players' public keys, shared with multiplayer when a caller supplies them.
        var accountKeys = settings.AccountKeys ?? new AccountKeys();
        if (settings.StartupFixtures && liveWar?.HasWar != true)
        {
            using var war = LoadResource("war-data.json");
            warClock.Set(war.RootElement.GetProperty("status").GetProperty("time").GetInt64());
        }
        var startup = new StartupBodies(profile, liveWar, fixtures);
        if (settings.RegisteredItems != null) settings.RegisteredItems.Source = path => startup.TryGet(path, out var body, out _) ? body : null;
        Directory.CreateDirectory(settings.EvidenceDirectory);
        var auditPath = Path.Combine(settings.EvidenceDirectory, "requests.jsonl");
        // A fresh evidence directory prevents accidental mixing of runs.
        using (new FileStream(auditPath, FileMode.CreateNew, FileAccess.Write)) { }
        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions
        {
            ApplicationName = typeof(LocalBackend).Assembly.GetName().Name,
            EnvironmentName = "Production"
        });
        builder.WebHost.UseKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, settings.Port);
            options.Limits.MaxRequestBodySize = MaximumBodyBytes;
        });
        var app = builder.Build();
        var requests = new Requests
        {
            Mode = settings.Mode, StartupFixtures = settings.StartupFixtures, Profile = profile, Missions = missions, Presence = presence,
            PlayFab = playFab, CaptureBodies = captureBodies, SeasonPassDetails = seasonPassDetails, WarClock = warClock,
            AccountKeys = accountKeys, RegisteredItems = settings.RegisteredItems, Startup = startup, AuditPath = auditPath
        };
        app.Run(requests.HandleAsync);
        return app;
    }

    static void CheckSettings(BackendSettings settings)
    {
        if (settings.Port is < 0 or > 65535) throw new ArgumentOutOfRangeException("port");
        if (!Enum.IsDefined(settings.Mode)) throw new ArgumentOutOfRangeException("mode");
        if (settings.StartupFixtures && settings.Mode != LoginMode.LocalSession)
            throw new ArgumentException("Startup fixtures require local-session mode.");
        if (settings.Profile != null && !settings.StartupFixtures) throw new ArgumentException("A profile requires startup fixtures.");
    }

    // The startup bodies: the embedded fixtures, with a development catalogue or override in their place, and the
    // all-planets war's Status.
    static Dictionary<string, JsonElement> Fixtures(BackendSettings settings)
    {
        var fixtures = settings.StartupFixtures ? LoadStartupFixtures() : new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (settings.ItemCatalog != null)
        {
            if (!settings.StartupFixtures) throw new ArgumentException("An item catalogue requires startup fixtures.");
            using var data = JsonDocument.Parse(File.ReadAllText(settings.ItemCatalog));
            fixtures["/Progression/items"] = data.RootElement.GetProperty("items").Clone();
        }
        if (settings.FixtureOverride != null)
        {
            if (!settings.StartupFixtures) throw new ArgumentException("A fixture override requires startup fixtures.");
            Override(fixtures, settings.FixtureOverride);
        }
        // --war open: the all-planets war (OpenWar), made from the frozen war's bodies.
        if (settings.OpenWar)
        {
            if (!settings.StartupFixtures) throw new ArgumentException("The all-planets war requires startup fixtures.");
            fixtures[$"/WarSeason/{LiveWar.WarId}/Status"] = OpenWar.Status(fixtures[$"/WarSeason/{LiveWar.WarId}/warinfo"], fixtures[$"/WarSeason/{LiveWar.WarId}/Status"], seed: settings.WarSeed,
                dealSubfactions: settings.DealSubfactions, dealSeaf: settings.DealSeaf);
        }
        return fixtures;
    }

    static void Override(Dictionary<string, JsonElement> fixtures, string fixtureOverride)
    {
        using var data = JsonDocument.Parse(File.ReadAllText(fixtureOverride));
        foreach (var entry in data.RootElement.EnumerateObject())
        {
            if (!fixtures.ContainsKey(entry.Name)) throw new ArgumentException("Override for an unknown fixture: " + entry.Name);
            fixtures[entry.Name] = entry.Value.Clone();
        }
    }

    // Warbond pages (GET /SeasonPass/<id32>, request 0x6a23fbc2, parser 0x1068a80) from the snapshot export.
    static Dictionary<string, JsonElement> SeasonPassDetails()
    {
        var seasonPassDetails = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        using var data = LoadResource("progression-data.json");
        if (data.RootElement.TryGetProperty("seasonPassDetails", out var details))
            foreach (var detail in details.EnumerateObject()) seasonPassDetails[detail.Name] = detail.Value.Clone();
        return seasonPassDetails;
    }

    // A startup path's body: the local profile's, else the live war's (for a path the fixtures serve), else the synthetic
    // fixture's; source says which, for the audit.
    sealed class StartupBodies(ProfileStore? profile, LiveWar? liveWar, Dictionary<string, JsonElement> fixtures)
    {
        public bool TryGet(string path, out JsonElement body, out string source)
        {
            source = "local_profile";
            if (profile != null && profile.TryGetResponse(path, out body)) return true;
            source = "live_war";
            if (liveWar != null && fixtures.ContainsKey(path) && liveWar.TryGet(path, out body)) return true;
            source = "synthetic_startup_fixture";
            return fixtures.TryGetValue(path, out body);
        }
    }
}
