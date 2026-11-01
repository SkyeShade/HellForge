using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// Proof of concept of the asset migrator: a mod that ships the game's own resources becomes a mod that ships only what
// its author made, plus a recipe that rebuilds the original files from the player's own game files.
//
//   index   --game <build>=<data folder> [--game ...] [--add-to <index file>] --out <index file>
//   migrate --index <index file> --catalog <game-builds.json> --game <build>=<data folder> [...] --mod <mod folder> --out <folder>
//   rebuild --mod <migrated mod folder> [--catalog <game-builds.json>] --game <build>=<data folder> [...] --out <folder>
// A --game folder without bundles.nxa is read as an older build's loose archives.
//   compare <folder> <folder>
//   kitbash-fixture --game <build>=<data folder> --out <mod folder>   (a test kitbash made from real game resources)
var args_ = args.ToList();
SteamDepots? steam = null;
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
        "catalog-import" => CatalogImport(),
        "login" => Login(),
        "depot-files" => DepotFilesCommand(),
        "manifests" => ManifestsCommand(),
        "backfill" => Backfill(),
        _ => Usage(),
    };
}
catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException or InvalidOperationException or TimeoutException)
{
    Console.Error.WriteLine($"error: {e.Message}");
    return 1;
}
finally
{
    if (steam is not null)
    {
        Console.Error.WriteLine($"Steam: {steam.ChunksDownloaded:N0} chunks downloaded ({steam.BytesDownloaded / 1e6:F0} MB), {steam.ChunksFromDisk:N0} from the cache.");
        steam.TrimCache();
        steam.Dispose();
    }
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

// catalog-import --catalog <game-builds.json> --log <Steam content_log.txt> [--log ...]: adds the builds Steam logged
// installing (with their depot manifests) to the catalog.
int CatalogImport()
{
    var path = One("--catalog");
    var catalog = BuildCatalog.Load(path);
    var found = All("--log").Where(File.Exists).SelectMany(log => BuildCatalog.FromSteamLog(File.ReadLines(log), catalog.AppId)).ToList();
    var merged = catalog.Merge(found);
    merged.Save(path);
    Console.WriteLine($"{found.Select(b => b.BuildId).Distinct().Count()} builds in the logs; the catalog now lists {merged.Builds.Count} (was {catalog.Builds.Count}).");
    foreach (var b in merged.Builds) Console.WriteLine($"  {b.Seen ?? "?",-10} {b.BuildId,-9} {(b.Layout.Length == 0 ? "?" : b.Layout),-5} {string.Join(" ", b.Depots.Select(d => $"{d.Id}:{d.ManifestId}"))}");
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

// A data folder with bundles.nxa is read as Steam ships it today; one without, as the loose archives of older builds.
// "steam:<depot>:<manifest>[:<branch>]" reads that manifest of a depot from Steam, fetching only what is read.
List<IGameResources> Games() => All("--game").Select(g =>
{
    var split = g.IndexOf('=');
    if (split <= 0) throw new ArgumentException("--game takes <build>=<data folder> or <build>=steam:<depot>:<manifest>.");
    return Game(g[..split], g[(split + 1)..]);
}).ToList();

IGameResources Game(string label, string where)
{
    IDataFiles files = where.StartsWith("steam:", StringComparison.Ordinal) ? Depot(where) : new FolderFiles(where);
    if (!files.Exists(GameDataReader.IndexFile)) return new LooseGameResources(files, label);
    // The transition builds (December 2025 to January 2026) have both bundles.nxa and loose archives.
    return files.List().Any(n => n.Length == 16 && !n.Contains('.')) ? new MixedGameResources(files, label) : new GameResources(files, label);
}

SteamDepots Steam() => steam ??= SteamDepots.Connect().GetAwaiter().GetResult();

DepotFiles Depot(string where)
{
    var parts = where.Split(':');
    if (parts.Length < 3) throw new ArgumentException("steam:<depot>:<manifest>[:<branch>]");
    var manifest = Steam().Manifest(uint.Parse(parts[1]), ulong.Parse(parts[2]), parts.Length > 3 ? parts[3] : "public").GetAwaiter().GetResult();
    return new DepotFiles(Steam(), manifest);
}

// login --qr | --password: logs in to Steam once (QR code in the Steam app, or account name, password and Steam Guard
// code); the token is saved for the other commands. Run it in a terminal of your own: it asks questions.
int Login()
{
    var method = args_.Contains("--qr") ? SteamDepots.LoginMethod.Qr
        : args_.Contains("--password") ? SteamDepots.LoginMethod.Password
        : throw new ArgumentException("login takes --qr (scan a code in the Steam mobile app) or --password (account name, password and Steam Guard code).");
    using var s = SteamDepots.Connect(method).GetAwaiter().GetResult();
    Console.WriteLine("Logged in to Steam; the login is saved for the migrator's other commands.");
    return 0;
}

// backfill --index <file> --depot <id> --history <file> [--limit <n>]: indexes the depot's manifests from Steam, oldest
// first (each build mostly reuses the chunks the one before it downloaded), as builds labelled "<depot>:<manifest>".
// The index is saved after every build, so a stopped backfill resumes where it was; manifests Steam refuses are noted
// in <index>.refused and skipped.
int Backfill()
{
    var path = One("--index");
    var depot = uint.Parse(One("--depot"));
    var history = DepotHistory.Load(One("--history")).OrderBy(h => h.Time).ToList();
    var limit = All("--limit").Count > 0 ? int.Parse(One("--limit")) : int.MaxValue;
    var index = File.Exists(path) ? SourceIndex.Load(path) : new SourceIndex();
    var refusedFile = path + ".refused";
    var refused = File.Exists(refusedFile) ? File.ReadAllLines(refusedFile).ToHashSet() : [];
    var done = 0;
    foreach (var h in history)
    {
        var label = $"{depot}:{h.Manifest}";
        if (index.Builds.Contains(label) || refused.Contains(label)) continue;
        if (done++ >= limit) break;
        var clock = Stopwatch.StartNew();
        long downloaded = Steam().BytesDownloaded, reused = index.ArchivesReused;
        IGameResources game;
        try { game = Game(label, $"steam:{depot}:{h.Manifest}:{h.Branch ?? "public"}"); }
        catch (IOException e)
        {
            Console.WriteLine($"{h.Time:yyyy-MM-dd} {label}: refused by Steam ({e.Message}); skipped.");
            File.AppendAllLines(refusedFile, [label]);
            continue;
        }
        index.Add(game, (resources, bytes) => Console.Write($"\r{h.Time:yyyy-MM-dd} {label}: {resources:N0} resources, {bytes / 1e9:F1} GB of new parts read, {(Steam().BytesDownloaded - downloaded) / 1e9:F1} GB downloaded   "));
        (game as IDisposable)?.Dispose();
        var temp = path + ".tmp";
        index.Save(temp);
        File.Move(temp, path, true);
        Steam().TrimCache();
        Console.WriteLine($"\r{h.Time:yyyy-MM-dd} {label}{(h.Branch is null ? "" : " " + h.Branch)}: done in {clock.Elapsed.TotalMinutes:F1} min, {(Steam().BytesDownloaded - downloaded) / 1e9:F1} GB downloaded, " +
            $"{index.ArchivesReused - reused:N0} unchanged archives reused; index {index.Builds.Count} builds, {index.Contents.Count:N0} parts, {new FileInfo(path).Length / 1e9:F2} GB");
    }
    return 0;
}

// manifests --depot <id> --history <data/depot-history/<id>.txt>: fetches every manifest of the depot's history (file
// lists only, cached for later) and says which Steam serves, with each one's layout and data size.
int ManifestsCommand()
{
    var depot = uint.Parse(One("--depot"));
    var history = DepotHistory.Load(One("--history"));
    int ok = 0, refused = 0;
    long total = 0, unique = 0, chunkBytes = 0, chunkCompressed = 0;
    var seen = new HashSet<string>(StringComparer.Ordinal);
    var chunkSeen = new HashSet<string>(StringComparer.Ordinal);
    foreach (var h in history)
    {
        try
        {
            var m = Steam().Manifest(depot, h.Manifest, h.Branch ?? "public").GetAwaiter().GetResult();
            var files = new DepotFiles(Steam(), m);
            var size = files.List().Sum(n => (long)files.File(n).TotalSize);
            long added = 0;
            foreach (var n in files.List())
            {
                if (seen.Add(files.Hash(n) ?? n + h.Manifest)) added += (long)files.File(n).TotalSize;
                foreach (var c in files.File(n).Chunks)
                    if (chunkSeen.Add(Convert.ToHexString(c.ChunkID!))) { chunkBytes += c.UncompressedLength; chunkCompressed += c.CompressedLength; }
            }
            total += size;
            unique += added;
            ok++;
            var layout = files.Exists(GameDataReader.IndexFile) ? files.List().Count() > 40 ? "both" : "nxa" : "loose";
            Console.WriteLine($"{h.Time:yyyy-MM-dd} {h.Manifest,20} {h.Branch,-11} {layout,-5} {files.List().Count(),5} files {size / 1e9,6:F1} GB, {added / 1e9,6:F1} GB in files no earlier-listed manifest has");
        }
        catch (Exception e) when (e is IOException or InvalidOperationException)
        {
            refused++;
            Console.WriteLine($"{h.Time:yyyy-MM-dd} {h.Manifest,20} {h.Branch,-11} REFUSED: {e.Message}");
        }
    }
    Console.WriteLine($"{ok} manifests served, {refused} refused; {total / 1e12:F2} TB of data across the served ones, {unique / 1e12:F2} TB in distinct files, " +
        $"{chunkBytes / 1e12:F2} TB in distinct Steam chunks ({chunkCompressed / 1e12:F2} TB to download).");
    return 0;
}

// depot-files steam:<depot>:<manifest>: the manifest's data files, their sizes and the data layout.
int DepotFilesCommand()
{
    var files = Depot(args_[1]);
    var list = files.List().Select(n => (n, (long)files.File(n).TotalSize)).OrderByDescending(x => x.Item2).ToList();
    Console.WriteLine($"{files.Name}: {files.Manifest.Files?.Count ?? 0} files in the depot, {list.Count} in data/ ({list.Sum(x => x.Item2) / 1e9:F1} GB); " +
        $"layout {(files.Exists(GameDataReader.IndexFile) ? "nxa" : "loose")}; created {files.Manifest.CreationTime:yyyy-MM-dd}");
    foreach (var (n, size) in list.Take(8)) Console.WriteLine($"  {size / 1e6,10:F1} MB  {n}");
    var other = (files.Manifest.Files ?? []).Where(f => !f.FileName.Replace('\\', '/').StartsWith("data/", StringComparison.OrdinalIgnoreCase)).Select(f => f.FileName).Take(8);
    Console.WriteLine($"  outside data/: {string.Join(", ", other)}");
    return 0;
}

int Index()
{
    // --add-to: add builds to an existing index (each build once) instead of starting a new one.
    var index = All("--add-to").Count > 0 ? SourceIndex.Load(One("--add-to")) : new SourceIndex();
    foreach (var game in Games())
    {
        var clock = Stopwatch.StartNew();
        index.Add(game, (resources, bytes) => Console.Write($"\r  build {game.Build}: {resources:N0} resources, {bytes / 1e9:F1} GB read"));
        Console.WriteLine($" ({clock.Elapsed.TotalSeconds:F0} s{(game is GameResources { Skipped: > 0 } g ? $", {g.Skipped} parts over 256 MB skipped" : "")})");
        (game as IDisposable)?.Dispose();
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
    var (recipe, literals, report) = new Migrator(index, games).Migrate(files, Console.WriteLine);
    Console.WriteLine($"Matched in {clock.Elapsed.TotalSeconds:F1} s.");

    // The gate: the game content is only taken out of the mod once the recipe has rebuilt every file, from the game
    // folders alone, byte for byte (SHA-256). If anything fails, the mod is kept exactly as it was.
    string? rebuildError = null;
    try
    {
        var rebuilt = RecipeRebuilder.Rebuild(recipe, literals, games);
        foreach (var (path, bytes) in files.Where(f => rebuilt.ContainsKey(f.Item1)))
            if (!rebuilt[path].AsSpan().SequenceEqual(bytes)) throw new InvalidDataException($"{path} rebuilt differently.");
    }
    catch (InvalidDataException e) { rebuildError = e.Message; }
    var decided = MigrationVerdict.Decide(report, index, BuildCatalog.LoadWithHistory(One("--catalog")), recipe.Files.Count, rebuildError);
    if (decided.Kind is VerdictKind.NotMigrated or VerdictKind.NeedsReview) recipe = new Recipe();
    var verdict = decided.Text;
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
    var plan = RecipeRebuilder.FetchPlan(recipe, games.Select(g => g.Build).ToList(), All("--catalog").Count > 0 ? BuildCatalog.Load(One("--catalog")) : new BuildCatalog());
    if (plan.Count > 0)
    {
        Console.WriteLine("The recipe needs game files these game folders do not have:");
        foreach (var (build, needed, commands) in plan)
        {
            Console.WriteLine($"  build {build}: {string.Join(", ", needed)}");
            foreach (var command in commands) Console.WriteLine($"    Steam console: {command}");
        }
        return 1;
    }
    var rebuilt = RecipeRebuilder.Rebuild(recipe, literals, games, Console.WriteLine);
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
    // Whether the game has a resource with this name and part, and whether its bytes are the mod's.
    string SameKey(MigrationReport.ResourceLine r)
    {
        var bytes = files.Single(f => f.Path == r.File).Bytes.AsSpan((int)r.Start, (int)r.Size);
        var sha = SHA256.HashData(bytes);
        var places = index.WithKey(r.Key).SelectMany(c => index.Contents[c].Places.Where(p => p.Key == r.Key && p.Part == r.Part).Select(p => (c, p.Build))).ToList();
        if (places.Count == 0) return "not in indexed builds";
        var identical = places.Where(x => index.Contents[x.c].Sha256.AsSpan().SequenceEqual(sha)).Select(x => index.Builds[x.Build]).Distinct().ToList();
        var differs = places.Where(x => !index.Contents[x.c].Sha256.AsSpan().SequenceEqual(sha)).Select(x => index.Builds[x.Build]).Distinct().ToList();
        return string.Join("; ", new[] { identical.Count > 0 ? $"identical in {string.Join(", ", identical)}" : null, differs.Count > 0 ? $"different bytes in {string.Join(", ", differs)}" : null }.Where(x => x is not null));
    }
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
        s.AppendLine("| Resource | Part | Size | From game | Still shipped | Comes from build(s) | Same resource in the game |");
        s.AppendLine("|---|---|---|---|---|---|---|");
        foreach (var r in report.Resources.OrderBy(r => ResourceTypes.Describe(r.Key.Type)).ThenBy(r => r.Key.Name).ThenBy(r => r.Part))
        {
            var builds = string.Join(", ", r.Sources.OrderByDescending(x => x.Value).Select(x => Builds(x.Key)).Distinct());
            s.AppendLine($"| {ResourceTypes.Describe(r.Key.Type)} `{r.Key.Name:x16}` | {r.Part} | {Size(r.Size)} | {Pct(r.FromGame, r.Size)} | {Pct(r.Literal, r.Size)} | {(builds.Length == 0 ? "-" : builds)} | {SameKey(r)} |");
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
