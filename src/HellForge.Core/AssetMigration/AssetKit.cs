using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// The mod author's side of the asset kit (docs/modding/assets.md): which of a mod's game patch bytes are Arrowhead's
// (check), and the mod without them, as the author's own bytes plus a recipe that rebuilds the rest from each player's
// game (strip). Everything is matched against the builds of the game this PC has (GameBuilds.Local), fingerprinted once
// into an index that holds hashes only, never game content.
public static class AssetKit
{
    // Where the index of this PC's builds is kept: the installation's temp folder, or the user's local data folder.
    public static string CacheFolder(InstallLayout? install) => install != null
        ? Path.Combine(install.Temp, "asset-kit")
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolderOption.Create), "HellForge", "asset-kit");

    // The index of these builds, from the cache when it holds exactly them, else fingerprinted now (about a minute per
    // build: every resource of the build is read once) and kept in place of any older one.
    public static SourceIndex Index(IReadOnlyList<LocalBuild> builds, string cacheFolder, Action<string>? log = null)
    {
        if (builds.Count == 0)
            throw new InvalidDataException("No build of the game is on this PC to compare the mod with: set up HellForge's game copy, or install HELLDIVERS 2 in Steam.");
        var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join("\n", builds.Select(b => b.Label).Order(StringComparer.Ordinal)))))[..12];
        Directory.CreateDirectory(cacheFolder);
        var path = Path.Combine(cacheFolder, $"index-{key}.hfsi");
        if (File.Exists(path))
        {
            try { return SourceIndex.Load(path); }
            catch (Exception error) when (error is InvalidDataException or IOException or EndOfStreamException) { log?.Invoke($"The cached index could not be read ({error.Message}); making it again."); }
        }
        var index = new SourceIndex();
        foreach (var build in builds)
        {
            var clock = Stopwatch.StartNew();
            log?.Invoke($"Fingerprinting {build.What} (once per build; about a minute)...");
            var game = GameBuilds.Open(build);
            try { index.Add(game); }
            finally { (game as IDisposable)?.Dispose(); }
            log?.Invoke($"  done in {clock.Elapsed.TotalSeconds:F0} s.");
        }
        index.Seal();
        var partial = path + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
        index.Save(partial);
        File.Move(partial, path, overwrite: true);
        foreach (var stale in Directory.GetFiles(cacheFolder, "index-*.hfsi").Where(f => !string.Equals(f, path, StringComparison.OrdinalIgnoreCase)))
            try { File.Delete(stale); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return index;
    }

    // The mod files a recipe may take: its game patch files (data/ and a pack's option folders), as a package would ship
    // them (no workspace leftovers, nothing a recipe already rebuilds).
    public static List<string> PatchFiles(string modFolder) =>
        Directory.GetFiles(modFolder, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(modFolder, f).Replace('\\', '/'))
            .Where(f => !ModCatalog.IsWorkspaceFile(f) && ModCatalog.PatchFile().IsMatch(Path.GetFileName(f)))
            .Order(StringComparer.Ordinal).ToList();

    public enum Outcome
    {
        NoGameContent,  // nothing in the mod's patches is from the builds checked: it ships as it is
        Stripped,       // the game's bytes are out, and the recipe rebuilds every file byte for byte
        Failed,         // the recipe did not rebuild the mod exactly: nothing was written, the mod stays as it is
    }

    public sealed record Result(Outcome Outcome, string Summary, Recipe Recipe, MigrationReport Report, long ModBytes, long FromGame,
        IReadOnlyList<MigrationReport.ResourceLine> Unknown, string? ShippedFolder, string ReportText);

    // Matches the mod's patch files against the index and checks that the recipe rebuilds them, from the given builds
    // alone, byte for byte. With outFolder, writes the mod there as it is to be shared: every file it has, but the patch
    // files the recipe rebuilds, plus the recipe and its literals; nothing is written unless the check passed. The mod
    // folder itself is never changed.
    // name: how people read a build's label (LocalBuild.What); the label itself when null or unknown.
    public static Result Strip(string modFolder, SourceIndex index, IReadOnlyList<IGameResources> games, string? outFolder, Action<string>? log = null,
        Func<string, string?>? name = null)
    {
        string Named(string label) => name?.Invoke(label) ?? label;
        modFolder = Path.GetFullPath(modFolder);
        if (ModAssets.HasRecipe(modFolder))
            throw new InvalidDataException($"The mod already ships a recipe ({Recipe.FileName}): strip the folder you made it from, not a stripped copy.");
        if (outFolder != null)
        {
            outFolder = Path.GetFullPath(outFolder);
            if (LocalInstall.Within(outFolder, modFolder) || LocalInstall.Within(modFolder, outFolder))
                throw new InvalidDataException("The output folder must be outside the mod's folder (and not hold it).");
        }
        var patches = PatchFiles(modFolder);
        var mod = ModFiles.InFolder(modFolder, patches);
        var literalsPath = Path.Combine(Path.GetTempPath(), $"hellforge-literals-{Guid.NewGuid():N}");
        using var literals = new FileStream(literalsPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 16, FileOptions.DeleteOnClose);
        var migrator = new Migrator(index, games);
        var clock = Stopwatch.StartNew();
        var (recipe, report) = migrator.Migrate(mod, literals);
        migrator.ReleaseCaches();
        literals.Flush();
        log?.Invoke($"Matched {patches.Count} patch files ({mod.TotalSize / 1e6:F1} MB) in {clock.Elapsed.TotalSeconds:F1} s.");
        long fromGame = report.Files.Sum(f => f.FromGame);
        var unknown = report.Resources.Where(r => !r.InIndex && r.Literal > 0).ToList();

        // The gate: the recipe rebuilds every file from the game alone, each to the original's SHA-256.
        string? failure = null;
        if (report.Unreadable.Count > 0)
            failure = $"the mod shares content with {report.Unreadable.Count} game resource parts the builds here could not read back.";
        else if (recipe.Files.Count > 0)
        {
            try { RecipeRebuilder.Write(recipe, literals, games, _ => Stream.Null); }
            catch (Exception error) when (error is InvalidDataException or IOException) { failure = error.Message; }
        }
        Outcome outcome = failure != null ? Outcome.Failed : recipe.Files.Count == 0 ? Outcome.NoGameContent : Outcome.Stripped;
        var builds = BuildsUsed(index, report).Select(Named).ToList();
        var checkedBuilds = string.Join(" and ", index.Builds.Select(Named));
        var summary = outcome switch
        {
            Outcome.Failed => $"NOT STRIPPED: {failure} Nothing was written; the mod stays as it is.",
            Outcome.NoGameContent => $"NO GAME CONTENT FOUND in the mod's {patches.Count} patch files (checked against {checkedBuilds}): it ships as it is.",
            _ => $"STRIPPED: {Size(fromGame)} of Arrowhead's content ({Pct(fromGame, mod.TotalSize)} of the mod's patch files) is out; " +
                 $"each player's HellForge rebuilds it from their own game ({string.Join(", ", builds)}), checked by SHA-256.",
        };
        if (outcome != Outcome.Failed && unknown.Count > 0)
            summary += $" {unknown.Count} resource parts ({Size(unknown.Sum(r => r.Literal))}) are in none of the builds checked: your own work, or content from an older build (cut content) this PC does not have, which stays in the mod.";

        string? shipped = null;
        if (outFolder != null && outcome != Outcome.Failed)
        {
            shipped = outFolder;
            if (Directory.Exists(shipped)) Directory.Delete(shipped, recursive: true);
            Directory.CreateDirectory(shipped);
            var inRecipe = recipe.Files.Select(f => f.Path).ToHashSet(StringComparer.Ordinal);
            foreach (var file in Directory.GetFiles(modFolder, "*", SearchOption.AllDirectories))
            {
                var relative = Path.GetRelativePath(modFolder, file).Replace('\\', '/');
                if (ModCatalog.IsWorkspaceFile(relative) || inRecipe.Contains(relative)) continue;
                var target = Path.Combine(shipped, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(file, target);
            }
            if (recipe.Files.Count > 0)
            {
                recipe.Save(shipped);
                using var output = File.Create(Path.Combine(shipped, Recipe.LiteralsName));
                literals.Position = 0;
                literals.CopyTo(output);
            }
        }
        var text = ReportText(Path.GetFileName(modFolder), summary, index, recipe, report, mod.TotalSize, shipped, Named);
        return new(outcome, summary, recipe, report, mod.TotalSize, fromGame, unknown, shipped, text);
    }

    // The builds a recipe's game content comes from, as their labels.
    static List<string> BuildsUsed(SourceIndex index, MigrationReport report) =>
        report.SourceBytes.Keys.SelectMany(c => index.Contents[c].Places.Take(1)).Select(p => index.Builds[p.Build]).Distinct().Order(StringComparer.Ordinal).ToList();

    static string Size(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F2} MB" : bytes >= 1e3 ? $"{bytes / 1e3:F1} KB" : $"{bytes} B";
    static string Pct(long part, long whole) => whole == 0 ? "-" : $"{100.0 * part / whole:F1}%";

    // The report an author reads (asset-report.md): what came from the game and from which build, what stays, and why.
    public static string ReportText(string name, string summary, SourceIndex index, Recipe recipe, MigrationReport report, long total, string? shipped,
        Func<string, string> named)
    {
        var s = new StringBuilder();
        long fromGame = report.Files.Sum(f => f.FromGame);
        long fill = report.Files.Sum(f => f.Fill);
        s.AppendLine($"# Asset report: {name}");
        s.AppendLine();
        s.AppendLine($"**{summary}**");
        s.AppendLine();
        s.AppendLine($"Checked against: {string.Join(", ", index.Builds.Select(named))}.");
        s.AppendLine();
        s.AppendLine("| | |");
        s.AppendLine("|---|---|");
        s.AppendLine($"| Patch files | {report.Files.Count}, {Size(total)} |");
        s.AppendLine($"| Arrowhead's content, rebuilt on each player's PC | {Size(fromGame)} ({Pct(fromGame, total)}) |");
        s.AppendLine($"| Padding (runs of one byte) | {Size(fill)} |");
        s.AppendLine($"| Your bytes, shipped | {Size(total - fromGame - fill)} ({Pct(total - fromGame - fill, total)}) |");
        if (shipped != null)
            s.AppendLine($"| The shareable mod on disk | {Size(Directory.GetFiles(shipped, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length))} |");
        s.AppendLine();
        s.AppendLine("## Patch files");
        s.AppendLine();
        s.AppendLine("| File | Size | From the game | Yours | Ships as |");
        s.AppendLine("|---|---|---|---|---|");
        foreach (var f in report.Files)
            s.AppendLine($"| `{f.Path}` | {Size(f.Size)} | {Pct(f.FromGame, f.Size)} | {Pct(f.Literal, f.Size)} | {(f.InRecipe ? "recipe" : "as it is")} |");
        var unknown = report.Resources.Where(r => !r.InIndex && r.Literal > 0).ToList();
        if (unknown.Count > 0)
        {
            s.AppendLine();
            s.AppendLine("## Resources in none of the builds checked");
            s.AppendLine();
            s.AppendLine("These stay in the mod as they are. A resource you made is yours to ship. One copied from an older build of the game " +
                "(cut content) is Arrowhead's: it is not on this PC to match, so check where it came from before you share the mod.");
            s.AppendLine();
            s.AppendLine("| File | Resource | Part | Bytes kept |");
            s.AppendLine("|---|---|---|---|");
            foreach (var r in unknown.OrderByDescending(r => r.Literal).Take(60))
                s.AppendLine($"| `{r.File}` | {ResourceTypes.Describe(r.Key.Type)} `{r.Key.Name:x16}` | {r.Part} | {Size(r.Literal)} |");
            if (unknown.Count > 60) s.AppendLine($"| ... and {unknown.Count - 60} more | | | |");
        }
        if (report.SourceBytes.Count > 0)
        {
            s.AppendLine();
            s.AppendLine("## Game resources the mod uses");
            s.AppendLine();
            s.AppendLine("| Game resource | Part | Bytes | In build |");
            s.AppendLine("|---|---|---|---|");
            foreach (var (content, bytes) in report.SourceBytes.OrderByDescending(x => x.Value).Take(60))
            {
                var place = index.Contents[content].Places[0];
                s.AppendLine($"| {ResourceTypes.Describe(place.Key.Type)} `{place.Key.Name:x16}` | {place.Part} | {Size(bytes)} | " +
                    $"{string.Join(", ", index.Contents[content].Places.Select(p => named(index.Builds[p.Build])).Distinct())} |");
            }
            if (report.SourceBytes.Count > 60) s.AppendLine($"| ... and {report.SourceBytes.Count - 60} more | | | |");
        }
        return s.ToString();
    }
}
