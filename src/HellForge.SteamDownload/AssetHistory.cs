using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.SteamDownload;

// The author's side of past builds (assets-check / assets-strip --history yes): what a mod still ships after matching it
// against the builds on this PC is checked against every build of the game since release, from Steam's depot manifests
// (each chunk's SHA-1 and rolling checksum; hashes only), with the author's own sign-in. Runs of the mod that are chunks
// Steam shipped are proven to be the game's, under any name; the game resources at those places, in the build that has
// them, are then fingerprinted alone (only the chunks they sit in are fetched) and the mod is matched again, so cut
// content from an older build goes into the recipe like anything else.
public static class AssetHistory
{
    // The chunk index of every data-depot manifest since release that Steam serves, built from the manifests (fetched
    // once, then cached) and kept beside the index.
    public static SteamChunkIndex ChunkIndex(SteamBuilds steam, Action<string>? log = null)
    {
        var history = DepotHistory.Embedded(GameBuilds.DataDepot).OrderBy(h => h.Time).ToList();
        var path = Path.Combine(steam.Store.Folder, $"steam-chunks-{history.Count}.hfsc");
        if (File.Exists(path))
        {
            try { return SteamChunkIndex.Load(path); }
            catch (Exception error) when (error is InvalidDataException or IOException or EndOfStreamException) { log?.Invoke($"The cached chunk index could not be read ({error.Message}); making it again."); }
        }
        var index = new SteamChunkIndex();
        var done = 0;
        foreach (var h in history)
        {
            var label = GameBuilds.Label(GameBuilds.DataDepot, h.Manifest.ToString());
            DepotManifest manifest;
            try { manifest = steam.Manifest(label); }
            catch (DepotAccessDeniedException) { continue; } // the launch-week manifests Steam no longer serves
            index.AddBuild(label, h.Time, manifest.Files.Where(f => !f.IsDirectory).Select(f => (f.Path,
                f.Chunks.Select(c => ((long)c.Offset, (int)c.OriginalSize, Convert.FromHexString(c.Sha1), c.Crc)))));
            if (++done % 10 == 0) log?.Invoke($"  {done} of {history.Count} past builds' manifests read...");
        }
        var partial = path + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
        index.Save(partial);
        File.Move(partial, path, overwrite: true);
        log?.Invoke($"Chunk index of {index.Builds.Count} builds since release made.");
        return index;
    }

    // From a first match's report: the past builds that hold what the mod still ships, with the game resources there,
    // fingerprinted into the index (as partial builds), opened from Steam for the second match. Empty when nothing the mod
    // ships is a chunk of a build not already in the index.
    public static List<IGameResources> Resolve(string modFolder, SourceIndex index, MigrationReport report, SteamChunkIndex chunks, SteamBuilds steam,
        Action<string>? log = null)
    {
        var plan = Plan(modFolder, index, report, chunks, steam, out var opened);
        foreach (var (build, (archives, keys)) in plan)
        {
            log?.Invoke($"Fingerprinting {keys.Count} game resources of build {build} (from {chunks.Builds[chunks.BuildIndex(build)].Time:yyyy-MM-dd}) from Steam...");
            index.Add(opened[build], null, archives, keys);
        }
        index.Seal();
        return [.. plan.Keys.Select(b => opened[b])];
    }

    // Per past build: the archives and resources to fingerprint. Each run the mod still ships that is a Steam chunk is
    // placed in the latest build that has it (or one already planned), and the resources at that place in that build
    // are taken, with the mod's own leftover resources of that file (too small for a chunk of their own to prove them).
    static Dictionary<string, (HashSet<string> Archives, HashSet<ResourceKey> Keys)> Plan(string modFolder, SourceIndex index, MigrationReport report,
        SteamChunkIndex chunks, SteamBuilds steam, out Dictionary<string, IGameResources> opened)
    {
        var plan = new Dictionary<string, (HashSet<string> Archives, HashSet<ResourceKey> Keys)>(StringComparer.Ordinal);
        var games = new Dictionary<string, IGameResources>(StringComparer.Ordinal);
        foreach (var (path, kept) in report.Kept.Where(k => k.Value.Count > 0))
        {
            var leftover = report.Resources.Where(r => r.File == path && !r.InIndex && r.Literal > 0).Select(r => r.Key).ToList();
            foreach (var match in chunks.Scan(File.ReadAllBytes(Path.Combine(modFolder, path)), kept))
                if (Place(match, chunks, plan, index) is { } place)
                    AddPlace(plan, games, steam, place, match.Length, leftover);
        }
        opened = games;
        return plan;
    }

    // The game resources at one proven place of a build (opened from Steam on first use), and the mod's leftover
    // resources of that file, into the plan.
    static void AddPlace(Dictionary<string, (HashSet<string> Archives, HashSet<ResourceKey> Keys)> plan, Dictionary<string, IGameResources> games,
        SteamBuilds steam, (string Build, string File, long Offset) place, int length, IReadOnlyList<ResourceKey> leftover)
    {
        var game = games.TryGetValue(place.Build, out var known) ? known : games[place.Build] = steam.Open(place.Build);
        var found = game.At(place.File["data/".Length..], place.Offset, length).ToList();
        if (found.Count == 0) return;
        if (!plan.TryGetValue(place.Build, out var entry)) plan[place.Build] = entry = (new(StringComparer.Ordinal), []);
        foreach (var (key, archive) in found)
        {
            entry.Archives.Add(archive);
            entry.Keys.Add(key);
        }
        foreach (var key in leftover) entry.Keys.Add(key);
    }

    // The build to read a matched chunk from, and where it is there: one already planned that has it, else the latest;
    // null when that build is already whole in the index, or the chunk is not in its data folder.
    static (string Build, string File, long Offset)? Place(SteamChunkIndex.Match match, SteamChunkIndex chunks,
        Dictionary<string, (HashSet<string> Archives, HashSet<ResourceKey> Keys)> plan, SourceIndex index)
    {
        var ids = chunks.BuildIdsOf(match.Chunk).ToList();
        if (ids.Count == 0) return null;
        var pick = ids.Where(b => plan.ContainsKey(chunks.Builds[b].Label)).DefaultIfEmpty(ids.MaxBy(b => chunks.Builds[b].Time)).First();
        var build = chunks.Builds[pick].Label;
        if (index.Builds.Contains(build) && !index.PartialBuilds.Contains(build)) return null;
        if (chunks.Locate(match.Chunk, pick) is not { } at || !at.File.StartsWith("data/", StringComparison.Ordinal)) return null;
        return (build, at.File, at.Offset);
    }
}
