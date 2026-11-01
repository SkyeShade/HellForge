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
DepotFiles? lastDepot = null;
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
        "names" => Names(),
        "resolve" => Resolve(),
        "probe-range" => ProbeRange(),
        "chunk-stats" => ChunkStats(),
        "checksum-probe" => ChecksumProbe(),
        "steam-chunks" => SteamChunks(),
        "scan" => Scan(),
        "chunk-delta" => ChunkDelta(),
        "local-check" => LocalCheck(),
        _ => Usage(),
    };
}
catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException or InvalidOperationException or TimeoutException)
{
    Console.Error.WriteLine(Environment.GetEnvironmentVariable("HF_MIGRATOR_DEBUG") is { Length: > 0 } ? $"error: {e}" : $"error: {e.Message}");
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

// A build read only when it is first needed: the index's builds read from Steam ("<depot>:<manifest>") are given to the
// migrator this way, so only the ones a mod needs are opened.
IGameResources Lazy(string label, Func<IGameResources> open) => new LazyGame(label, open);

// The index's Steam builds ("<depot>:<manifest>"), lazily, with the branch from the depot history.
IEnumerable<IGameResources> SteamBuilds(SourceIndex index, IEnumerable<string> have)
{
    var catalog = All("--catalog").Count > 0 ? BuildCatalog.LoadWithHistory(One("--catalog")) : null;
    foreach (var label in index.Builds.Where(b => b.Contains(':') && !have.Contains(b)))
    {
        var parts = label.Split(':');
        var branch = catalog?.History?.FirstOrDefault(h => h.Manifest.ToString() == parts[1])?.Branch ?? "public";
        yield return Lazy(label, () => Game(label, $"steam:{parts[0]}:{parts[1]}:{branch}"));
    }
}

IGameResources Game(string label, string where)
{
    IDataFiles files = where.StartsWith("steam:", StringComparison.Ordinal) ? Depot(where) : new FolderFiles(where);
    if (!files.Exists(GameDataReader.IndexFile)) return new LooseGameResources(files, label);
    // The transition builds (December 2025 to January 2026) have both bundles.nxa and loose archives.
    return files.List().Any(n => n.Length == 16 && !n.Contains('.')) ? new MixedGameResources(files, label) : new GameResources(files, label);
}

SteamDepots Steam()
{
    if (steam is not null) return steam;
    steam = SteamDepots.Connect().GetAwaiter().GetResult();
    foreach (var (manifest, install) in Installed()) steam.Local.Add(manifest, install);
    if (steam.Local.Count > 0) Console.WriteLine($"Steam: {steam.Local.Count:N0} chunks are read from the builds installed here, not downloaded.");
    return steam;
}

// --local <depot>:<manifest>=<install folder>: a build installed on this PC (the folder holding data/), by its manifest.
List<(SteamKit2.DepotManifest Manifest, string Install)> Installed() => All("--local").Select(spec =>
{
    var (id, install) = (spec[..spec.IndexOf('=')], spec[(spec.IndexOf('=') + 1)..]);
    var parts = id.Split(':');
    var manifest = SteamDepots.Cached(uint.Parse(parts[0]), ulong.Parse(parts[1])) ?? throw new IOException($"manifest {id} is not cached.");
    return (manifest, install);
}).ToList();

DepotFiles Depot(string where)
{
    var parts = where.Split(':');
    if (parts.Length < 3) throw new ArgumentException("steam:<depot>:<manifest>[:<branch>]");
    var manifest = Steam().Manifest(uint.Parse(parts[1]), ulong.Parse(parts[2]), parts.Length > 3 ? parts[3] : "public").GetAwaiter().GetResult();
    return lastDepot = new DepotFiles(Steam(), manifest);
}

// login --qr | --password: logs in to Steam once (QR code in the Steam app, or account name, password and Steam Guard
// code); the token is saved for the other commands. Run it in a terminal of your own: it asks questions.
int Login()
{
    var method = args_.Contains("--qr") ? SteamDepots.LoginMethod.Qr
        : args_.Contains("--password") ? SteamDepots.LoginMethod.Password
        : args_.Contains("--check") ? SteamDepots.LoginMethod.Saved
        : throw new ArgumentException("login takes --qr (scan a code in the Steam mobile app), --password (account name, password and Steam Guard code) or --check (the saved login).");
    using var s = SteamDepots.Connect(method).GetAwaiter().GetResult();
    Console.WriteLine(method == SteamDepots.LoginMethod.Saved ? "The saved Steam login works." : "Logged in to Steam; the login is saved for the migrator's other commands.");
    return 0;
}

// names --map <file> --depot <id> --history <file>: the name map of every manifest in the depot's history, oldest first,
// from the archives' tables only (unchanged archives are not read again); saved after every build, so it resumes.
int Names()
{
    var path = One("--map");
    var depot = uint.Parse(One("--depot"));
    var history = DepotHistory.Load(One("--history")).OrderBy(h => h.Time).ToList();
    var unavailable = Path.ChangeExtension(One("--history"), ".unavailable");
    var skip = File.Exists(unavailable) ? File.ReadLines(unavailable).Select(l => l.Split('#')[0].Trim()).Where(l => l.Length > 0).ToHashSet() : [];
    var map = File.Exists(path) ? NameMap.Load(path) : new NameMap();
    var total = Stopwatch.StartNew();
    foreach (var h in history)
    {
        var label = $"{depot}:{h.Manifest}";
        if (map.Builds.Contains(label) || skip.Contains(h.Manifest.ToString())) continue;
        var clock = Stopwatch.StartNew();
        var downloaded = Steam().BytesDownloaded;
        var (read, reused) = (map.TablesRead, map.TablesReused);
        var game = Game(label, $"steam:{depot}:{h.Manifest}:{h.Branch ?? "public"}");
        // The tables of archives the map does not know yet are fetched in parallel first; reading them is then local.
        if (lastDepot is { } files)
        {
            var fresh = files.List().Where(n => n.Length == 16 && !n.Contains('.') && (game.TableHash(n) is not { } hash || !map.Knows(hash))).ToList();
            Console.Write($"\r{h.Time:yyyy-MM-dd} {label}: fetching {fresh.Count:N0} new tables   ");
            files.PrefetchFirstChunks(fresh);
        }
        map.Add(game, n => Console.Write($"\r{h.Time:yyyy-MM-dd} {label}: {n:N0} archives   "));
        (game as IDisposable)?.Dispose();
        map.Save(path);
        Console.WriteLine($"\r{h.Time:yyyy-MM-dd} {label}{(h.Branch is null ? "" : " " + h.Branch)}: {map.TablesRead - read:N0} tables read, {map.TablesReused - reused:N0} unchanged, " +
            $"{(Steam().BytesDownloaded - downloaded) / 1e6:F0} MB downloaded, {clock.Elapsed.TotalSeconds:F0} s");
    }
    Console.WriteLine($"Name map: {map.Builds.Count} builds, {map.ArchiveCount:N0} distinct archive tables; {Steam().BytesDownloaded / 1e9:F2} GB downloaded in {total.Elapsed.TotalMinutes:F1} min.");
    return 0;
}

// resolve --index <file> --map <name map> --catalog <game-builds.json> --game <build>=<folder> [...] --mod <folder> --out <folder>
// Migrates the mod; the leftover resources whose names the game has (in some build, by the name map) get their archives
// fingerprinted from Steam (only those archives, in the newest build that has them, sharing builds where it can), into
// the index as partial builds; then the mod is migrated again with them.
int Resolve()
{
    var indexPath = One("--index");
    var index = SourceIndex.Load(indexPath);
    var names = All("--map").Count > 0 ? NameMap.Load(One("--map")) : null;
    var chunks = All("--chunks").Count > 0 ? SteamChunkIndex.Load(One("--chunks")) : null;
    if (names is null && chunks is null) throw new ArgumentException("resolve needs --map (the name map), --chunks (the Steam chunk index), or both.");
    var catalog = BuildCatalog.LoadWithHistory(One("--catalog"));
    var modFolder = Path.GetFullPath(One("--mod"));
    var files = Directory.EnumerateFiles(modFolder, "*", SearchOption.AllDirectories)
        .Select(f => (Path.GetRelativePath(modFolder, f).Replace('\\', '/'), File.ReadAllBytes(f)))
        .OrderBy(f => f.Item1, StringComparer.Ordinal).ToList();
    var games = Games();
    games.AddRange(SteamBuilds(index, games.Select(g => g.Build).ToList()));
    var (_, _, report) = new Migrator(index, games).Migrate(files);
    var left = report.Resources.Where(r => r.Literal > 0).ToList();
    DateTime When(string build) => catalog.History?.FirstOrDefault(h => $"{catalog.DataDepot}:{h.Manifest}" == build)?.Time ?? DateTime.MinValue;
    // Per build: the archives to fingerprint, and (when known) just the resources of them that are needed.
    var plan = new Dictionary<string, (HashSet<string> Archives, HashSet<ResourceKey>? Keys)>(StringComparer.Ordinal);
    var opened = new Dictionary<string, IGameResources>(StringComparer.Ordinal);
    IGameResources Open(string build)
    {
        if (opened.TryGetValue(build, out var game)) return game;
        var parts = build.Split(':');
        var branch = catalog.History?.FirstOrDefault(h => h.Manifest.ToString() == parts[1])?.Branch ?? "public";
        return opened[build] = Game(build, $"steam:{parts[0]}:{parts[1]}:{branch}");
    }

    // The Steam chunks the bytes the mod still ships are: each is found in a build (the latest that has it, or one
    // already planned), at a place in a game file; the game resources at that place, and the mod file's leftover
    // resources no indexed build has by name (smaller than a chunk, so the scan cannot prove them), are fingerprinted
    // from that archive alone.
    if (chunks is not null)
    {
        var clock = Stopwatch.StartNew();
        foreach (var (path, bytes) in files)
        {
            if (!report.Kept.TryGetValue(path, out var kept) || kept.Count == 0) continue;
            var mine = left.Where(r => r.File == path && !r.InIndex).ToList();
            foreach (var m in chunks.Scan(bytes))
            {
                if (!kept.Any(k => k.Start < m.Offset + m.Length && m.Offset < k.Start + k.Length)) continue;
                var ids = chunks.BuildIdsOf(m.Chunk).ToList();
                var pick = ids.Where(b => plan.ContainsKey(chunks.Builds[b].Label)).DefaultIfEmpty(ids.MaxBy(b => chunks.Builds[b].Time)).First();
                var build = chunks.Builds[pick].Label;
                if (index.Builds.Contains(build) && !index.PartialBuilds.Contains(build)) continue;
                if (chunks.Locate(m.Chunk, pick) is not { } at || !at.File.StartsWith("data/", StringComparison.Ordinal)) continue;
                var game = Open(build);
                var found = game.At(at.File["data/".Length..], at.Offset, m.Length).ToList();
                if (found.Count == 0) continue;
                if (!plan.TryGetValue(build, out var entry)) plan[build] = entry = (new(StringComparer.Ordinal), new());
                foreach (var (key, archive) in found)
                {
                    entry.Archives.Add(archive);
                    entry.Keys!.Add(key);
                }
                foreach (var r in mine) entry.Keys!.Add(r.Key);
            }
        }
        Console.WriteLine($"Steam chunks: {plan.Sum(p => p.Value.Keys?.Count ?? 0)} game resources to fingerprint, in {plan.Sum(p => p.Value.Archives.Count)} archives of {plan.Count} builds ({clock.Elapsed.TotalSeconds:F1} s).");
    }
    if (names is not null)
    {
        var wanted = left.Where(r => !r.InIndex && names.Has(r.Key)).Select(r => r.Key).Distinct().ToList();
        Console.WriteLine($"{wanted.Count} leftover resources are the game's by name.");
        foreach (var key in wanted)
        {
            var found = names.Find(key).ToList();
            var pick = found.FirstOrDefault(f => plan.ContainsKey(f.Build));
            if (pick.Build is null) pick = found.OrderByDescending(f => When(f.Build)).First();
            if (!plan.TryGetValue(pick.Build, out var entry)) plan[pick.Build] = entry = (new(StringComparer.Ordinal), new());
            entry.Archives.Add(pick.Archive);
            entry.Keys?.Add(key);
        }
    }
    foreach (var (build, (archives, keys)) in plan)
    {
        var clock = Stopwatch.StartNew();
        var downloaded = Steam().BytesDownloaded;
        index.Add(Open(build), (n, bytes) => Console.Write($"\r  {When(build):yyyy-MM-dd} {build}: {n:N0} resources, {bytes / 1e6:F1} MB read   "), archives, keys);
        Console.WriteLine($"\r  {When(build):yyyy-MM-dd} {build}: fingerprinted {keys?.Count.ToString("N0") ?? "all"} resources of {archives.Count} archives " +
            $"in {clock.Elapsed.TotalSeconds:F0} s, {(Steam().BytesDownloaded - downloaded) / 1e6:F1} MB downloaded");
    }
    if (plan.Count > 0)
    {
        index.Save(indexPath + ".tmp");
        File.Move(indexPath + ".tmp", indexPath, true);
    }
    return Migrate();
}

// steam-chunks --out <file> --depot <id> --history <file>: the Steam chunk index of every manifest in the depot's history
// (from the cached manifests; any not cached are fetched, which needs the Steam login). No game content is read.
int SteamChunks()
{
    var depot = uint.Parse(One("--depot"));
    var history = DepotHistory.Load(One("--history")).OrderBy(h => h.Time).ToList();
    var index = new SteamChunkIndex();
    var clock = Stopwatch.StartNew();
    foreach (var h in history)
    {
        var m = SteamDepots.Cached(depot, h.Manifest);
        if (m is null)
        {
            try { m = Steam().Manifest(depot, h.Manifest, h.Branch ?? "public").GetAwaiter().GetResult(); }
            catch (Exception e) when (e is IOException or InvalidOperationException) { Console.WriteLine($"  {h.Time:yyyy-MM-dd} {h.Manifest}: {e.Message}"); continue; }
        }
        index.AddBuild($"{depot}:{h.Manifest}", h.Time, (m.Files ?? []).Select(f => (f.FileName.Replace('\\', '/'),
            f.Chunks.Select(c => ((long)c.Offset, (int)c.UncompressedLength, c.ChunkID!, c.Checksum)))));
    }
    index.Save(One("--out"));
    Console.WriteLine($"Steam chunk index: {index.Builds.Count} builds, {index.ChunkCount:N0} distinct chunks ({index.ChunkBytes / 1e12:F2} TB of game data described), " +
        $"{new FileInfo(One("--out")).Length / 1e6:F0} MB, in {clock.Elapsed.TotalSeconds:F0} s.");
    return 0;
}

// chunk-delta --chunks <file> --have <label>...: per build, its chunk bytes and those none of the have builds (the ones
// installed here) has: what indexing that build would download.
int ChunkDelta()
{
    var index = SteamChunkIndex.Load(One("--chunks"));
    var have = All("--have").Select(index.BuildIndex).ToList();
    if (have.Any(h => h < 0)) throw new ArgumentException("a --have build is not in the chunk index.");
    long sumTotal = 0, sumMissing = 0;
    foreach (var (b, i) in index.Builds.Select((b, i) => (b, i)).OrderBy(x => x.b.Time))
    {
        var (total, missing) = index.Delta(i, have);
        sumTotal += total;
        sumMissing += missing;
        Console.WriteLine($"{b.Time:yyyy-MM-dd} {b.Label,-30} {total / 1e9,7:F1} GB, {missing / 1e9,7:F2} GB not in the builds here ({100.0 * missing / Math.Max(1, total),5:F1}%)");
    }
    Console.WriteLine($"All {index.Builds.Count} builds: {sumTotal / 1e12:F2} TB, {sumMissing / 1e12:F2} TB not in the builds here.");
    foreach (var from in All("--from").Select(DateTime.Parse))
    {
        var wanted = index.Builds.Select((b, i) => (b, i)).Where(x => x.b.Time >= from).Select(x => x.i).ToList();
        Console.WriteLine($"The {wanted.Count} builds since {from:yyyy-MM-dd} together: {index.MissingUnion(wanted, have) / 1e9:F1} GB not in the builds here.");
    }
    return 0;
}

// local-check --local <depot>:<manifest>=<install folder> [--sample N]: how many of the installed build's chunks read
// back from its files with the right SHA-1 (a random sample). No login.
int LocalCheck()
{
    var sample = All("--sample").Select(int.Parse).DefaultIfEmpty(400).First();
    foreach (var (manifest, install) in Installed())
    {
        var local = new LocalChunks();
        local.Add(manifest, install);
        var all = (manifest.Files ?? []).SelectMany(f => f.Chunks).ToList();
        var pick = all.OrderBy(_ => Random.Shared.Next()).Take(sample).ToList();
        var known = pick.Count(local.Has);
        var ok = pick.Count(c => local.TryRead(c) is not null);
        Console.WriteLine($"{manifest.DepotID}:{manifest.ManifestGID} at {install}: {local.Count:N0} of {all.Select(c => Convert.ToHexString(c.ChunkID!)).Distinct().Count():N0} chunks mapped to files here; " +
            $"sample of {pick.Count}: {known} mapped, {ok} read back with the right SHA-1, {local.Failed} failed.");
    }
    return 0;
}

// scan --chunks <file> --mod <folder>: the runs of each mod file that are chunks Steam shipped, with the builds and game
// files they are from. Reads nothing from Steam.
int Scan()
{
    var index = SteamChunkIndex.Load(One("--chunks"));
    var mod = Path.GetFullPath(One("--mod"));
    long total = 0, matched = 0;
    var clock = Stopwatch.StartNew();
    using var list = All("--out").SingleOrDefault() is { } outPath ? new StreamWriter(outPath) : null;
    foreach (var file in Directory.EnumerateFiles(mod, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
    {
        var data = File.ReadAllBytes(file);
        total += data.Length;
        var found = index.Scan(data);
        foreach (var m in found)
            list?.WriteLine($"{Path.GetRelativePath(mod, file)}\t{m.Offset}\t{m.Length}\t{index.FileOf(m.Chunk)}\t{string.Join(",", index.BuildsOf(m.Chunk).Select(b => b.Label))}");
        if (found.Count == 0) continue;
        var bytes = found.Sum(m => (long)m.Length);
        matched += bytes;
        var builds = found.SelectMany(m => index.BuildsOf(m.Chunk)).Distinct().OrderBy(b => b.Time).ToList();
        var sources = found.GroupBy(m => index.FileOf(m.Chunk)).OrderByDescending(g => g.Sum(m => (long)m.Length)).Take(3)
            .Select(g => $"{g.Key} ({g.Sum(m => (long)m.Length) / 1e6:F1} MB)");
        Console.WriteLine($"{Path.GetRelativePath(mod, file)}: {bytes / 1e6:F1} of {data.Length / 1e6:F1} MB are Steam chunks ({found.Count} chunks), " +
            $"in {builds.Count} builds {builds.First().Time:yyyy-MM-dd} to {builds.Last().Time:yyyy-MM-dd}; from {string.Join(", ", sources)}");
    }
    Console.WriteLine($"{matched / 1e6:F1} of {total / 1e6:F1} MB proven to be bytes Steam shipped, in {clock.Elapsed.TotalSeconds:F1} s.");
    return 0;
}

// checksum-probe <depot> <manifest>: which checksum the manifest's chunk "Checksum" is, tried against cached chunks.
int ChecksumProbe()
{
    var m = SteamDepots.Cached(uint.Parse(args_[1]), ulong.Parse(args_[2])) ?? throw new ArgumentException("That manifest is not cached.");
    static uint Adler(ReadOnlySpan<byte> d, uint a, uint b)
    {
        foreach (var x in d) { a = (a + x) % 65521; b = (b + a) % 65521; }
        return (b << 16) | a;
    }
    int tried = 0, seed0 = 0, seed1 = 0, crc = 0;
    foreach (var c in (m.Files ?? []).SelectMany(f => f.Chunks))
    {
        var id = Convert.ToHexString(c.ChunkID!).ToLowerInvariant();
        var file = Path.Combine(SteamDepots.CacheFolder, "chunks", id[..2], id);
        if (!File.Exists(file)) continue;
        var data = File.ReadAllBytes(file);
        tried++;
        if (Adler(data, 0, 0) == c.Checksum) seed0++;
        if (Adler(data, 1, 0) == c.Checksum) seed1++;
        if (System.IO.Hashing.Crc32.HashToUInt32(data) == c.Checksum) crc++;
        if (tried == 40) break;
    }
    Console.WriteLine($"{tried} cached chunks: Adler-32 seeded 0 matches {seed0}, standard Adler-32 {seed1}, CRC-32 {crc}");
    return 0;
}

// chunk-stats <depot> <manifest>: how Steam cut the manifest's files into chunks (from the cached manifest; no login).
int ChunkStats()
{
    var m = SteamDepots.Cached(uint.Parse(args_[1]), ulong.Parse(args_[2])) ?? throw new ArgumentException("That manifest is not cached.");
    var chunks = (m.Files ?? []).SelectMany(f => f.Chunks.OrderBy(c => c.Offset).Select((c, i) => (f, c, i, last: i == f.Chunks.Count - 1))).ToList();
    var inner = chunks.Where(x => !x.last).ToList();
    Console.WriteLine($"{m.Files?.Count} files, {chunks.Count:N0} chunks; {inner.Count:N0} not last in their file");
    Console.WriteLine($"  inner chunk lengths: {string.Join(", ", inner.GroupBy(x => x.c.UncompressedLength).OrderByDescending(g => g.Count()).Take(6).Select(g => $"{g.Key:N0} x{g.Count():N0}"))}");
    Console.WriteLine($"  inner chunk offsets that are multiples of 1 MiB: {inner.Count(x => x.c.Offset % 1048576 == 0):N0} of {inner.Count:N0}");
    var gaps = chunks.GroupBy(x => x.f).Count(g => g.OrderBy(x => x.c.Offset).Select(x => x.c).Zip(g.OrderBy(x => x.c.Offset).Select(x => x.c).Skip(1)).Any(p => p.First.Offset + p.First.UncompressedLength != p.Second.Offset));
    Console.WriteLine($"  files whose chunks leave gaps or overlap: {gaps}");
    return 0;
}

// probe-range steam:<depot>:<manifest> <file> [bytes]: asks the content servers for only the first bytes of the file's
// first chunk (an HTTP range), decrypts that prefix and says what compression the chunk uses. Diagnostic.
int ProbeRange()
{
    var files = Depot(args_[1]);
    var name = args_[2];
    var want = args_.Count > 3 ? int.Parse(args_[3]) : 16384;
    var chunk = files.File(name).Chunks.OrderBy(c => c.Offset).First();
    var id = Convert.ToHexString(chunk.ChunkID!).ToLowerInvariant();
    var key = Steam().Key(files.Manifest.DepotID).GetAwaiter().GetResult();
    Console.WriteLine($"{name}: first chunk {id}, {chunk.CompressedLength:N0} bytes stored, {chunk.UncompressedLength:N0} uncompressed");
    using var http = new HttpClient();
    foreach (var server in Steam().Servers.Take(4))
    {
        var scheme = server.Protocol == SteamKit2.CDN.Server.ConnectionProtocol.HTTPS ? "https" : "http";
        var url = $"{scheme}://{server.VHost}:{server.Port}/depot/{files.Manifest.DepotID}/chunk/{id}";
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, want - 1);
        try
        {
            using var response = http.Send(request);
            var body = response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult();
            Console.WriteLine($"  {server.Type} {server.VHost}: HTTP {(int)response.StatusCode}, {body.Length:N0} bytes, range {response.Content.Headers.ContentRange}");
            if (body.Length < 32) continue;
            using var aes = System.Security.Cryptography.Aes.Create();
            aes.Key = key;
            var iv = aes.DecryptEcb(body.AsSpan(0, 16), System.Security.Cryptography.PaddingMode.None);
            var blocks = (body.Length - 16) / 16 * 16;
            var plain = aes.DecryptCbc(body.AsSpan(16, blocks), iv, System.Security.Cryptography.PaddingMode.None);
            var head = plain.AsSpan(0, 16).ToArray();
            Console.WriteLine($"    decrypted prefix: {Convert.ToHexString(head)}  \"{new string(head.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray())}\"");
            return 0;
        }
        catch (Exception e) when (e is HttpRequestException or IOException) { Console.WriteLine($"  {server.VHost}: {e.Message}"); }
    }
    return 1;
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
    long total = 0, unique = 0, chunkBytes = 0, chunkCompressed = 0, tableChunks = 0, tableBytes = 0;
    var tableSeen = new HashSet<string>(StringComparer.Ordinal);
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
                // What the name map reads: the chunk at the start of each distinct archive (and of bundles.nxa's files).
                var first = files.File(n).Chunks.OrderBy(c => c.Offset).FirstOrDefault();
                if (first is not null && (n.Length == 16 || n.EndsWith(".nxa")) && tableSeen.Add(Convert.ToHexString(first.ChunkID!))) { tableChunks++; tableBytes += first.CompressedLength; }
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
        $"{chunkBytes / 1e12:F2} TB in distinct Steam chunks ({chunkCompressed / 1e12:F2} TB to download). " +
        $"The name map's first chunks: {tableChunks:N0} distinct, {tableBytes / 1e9:F1} GB to download (packed builds read more inside bundles.*.nxa).");
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
    games.AddRange(SteamBuilds(index, games.Select(g => g.Build).ToList()));
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
    if (All("--chunks").Count > 0)
    {
        clock.Restart();
        report.Shipped.AddRange(SteamChunkIndex.Load(One("--chunks")).Runs(files));
        Console.WriteLine($"Steam chunks: {report.Shipped.Sum(r => r.Length) / 1e6:F1} MB of the mod are chunks Steam shipped ({clock.Elapsed.TotalSeconds:F1} s).");
    }

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
    var decided = MigrationVerdict.Decide(report, index, BuildCatalog.LoadWithHistory(One("--catalog")), recipe.Files.Count, rebuildError,
        All("--map").Count > 0 ? NameMap.Load(One("--map")) : null);
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
    var (leftBytes, leftRuns) = report.ShippedLeft();
    if (leftBytes > 0)
    {
        s.AppendLine();
        s.AppendLine("## Still shipped, but bytes Steam shipped");
        s.AppendLine();
        s.AppendLine($"{Size(leftBytes)} of the bytes the mod still ships are chunks of the game's depot: their SHA-1 is in the depot manifests of these builds. " +
            "resolve --chunks fingerprints the game resources at those places and migrates again.");
        s.AppendLine();
        s.AppendLine("| Mod file | At | Run | Still shipped | Game file | Builds | From | To |");
        s.AppendLine("|---|---|---|---|---|---|---|---|");
        foreach (var (r, bytes) in leftRuns.Take(40))
            s.AppendLine($"| `{r.File}` | {r.Offset:N0} | {Size(r.Length)} | {Size(bytes)} | `{r.GameFile}` | {r.Builds} | {r.First:yyyy-MM-dd} | {r.Last:yyyy-MM-dd} |");
        if (leftRuns.Count > 40) s.AppendLine($"| ... and {leftRuns.Count - 40} more | | | | | | | |");
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

sealed class LazyGame(string label, Func<IGameResources> open) : IGameResources
{
    IGameResources? game;
    IGameResources Game => game ??= open();
    public string Build => label;
    public IEnumerable<(HellForge.Core.GameData.ResourceKey Key, string Archive, Func<HellForge.Core.GameData.ResourceParts> Read)> All() => Game.All();
    public string? ArchiveHash(string archive) => Game.ArchiveHash(archive);
    public IEnumerable<(string Archive, Func<IReadOnlyList<HellForge.Core.GameData.ResourceKey>> Keys)> Tables() => Game.Tables();
    public string? TableHash(string archive) => Game.TableHash(archive);
    public byte[]? Read(HellForge.Core.GameData.ResourceKey key, PartKind part) => Game.Read(key, part);
    public IEnumerable<(HellForge.Core.GameData.ResourceKey Key, string Archive, Func<HellForge.Core.GameData.ResourceParts> Read)> In(IReadOnlySet<string> archives) => Game.In(archives);
    public IEnumerable<(HellForge.Core.GameData.ResourceKey Key, string Archive)> At(string file, long offset, long length) => Game.At(file, offset, length);
    public IReadOnlyList<byte[]> ReadIn(HellForge.Core.GameData.ResourceKey key, PartKind part, string? archive) => Game.ReadIn(key, part, archive);
}
