using HellForge.Core;

namespace HellForge.SteamDownload;

// Where a chunk's bytes may already be on this PC: a file and the offset they start at. Only a candidate: the bytes read
// there are used only when their SHA-1 is the chunk's (a Steam chunk's id is the SHA-1 of its content).
public readonly record struct ChunkSource(string Path, ulong Offset);

// One chunk of a file of the supported build, with the places on this PC it may be copied from, best first. Known: the
// installed build's manifest lists a chunk with this content there, so it is expected to be copied, not downloaded.
public sealed record PlannedChunk(DepotChunk Chunk, IReadOnlyList<ChunkSource> Local, bool Known);

// One file of the supported build, as the chunks of the pinned manifest that make it up, in order.
public sealed record PlannedFile(SupportedFile File, IReadOnlyList<PlannedChunk> Chunks)
{
    public long KnownLocalBytes => Chunks.Where(c => c.Known).Sum(c => (long)c.Chunk.OriginalSize);
    // Bytes no local copy is expected for: what Valve's servers send unless the same-position guess turns out right.
    public long DownloadBytes => Chunks.Where(c => !c.Known).Sum(c => (long)c.Chunk.OriginalSize);
}

public sealed record DepotPlan(IReadOnlyList<PlannedFile> Files)
{
    public long TotalBytes => Files.Sum(f => f.File.Size);
    public long KnownLocalBytes => Files.Sum(f => f.KnownLocalBytes);
    public long DownloadBytes => Files.Sum(f => f.DownloadBytes);
}

// What is on this PC to copy from: Steam's installed game folder (null: none usable) and the manifests of the build it
// has (empty: unknown), which say which chunks each of its files holds.
public sealed record PlanSources(string? SteamGame, IReadOnlyList<DepotManifest> Installed);

// Plans how setup writes the supported build: every file is assembled from the chunks of its pinned depot manifest, each
// chunk copied from a local file that holds it (Steam's install of another build shares most of them) or downloaded.
// The plan only proposes; DepotAssembler checks every chunk and every file against its hash.
public static class DepotPlanner
{
    public static DepotPlan Plan(IReadOnlyList<SupportedFile> files, IReadOnlyDictionary<uint, DepotManifest> targets, PlanSources sources)
    {
        var known = KnownChunks(sources);
        return new([.. files.Select(file => PlanFile(file, TargetOf(file, targets), sources.SteamGame, known))]);
    }

    // The pinned manifest's entry for one of the build's files. Its size and SHA-1 must be the build's (the manifest is
    // the one supported-build.json was made from) and its chunks must cover the file exactly once, in order.
    public static DepotFile TargetOf(SupportedFile file, IReadOnlyDictionary<uint, DepotManifest> targets)
    {
        if (!targets.TryGetValue(file.Depot, out var manifest)) throw new InvalidDataException($"No manifest for depot {file.Depot}.");
        var entry = manifest.Files.FirstOrDefault(f => !f.IsDirectory && f.Path == file.Path)
            ?? throw new InvalidDataException($"Steam's manifest {manifest.DepotId}_{manifest.ManifestId} does not list {file.Path}.");
        if ((long)entry.Size != file.Size || !entry.Sha1.Equals(file.Sha1, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Steam's manifest {manifest.DepotId}_{manifest.ManifestId} lists {file.Path} other than the supported build has it.");
        ulong next = 0;
        foreach (var chunk in entry.Chunks.OrderBy(c => c.Offset))
        {
            if (chunk.Offset != next || chunk.OriginalSize == 0 || chunk.OriginalSize > MaxChunk || chunk.Sha1.Length != 40)
                throw new InvalidDataException($"Steam's manifest lists chunks for {file.Path} that do not cover it.");
            next += chunk.OriginalSize;
        }
        if (next != entry.Size) throw new InvalidDataException($"Steam's manifest lists chunks for {file.Path} that do not cover it.");
        return entry;
    }

    // Steam's chunks are 1 MiB at most; a larger one is a damaged manifest.
    public const uint MaxChunk = 16 << 20;

    static PlannedFile PlanFile(SupportedFile file, DepotFile target, string? steamGame, Dictionary<string, List<ChunkSource>> known)
    {
        var samePath = steamGame == null ? null : Path.Combine(steamGame, file.Path);
        var sameLength = samePath != null && File.Exists(samePath) ? new FileInfo(samePath).Length : -1;
        var chunks = new List<PlannedChunk>();
        foreach (var chunk in target.Chunks.OrderBy(c => c.Offset))
        {
            var local = new List<ChunkSource>();
            var isKnown = known.TryGetValue(chunk.Sha1.ToUpperInvariant(), out var listed);
            if (isKnown) local.AddRange(listed!);
            // the same bytes at the same place in Steam's file: most of an updated file is unchanged, and this needs no
            // manifest of the installed build
            var same = new ChunkSource(samePath ?? "", chunk.Offset);
            if (samePath != null && (long)(chunk.Offset + chunk.OriginalSize) <= sameLength && !local.Contains(same)) local.Add(same);
            chunks.Add(new(chunk, local, isKnown));
        }
        return new(file, chunks);
    }

    // Every chunk the installed build's manifests place in a file that is on disk at the size they list, by its SHA-1.
    static Dictionary<string, List<ChunkSource>> KnownChunks(PlanSources sources)
    {
        var known = new Dictionary<string, List<ChunkSource>>(StringComparer.Ordinal);
        if (sources.SteamGame == null) return known;
        foreach (var file in sources.Installed.SelectMany(m => m.Files).Where(f => !f.IsDirectory && SafeRelative(f.Path)))
        {
            var path = Path.Combine(sources.SteamGame, file.Path);
            var info = new FileInfo(path);
            if (!info.Exists || (ulong)info.Length != file.Size) continue;
            foreach (var chunk in file.Chunks)
            {
                var key = chunk.Sha1.ToUpperInvariant();
                if (!known.TryGetValue(key, out var places)) known[key] = places = [];
                if (places.Count < 4) places.Add(new(path, chunk.Offset));
            }
        }
        return known;
    }

    // A manifest's file name that stays inside the folder it is read from.
    static bool SafeRelative(string path) =>
        path.Length > 0 && !Path.IsPathRooted(path) && !path.Split('/', '\\').Any(part => part is ".." or "." or "");
}
