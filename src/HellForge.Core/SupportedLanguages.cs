using System.Text.Json;

namespace HellForge.Core;

// One HD2 language pack at the supported build. Every language but English is a free DLC of its own (German is app
// 2506220, Japanese 2506270) whose one depot carries the same ID as the DLC; Steam installs it beside the base game. The
// base build's depots hold English only, so a copy without the pack crashes when the game switches to that language.
public sealed record SupportedLanguage(string Name, uint DlcAppId, string ManifestId, IReadOnlyList<SupportedFile> Files)
{
    public uint Depot => DlcAppId;
    public long TotalBytes => Files.Sum(f => f.Size);

    // The pack as a build of one depot, so everything made for the base build works on it unchanged: the download_depot
    // command (SteamDepotDownload), copying (GameCopier.CopyFromDepots), IsComplete and Verify.
    public SupportedBuild AsBuild(SupportedBuild build) =>
        build with { Depots = [new SupportedDepot(Depot, ManifestId)], Files = Files };
}

// The language packs pinned for the supported build (supported-languages.json, made with HellForge.Cli
// supported-language). A pack is optional: setup offers the ones the player has installed in Steam, and a copy without
// any plays in English as before.
public sealed record SupportedLanguages(int SchemaVersion, string BuildId, IReadOnlyList<SupportedLanguage> Languages)
{
    public const string ResourceName = "HellForge.Core.supported-languages.json";
    static readonly JsonSerializerOptions Json = new(CopyPreparer.JsonOptions);

    static readonly Lazy<SupportedLanguages> current = new(LoadEmbedded);
    public static SupportedLanguages Current => current.Value;

    static SupportedLanguages LoadEmbedded()
    {
        using var stream = typeof(SupportedLanguages).Assembly.GetManifestResourceStream(ResourceName)
            ?? throw new InvalidDataException("The supported language list is missing from HellForge.Core.");
        var list = Load(stream);
        // A list made for another build would copy files the game does not expect: no packs then.
        return list.BuildId == SupportedBuild.Current.BuildId ? list : list with { Languages = [] };
    }

    public static SupportedLanguages Load(Stream stream) =>
        JsonSerializer.Deserialize(stream, CoreJsonCamel.Default.SupportedLanguages) ?? throw new InvalidDataException("Empty supported language list.");

    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, Json) + "\n");

    public SupportedLanguage? Find(string name) =>
        Languages.FirstOrDefault(l => l.Name.Equals(name, StringComparison.OrdinalIgnoreCase));

    // The packs this list pins that the player's Steam has installed (the app manifest's InstalledDepots with a dlcappid).
    // Only which ones: the installed files belong to Steam's current build, so setup copies the pinned manifest's instead.
    public IReadOnlyList<SupportedLanguage> InstalledIn(string appManifestPath)
    {
        LocalInstall.NoLinks(appManifestPath);
        var app = ValveData.Parse(File.ReadAllText(appManifestPath)).Children["AppState"];
        if (!app.Children.TryGetValue("InstalledDepots", out var installed)) return [];
        var dlc = installed.Children.Values.Where(d => d.Values.ContainsKey("dlcappid"))
            .Select(d => d.Values["dlcappid"]).ToHashSet(StringComparer.Ordinal);
        return [.. Languages.Where(l => dlc.Contains(l.DlcAppId.ToString()))];
    }

    // The packs whose files are all in the game copy (present with their size; Verify proves the content).
    public IReadOnlyList<SupportedLanguage> InCopy(string gameRoot) =>
        [.. Languages.Where(l => l.AsBuild(SupportedBuild.Current).IsComplete(gameRoot))];

    // A pack's entry from Steam's download of it at the pinned manifest (download_depot 553850 <depot> <manifest>, into
    // <Steam>/steamapps/content/app_553850/depot_<depot>). With Steam's cached depot manifest the hashes are Steam's;
    // without one the downloaded files are hashed, which is what Steam verified them against while downloading.
    public static SupportedLanguage FromDownload(string name, uint dlcAppId, string manifestId, string depotFolder, string? depotCache)
    {
        if (string.IsNullOrWhiteSpace(name) || !name.All(c => c is >= 'a' and <= 'z'))
            throw new ArgumentException("A language name is Steam's, in lowercase letters (german, japanese).");
        if (manifestId.Length == 0 || !manifestId.All(char.IsAsciiDigit)) throw new ArgumentException("--manifest is the depot manifest's number.");
        depotFolder = LocalInstall.FullPath(depotFolder);
        LocalInstall.NoLinks(depotFolder);
        if (!Directory.Exists(depotFolder)) throw new InvalidDataException($"{depotFolder} does not exist; run download_depot first.");
        var cached = depotCache == null ? null : Path.Combine(depotCache, $"{dlcAppId}_{manifestId}.manifest");
        var files = cached != null && File.Exists(cached) ? FilesFromManifest(cached, dlcAppId, manifestId, depotFolder) : FilesFromFolder(depotFolder, dlcAppId);
        files.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
        if (files.Count == 0) throw new InvalidDataException($"{depotFolder} holds no files.");
        // A pack only adds files: one the base build already has would be overwritten in the copy.
        var clash = files.FirstOrDefault(f => SupportedBuild.Current.Files.Any(b => b.Path == f.Path));
        if (clash != null) throw new InvalidDataException($"{clash.Path} is in the base build too.");
        return new(name, dlcAppId, manifestId, files);
    }

    // The pack's files as Steam's cached depot manifest lists them, each present in the download with its size.
    static List<SupportedFile> FilesFromManifest(string cached, uint dlcAppId, string manifestId, string depotFolder)
    {
        var manifest = SteamDepotManifest.Load(cached);
        if (manifest.DepotId != dlcAppId || manifest.ManifestId.ToString() != manifestId)
            throw new InvalidDataException($"Depot manifest {cached} names another depot or manifest.");
        if (manifest.FilenamesEncrypted) throw new InvalidDataException($"Depot {dlcAppId} has encrypted file names.");
        List<SupportedFile> files = [.. manifest.Files.Where(f => !f.IsDirectory).Select(f => new SupportedFile(f.Path, (long)f.Size, f.Sha1, dlcAppId))];
        var missing = files.FirstOrDefault(f => new FileInfo(Path.Combine(depotFolder, f.Path)) is var info && (!info.Exists || info.Length != f.Size));
        if (missing != null) throw new InvalidDataException($"The download lacks {missing.Path}; let download_depot finish.");
        return files;
    }

    // The pack's files as downloaded, each hashed.
    static List<SupportedFile> FilesFromFolder(string depotFolder, uint dlcAppId) =>
        [.. Directory.EnumerateFiles(depotFolder, "*", SearchOption.AllDirectories)
            .Select(path => new SupportedFile(Path.GetRelativePath(depotFolder, path).Replace('\\', '/'), new FileInfo(path).Length,
                SupportedBuild.Sha1(path), dlcAppId))];

    // Adds or replaces one pack in a list file (a new file when it does not exist yet).
    public static SupportedLanguages With(string? existingPath, SupportedLanguage language)
    {
        SupportedLanguages list;
        if (existingPath != null && File.Exists(existingPath))
            using (var stream = File.OpenRead(existingPath)) list = Load(stream);
        else list = new SupportedLanguages(1, SupportedBuild.Current.BuildId, []);
        if (list.BuildId != SupportedBuild.Current.BuildId)
            throw new InvalidDataException($"{existingPath} is for build {list.BuildId}; HellForge supports build {SupportedBuild.Current.BuildId}.");
        return list with { Languages = [.. list.Languages.Where(l => l.DlcAppId != language.DlcAppId), language] };
    }
}
