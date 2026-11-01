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

    static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public static BuildCatalog Load(string path) =>
        JsonSerializer.Deserialize<BuildCatalog>(File.ReadAllText(path), Json) ?? throw new InvalidDataException($"{path} is empty.");

    public static BuildCatalog Parse(string json) =>
        JsonSerializer.Deserialize<BuildCatalog>(json, Json) ?? throw new InvalidDataException("The build catalog is empty.");

    public Build? Find(string buildId) => Builds.FirstOrDefault(b => b.BuildId == buildId);

    // The catalog's builds the index does not hold yet.
    public List<Build> Missing(SourceIndex index) => Builds.Where(b => !index.Builds.Contains(b.BuildId)).ToList();

    // Every build since release is in the index: content found in none of them is not the game's.
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

    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, new JsonSerializerOptions(Json)
    {
        WriteIndented = true, DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
    }) + "\n");

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

    public static MigrationVerdict Decide(MigrationReport report, SourceIndex index, BuildCatalog catalog, int recipeFiles, string? rebuildError)
    {
        if (rebuildError is not null) return new(VerdictKind.NotMigrated, $"NOT MIGRATED: {rebuildError} The mod is kept as it was, for review.");
        if (report.Unreadable.Count > 0)
        {
            var builds = report.Unreadable.SelectMany(c => index.Contents[c].Places).Select(p => index.Builds[p.Build]).Distinct();
            return new(VerdictKind.NeedsReview, $"NEEDS REVIEW: the mod shares content with {report.Unreadable.Count} game resource parts from builds not given here ({string.Join(", ", builds)}); it is kept as it was.");
        }
        var unknown = report.Resources.Where(r => !r.InIndex && r.Literal > 0).ToList();
        long unknownBytes = unknown.Sum(r => r.Literal);
        var covered = catalog.Covers(index);
        if (unknown.Count > 0 && !covered)
        {
            var missing = catalog.Missing(index);
            var why = missing.Count > 0
                ? $"{missing.Count} known builds are not indexed yet ({string.Join(", ", missing.Select(b => b.BuildId))})"
                : "the build catalog does not list every build since release yet";
            return new(VerdictKind.PartlyMigrated,
                $"PARTLY MIGRATED, NEEDS REVIEW: the recipe rebuilds all {recipeFiles} files byte for byte (SHA-256) and the content of the {index.Builds.Count} indexed builds is purged, " +
                $"but {unknown.Count} resource parts ({unknownBytes:N0} bytes) are resources no indexed build has, and {why}. They may be cut content from a build not indexed yet.");
        }
        if (recipeFiles == 0) return new(VerdictKind.NoGameContent, "NO GAME CONTENT FOUND: the mod ships as it is.");
        var rest = unknown.Count > 0
            ? $" {unknown.Count} resource parts ({unknownBytes:N0} bytes) are in none of the {index.Builds.Count} builds since release: the author's own."
            : "";
        return new(VerdictKind.Verified, $"VERIFIED: all {recipeFiles} recipe files rebuilt from the game folders match the originals (SHA-256); game content purged.{rest}");
    }
}
