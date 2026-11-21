using System.IO.Compression;
using System.Runtime.ExceptionServices;
using System.Security.Cryptography;
using System.Text.Json;

namespace HellForge.Core;

// A build in a downpatch file: its Steam build id, the hashes that name its executable and game.dll (BuildPin), and its
// depot manifests.
public sealed record DownpatchBuild(string BuildId, string ExeSha256, string GameDllSha256, IReadOnlyList<SupportedDepot> Depots);

// One file of the build a downpatch file makes. Op "same": the source's file at From is already this file. "patch": the
// delta in Entry makes it from the source's file at From (FromSize, FromSha1). "new": Entry carries it whole.
public sealed record DownpatchEntry(string Path, string Op, long Size, string Sha1, string? From = null, long FromSize = 0,
    string? FromSha1 = null, string? Entry = null, long EntrySize = 0, string? EntrySha256 = null);

public sealed record DownpatchManifest(string Format, int FormatVersion, string AppId, DownpatchBuild Source, DownpatchBuild Target,
    string CreatedWith, IReadOnlyList<DownpatchEntry> Files);

// The HELLDIVERS 2 a downpatch file is made from or applied to: Steam's installed game.
public sealed record DownpatchSource(string Folder, BuildPin Pin, IReadOnlyList<SupportedDepot> Depots)
{
    public static DownpatchSource Of(SteamGame game)
    {
        var app = ValveData.Parse(File.ReadAllText(game.ManifestPath)).Children["AppState"];
        var depots = app.Children.TryGetValue("InstalledDepots", out var installed)
            ? installed.Children.Where(d => uint.TryParse(d.Key, out _)).OrderBy(d => d.Key, StringComparer.Ordinal)
                .Select(d => new SupportedDepot(uint.Parse(d.Key), d.Value.Values.GetValueOrDefault("manifest") ?? "")).ToList()
            : [];
        return new(game.Report.Source, game.Report.Pin, depots);
    }
}

public sealed record DownpatchProgress(string Stage, long Done, long Total, string File);

// What a downpatch file made or wrote: the builds, its files by operation, the bytes of the build and of the file.
public sealed record DownpatchResult(string SourceBuild, string TargetBuild, int Files, int Same, int Patched, int New, int Kept,
    long Bytes, long PackBytes, double Seconds);

// A downpatch file: HELLDIVERS 2 as Steam has it (the source build) into the build HellForge runs, made on a player's PC
// from their Steam game and their HellForge installation (DownpatchMaker), passed on by players themselves. HellForge never
// hosts, fetches or lists them. One zip: hellforge-downpatch.json lists every file of the build, each one either the
// source's own file, a BinaryDelta from it, or carried whole; the deltas are stored, not compressed again. Applying
// trusts nothing in the file: every file written must have the build's size and SHA-1 from supported-build.json.
public static partial class Downpatch
{
    public const string Format = "hellforge-downpatch";
    public const int FormatVersion = 1;
    public const string ManifestName = "hellforge-downpatch.json";
    const long ManifestLimit = 16 << 20;

    // The free-space lookup; tests give a full drive.
    internal static Func<string, DiskSpaceInfo> SpaceOf { get; set; } = DiskSpace.ForFolder;

    public static string FileName(string sourceBuild, string targetBuild) => $"HellForge-downpatch-{sourceBuild}-to-{targetBuild}.zip";

    // The manifest of the file at packPath, checked for its format and that each entry it names is in the zip.
    public static DownpatchManifest ReadManifest(string packPath)
    {
        using var zip = OpenZip(packPath);
        return ReadManifest(zip);
    }

    static ZipArchive OpenZip(string packPath)
    {
        try { return new ZipArchive(new FileStream(packPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16), ZipArchiveMode.Read); }
        catch (InvalidDataException error) { throw new InvalidDataException("This is not a HellForge downpatch file, or it is damaged (it is not a whole zip): copy it again.", error); }
    }

    static DownpatchManifest ReadManifest(ZipArchive zip)
    {
        var entry = zip.GetEntry(ManifestName);
        if (entry == null || entry.Length > ManifestLimit) throw new InvalidDataException($"This is not a HellForge downpatch file: it has no {ManifestName}.");
        DownpatchManifest? manifest;
        try
        {
            using var stream = entry.Open();
            manifest = JsonSerializer.Deserialize(stream, CoreJsonCamel.Default.DownpatchManifest);
        }
        catch (JsonException error) { throw new InvalidDataException("The downpatch file's list of files is damaged: copy the file again.", error); }
        if (manifest?.Format != Format) throw new InvalidDataException("This is not a HellForge downpatch file.");
        if (manifest.FormatVersion != FormatVersion)
            throw new InvalidDataException($"This downpatch file has format {manifest.FormatVersion}, which this HellForge cannot read: it was made by another version.");
        if (manifest.AppId != LocalInstall.AppId || manifest.Source?.BuildId == null || manifest.Target?.BuildId == null || manifest.Files == null ||
            manifest.Files.Any(f => f?.Path == null || f.Op == null || f.Sha1 == null))
            throw new InvalidDataException("The downpatch file's list of files is incomplete: copy the file again.");
        foreach (var file in manifest.Files.Where(f => f.Entry != null))
            if (zip.GetEntry(file.Entry!) is not { } part || part.Length != file.EntrySize)
                throw new InvalidDataException($"The downpatch file is damaged: its part for {file.Path} is missing or cut short. Copy it again.");
        return manifest;
    }

    // The manifest's files against the build: each of the build's files once, with the build's size and hash (any
    // version of a replaceable one), each operation complete, and every path a plain relative one.
    public static IReadOnlyList<(DownpatchEntry Entry, SupportedFile File)> Plan(DownpatchManifest manifest, SupportedBuild build)
    {
        if (manifest.Target.BuildId != build.BuildId)
            throw new InvalidDataException($"This downpatch file makes HELLDIVERS 2 build {manifest.Target.BuildId}, but this HellForge runs build {build.BuildId}: it needs a file made by this version of HellForge.");
        var listed = build.Files.ToDictionary(f => f.Path, StringComparer.Ordinal);
        var plan = new List<(DownpatchEntry, SupportedFile)>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in manifest.Files)
        {
            if (!listed.TryGetValue(entry.Path, out var file) || !seen.Add(entry.Path))
                throw new InvalidDataException($"The downpatch file names a file that is not one of build {build.BuildId}'s, or names it twice: {entry.Path}.");
            if (!SupportedBuild.IsReplaceable(file.Path) && (entry.Size != file.Size || !entry.Sha1.Equals(file.Sha1, StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException($"The downpatch file's {entry.Path} is not build {build.BuildId}'s.");
            if (!Complete(entry)) throw new InvalidDataException($"The downpatch file's entry for {entry.Path} is not one this HellForge can use.");
            plan.Add((entry, file));
        }
        if (seen.Count != listed.Count)
            throw new InvalidDataException($"The downpatch file leaves out {listed.Count - seen.Count} of build {build.BuildId}'s files.");
        return plan;
    }

    static bool Complete(DownpatchEntry entry) => entry.Op switch
    {
        "same" => IsPlainPath(entry.From),
        "patch" => IsPlainPath(entry.From) && entry.Entry != null && entry.EntrySha256 != null,
        "new" => entry.From == null && entry.Entry != null && entry.EntrySha256 != null,
        _ => false
    };

    // A path inside the game folder: relative, forward slashes, no empty, "." or ".." parts, nothing a file system reads
    // as a drive or stream.
    public static bool IsPlainPath(string? path) =>
        path is { Length: > 0 and < 400 } && path.IndexOfAny(['\\', ':', '\0']) < 0 && !path.StartsWith('/') &&
        path.Split('/').All(part => part.Length > 0 && part != "." && part != ".." && part.Trim() == part);

    // The source must be the build the file was made from: the same executable and game.dll (Steam's build id can lag
    // behind its files). Says which build the file needs when it is another one.
    public static void CheckSource(DownpatchManifest manifest, DownpatchSource source)
    {
        var made = manifest.Source;
        if (made.ExeSha256.Equals(source.Pin.ExeSha256, StringComparison.OrdinalIgnoreCase) &&
            made.GameDllSha256.Equals(source.Pin.GameDllSha256, StringComparison.OrdinalIgnoreCase)) return;
        if (made.BuildId != source.Pin.BuildId)
            throw new InvalidDataException($"This downpatch file was made from HELLDIVERS 2 build {made.BuildId}, but your Steam has build {source.Pin.BuildId}. " +
                $"It only works on build {made.BuildId}. Ask for one made from build {source.Pin.BuildId}, or use the Steam download.");
        throw new InvalidDataException($"Your Steam's HELLDIVERS 2 says it is build {source.Pin.BuildId}, but its game files are not that build's: " +
            "verify the game in Steam, then try again.");
    }

    // Writes the build into destination from Steam's game (source) and the downpatch file. A file already there with the
    // build's content is kept, so a stopped run continues; each one written is checked, then moved in place.
    public static DownpatchResult Apply(string packPath, DownpatchSource source, string destination, SupportedBuild build,
        Action<DownpatchProgress>? progress = null, CancellationToken cancellation = default)
    {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        packPath = LocalInstall.FullPath(packPath);
        var sourceRoot = LocalInstall.NoLinks(source.Folder, source.Folder);
        destination = LocalInstall.NoLinks(destination, destination);
        if (LocalInstall.Within(destination, sourceRoot) || LocalInstall.Within(sourceRoot, destination))
            throw new InvalidDataException("The game folder must not overlap Steam's HELLDIVERS 2 folder.");
        DownpatchManifest manifest;
        using (var zip = OpenZip(packPath)) manifest = ReadManifest(zip);
        var plan = Plan(manifest, build);
        CheckSource(manifest, source);
        foreach (var (entry, _) in plan.Where(p => p.Entry.From != null))
            if (new FileInfo(LocalInstall.NoLinks(Path.Combine(sourceRoot, entry.From!), sourceRoot)) is not { Exists: true } from || from.Length != entry.FromSize)
                throw new InvalidDataException($"Steam's HELLDIVERS 2 lacks {entry.From}, or it is not the file build {manifest.Source.BuildId} has: verify the game in Steam, then try again.");
        Directory.CreateDirectory(destination);
        DiskSpace.Require(SpaceOf, (destination, plan.Where(p => !GameCopier.Present(Path.Combine(destination, p.File.Path), p.File)).Sum(p => p.File.Size)));

        var job = new ApplyJob(sourceRoot, destination, manifest.Source.BuildId, build.BuildId, plan.Sum(p => p.File.Size), progress);
        RunParallel(plan.OrderByDescending(p => p.File.Size).ToList(), () => OpenZip(packPath),
            (zip, item) => job.Write(zip, item.Entry, item.File, cancellation), cancellation);
        return new(manifest.Source.BuildId, build.BuildId, plan.Count, Count(plan, "same"), Count(plan, "patch"), Count(plan, "new"), job.Kept,
            plan.Sum(p => p.File.Size), new FileInfo(packPath).Length, clock.Elapsed.TotalSeconds);
    }

    static int Count(IReadOnlyList<(DownpatchEntry Entry, SupportedFile File)> plan, string op) => plan.Count(p => p.Entry.Op == op);

    // Runs work on items with a few threads, each with its own state (a zip reader: one is not safe to share). The
    // first failure is rethrown as it was.
    internal static void RunParallel<TState, TItem>(IReadOnlyList<TItem> items, Func<TState> state, Action<TState, TItem> work,
        CancellationToken cancellation, int? workers = null) where TState : IDisposable
    {
        try
        {
            Parallel.ForEach(items, new ParallelOptions { MaxDegreeOfParallelism = workers ?? Math.Clamp(Environment.ProcessorCount / 2, 1, 4), CancellationToken = cancellation },
                state, (item, _, own) => { work(own, item); return own; }, own => own.Dispose());
        }
        catch (AggregateException error)
        {
            var first = error.Flatten().InnerExceptions.FirstOrDefault(e => e is not OperationCanceledException) ?? error.InnerExceptions[0];
            ExceptionDispatchInfo.Capture(first).Throw();
        }
    }

    sealed class ApplyJob(string sourceRoot, string destination, string sourceBuild, string targetBuild, long total,
        Action<DownpatchProgress>? progress)
    {
        long done;
        int kept;
        public int Kept => kept;

        void Moved(long bytes, string file)
        {
            var now = Interlocked.Add(ref done, bytes);
            progress?.Invoke(new("Writing the game files", now, total, file));
        }

        public void Write(ZipArchive zip, DownpatchEntry entry, SupportedFile file, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            var to = Path.Combine(destination, file.Path);
            if (OperatingSystem.IsLinux()) to = LocalInstall.NoLinks(to, destination);
            if (SupportedBuild.Accepts(to, file))
            {
                Interlocked.Increment(ref kept);
                Moved(file.Size, file.Path);
                return;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            var partial = to + ".partial";
            string? problem;
            try { problem = WritePartial(zip, entry, partial, cancellation); }
            catch
            {
                File.Delete(partial);
                throw;
            }
            if (problem != null)
            {
                File.Delete(partial);
                throw new InvalidDataException(problem);
            }
            File.Move(partial, to, overwrite: true);
        }

        // The file at partial, made by its operation; null when it came out as the build's file, else why not.
        string? WritePartial(ZipArchive zip, DownpatchEntry entry, string partial, CancellationToken cancellation)
        {
            var from = entry.From == null ? null : LocalInstall.NoLinks(Path.Combine(sourceRoot, entry.From), sourceRoot);
            using var file = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
            using var output = new HashingStream(file, HashAlgorithmName.SHA1, leaveOpen: true) { Passed = count => Moved(count, entry.Path) };
            var damaged = false;
            if (entry.Op == "same")
            {
                using var input = new FileStream(from!, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
                input.CopyTo(output, 1 << 20);
            }
            else
            {
                using var part = new HashingStream(zip.GetEntry(entry.Entry!)!.Open(), HashAlgorithmName.SHA256);
                using var handle = from == null ? null : File.OpenHandle(from, FileMode.Open, FileAccess.Read, FileShare.Read);
                try { BinaryDelta.Apply(handle, part, output, cancellation); }
                catch (InvalidDataException) { damaged = true; }
                part.Drain();
                damaged |= part.Bytes != entry.EntrySize || !part.Hex().Equals(entry.EntrySha256, StringComparison.OrdinalIgnoreCase);
            }
            file.Flush(true);
            if (damaged) return $"The downpatch file is damaged (its part for {entry.Path}): copy it again from whoever made it, or make a new one.";
            if (output.Bytes == entry.Size && output.Hex().Equals(entry.Sha1, StringComparison.OrdinalIgnoreCase)) return null;
            return entry.Op == "new"
                ? $"The downpatch file's {entry.Path} is not build {targetBuild}'s: the file is damaged."
                : $"{entry.Path} did not come out as build {targetBuild}'s: Steam's {entry.From} is not the file build {sourceBuild} has. Verify the game in Steam, then try again.";
        }
    }
}
