using HellForge.Core;

namespace HellForge.SteamDownload;

// What setup asks of Steam once signed in (SteamDepotSession; fakes in the tests).
public interface IDepotService
{
    // The depot's decryption key: only for an account whose licence covers the depot (DepotAccessDeniedException).
    Task<byte[]> DepotKeyAsync(uint app, uint depot, CancellationToken cancellation);
    // The code that lets this account fetch one manifest of the depot; 0 when Steam does not grant it.
    Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, CancellationToken cancellation);
    Task<DepotManifest> ManifestAsync(uint app, uint depot, ulong manifest, ulong requestCode, byte[] depotKey, CancellationToken cancellation);
    // One chunk, decrypted and decompressed into buffer; its length.
    Task<int> ChunkAsync(uint app, uint depot, DepotChunk chunk, byte[] depotKey, byte[] buffer, CancellationToken cancellation);
}

// Steam would not let this account have a depot or manifest: a borrowed game (Steam Family Sharing) or no licence for it.
public sealed class DepotAccessDeniedException(uint depot, string reason)
    : Exception($"Steam did not let this account download depot {depot} ({reason}).")
{
    public uint Depot { get; } = depot;
}

// The supported build's download: into Destination, copying what Steam's installed game (Installed, under SteamRoot) already
// holds. Only: the build's files to fetch (null: all of them).
public sealed record BuildDownloadJob(SupportedBuild Build, string Destination, string? SteamRoot, SteamGame? Installed,
    IReadOnlyCollection<string>? Only = null);

// Downloads HELLDIVERS 2's supported build from Valve's servers for the signed-in account, at the depot manifests
// supported-build.json pins. HellForge hosts no game files: every byte comes from the player's own Steam install or from
// Steam's content servers under the player's licence.
public static class SteamBuildDownload
{
    public sealed record Planned(DepotPlan Plan, IReadOnlyDictionary<uint, byte[]> Keys, IReadOnlyList<string> Notes);

    // The plan: the pinned manifests (with this account's request codes), the installed build's manifests where they
    // can be had, and which chunks come from where. Notes: what planning had to do without (plain words).
    public static async Task<Planned> PlanAsync(IDepotService steam, BuildDownloadJob job, Action<string>? status, CancellationToken cancellation)
    {
        var app = uint.Parse(job.Build.AppId);
        var files = Selected(job);
        var keys = new Dictionary<uint, byte[]>();
        var targets = new Dictionary<uint, DepotManifest>();
        foreach (var depot in job.Build.Depots.Where(d => files.Any(f => f.Depot == d.Id)))
        {
            status?.Invoke($"Asking Steam for depot {depot.Id} of build {job.Build.BuildId}...");
            keys[depot.Id] = await steam.DepotKeyAsync(app, depot.Id, cancellation);
            targets[depot.Id] = await Manifest(steam, app, depot.Id, ulong.Parse(depot.ManifestId), keys[depot.Id], cancellation)
                ?? throw new DepotAccessDeniedException(depot.Id, "no manifest request code for the supported build");
        }
        var notes = new List<string>();
        var installed = await InstalledManifests(steam, job, keys, notes, cancellation);
        var steamGame = job.Installed?.Report.Source;
        return new(DepotPlanner.Plan(files, targets, new(steamGame, installed)), keys, notes);
    }

    // Plans, then writes the files (DepotAssembler). Returns the plan it followed.
    public static async Task<Planned> DownloadAsync(IDepotService steam, BuildDownloadJob job, Action<string>? status,
        Action<AssembleProgress>? progress, CancellationToken cancellation)
    {
        var planned = await PlanAsync(steam, job, status, cancellation);
        status?.Invoke("Downloading from Steam...");
        await new DepotAssembler(new Fetcher(steam, uint.Parse(job.Build.AppId), planned.Keys)).AssembleAsync(planned.Plan, job.Destination, progress, cancellation);
        return planned;
    }

    static IReadOnlyList<SupportedFile> Selected(BuildDownloadJob job)
    {
        if (job.Only == null) return job.Build.Files;
        var unknown = job.Only.Where(path => !job.Build.Files.Any(f => f.Path == path)).ToList();
        if (unknown.Count > 0) throw new ArgumentException($"Not a file of build {job.Build.BuildId}: {unknown[0]}.");
        return [.. job.Build.Files.Where(f => job.Only.Contains(f.Path))];
    }

    static async Task<DepotManifest?> Manifest(IDepotService steam, uint app, uint depot, ulong manifest, byte[] key, CancellationToken cancellation)
    {
        var code = await steam.ManifestRequestCodeAsync(app, depot, manifest, cancellation);
        if (code == 0) return null;
        var found = await steam.ManifestAsync(app, depot, manifest, code, key, cancellation);
        if (found.DepotId != depot || found.ManifestId != manifest) throw new InvalidDataException($"Steam sent another manifest than {depot}_{manifest}.");
        if (found.FilenamesEncrypted) throw new InvalidDataException($"Steam's manifest {depot}_{manifest} came with its file names encrypted.");
        return found;
    }

    // The manifests of the build Steam has installed (its app manifest names them): Steam's own cached copy in
    // <Steam>/depotcache when it has one, else fetched like the pinned ones. Without them a chunk is still looked for at
    // the same place in Steam's file, so a missing one costs download size, never correctness.
    static async Task<IReadOnlyList<DepotManifest>> InstalledManifests(IDepotService steam, BuildDownloadJob job, IReadOnlyDictionary<uint, byte[]> keys,
        List<string> notes, CancellationToken cancellation)
    {
        var found = new List<DepotManifest>();
        if (job.Installed == null) return found;
        var app = uint.Parse(job.Build.AppId);
        foreach (var (depot, manifest) in InstalledDepots(job.Installed.ManifestPath).Where(d => keys.ContainsKey(d.Depot)))
        {
            if (job.Build.Depots.Any(d => d.Id == depot && d.ManifestId == manifest.ToString())) continue;
            var cached = job.SteamRoot == null ? null : CachedManifest(job.SteamRoot, depot, manifest);
            try { cached ??= await Manifest(steam, app, depot, manifest, keys[depot], cancellation); }
            catch (Exception error) when (error is IOException or InvalidDataException or HttpRequestException or TimeoutException or DepotAccessDeniedException) { }
            if (cached != null) found.Add(cached);
            else notes.Add($"The layout of your installed depot {depot} is unknown, so fewer of its parts can be reused.");
        }
        return found;
    }

    // The app manifest's installed depots: (depot, manifest).
    public static IReadOnlyList<(uint Depot, ulong Manifest)> InstalledDepots(string appManifest)
    {
        var app = ValveData.Parse(File.ReadAllText(appManifest)).Children["AppState"];
        if (!app.Children.TryGetValue("InstalledDepots", out var depots)) return [];
        return [.. depots.Children.Where(d => uint.TryParse(d.Key, out _) && d.Value.Children.ContainsKey("manifest") &&
                ulong.TryParse(d.Value.Required("manifest"), out _))
            .Select(d => (uint.Parse(d.Key), ulong.Parse(d.Value.Required("manifest"))))];
    }

    // Steam's cached copy of a manifest, when it is that manifest with readable file names.
    public static DepotManifest? CachedManifest(string steamRoot, uint depot, ulong manifest)
    {
        var path = Path.Combine(steamRoot, "depotcache", $"{depot}_{manifest}.manifest");
        try
        {
            if (!File.Exists(path)) return null;
            var parsed = SteamDepotManifest.Load(path);
            return parsed.DepotId == depot && parsed.ManifestId == manifest && !parsed.FilenamesEncrypted ? parsed : null;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException) { return null; }
    }

    sealed class Fetcher(IDepotService steam, uint app, IReadOnlyDictionary<uint, byte[]> keys) : IChunkFetcher
    {
        public Task<int> FetchAsync(uint depot, DepotChunk chunk, byte[] buffer, CancellationToken cancellation) =>
            steam.ChunkAsync(app, depot, chunk, keys[depot], buffer, cancellation);
    }
}
