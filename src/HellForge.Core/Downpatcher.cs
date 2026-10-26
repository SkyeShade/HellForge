using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// A patch from one Steam build to the supported one. SourceExeSha256 and SourceGameDllSha256 name the source build by its
// files too, so a copy Steam labels with another build id (a stale manifest) still finds its patch.
public sealed record DownpatchPack(string SourceBuild, string TargetBuild, string Url, string Sha256, long Size,
    string SourceExeSha256 = "", string SourceGameDllSha256 = "");

// What a patch reports as it goes: the pack's download (bytes so far, of all) and the files written (bytes so far, of the
// build's, and the file).
public sealed record DownpatchProgress(Action<long, long>? Download = null, Action<long, long, string>? Apply = null);

// What Downpatcher.ApplyPack patches: Steam's game folder (the pack's source build) into the copy at GameDestination, from
// the downloaded pack at PackPath, which Pack lists, into Build, with the patch tool (Hpatchz); Http fetches
// DownloadOverrides' files (null: none can be).
public sealed record DownpatchJob(string SteamGame, string GameDestination, string PackPath, DownpatchPack Pack, SupportedBuild Build,
    string Hpatchz, HttpClient? Http);

// Patches HELLDIVERS 2 as Steam has it into the supported build. Packs are listed in downpatch/index.json on
// files.hf-mod.com (scripts/New-DownpatchPack.ps1 builds one for each new Steam build), so a game update needs a new pack,
// not a new HellForge. The list only says which pack to try: whatever a pack holds, every file it writes must be one of
// the supported build's files with Steam's SHA-1 for it (supported-build.json, part of this release), else setup stops.
public static partial class Downpatcher
{
    public const string IndexUrl = "https://files.hf-mod.com/downpatch/index.json";
    const string PackPrefix = "https://files.hf-mod.com/downpatch/";

    // Packs known when this release was made: used when the list cannot be read and was never read before.
    public static readonly DownpatchPack[] BuiltIn =
    [
        new("25988413", "25480438",
            "https://files.hf-mod.com/downpatch/downpatch-25988413-to-25480438.zip",
            "4CE81BFB2CB9D47848A5C3518F511D4BB14B6BDF18748007F93B46B1E0BD03AC",
            1_680_909_152,
            "2CE8B3D74C5573088E4211B060CED0963F1FC1E74526E2645399780B8FB13FF7",
            "A457433F0EB766FBE15A6DE5A16F0FBBB1FD3C840C5607A5F0B2EBF53B112949")
    ];

    static DownpatchPack[] known = BuiltIn;

    // The packs setup can use now: the published list (or its cached copy) with the built-in ones.
    public static IReadOnlyList<DownpatchPack> Packs => Volatile.Read(ref known);

    // HDiffPatch v5.1.3's Windows build: the release zip and the hpatchz.exe in it, pinned.
    const string HpatchzUrl = "https://github.com/sisong/HDiffPatch/releases/download/v5.1.3/hdiffpatch_v5.1.3_bin_windows64.zip";
    const string HpatchzZipSha256 = "77F141386E5D8F785C1C846E10FBBC19B6C05AA00E3F59CC44670FB3F0E2AE94";
    const string HpatchzSha256 = "9703C694B5955C576D9F0E26E98B60941F0BBB53B382B1F1988D75D461E580CB";

    public enum ListSource { Online, Cached, BuiltIn }

    // Reads the published list into Packs; when it cannot be fetched, the copy saved last time (cacheFile), else only the
    // built-in packs. Entries that are not well formed are left out.
    public static async Task<ListSource> RefreshAsync(HttpClient http, string cacheFile, CancellationToken cancellation = default)
    {
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            timeout.CancelAfter(TimeSpan.FromSeconds(15));
            var text = await http.GetStringAsync(IndexUrl, timeout.Token);
            var packs = ParseIndex(text);
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(cacheFile))!);
            File.WriteAllText(cacheFile + ".partial", text);
            File.Move(cacheFile + ".partial", cacheFile, overwrite: true);
            Volatile.Write(ref known, Merge(packs));
            return ListSource.Online;
        }
        catch (Exception error) when (error is HttpRequestException or InvalidDataException or JsonException or IOException or UnauthorizedAccessException ||
                                      error is OperationCanceledException && !cancellation.IsCancellationRequested) { }
        try
        {
            if (File.Exists(cacheFile))
            {
                Volatile.Write(ref known, Merge(ParseIndex(File.ReadAllText(cacheFile))));
                return ListSource.Cached;
            }
        }
        catch (Exception error) when (error is InvalidDataException or JsonException or IOException or UnauthorizedAccessException) { }
        Volatile.Write(ref known, BuiltIn);
        return ListSource.BuiltIn;
    }

    [GeneratedRegex("^[0-9]{1,12}$")] private static partial Regex BuildId();
    [GeneratedRegex("^[0-9A-Fa-f]{64}$")] private static partial Regex Sha256Hex();
    [GeneratedRegex("^[A-Za-z0-9._-]+\\.zip$")] private static partial Regex PackName();

    // The list's packs that are well formed and on files.hf-mod.com/downpatch/; throws when the list itself is not one.
    public static IReadOnlyList<DownpatchPack> ParseIndex(string json)
    {
        var index = JsonSerializer.Deserialize<DownpatchIndex>(json, ManifestJson) ?? throw new InvalidDataException("Empty patch list.");
        if (index.SchemaVersion != 1 || index.Packs == null) throw new InvalidDataException($"Patch list version {index.SchemaVersion} is not one this HellForge reads.");
        return [.. index.Packs.OfType<DownpatchIndexEntry>().Where(p => BuildId().IsMatch(p.SourceBuild ?? "") && BuildId().IsMatch(p.TargetBuild ?? "") &&
                p.Url != null && p.Url.StartsWith(PackPrefix, StringComparison.Ordinal) && PackName().IsMatch(p.Url[PackPrefix.Length..]) &&
                Sha256Hex().IsMatch(p.Sha256 ?? "") && p.Size > 0 && p.Size < 64L << 30 &&
                (p.SourceExeSha256 is null or "" || Sha256Hex().IsMatch(p.SourceExeSha256)) &&
                (p.SourceGameDllSha256 is null or "" || Sha256Hex().IsMatch(p.SourceGameDllSha256)))
            .Select(p => new DownpatchPack(p.SourceBuild!, p.TargetBuild!, p.Url!, p.Sha256!.ToUpperInvariant(), p.Size,
                p.SourceExeSha256?.ToUpperInvariant() ?? "", p.SourceGameDllSha256?.ToUpperInvariant() ?? ""))];
    }

    // The published packs first (a re-made pack replaces the built-in one for its builds), then the built-in ones.
    static DownpatchPack[] Merge(IReadOnlyList<DownpatchPack> published) =>
        [.. published, .. BuiltIn.Where(b => !published.Any(p => p.SourceBuild == b.SourceBuild && p.TargetBuild == b.TargetBuild))];

    public static DownpatchPack? FindPack(string sourceBuild) =>
        Packs.FirstOrDefault(p => p.SourceBuild == sourceBuild && p.TargetBuild == SupportedBuild.Current.BuildId);

    // The patch for an installed game: by its build id, else by its executable's and game.dll's hashes.
    public static DownpatchPack? FindPack(BuildPin pin) =>
        FindPack(pin.BuildId) ?? Packs.FirstOrDefault(p => p.TargetBuild == SupportedBuild.Current.BuildId && p.SourceExeSha256.Length > 0 &&
            p.SourceExeSha256.Equals(pin.ExeSha256, StringComparison.OrdinalIgnoreCase) &&
            p.SourceGameDllSha256.Equals(pin.GameDllSha256, StringComparison.OrdinalIgnoreCase));

    public static async Task ApplyAsync(
        string steamGame, string gameDestination, string tempFolder,
        DownpatchPack pack, HttpClient http,
        DownpatchProgress? progress = null,
        CancellationToken cancellation = default)
    {
        steamGame = LocalInstall.FullPath(steamGame);
        gameDestination = LocalInstall.FullPath(gameDestination);
        LocalInstall.NoLinks(steamGame);
        LocalInstall.NoLinks(gameDestination);
        Directory.CreateDirectory(gameDestination);
        Directory.CreateDirectory(tempFolder);
        // before the download: room for the pack and every game file not yet in place (ApplyPack checks again with the
        // largest patch it extracts)
        var build = SupportedBuild.Current;
        var packThere = new FileInfo(Path.Combine(tempFolder, $"downpatch-{pack.SourceBuild}-to-{pack.TargetBuild}.zip"));
        DiskSpace.Require((gameDestination, build.Files.Where(f => !GameCopier.Present(Path.Combine(gameDestination, f.Path), f)).Sum(f => f.Size)),
            (tempFolder, packThere.Exists && packThere.Length == pack.Size ? 0 : pack.Size));

        var hpatchz = await EnsureHpatchz(tempFolder, http, cancellation);
        var packPath = await DownloadPack(tempFolder, pack, http, progress?.Download, cancellation);
        ApplyPack(new(steamGame, gameDestination, packPath, pack, SupportedBuild.Current, hpatchz, http), progress?.Apply, cancellation);
    }

    static string Sha256(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    static async Task<string> EnsureHpatchz(string tempFolder, HttpClient http, CancellationToken cancellation)
    {
        var toolsDir = Path.Combine(tempFolder, "tools");
        var hpatchz = Path.Combine(toolsDir, "hpatchz.exe");
        if (File.Exists(hpatchz) && Sha256(hpatchz) == HpatchzSha256) return hpatchz;

        Directory.CreateDirectory(toolsDir);
        var zipPath = Path.Combine(toolsDir, "hdiffpatch.zip");
        using (var response = await http.GetAsync(HpatchzUrl, HttpCompletionOption.ResponseHeadersRead, cancellation))
        {
            response.EnsureSuccessStatusCode();
            await using var stream = await response.Content.ReadAsStreamAsync(cancellation);
            await using var file = new FileStream(zipPath, FileMode.Create, FileAccess.Write, FileShare.None);
            await stream.CopyToAsync(file, cancellation);
        }
        if (Sha256(zipPath) != HpatchzZipSha256)
        {
            File.Delete(zipPath);
            throw new InvalidDataException("The patch tool (HDiffPatch 5.1.3) did not download intact; try again.");
        }
        using (var zip = ZipFile.OpenRead(zipPath))
        {
            var entry = zip.GetEntry("windows64/hpatchz.exe") ?? throw new InvalidDataException("The patch tool's zip has no windows64/hpatchz.exe.");
            entry.ExtractToFile(hpatchz + ".partial", overwrite: true);
        }
        File.Delete(zipPath);
        if (Sha256(hpatchz + ".partial") != HpatchzSha256) throw new InvalidDataException("The patch tool is not the pinned hpatchz.exe.");
        File.Move(hpatchz + ".partial", hpatchz, overwrite: true);
        return hpatchz;
    }

    static async Task<string> DownloadPack(string tempFolder, DownpatchPack pack, HttpClient http,
        Action<long, long>? progress, CancellationToken cancellation)
    {
        var packPath = Path.Combine(tempFolder, $"downpatch-{pack.SourceBuild}-to-{pack.TargetBuild}.zip");

        // a pack downloaded before (setup stopped later): kept only while it is still the listed one
        if (File.Exists(packPath) && new FileInfo(packPath).Length == pack.Size && Sha256(packPath).Equals(pack.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            progress?.Invoke(pack.Size, pack.Size);
            return packPath;
        }

        var partial = packPath + ".partial";
        if (File.Exists(partial)) File.Delete(partial);

        using var response = await http.GetAsync(pack.Url, HttpCompletionOption.ResponseHeadersRead, cancellation);
        response.EnsureSuccessStatusCode();

        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long downloaded = 0;
        await using (var stream = await response.Content.ReadAsStreamAsync(cancellation))
        await using (var file = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 81920))
        {
            var buffer = new byte[81920];
            int read;
            while ((read = await stream.ReadAsync(buffer, cancellation)) > 0)
            {
                cancellation.ThrowIfCancellationRequested();
                downloaded += read;
                if (downloaded > pack.Size) break;
                sha.AppendData(buffer, 0, read);
                await file.WriteAsync(buffer.AsMemory(0, read), cancellation);
                progress?.Invoke(downloaded, pack.Size);
            }
        }

        var hash = Convert.ToHexString(sha.GetHashAndReset());
        if (downloaded != pack.Size || !hash.Equals(pack.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(partial);
            throw new InvalidDataException($"The build patch did not download intact (expected SHA-256 {pack.Sha256[..16]}...); try again.");
        }

        File.Move(partial, packPath, overwrite: true);
        return packPath;
    }

    // The first pack patched data/settings.ini from Steam's copy, which players change: this one file comes from here
    // instead. Later packs carry it themselves (an add). Checked against the build's hash like every other file.
    static readonly Dictionary<string, string> DownloadOverrides = new(StringComparer.Ordinal)
    {
        ["data/settings.ini"] = "https://files.hf-mod.com/downpatch/settings-25480438.ini"
    };

    // Writes the build's files into gameDestination from the pack. The pack's manifest must name exactly the build's files
    // and nothing else; each one written must have the build's size and SHA-1, or setup stops before it is used.
    public static void ApplyPack(DownpatchJob job, Action<long, long, string>? progress, CancellationToken cancellation)
    {
        using var zip = ZipFile.OpenRead(job.PackPath);
        var manifest = ReadManifest(zip, job);

        // exactly the build's files: a path outside the list (or ../) never reaches the file system
        var listed = job.Build.Files.ToDictionary(f => f.Path, StringComparer.Ordinal);
        CheckNamed(manifest, listed, job.Build);
        RequireSpace(zip, job, manifest, listed);

        long done = 0;
        long total = job.Build.TotalBytes;
        foreach (var file in manifest.Files!)
        {
            cancellation.ThrowIfCancellationRequested();
            var expected = listed[file.Path];
            var destPath = Destination(job.GameDestination, file);
            Directory.CreateDirectory(Path.GetDirectoryName(destPath)!);

            if (!Matches(destPath, expected)) WriteFile(zip, job, file, expected, destPath, cancellation);

            done += expected.Size;
            progress?.Invoke(done, total, file.Path);
        }
    }

    // The pack's downpatch.json, which must be for this pack's builds and the build being made.
    static DownpatchManifest ReadManifest(ZipArchive zip, DownpatchJob job)
    {
        var manifestEntry = zip.GetEntry("downpatch.json")
            ?? throw new InvalidDataException("The build patch has no downpatch.json.");

        DownpatchManifest manifest;
        using (var reader = new StreamReader(manifestEntry.Open()))
            manifest = JsonSerializer.Deserialize<DownpatchManifest>(reader.ReadToEnd(), ManifestJson)
                ?? throw new InvalidDataException("The build patch's manifest is empty.");
        if (manifest.TargetBuild != job.Build.BuildId || manifest.TargetBuild != job.Pack.TargetBuild || manifest.SourceBuild != job.Pack.SourceBuild)
            throw new InvalidDataException($"The build patch is for {manifest.SourceBuild} to {manifest.TargetBuild}, not {job.Pack.SourceBuild} to {job.Build.BuildId}.");
        return manifest;
    }

    // The manifest names each of the build's files once, and nothing else.
    static void CheckNamed(DownpatchManifest manifest, Dictionary<string, SupportedFile> listed, SupportedBuild build)
    {
        var named = new HashSet<string>(StringComparer.Ordinal);
        foreach (var file in manifest.Files ?? [])
            if (file == null || !listed.ContainsKey(file.Path) || !named.Add(file.Path))
                throw new InvalidDataException($"The build patch names a file that is not one of build {build.BuildId}'s once: {file?.Path}.");
        if (named.Count != listed.Count)
            throw new InvalidDataException($"The build patch leaves out {listed.Count - named.Count} of build {build.BuildId}'s files.");
    }

    // Room for every file not yet in place, and beside the pack for the largest patch extracted at a time. A drive
    // that filled up mid-patch used to end in hpatchz exit code 24 and "validate the game in Steam".
    static void RequireSpace(ZipArchive zip, DownpatchJob job, DownpatchManifest manifest, Dictionary<string, SupportedFile> listed)
    {
        var pending = manifest.Files!.Where(f => !GameCopier.Present(Destination(job.GameDestination, f), listed[f.Path])).ToList();
        long largestPatch = pending.Where(f => f.Op == "patch" && f.PatchFile is { Length: > 0 })
            .Select(f => zip.GetEntry("patches/" + f.PatchFile)?.Length ?? 0).DefaultIfEmpty(0).Max();
        DiskSpace.Require((job.GameDestination, pending.Sum(f => listed[f.Path].Size)), (Path.GetDirectoryName(job.PackPath)!, largestPatch));
    }

    static string Destination(string gameDestination, DownpatchFile file) =>
        Path.Combine(gameDestination, file.Path.Replace('/', Path.DirectorySeparatorChar));

    // One of the build's files at destPath, made by its operation (or downloaded, for DownloadOverrides), kept only with
    // the build's size and SHA-1.
    static void WriteFile(ZipArchive zip, DownpatchJob job, DownpatchFile file, SupportedFile expected, string destPath, CancellationToken cancellation)
    {
        var partial = destPath + ".partial";
        var src = Path.Combine(job.SteamGame, file.Path.Replace('/', Path.DirectorySeparatorChar));
        string FromSteam() => File.Exists(src) ? src
            : throw new InvalidDataException($"Steam is missing {file.Path}; validate the game in Steam and start setup again.");

        if (DownloadOverrides.TryGetValue(file.Path, out var overrideUrl) && file.Op != "add")
            DownloadOverride(job.Http, file, overrideUrl, partial, cancellation);
        else switch (file.Op)
        {
            case "copy":
                File.Copy(FromSteam(), partial, overwrite: true);
                break;
            case "patch":
                Patch(zip, job, file, FromSteam(), partial);
                break;
            case "add":
            {
                var addEntry = zip.GetEntry("add/" + file.Path)
                    ?? throw new InvalidDataException($"The build patch is missing add/{file.Path}.");
                addEntry.ExtractToFile(partial, overwrite: true);
                break;
            }
            default:
                throw new InvalidDataException($"Unknown operation in the build patch: {file.Op}");
        }

        if (!Matches(partial, expected))
        {
            if (File.Exists(partial)) File.Delete(partial);
            throw new InvalidDataException(file.Op is "copy" or "patch"
                ? $"{file.Path} did not come out as build {job.Build.BuildId}: Steam's copy is not the build this patch was made from. Validate the game in Steam and start setup again."
                : $"{file.Path} in the build patch is not build {job.Build.BuildId}'s.");
        }
        File.Move(partial, destPath, overwrite: true);
    }

    static void DownloadOverride(HttpClient? http, DownpatchFile file, string overrideUrl, string partial, CancellationToken cancellation)
    {
        if (http == null) throw new InvalidOperationException("No HTTP client for " + file.Path + ".");
        using var response = http.GetAsync(overrideUrl, cancellation).GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();
        using var stream = response.Content.ReadAsStreamAsync(cancellation).GetAwaiter().GetResult();
        using var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None);
        stream.CopyTo(output);
    }

    // Steam's file (from) through the pack's patch for it into partial, with hpatchz.
    static void Patch(ZipArchive zip, DownpatchJob job, DownpatchFile file, string from, string partial)
    {
        var patchEntry = (file.PatchFile is { Length: > 0 } name ? zip.GetEntry("patches/" + name) : null)
            ?? throw new InvalidDataException($"The build patch is missing the patch for {file.Path}.");
        var patchTemp = Path.Combine(Path.GetDirectoryName(job.PackPath)!, $"hf-patch-{Guid.NewGuid():N}.hdiff");
        try
        {
            patchEntry.ExtractToFile(patchTemp, overwrite: true);
            // hpatchz writes progress to its console: not redirected, so a full pipe never stalls it
            var psi = new ProcessStartInfo(job.Hpatchz) { ArgumentList = { "-f", from, patchTemp, partial }, UseShellExecute = false, CreateNoWindow = true };
            using var proc = Process.Start(psi) ?? throw new InvalidOperationException("hpatchz did not start.");
            proc.WaitForExit();
            if (proc.ExitCode != 0)
            {
                if (File.Exists(partial)) File.Delete(partial); /* a half-written file keeps the space it took */
                throw PatchFailed(file.Path, proc.ExitCode, partial);
            }
        }
        finally
        {
            if (File.Exists(patchTemp)) File.Delete(patchTemp);
        }
    }

    static bool Matches(string path, SupportedFile file) => SupportedBuild.Accepts(path, file);

    // hpatchz's failure (HDiffPatch 5.1.3, hpatchz.c THPatchResult): 24 is HPATCH_FILEWRITE_NO_SPACE_ERROR, the drive it
    // wrote `output` to filled up; anything else is Steam's copy or the patch.
    public static Exception PatchFailed(string file, int exitCode, string output) => exitCode == 24
        ? new NotEnoughSpaceException($"{new DriveInfo(Path.GetPathRoot(Path.GetFullPath(output))!).Name} ran out of space while patching {file} " +
            $"(hpatchz exit code 24).")
        : new InvalidDataException($"Patching {file} failed (hpatchz exit code {exitCode}): validate the game in Steam and start setup again.");

    sealed record DownpatchIndex(int SchemaVersion, DownpatchIndexEntry?[]? Packs);

    sealed record DownpatchIndexEntry(string? SourceBuild, string? TargetBuild, string? Url, string? Sha256, long Size,
        string? SourceExeSha256, string? SourceGameDllSha256);

    sealed record DownpatchManifest(string SourceBuild, string TargetBuild, DownpatchFile[]? Files);

    sealed record DownpatchFile
    {
        public string Path { get; init; } = "";
        public string Op { get; init; } = "";
        public long Size { get; init; }
        public string? Sha1 { get; init; }
        public string? PatchFile { get; init; }
        public long TargetSize { get; init; }
        public string? TargetSha1 { get; init; }
    }

    static readonly JsonSerializerOptions ManifestJson = new() { PropertyNameCaseInsensitive = true };
}
