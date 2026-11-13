using System.Text.Json;

namespace HellForge.Core.AssetMigration;

// Every build of the game the migrator knows of (data/game-builds.json): its Steam depots at that build's manifests (what
// "download_depot <app> <depot> <manifest>" fetches) and its data layout ("nxa": bundles.nxa; "loose": one file per
// archive). HistoryComplete says the list holds every build since release; maintainers set it once it does.
public sealed class BuildCatalog
{
    public sealed record Depot(uint Id, string ManifestId);
    // Seen: the earliest date this build was seen installed (yyyy-MM-dd), for ordering; Layout "" when not known yet.
    public sealed record Build(string BuildId, string Layout, List<Depot> Depots, string? Note = null, string? Seen = null);

    public int SchemaVersion { get; init; } = 1;
    public string AppId { get; init; } = "553850";
    public bool HistoryComplete { get; init; }
    public List<Build> Builds { get; init; } = [];

    public static BuildCatalog Load(string path) =>
        JsonSerializer.Deserialize(File.ReadAllText(path), AssetJson.Default.BuildCatalog) ?? throw new InvalidDataException($"{path} is empty.");

    public static BuildCatalog Parse(string json) =>
        JsonSerializer.Deserialize(json, AssetJson.Default.BuildCatalog) ?? throw new InvalidDataException("The build catalog is empty.");

    public Build? Find(string buildId) => Builds.FirstOrDefault(b => b.BuildId == buildId);

    // The data depot (553853 holds the game's data) and its manifest history since release: the index covers the game
    // once every manifest in it that Steam serves is indexed. Without a history the catalog's builds are the list.
    public uint DataDepot { get; init; } = 553853;
    [System.Text.Json.Serialization.JsonIgnore] public List<DepotHistory>? History { get; set; }
    // Manifests Steam refuses to serve (the launch-week builds): nobody can index them, so they are a known gap.
    [System.Text.Json.Serialization.JsonIgnore] public HashSet<ulong> Unavailable { get; set; } = [];

    // Loads the catalog with the data depot's history beside it (data/depot-history/<depot>.txt, and the manifests Steam
    // refused, <depot>.unavailable), when they are there.
    public static BuildCatalog LoadWithHistory(string path)
    {
        var catalog = Load(path);
        var folder = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path))!, "depot-history");
        var history = Path.Combine(folder, $"{catalog.DataDepot}.txt");
        if (File.Exists(history)) catalog.History = DepotHistory.Load(history);
        var unavailable = Path.Combine(folder, $"{catalog.DataDepot}.unavailable");
        if (File.Exists(unavailable))
            catalog.Unavailable = File.ReadLines(unavailable).Select(l => l.Split('#')[0].Trim()).Where(l => l.Length > 0).Select(ulong.Parse).ToHashSet();
        return catalog;
    }

    // What the index still lacks: data manifests of the history (or catalog builds, without one) it holds neither as
    // "<depot>:<manifest>" nor through a catalog build with that manifest.
    public List<string> Missing(SourceIndex index)
    {
        if (History is null) return Builds.Where(b => !index.Builds.Contains(b.BuildId)).Select(b => b.BuildId).ToList();
        var indexed = index.Builds.Where(b => !index.PartialBuilds.Contains(b)).ToHashSet();
        foreach (var b in Builds.Where(b => indexed.Contains(b.BuildId)))
            foreach (var d in b.Depots.Where(d => d.Id == DataDepot)) indexed.Add($"{DataDepot}:{d.ManifestId}");
        return History.Where(h => !Unavailable.Contains(h.Manifest) && !indexed.Contains($"{DataDepot}:{h.Manifest}"))
            .Select(h => $"{h.Time:yyyy-MM-dd} ({DataDepot}:{h.Manifest}{(h.Branch is null ? "" : ", " + h.Branch)})").ToList();
    }

    // The data manifests of the history (that Steam serves) the name map does not have yet.
    public List<string> MissingNames(NameMap names)
    {
        if (History is null) return [];
        var have = names.Builds.ToHashSet(StringComparer.Ordinal);
        return History.Where(h => !Unavailable.Contains(h.Manifest) && !have.Contains($"{DataDepot}:{h.Manifest}"))
            .Select(h => $"{h.Time:yyyy-MM-dd} ({DataDepot}:{h.Manifest})").ToList();
    }

    // Every build since release that can be indexed is: content found in none of them is not the game's.
    public bool Covers(SourceIndex index) => HistoryComplete && Missing(index).Count == 0;

    // Builds from Steam's content log (Steam/logs/content_log*.txt): every update Steam finished installing is logged as
    //   [date time] AppID 553850 finished update, 3 mounted depots (BuildID 23354408) : 553851 (3449...),553853 (1424...),
    // which gives the build with each depot's manifest. Builds already in the catalog are left as they are.
    static readonly System.Text.RegularExpressions.Regex Finished = new(
        @"^\[(?<date>\d{4}-\d{2}-\d{2})[^\]]*\] AppID (?<app>\d+) finished update, \d+ mounted depots \(BuildID (?<build>\d+)\) : (?<depots>.*)$");
    static readonly System.Text.RegularExpressions.Regex DepotEntry = new(@"(?<depot>\d+) \((?<manifest>\d+)\)");

    public static List<Build> FromSteamLog(IEnumerable<string> lines, string appId)
    {
        var found = new Dictionary<string, Build>(StringComparer.Ordinal);
        foreach (var line in lines)
        {
            var m = Finished.Match(line.TrimEnd());
            if (!m.Success || m.Groups["app"].Value != appId) continue;
            var depots = DepotEntry.Matches(m.Groups["depots"].Value).Select(d => new Depot(uint.Parse(d.Groups["depot"].Value), d.Groups["manifest"].Value)).ToList();
            var id = m.Groups["build"].Value;
            if (depots.Count > 0 && !found.ContainsKey(id)) found[id] = new Build(id, "", depots, "from a Steam content log", m.Groups["date"].Value);
        }
        return [.. found.Values];
    }

    // The catalog with these builds added (those it does not have yet), in order of when they were seen.
    public BuildCatalog Merge(IEnumerable<Build> builds)
    {
        var all = Builds.ToList();
        foreach (var b in builds)
            if (all.All(x => x.BuildId != b.BuildId)) all.Add(b);
            else
            {
                var i = all.FindIndex(x => x.BuildId == b.BuildId);
                if (all[i].Seen is null && b.Seen is not null) all[i] = all[i] with { Seen = b.Seen };
            }
        return new BuildCatalog
        {
            SchemaVersion = SchemaVersion, AppId = AppId, HistoryComplete = HistoryComplete,
            Builds = all.OrderBy(b => b.Seen ?? "9999").ThenBy(b => long.Parse(b.BuildId)).ToList(),
        };
    }

    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, AssetJson.Default.BuildCatalog) + "\n");

    public IEnumerable<string> Commands(Build build) => build.Depots.Select(d => $"download_depot {AppId} {d.Id} {d.ManifestId}");
}

public enum VerdictKind { Verified, NoGameContent, PartlyMigrated, NeedsReview, NotMigrated }

// What happens to an uploaded mod, from its migration:
//   NotMigrated     the recipe did not rebuild the mod byte for byte: it is kept as it was.
//   NeedsReview     it shares content with an indexed build the migrator could not read: kept as it was.
//   PartlyMigrated  it rebuilds, the indexed builds' content is purged, but it keeps resources no indexed build has while
//                   the index does not cover the whole history: they may be cut content from a build not indexed yet.
//   Verified        it rebuilds and every piece of the game it holds is purged; what is left is in none of the builds
//                   since release, so it is the author's own.
//   NoGameContent   nothing in it is from the game: it ships as it is.
public sealed record MigrationVerdict(VerdictKind Kind, string Text)
{
    public bool Publishable => Kind is VerdictKind.Verified or VerdictKind.NoGameContent;

    // With a name map that covers the history, a leftover resource is checked by name against every build: a name the
    // game has (in any build) is the game's, and stays in review until its content is fingerprinted (resolve does it);
    // a name no build has is the author's own. Without one, the content index's coverage decides, as before.
    public static MigrationVerdict Decide(MigrationReport report, SourceIndex index, BuildCatalog catalog, int recipeFiles, string? rebuildError, NameMap? names)
    {
        if (names is null) return Decide(report, index, catalog, recipeFiles, rebuildError);
        if (rebuildError is not null) return new(VerdictKind.NotMigrated, $"NOT MIGRATED: {rebuildError} The mod is kept as it was, for review.");
        if (report.Unreadable.Count > 0) return Decide(report, index, catalog, recipeFiles, rebuildError);
        if (ShippedBySteam(report) is { } shipped) return shipped;
        var unknown = report.Resources.Where(r => !r.InIndex && r.Literal > 0).ToList();
        var byName = unknown.Where(r => names.Has(r.Key)).ToList();
        var own = unknown.Except(byName).ToList();
        if (byName.Count > 0)
        {
            var builds = byName.SelectMany(r => names.Find(r.Key)).Select(f => f.Build).Distinct().ToList();
            return new(VerdictKind.PartlyMigrated,
                $"PARTLY MIGRATED, NEEDS REVIEW: {byName.Count} resource parts ({byName.Sum(r => r.Literal):N0} bytes) are game resources by name, from {builds.Count} builds " +
                $"({string.Join(", ", builds.Take(4))}{(builds.Count > 4 ? ", ..." : "")}), whose content is not fingerprinted yet: resolve fingerprints them and migrates again.");
        }
        var missing = catalog.MissingNames(names);
        if (own.Count > 0 && (!catalog.HistoryComplete || missing.Count > 0))
            return new(VerdictKind.PartlyMigrated,
                $"PARTLY MIGRATED, NEEDS REVIEW: {own.Count} resource parts ({own.Sum(r => r.Literal):N0} bytes) are in no build checked, but the name map lacks {missing.Count} builds " +
                $"({string.Join(", ", missing.Take(4))}{(missing.Count > 4 ? ", ..." : "")}).");
        if (recipeFiles == 0 && own.Count == 0) return new(VerdictKind.NoGameContent, "NO GAME CONTENT FOUND: the mod ships as it is.");
        var rest = own.Count > 0
            ? $" {own.Count} resource parts ({own.Sum(r => r.Literal):N0} bytes) have names no build since release has (checked by name against all {names.Builds.Count} builds; " +
              $"by content against {index.Builds.Count - index.PartialBuilds.Count} whole builds and parts of {index.PartialBuilds.Count}): the author's own."
            : "";
        return recipeFiles == 0
            ? new(VerdictKind.NoGameContent, $"NO GAME CONTENT FOUND: the mod ships as it is.{rest}")
            : new(VerdictKind.Verified, $"VERIFIED: all {recipeFiles} recipe files rebuilt from the game folders match the originals (SHA-256); game content purged.{rest}");
    }

    // Bytes still shipped that are chunks Steam shipped are the game's (proven by SHA-1 against the depot manifests),
    // however the resource is named: the mod is held until resolve fingerprints them from that build.
    static MigrationVerdict? ShippedBySteam(MigrationReport report)
    {
        var (bytes, runs) = report.ShippedLeft();
        if (bytes == 0) return null;
        var files = runs.GroupBy(r => r.Run.GameFile).OrderByDescending(g => g.Sum(r => r.Bytes)).ToList();
        var where = string.Join("; ", files.Take(3).Select(g => $"{g.Key} ({g.Sum(r => r.Bytes):N0} bytes), in {g.Max(r => r.Run.Builds)} builds from {g.Min(r => r.Run.First):yyyy-MM-dd} to {g.Max(r => r.Run.Last):yyyy-MM-dd}"));
        return new(VerdictKind.PartlyMigrated,
            $"PARTLY MIGRATED, NEEDS REVIEW: {bytes:N0} bytes the mod still ships are chunks Steam shipped in the game's depot (proven by SHA-1 against the depot manifests): " +
            $"{where}{(files.Count > 3 ? $"; and {files.Count - 3} more game files" : "")}. resolve --chunks fingerprints them from that build and migrates again.");
    }

    public static MigrationVerdict Decide(MigrationReport report, SourceIndex index, BuildCatalog catalog, int recipeFiles, string? rebuildError)
    {
        if (rebuildError is not null) return new(VerdictKind.NotMigrated, $"NOT MIGRATED: {rebuildError} The mod is kept as it was, for review.");
        if (report.Unreadable.Count > 0)
        {
            var builds = report.Unreadable.SelectMany(c => index.Contents[c].Places).Select(p => index.Builds[p.Build]).Distinct();
            return new(VerdictKind.NeedsReview, $"NEEDS REVIEW: the mod shares content with {report.Unreadable.Count} game resource parts from builds not given here ({string.Join(", ", builds)}); it is kept as it was.");
        }
        if (ShippedBySteam(report) is { } shipped) return shipped;
        var unknown = report.Resources.Where(r => !r.InIndex && r.Literal > 0).ToList();
        long unknownBytes = unknown.Sum(r => r.Literal);
        var covered = catalog.Covers(index);
        if (unknown.Count > 0 && !covered)
        {
            var missing = catalog.Missing(index);
            var why = missing.Count > 0
                ? $"{missing.Count} known builds are not indexed yet ({string.Join(", ", missing.Take(6))}{(missing.Count > 6 ? ", ..." : "")})"
                : "the build catalog does not list every build since release yet";
            var done = recipeFiles > 0
                ? $"the recipe rebuilds all {recipeFiles} files byte for byte (SHA-256) and the content of the {index.Builds.Count} indexed builds is purged"
                : $"the mod has no content of the {index.Builds.Count} indexed builds";
            return new(VerdictKind.PartlyMigrated,
                $"PARTLY MIGRATED, NEEDS REVIEW: {done}, " +
                $"but {unknown.Count} resource parts ({unknownBytes:N0} bytes) are resources no indexed build has, and {why}. They may be cut content from a build not indexed yet.");
        }
        if (recipeFiles == 0) return new(VerdictKind.NoGameContent, "NO GAME CONTENT FOUND: the mod ships as it is.");
        var rest = unknown.Count > 0
            ? $" {unknown.Count} resource parts ({unknownBytes:N0} bytes) are in none of the {index.Builds.Count} builds since release: the author's own."
            : "";
        return new(VerdictKind.Verified, $"VERIFIED: all {recipeFiles} recipe files rebuilt from the game folders match the originals (SHA-256); game content purged.{rest}");
    }
}

// A depot's manifest history (data/depot-history/<depot>.txt, from SteamDB): "<UTC time> <manifest> [branch]" lines,
// newest first; '#' starts a comment.
public sealed record DepotHistory(DateTime Time, ulong Manifest, string? Branch)
{
    public static List<DepotHistory> Load(string path) => Parse(File.ReadLines(path));

    // The history HellForge ships (data/depot-history/<depot>.txt, embedded): every manifest since release with the
    // branch it was published on, which Steam needs to send an old manifest. Empty for a depot it has none of.
    public static List<DepotHistory> Embedded(uint depot)
    {
        lock (embedded)
        {
            if (embedded.TryGetValue(depot, out var known)) return known;
            using var stream = typeof(DepotHistory).Assembly.GetManifestResourceStream($"HellForge.Core.depot-history.{depot}.txt");
            if (stream == null) return embedded[depot] = [];
            using var reader = new StreamReader(stream);
            return embedded[depot] = Parse(reader.ReadToEnd().Split('\n'));
        }
    }

    static readonly Dictionary<uint, List<DepotHistory>> embedded = [];

    public static List<DepotHistory> Parse(IEnumerable<string> lines) => lines
        .Select(l => l.Trim())
        .Where(l => l.Length > 0 && !l.StartsWith('#'))
        .Select(l => l.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        .Select(p => new DepotHistory(DateTime.Parse(p[0], null, System.Globalization.DateTimeStyles.AdjustToUniversal | System.Globalization.DateTimeStyles.AssumeUniversal), ulong.Parse(p[1]), p.Length > 2 ? p[2] : null))
        .ToList();
}
