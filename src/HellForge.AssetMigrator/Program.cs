using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// Proof of concept of the asset migrator: a mod that ships the game's own resources becomes a mod that ships only what
// its author made, plus a recipe that rebuilds the original files from the player's own game files.
//
//   index   --game <build>=<data folder> [--game ...] --out <index file>
//   migrate --index <index file> --game <build>=<data folder> [...] --mod <mod folder> --out <folder>
//   rebuild --mod <migrated mod folder> --game <build>=<data folder> [...] --out <folder>
//   compare <folder> <folder>
//   kitbash-fixture --game <build>=<data folder> --out <mod folder>   (a test kitbash made from real game resources)
var args_ = args.ToList();
if (args_.Count == 0) return Usage();
try
{
    return args_[0] switch
    {
        "index" => Index(),
        "migrate" => Migrate(),
        "rebuild" => Rebuild(),
        "compare" => Compare(args_[1], args_[2]),
        "kitbash-fixture" => KitbashFixture(),
        _ => Usage(),
    };
}
catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException)
{
    Console.Error.WriteLine($"error: {e.Message}");
    return 1;
}

// A kitbash as an author would make one from the game: weapon model A's mesh data with a stretch of weapon model B's
// spliced in, a texture with a 64 KB region repainted, and a resource of the author's own. Resources are picked by size
// in a fixed order, so the same build always gives the same fixture.
int KitbashFixture()
{
    var spec = All("--game").Single();
    using var reader = new GameDataReader(spec[(spec.IndexOf('=') + 1)..]);
    var units = reader.All(ResourceTypes.Unit).Where(u => u.Value.GpuSize is > 2_000_000 and < 8_000_000).OrderBy(u => u.Key).Take(2).ToList();
    var texture = reader.All(ResourceTypes.Texture).Where(t => t.Value.GpuSize is > 1_000_000 and < 4_000_000).OrderBy(t => t.Key).First();
    var a = reader.Read(units[0].Value);
    var b = reader.Read(units[1].Value);
    var t = reader.Read(texture.Value);
    var random = new Random(40);
    var splice = a.Gpu[..(a.Gpu.Length * 6 / 10)].Concat(b.Gpu[(b.Gpu.Length / 5)..(b.Gpu.Length / 5 + b.Gpu.Length * 4 / 10)]).ToArray();
    var repainted = t.Gpu.ToArray();
    random.NextBytes(repainted.AsSpan(repainted.Length / 3, 65536));
    var own = new byte[20000];
    random.NextBytes(own);
    var resources = new Dictionary<ResourceKey, ResourceParts>
    {
        [new(ResourceTypes.Unit, units[0].Key)] = a with { Gpu = splice },
        [new(ResourceTypes.Texture, texture.Key)] = t with { Gpu = repainted },
        [new(ResourceTypes.Material, 0x1234_5678_9abc_def0)] = new(own, [], []),
    };
    var (archive, gpu, stream) = ResourceArchive.Write(resources);
    var data = Path.Combine(One("--out"), "data");
    Directory.CreateDirectory(data);
    File.WriteAllBytes(Path.Combine(data, "c0ffee00c0ffee00.patch_0"), archive);
    File.WriteAllBytes(Path.Combine(data, "c0ffee00c0ffee00.patch_0.gpu_resources"), gpu);
    File.WriteAllBytes(Path.Combine(data, "c0ffee00c0ffee00.patch_0.stream"), stream);
    Console.WriteLine($"Kitbash: unit {units[0].Key:x16} ({a.Gpu.Length:N0} B mesh data) with {b.Gpu.Length * 4 / 10:N0} B of unit {units[1].Key:x16} spliced in; " +
        $"texture {texture.Key:x16} with 64 KB repainted; 20 KB of the author's own.");
    return 0;
}

int Usage()
{
    Console.Error.WriteLine("usage: index|migrate|rebuild|compare (see Program.cs)");
    return 2;
}

List<string> All(string flag)
{
    var found = new List<string>();
    for (var i = 1; i + 1 < args_.Count; i++) if (args_[i] == flag) found.Add(args_[i + 1]);
    return found;
}

string One(string flag) => All(flag).SingleOrDefault() ?? throw new ArgumentException($"{flag} is required.");

List<GameResources> Games() => All("--game").Select(g =>
{
    var split = g.IndexOf('=');
    if (split <= 0) throw new ArgumentException("--game takes <build>=<data folder>.");
    return new GameResources(g[(split + 1)..], g[..split]);
}).ToList();

int Index()
{
    var index = new SourceIndex();
    foreach (var game in Games())
    {
        var clock = Stopwatch.StartNew();
        index.Add(game, (resources, bytes) => Console.Write($"\r  build {game.Build}: {resources:N0} resources, {bytes / 1e9:F1} GB read"));
        Console.WriteLine($" ({clock.Elapsed.TotalSeconds:F0} s{(game.Skipped > 0 ? $", {game.Skipped} parts over 256 MB skipped" : "")})");
        game.Dispose();
    }
    index.Seal();
    var output = One("--out");
    index.Save(output);
    Console.WriteLine($"Index: {index.Builds.Count} builds, {index.Contents.Count:N0} distinct resource parts ({index.IndexedBytes / 1e9:F1} GB), " +
        $"{index.AnchorCount:N0} anchor fingerprints, {new FileInfo(output).Length / 1e6:F0} MB on disk.");
    return 0;
}

int Migrate()
{
    var index = SourceIndex.Load(One("--index"));
    var games = Games();
    var modFolder = Path.GetFullPath(One("--mod"));
    var outFolder = Path.GetFullPath(One("--out"));
    var shipped = Path.Combine(outFolder, "mod");
    if (Directory.Exists(outFolder)) Directory.Delete(outFolder, true);
    Directory.CreateDirectory(shipped);

    var files = Directory.EnumerateFiles(modFolder, "*", SearchOption.AllDirectories)
        .Select(f => (Path.GetRelativePath(modFolder, f).Replace('\\', '/'), File.ReadAllBytes(f)))
        .OrderBy(f => f.Item1, StringComparer.Ordinal).ToList();
    var clock = Stopwatch.StartNew();
    var (recipe, literals, report) = new Migrator(index, games.Cast<IGameResources>().ToList()).Migrate(files, Console.WriteLine);
    Console.WriteLine($"Matched in {clock.Elapsed.TotalSeconds:F1} s.");

    // The gate: the game content is only taken out of the mod once the recipe has rebuilt every file, from the game
    // folders alone, byte for byte (SHA-256). If anything fails, the mod is kept exactly as it was.
    string verdict;
    try
    {
        var rebuilt = RecipeRebuilder.Rebuild(recipe, literals, games.Cast<IGameResources>().ToList());
        foreach (var (path, bytes) in files.Where(f => rebuilt.ContainsKey(f.Item1)))
            if (!rebuilt[path].AsSpan().SequenceEqual(bytes)) throw new InvalidDataException($"{path} rebuilt differently.");
        verdict = report.Unreadable.Count > 0
            ? $"NEEDS REVIEW: the mod shares content with {report.Unreadable.Count} game resource parts from builds not given here ({string.Join(", ", report.Unreadable.SelectMany(c => index.Contents[c].Places).Select(p => index.Builds[p.Build]).Distinct())}); it is kept as it was."
            : rebuilt.Count == 0
                ? "NO GAME CONTENT FOUND: the mod ships as it is."
                : $"VERIFIED: all {rebuilt.Count} recipe files rebuilt from the game folders match the originals (SHA-256); game content purged.";
        if (report.Unreadable.Count > 0) recipe = new Recipe();
    }
    catch (InvalidDataException e)
    {
        verdict = $"NOT MIGRATED: {e.Message} The mod is kept as it was, for review.";
        recipe = new Recipe();
    }
    Console.WriteLine(verdict);
    File.WriteAllText(Path.Combine(outFolder, "verdict.txt"), verdict + Environment.NewLine);

    foreach (var (path, bytes) in files)
    {
        if (recipe.Files.Any(f => f.Path == path)) continue;
        var target = Path.Combine(shipped, path);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.WriteAllBytes(target, bytes);
    }
    if (recipe.Files.Count > 0) recipe.Save(shipped, literals);

    var text = Report(index, recipe, report, files, shipped, Path.GetFileName(modFolder), verdict);
    File.WriteAllText(Path.Combine(outFolder, "migration-report.md"), text);
    Console.WriteLine();
    Console.WriteLine(text);
    return 0;
}

int Rebuild()
{
    var mod = Path.GetFullPath(One("--mod"));
    var outFolder = Path.GetFullPath(One("--out"));
    if (Directory.Exists(outFolder)) Directory.Delete(outFolder, true);
    Directory.CreateDirectory(outFolder);
    foreach (var file in Directory.EnumerateFiles(mod, "*", SearchOption.AllDirectories))
    {
        var name = Path.GetFileName(file);
        if (name is Recipe.FileName or Recipe.LiteralsName) continue;
        var target = Path.Combine(outFolder, Path.GetRelativePath(mod, file));
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(file, target);
    }
    if (!File.Exists(Path.Combine(mod, Recipe.FileName)))
    {
        Console.WriteLine("No recipe: the mod ships as it is.");
        return 0;
    }
    var (recipe, literals) = Recipe.Load(mod);
    var clock = Stopwatch.StartNew();
    var games = Games();
    var rebuilt = RecipeRebuilder.Rebuild(recipe, literals, games.Cast<IGameResources>().ToList(), Console.WriteLine);
    foreach (var (path, bytes) in rebuilt)
    {
        var target = Path.Combine(outFolder, path);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.WriteAllBytes(target, bytes);
        Console.WriteLine($"Rebuilt {path} ({bytes.Length:N0} bytes, SHA-256 matches the original)");
    }
    Console.WriteLine($"Done in {clock.Elapsed.TotalSeconds:F1} s.");
    return 0;
}

int Compare(string a, string b)
{
    static Dictionary<string, string> Hashes(string root) => Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
        .ToDictionary(f => Path.GetRelativePath(root, f).Replace('\\', '/'), f => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(f))));
    var x = Hashes(a);
    var y = Hashes(b);
    var same = true;
    foreach (var path in x.Keys.Union(y.Keys).Order(StringComparer.Ordinal))
    {
        var ok = x.TryGetValue(path, out var hx) && y.TryGetValue(path, out var hy) && hx == hy;
        same &= ok;
        Console.WriteLine($"{(ok ? "IDENTICAL" : "DIFFERENT")}  {path}");
    }
    Console.WriteLine(same ? $"All {x.Count} files are byte-for-byte identical." : "The folders differ.");
    return same ? 0 : 1;
}

static string Size(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F2} MB" : bytes >= 1e3 ? $"{bytes / 1e3:F1} KB" : $"{bytes} B";
static string Pct(long part, long whole) => whole == 0 ? "-" : $"{100.0 * part / whole:F1}%";

string Report(SourceIndex index, Recipe recipe, MigrationReport report, List<(string Path, byte[] Bytes)> files, string shipped, string name, string verdict)
{
    var s = new StringBuilder();
    long total = files.Sum(f => (long)f.Bytes.Length);
    long fromGame = report.Files.Sum(f => f.FromGame);
    long fill = report.Files.Sum(f => f.Fill);
    long shippedBytes = Directory.EnumerateFiles(shipped, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length);
    string Builds(int content) => string.Join(" + ", index.Contents[content].Places.Select(p => index.Builds[p.Build]).Distinct());

    s.AppendLine($"# Migration report: {name}");
    s.AppendLine();
    s.AppendLine($"**{verdict}**");
    s.AppendLine();
    s.AppendLine("| | |");
    s.AppendLine("|---|---|");
    s.AppendLine($"| Original mod | {files.Count} files, {Size(total)} |");
    s.AppendLine($"| Game content found and replaced by references | {Size(fromGame)} ({Pct(fromGame, total)}) |");
    s.AppendLine($"| Padding / runs of one byte | {Size(fill)} ({Pct(fill, total)}) |");
    s.AppendLine($"| Bytes the mod still ships (author's own or unmatched) | {Size(total - fromGame - fill)} ({Pct(total - fromGame - fill, total)}) |");
    s.AppendLine($"| Migrated mod on disk (incl. recipe) | {Size(shippedBytes)} |");
    s.AppendLine($"| Game resource parts referenced | {recipe.Sources.Count} |");
    s.AppendLine();
    s.AppendLine("## Files");
    s.AppendLine();
    s.AppendLine("| File | Size | From game | Still shipped | How it ships |");
    s.AppendLine("|---|---|---|---|---|");
    foreach (var f in report.Files)
        s.AppendLine($"| `{f.Path}` | {Size(f.Size)} | {Pct(f.FromGame, f.Size)} | {Pct(f.Literal, f.Size)} | {(f.InRecipe ? "recipe" : "as is")} |");
    if (report.Resources.Count > 0)
    {
        s.AppendLine();
        s.AppendLine("## Resources in the mod's archives");
        s.AppendLine();
        s.AppendLine("| Resource | Part | Size | From game | Still shipped | Comes from build(s) |");
        s.AppendLine("|---|---|---|---|---|---|");
        foreach (var r in report.Resources.OrderBy(r => ResourceTypes.Describe(r.Key.Type)).ThenBy(r => r.Key.Name).ThenBy(r => r.Part))
        {
            var builds = string.Join(", ", r.Sources.OrderByDescending(x => x.Value).Select(x => Builds(x.Key)).Distinct());
            s.AppendLine($"| {ResourceTypes.Describe(r.Key.Type)} `{r.Key.Name:x16}` | {r.Part} | {Size(r.Size)} | {Pct(r.FromGame, r.Size)} | {Pct(r.Literal, r.Size)} | {(builds.Length == 0 ? "-" : builds)} |");
        }
    }
    s.AppendLine();
    s.AppendLine("## Game content the mod reuses");
    s.AppendLine();
    s.AppendLine("| Game resource | Part | Bytes reused | In build(s) |");
    s.AppendLine("|---|---|---|---|");
    foreach (var (content, bytes) in report.SourceBytes.OrderByDescending(x => x.Value).Take(40))
    {
        var place = index.Contents[content].Places[0];
        s.AppendLine($"| {ResourceTypes.Describe(place.Key.Type)} `{place.Key.Name:x16}` | {place.Part} | {Size(bytes)} | {Builds(content)} |");
    }
    if (report.SourceBytes.Count > 40) s.AppendLine($"| ... and {report.SourceBytes.Count - 40} more | | | |");
    return s.ToString();
}
