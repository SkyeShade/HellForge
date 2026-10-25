using System.Security.Cryptography;
using System.Text.Json;

namespace HellForge.Core;

public sealed record SupportedDepot(uint Id, string ManifestId);
public sealed record SupportedFile(string Path, long Size, string Sha1, uint Depot);

// The HD2 build HellForge's runtime patches target, as Steam publishes it: the files of its depots with sizes and SHA-1
// hashes from Steam's depot manifests. Setup copies exactly these files and sessions verify them. The list holds names
// and hashes only, no game content.
public sealed record SupportedBuild(int SchemaVersion, string AppId, string BuildId, string ExeSha256, string GameDllSha256,
    IReadOnlyList<SupportedDepot> Depots, IReadOnlyList<SupportedFile> Files)
{
    public const string ResourceName = "HellForge.Core.supported-build.json";
    static readonly JsonSerializerOptions Json = new(CopyPreparer.JsonOptions);

    public long TotalBytes => Files.Sum(f => f.Size);

    // Depot files HellForge's copy never uses: modules that never run there (they update themselves, so Steam's
    // validation on 2026-10-14 found one changed) and their installers. The list leaves them out, so setup does not copy
    // them and sessions do not look for them.
    public static bool IsUnused(string path) =>
        path is "bin/GameGuard.des" or "tools/GGSetup.exe" or "tools/gguninst.exe" ||
        path.StartsWith("bin/GameGuard/", StringComparison.Ordinal);

    // Upscaler libraries players swap for newer versions (DLSS, XeSS, FSR): graphics only, no gameplay. They must be
    // present, but any version counts as the build's, so sessions start with a swapped one and repairs keep it.
    public static bool IsReplaceable(string path) =>
        path is "bin/nvngx_dlss.dll" or "bin/libxess.dll" or "bin/amd_fidelityfx_upscaler_dx12.dll";

    // A file in place that counts as the build's: its content, or any version of a replaceable one.
    public static bool Accepts(string path, SupportedFile file)
    {
        var info = new FileInfo(path);
        return info.Exists && (IsReplaceable(file.Path) ||
            info.Length == file.Size && Sha1(path).Equals(file.Sha1, StringComparison.OrdinalIgnoreCase));
    }

    static readonly Lazy<SupportedBuild> current = new(LoadEmbedded);
    public static SupportedBuild Current => current.Value;

    static SupportedBuild LoadEmbedded()
    {
        using var stream = typeof(SupportedBuild).Assembly.GetManifestResourceStream(ResourceName)
            ?? throw new InvalidDataException("The supported build list is missing from HellForge.Core.");
        return Load(stream);
    }

    public static SupportedBuild Load(Stream stream) =>
        JsonSerializer.Deserialize(stream, CoreJsonCamel.Default.SupportedBuild) ?? throw new InvalidDataException("Empty supported build list.");

    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, Json) + "\n");

    // Builds the list from an installed, Steam-validated HD2: the app manifest names the build and its depot manifests,
    // which Steam caches in <Steam>/depotcache.
    public static SupportedBuild FromSteam(string appManifestPath, string depotCache)
    {
        var report = LocalInstall.Inspect(appManifestPath);
        var app = ValveData.Parse(File.ReadAllText(appManifestPath)).Children["AppState"];
        if (!app.Children.TryGetValue("InstalledDepots", out var installed) || installed.Children.Count == 0)
            throw new InvalidDataException("The app manifest lists no installed depots.");
        var depots = new List<SupportedDepot>();
        var files = new SortedDictionary<string, SupportedFile>(StringComparer.Ordinal);
        foreach (var (id, entry) in installed.Children.OrderBy(x => x.Key, StringComparer.Ordinal))
        {
            var manifestId = entry.Required("manifest");
            var path = Path.Combine(depotCache, $"{id}_{manifestId}.manifest");
            if (!File.Exists(path)) throw new InvalidDataException($"Steam has not cached depot manifest {id}_{manifestId}.");
            var manifest = SteamDepotManifest.Load(path);
            if (manifest.DepotId.ToString() != id || manifest.ManifestId.ToString() != manifestId)
                throw new InvalidDataException($"Depot manifest {path} names another depot or manifest.");
            if (manifest.FilenamesEncrypted) throw new InvalidDataException($"Depot {id} has encrypted file names.");
            depots.Add(new(manifest.DepotId, manifestId));
            foreach (var file in manifest.Files.Where(f => !f.IsDirectory && !IsUnused(f.Path)))
                if (!files.TryAdd(file.Path, new(file.Path, (long)file.Size, file.Sha1, manifest.DepotId)))
                    throw new InvalidDataException($"Two depots provide {file.Path}.");
        }
        foreach (var required in new[] { LocalInstall.Exe, LocalInstall.GameDll })
        {
            if (!files.TryGetValue(required, out var file)) throw new InvalidDataException($"The depots lack {required}.");
            if (Sha1(Path.Combine(report.Source, required)) != file.Sha1)
                throw new InvalidDataException($"The installed {required} differs from its depot; validate the game in Steam first.");
        }
        return new(1, LocalInstall.AppId, report.Pin.BuildId, report.Pin.ExeSha256, report.Pin.GameDllSha256, depots, files.Values.ToList());
    }

    // A quick check without hashing: every file is present with its size. Verify proves the content.
    // Sessions write data/settings.ini from a kept original (GameSettings), so that original is the copy checked here.
    public bool IsComplete(string gameRoot) => Files.All(file =>
    {
        var path = Path.Combine(gameRoot, file.Path);
        if (File.Exists(path + GameSettings.OriginalSuffix)) path += GameSettings.OriginalSuffix;
        var info = new FileInfo(path);
        return info.Exists && (info.Length == file.Size || IsReplaceable(file.Path));
    });

    public static string Sha1(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexString(SHA1.HashData(stream));
    }

    // Problems that keep a game folder from being this build; empty when it matches. With a cache file, a file whose
    // size and last write time match its last successful check is not hashed again.
    public IReadOnlyList<string> Verify(string gameRoot, string? cachePath = null, Action<long, long>? progress = null,
        CancellationToken cancellation = default)
    {
        gameRoot = LocalInstall.FullPath(gameRoot);
        LocalInstall.NoLinks(gameRoot);
        var cache = LoadCache(cachePath);
        var verified = new Dictionary<string, string>(StringComparer.Ordinal);
        var problems = new List<string>();
        long done = 0, total = TotalBytes;
        foreach (var file in Files)
        {
            cancellation.ThrowIfCancellationRequested();
            var path = Path.Combine(gameRoot, file.Path);
            var info = new FileInfo(path);
            if (!info.Exists) problems.Add("missing: " + file.Path);
            else if (IsReplaceable(file.Path)) { }
            else if (info.Length != file.Size) problems.Add("wrong size: " + file.Path);
            else
            {
                var stamp = $"{info.Length}|{info.LastWriteTimeUtc.Ticks}|{file.Sha1}";
                if (cache.TryGetValue(file.Path, out var cached) && cached == stamp) verified[file.Path] = stamp;
                else if (!Sha1(path).Equals(file.Sha1, StringComparison.OrdinalIgnoreCase)) problems.Add("changed: " + file.Path);
                else verified[file.Path] = stamp;
            }
            done += file.Size;
            progress?.Invoke(done, total);
        }
        if (cachePath != null) File.WriteAllText(cachePath, JsonSerializer.Serialize(verified));
        return problems;
    }

    static Dictionary<string, string> LoadCache(string? path)
    {
        if (path == null || !File.Exists(path)) return new(StringComparer.Ordinal);
        try { return JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(path)) ?? new(StringComparer.Ordinal); }
        catch (JsonException) { return new(StringComparer.Ordinal); }
    }
}
