using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// The asset migrator (HellForge.Core.AssetMigration): a mod built from the game's resources becomes a recipe plus the
// author's own bytes, and the recipe rebuilds the mod byte for byte from the game. Every "game" here is synthetic.
static class AssetMigrationTests
{
    sealed class FakeGame(string build, Dictionary<ResourceKey, ResourceParts> resources) : IGameResources
    {
        public string Build => build;
        public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All() => resources.Select(r => (r.Key, "00000000000000ff", (Func<ResourceParts>)(() => r.Value)));
        public byte[]? Read(ResourceKey key, PartKind part) => resources.TryGetValue(key, out var p) ? Parts.Of(p, part) : null;
    }

    // A folder whose files report their SHA-1, as a depot manifest does.
    sealed class HashedFolder(string folder) : IDataFiles
    {
        readonly FolderFiles inner = new(folder);
        public int Opened;
        public string Name => inner.Name;
        public bool Exists(string name) => inner.Exists(name);
        public Stream Open(string name) { Opened++; return inner.Open(name); }
        public IEnumerable<string> List() => inner.List();
        public string? Hash(string name) => Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(File.ReadAllBytes(Path.Combine(inner.Folder, name))));
    }

    static byte[] Noise(int seed, int length)
    {
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }

    static byte[] Concat(params byte[][] parts) => parts.SelectMany(p => p).ToArray();

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var rifle = new ResourceKey(ResourceTypes.Unit, 0x1111);
        var scope = new ResourceKey(ResourceTypes.Unit, 0x2222);
        var skin = new ResourceKey(ResourceTypes.Texture, 0x3333);
        var rifleGpu = Noise(1, 200_000);
        var scopeGpu = Noise(2, 90_000);
        var skinGpu = Noise(3, 300_000);
        var oldBuild = new FakeGame("100", new()
        {
            [rifle] = new(Noise(4, 4000), rifleGpu, []),
            [skin] = new(Noise(5, 600), skinGpu, []),
        });
        // A newer build adds the scope; the rifle and skin are unchanged.
        var newBuild = new FakeGame("200", new()
        {
            [rifle] = new(Noise(4, 4000), rifleGpu, []),
            [skin] = new(Noise(5, 600), skinGpu, []),
            [scope] = new(Noise(6, 3000), scopeGpu, []),
        });
        SourceIndex Index()
        {
            var index = new SourceIndex();
            index.Add(oldBuild);
            index.Add(newBuild);
            index.Seal();
            return index;
        }

        // A kitbash: the rifle's barrel half, the newer build's scope, the author's own grip, a texture with one region
        // repainted, and zero padding between them, at offsets that line up with nothing in the game.
        var grip = Noise(7, 25_000);
        var repainted = skinGpu.ToArray();
        Noise(8, 4_000).CopyTo(repainted, 120_000);
        var kitbash = Concat([1, 2, 3], rifleGpu[..100_000], new byte[333], scopeGpu, grip, repainted);
        var readme = "My kitbash, made by me."u8.ToArray();
        IReadOnlyList<(string, byte[])> mod = [("data/kitbash.bin", kitbash), ("README.txt", readme)];

        test("The source index holds fingerprints of each distinct game part once, and no game bytes", () =>
        {
            var index = Index();
            check(index.Contents.Count == 6, $"distinct parts: {index.Contents.Count}");
            check(index.Contents.Single(c => c.Size == rifleGpu.Length).Places.Count == 2, "the rifle's GPU part is in both builds");
            var file = Path.Combine(root, "migration.hfsi");
            index.Save(file);
            var saved = File.ReadAllBytes(file);
            check(saved.AsSpan().IndexOf(rifleGpu.AsSpan(5000, 64)) < 0, "the index file holds game bytes");
            var loaded = SourceIndex.Load(file);
            check(loaded.Contents.Count == 6 && loaded.AnchorCount == index.AnchorCount, "the index does not load back the same");
        });

        test("A kitbash's game pieces become references; only the author's bytes ship", () =>
        {
            var (recipe, literals, report) = new Migrator(Index(), [oldBuild, newBuild]).Migrate(mod);
            var line = report.Files.Single(f => f.Path == "data/kitbash.bin");
            long game = rifleGpu.Length / 2 + scopeGpu.Length + skinGpu.Length - 4_000;
            check(line.FromGame >= game - 200 && line.FromGame <= game + 16, $"from game {line.FromGame}, expected about {game}");
            check(literals.Length <= grip.Length + 4_000 + 3 + 200, $"literals {literals.Length}");
            check(literals.AsSpan().IndexOf(grip.AsSpan(1000, 64)) >= 0, "the author's grip is shipped");
            check(literals.AsSpan().IndexOf(scopeGpu.AsSpan(1000, 64)) < 0 && literals.AsSpan().IndexOf(rifleGpu.AsSpan(1000, 64)) < 0, "game bytes are shipped");
            check(recipe.Files.Count == 1 && !report.Files.Single(f => f.Path == "README.txt").InRecipe, "the readme should ship as it is");
            check(recipe.Sources.Any(s => s.Places.All(p => p.Build == "200")), "the scope comes from the newer build only");
        });

        test("The recipe rebuilds the mod byte for byte from the game", () =>
        {
            var (recipe, literals, _) = new Migrator(Index(), [oldBuild, newBuild]).Migrate(mod);
            var folder = Path.Combine(root, "recipe");
            recipe.Save(folder, literals);
            var (loaded, loadedLiterals) = Recipe.Load(folder);
            var rebuilt = RecipeRebuilder.Rebuild(loaded, loadedLiterals, [oldBuild, newBuild]);
            check(rebuilt["data/kitbash.bin"].AsSpan().SequenceEqual(kitbash), "the rebuilt file differs");
        });

        test("A rebuild without the build a piece comes from, or with a changed piece, is refused", () =>
        {
            var (recipe, literals, _) = new Migrator(Index(), [oldBuild, newBuild]).Migrate(mod);
            reject(() => RecipeRebuilder.Rebuild(recipe, literals, [oldBuild]));
            var changed = scopeGpu.ToArray();
            changed[10] ^= 1;
            var patched = new FakeGame("200", new() { [rifle] = new(Noise(4, 4000), rifleGpu, []), [skin] = new(Noise(5, 600), skinGpu, []), [scope] = new(Noise(6, 3000), changed, []) });
            reject(() => RecipeRebuilder.Rebuild(recipe, literals, [oldBuild, patched]));
        });

        test("A mod with nothing from the game is left as it is", () =>
        {
            var (recipe, literals, report) = new Migrator(Index(), [oldBuild, newBuild]).Migrate([("data/own.bin", Noise(9, 50_000))]);
            check(recipe.Files.Count == 0 && literals.Length == 0 && !report.Files[0].InRecipe, "an original mod was put in a recipe");
        });

        test("An older build's loose archives are read and indexed like the packed ones", () =>
        {
            var folder = Path.Combine(root, "loose-build");
            Directory.CreateDirectory(folder);
            var apc = new ResourceKey(ResourceTypes.Unit, 0x4444);
            var apcParts = new ResourceParts(Noise(11, 2000), Noise(12, 80_000), Noise(13, 5000));
            var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [apc] = apcParts, [rifle] = new(Noise(4, 4000), rifleGpu, []) });
            File.WriteAllBytes(Path.Combine(folder, "00000000000000aa"), archive);
            File.WriteAllBytes(Path.Combine(folder, "00000000000000aa.gpu_resources"), gpu);
            File.WriteAllBytes(Path.Combine(folder, "00000000000000aa.stream"), stream);
            var old = new LooseGameResources(folder, "50");
            check(old.Read(apc, PartKind.Gpu)!.AsSpan().SequenceEqual(apcParts.Gpu) && old.Read(apc, PartKind.Stream)!.AsSpan().SequenceEqual(apcParts.Stream), "the loose parts read back wrong");
            var index = new SourceIndex();
            index.Add(old);
            index.Add(newBuild);
            index.Seal();
            var cut = Concat(apcParts.Gpu, Noise(14, 3000));
            var (recipe, literals, report) = new Migrator(index, [old, newBuild]).Migrate([("data/apc.bin", cut)]);
            check(report.Files[0].FromGame == apcParts.Gpu.Length, $"from game {report.Files[0].FromGame}");
            check(RecipeRebuilder.Rebuild(recipe, literals, [old, newBuild])["data/apc.bin"].AsSpan().SequenceEqual(cut), "the cut content does not rebuild");
        });

        // A mod archive with one resource the newer build has and one no build has (cut content, or the author's own).
        var unknownKey = new ResourceKey(ResourceTypes.Texture, 0x5555);
        var unknownParts = new ResourceParts(Noise(15, 400), Noise(16, 60_000), []);
        var (modArchive, modGpu, modStream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
        {
            [scope] = new(Noise(6, 3000), scopeGpu, []),
            [unknownKey] = unknownParts,
        });
        IReadOnlyList<(string, byte[])> archiveMod = [("data/abcd.patch_0", modArchive), ("data/abcd.patch_0.gpu_resources", modGpu), ("data/abcd.patch_0.stream", modStream)];
        string Catalog(bool complete, params string[] builds) =>
            $$"""{"schemaVersion": 1, "appId": "553850", "historyComplete": {{(complete ? "true" : "false")}}, "builds": [{{string.Join(",", builds.Select(b => $$"""{"buildId": "{{b}}", "layout": "{{(b == "50" ? "loose" : "nxa")}}", "depots": [{"id": 553851, "manifestId": "{{b}}0001"}]}"""))}}]}""";
        MigrationVerdict Verdict(SourceIndex index, IReadOnlyList<IGameResources> games, string catalog)
        {
            var (recipe, literals, report) = new Migrator(index, games).Migrate(archiveMod);
            RecipeRebuilder.Rebuild(recipe, literals, games);
            return MigrationVerdict.Decide(report, index, BuildCatalog.Parse(catalog), recipe.Files.Count, null);
        }

        test("Content no indexed build has needs a review until every build since release is indexed", () =>
        {
            var index = Index();
            var partly = Verdict(index, [oldBuild, newBuild], Catalog(false, "100", "200"));
            check(partly.Kind == VerdictKind.PartlyMigrated && !partly.Publishable, partly.Text);
            var missing = Verdict(index, [oldBuild, newBuild], Catalog(true, "50", "100", "200"));
            check(missing.Kind == VerdictKind.PartlyMigrated && missing.Text.Contains("(50)"), missing.Text);
            var covered = Verdict(index, [oldBuild, newBuild], Catalog(true, "100", "200"));
            check(covered.Kind == VerdictKind.Verified && covered.Publishable && covered.Text.Contains("author's own"), covered.Text);
        });

        test("A player without an old build is told which of its archive files the recipe needs, and how to fetch them", () =>
        {
            var folder = Path.Combine(root, "loose-plan");
            Directory.CreateDirectory(folder);
            var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [unknownKey] = unknownParts });
            File.WriteAllBytes(Path.Combine(folder, "00000000000000bb"), archive);
            File.WriteAllBytes(Path.Combine(folder, "00000000000000bb.gpu_resources"), gpu);
            File.WriteAllBytes(Path.Combine(folder, "00000000000000bb.stream"), stream);
            var old = new LooseGameResources(folder, "50");
            var index = new SourceIndex();
            index.Add(old);
            index.Add(oldBuild);
            index.Add(newBuild);
            index.Seal();
            var (recipe, literals, report) = new Migrator(index, [old, oldBuild, newBuild]).Migrate(archiveMod);
            var catalog = BuildCatalog.Parse(Catalog(true, "50", "100", "200"));
            check(MigrationVerdict.Decide(report, index, catalog, recipe.Files.Count, null).Kind == VerdictKind.Verified, "the cut content was not found in the old build");
            var plan = RecipeRebuilder.FetchPlan(recipe, ["100", "200"], catalog);
            check(plan.Count == 1 && plan[0].Build == "50", $"plan: {string.Join("; ", plan.Select(p => p.Build))}");
            check(plan[0].Files.SetEquals(["data/00000000000000bb", "data/00000000000000bb.gpu_resources"]), string.Join(", ", plan[0].Files));
            check(plan[0].Commands.SequenceEqual(["download_depot 553850 553851 500001"]), string.Join(", ", plan[0].Commands));
            check(RecipeRebuilder.FetchPlan(recipe, ["50", "100", "200"], catalog).Count == 0, "a player with every build needs nothing");
        });

        test("A saved index takes another build and finds its content", () =>
        {
            var first = new SourceIndex();
            first.Add(oldBuild);
            var file = Path.Combine(root, "add-to.hfsi");
            first.Save(file);
            var index = SourceIndex.Load(file);
            var before = index.AnchorCount;
            index.Add(newBuild);
            index.Save(file);
            index = SourceIndex.Load(file);
            check(index.Builds.SequenceEqual(["100", "200"]) && index.AnchorCount > before, $"builds {string.Join(",", index.Builds)}, anchors {index.AnchorCount}");
            var (_, _, report) = new Migrator(index, [oldBuild, newBuild]).Migrate([("x", scopeGpu)]);
            check(report.Files[0].FromGame == scopeGpu.Length, "the added build's content is not found");
        });

        test("An archive unchanged since an earlier build is taken from the index without reading it", () =>
        {
            var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [rifle] = new(Noise(4, 4000), rifleGpu, []) });
            var folders = new[] { "memo-a", "memo-b" }.Select(n => Path.Combine(root, n)).ToArray();
            foreach (var f in folders)
            {
                Directory.CreateDirectory(f);
                File.WriteAllBytes(Path.Combine(f, "00000000000000cc"), archive);
                File.WriteAllBytes(Path.Combine(f, "00000000000000cc.gpu_resources"), gpu);
                File.WriteAllBytes(Path.Combine(f, "00000000000000cc.stream"), stream);
            }
            var first = new HashedFolder(folders[0]);
            var second = new HashedFolder(folders[1]);
            var index = new SourceIndex();
            index.Add(new LooseGameResources(first, "1"));
            var file = Path.Combine(root, "memo.hfsi");
            index.Save(file);
            index = SourceIndex.Load(file);
            var opened = second.Opened;
            index.Add(new LooseGameResources(second, "2"));
            index.Seal();
            check(index.ArchivesReused == 1, $"reused {index.ArchivesReused}");
            check(second.Opened - opened <= 1, $"the unchanged archive was read ({second.Opened - opened} opens beyond its table)");
            var content = index.Contents.Single(c => c.Size == rifleGpu.Length);
            check(content.Places.Select(p => index.Builds[p.Build]).Distinct().SequenceEqual(["1", "2"]), "the second build is not a place of the rifle");
        });

        // Two old-layout builds as a depot history: build 553853:1 has the APC's archive, 553853:2 (newer) has it unchanged.
        var apcKey = new ResourceKey(ResourceTypes.Unit, 0x6666);
        var apcParts = new ResourceParts(Noise(21, 1500), Noise(22, 70_000), Noise(23, 4000));
        (LooseGameResources Game, HashedFolder Files)[] History()
        {
            var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [apcKey] = apcParts });
            return new[] { "553853:1", "553853:2" }.Select(label =>
            {
                var folder = Path.Combine(root, "history-" + label.Replace(':', '-'));
                Directory.CreateDirectory(folder);
                File.WriteAllBytes(Path.Combine(folder, "00000000000000dd"), archive);
                File.WriteAllBytes(Path.Combine(folder, "00000000000000dd.gpu_resources"), gpu);
                File.WriteAllBytes(Path.Combine(folder, "00000000000000dd.stream"), stream);
                var files = new HashedFolder(folder);
                return (new LooseGameResources(files, label), files);
            }).ToArray();
        }
        BuildCatalog HistoryCatalog()
        {
            var catalog = BuildCatalog.Parse("""{"schemaVersion": 1, "appId": "553850", "historyComplete": true, "dataDepot": 553853, "builds": []}""");
            catalog.History = DepotHistory.Parse(["2024-04-29T09:00:20Z 1", "2024-05-02T09:00:21Z 2"]);
            return catalog;
        }
        var apcMod = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [apcKey] = apcParts, [unknownKey] = unknownParts });
        IReadOnlyList<(string, byte[])> apcFiles = [("data/ee.patch_0", apcMod.Archive), ("data/ee.patch_0.gpu_resources", apcMod.Gpu), ("data/ee.patch_0.stream", apcMod.Stream)];

        test("The name map lists every build's resources from their tables, reading an unchanged archive's table once", () =>
        {
            var builds = History();
            var map = new NameMap();
            foreach (var (game, _) in builds) map.Add(game);
            check(map.TablesRead == 1 && map.TablesReused == 1, $"read {map.TablesRead}, reused {map.TablesReused}");
            var file = Path.Combine(root, "names.hfnm");
            map.Save(file);
            map = NameMap.Load(file);
            check(map.Find(apcKey).Select(f => f.Build).SequenceEqual(["553853:1", "553853:2"]), string.Join(",", map.Find(apcKey)));
            check(map.Has(apcKey) && !map.Has(unknownKey), "the map has the wrong resources");
        });

        test("A leftover resource the game has by name needs review until its archive is fingerprinted; a name no build has is the author's", () =>
        {
            var builds = History();
            var map = new NameMap();
            foreach (var (game, _) in builds) map.Add(game);
            var index = Index();
            var (recipe, literals, report) = new Migrator(index, [oldBuild, newBuild]).Migrate(apcFiles);
            var before = MigrationVerdict.Decide(report, index, HistoryCatalog(), recipe.Files.Count, null, map);
            check(before.Kind == VerdictKind.PartlyMigrated && before.Text.Contains("by name"), before.Text);

            // resolve: only the archive that holds the resource, in the newest build that has it.
            index.Add(builds[1].Game, null, new HashSet<string> { "00000000000000dd" });
            index.Seal();
            check(index.PartialBuilds.Contains("553853:2") && HistoryCatalog().Missing(index).Count == 2, "a partial build counts as covered");
            (recipe, literals, report) = new Migrator(index, [oldBuild, newBuild, builds[1].Game]).Migrate(apcFiles);
            var after = MigrationVerdict.Decide(report, index, HistoryCatalog(), recipe.Files.Count, null, map);
            check(after.Kind == VerdictKind.Verified && after.Text.Contains("author's own"), after.Text);
            check(report.Resources.Where(r => r.Key == apcKey).All(r => r.Literal == 0), "the APC's parts were not purged");
            check(RecipeRebuilder.Rebuild(recipe, literals, [oldBuild, newBuild, builds[1].Game]).Count == 3, "the mod does not rebuild");

            var partial = HistoryCatalog();
            partial.History!.Add(new DepotHistory(new DateTime(2024, 6, 1), 3, null));
            check(MigrationVerdict.Decide(report, index, partial, recipe.Files.Count, null, map).Kind == VerdictKind.PartlyMigrated, "a name map missing a build passed");
        });

        test("The same piece at any byte offset is found", () =>
        {
            for (var shift = 0; shift < 20; shift++)
            {
                var file = Concat(Noise(10 + shift, shift), skinGpu[50_000..60_000], Noise(40 + shift, 7));
                var (_, _, report) = new Migrator(Index(), [oldBuild, newBuild]).Migrate([("x", file)]);
                check(report.Files[0].FromGame == 10_000, $"shift {shift}: {report.Files[0].FromGame}");
            }
        });

        test("Padding between game pieces is a fill, however short: the mod ships none of it", () =>
        {
            var file = Concat(skinGpu[..20_000], new byte[8], rifleGpu[..20_000], [0x11, 0x11], skinGpu[30_000..40_000], [0x42]);
            var (recipe, literals, report) = new Migrator(Index(), [oldBuild, newBuild]).Migrate([("x", file)]);
            check(report.Files[0].Literal == 1 && report.Kept["x"].Sum(k => k.Length) == 1 && literals.Length == 1, $"literal {report.Files[0].Literal}");
            check(RecipeRebuilder.Rebuild(recipe, literals, [oldBuild, newBuild])["x"].AsSpan().SequenceEqual(file), "padding does not rebuild");
        });

        test("In periodic data, the source that matches throughout wins over a similar one that matches most of each period", () =>
        {
            // Two textures of 256-byte blocks: the same 242 bytes in each block, and 14 bytes that differ between them.
            var unit = Noise(80, 242);
            byte[] Blocks(int seed) { var tail = Noise(seed, 14); return Enumerable.Range(0, 400).SelectMany(_ => unit.Concat(tail)).ToArray(); }
            // Each with data of its own before the blocks, which brings it in as a candidate; the mod has more of the
            // similar one's, so that one is tried first.
            var shared = Noise(83, 12_000);
            var similar = Concat(shared, Blocks(81));
            var exact = Concat(Noise(84, 4000), Blocks(82));
            var game = new FakeGame("300", new()
            {
                [new ResourceKey(ResourceTypes.Texture, 0x8001)] = new([], similar, []),
                [new ResourceKey(ResourceTypes.Texture, 0x8002)] = new([], exact, []),
            });
            var index = new SourceIndex();
            index.Add(game);
            index.Seal();
            var mod = Concat(shared, exact);
            var (recipe, literals, report) = new Migrator(index, [game]).Migrate([("data/tex.stream", mod)]);
            check(report.Files[0].Literal == 0 && literals.Length == 0, $"{report.Files[0].Literal} literal bytes");
            check(RecipeRebuilder.Rebuild(recipe, literals, [game])["data/tex.stream"].AsSpan().SequenceEqual(mod), "it does not rebuild");
        });

        test("A resource two archives of a build hold with different bytes is read from the archive the index names", () =>
        {
            var folder = Path.Combine(root, "two-copies");
            Directory.CreateDirectory(folder);
            var hull = new ResourceKey(ResourceTypes.Unit, 0x9001);
            var older = new ResourceParts(Noise(91, 600), Noise(92, 30_000), []);
            var newer = new ResourceParts(Noise(93, 600), Noise(94, 30_000), []);
            foreach (var (name, parts) in new[] { ("00000000000000aa", newer), ("00000000000000bb", older) })
            {
                var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [hull] = parts });
                File.WriteAllBytes(Path.Combine(folder, name), archive);
                File.WriteAllBytes(Path.Combine(folder, name + ".gpu_resources"), gpu);
                File.WriteAllBytes(Path.Combine(folder, name + ".stream"), stream);
            }
            var game = new LooseGameResources(folder, "553853:7");
            check(game.Read(hull, PartKind.Gpu)!.AsSpan().SequenceEqual(newer.Gpu), "the first archive's copy is not the build's own");
            var index = new SourceIndex();
            index.Add(game, null, new HashSet<string> { "00000000000000bb" });
            index.Seal();
            var mod = Concat(older.Gpu, Noise(95, 500));
            var (recipe, literals, report) = new Migrator(index, [game]).Migrate([("data/hull.gpu_resources", mod)]);
            check(report.Unreadable.Count == 0 && report.Files[0].FromGame == older.Gpu.Length, $"from game {report.Files[0].FromGame}, unreadable {report.Unreadable.Count}");
            check(RecipeRebuilder.Rebuild(recipe, literals, [game])["data/hull.gpu_resources"].AsSpan().SequenceEqual(mod), "it does not rebuild from the second archive");
        });

        // A depot manifest's view of a game file: its chunks' offsets, lengths, SHA-1s and Adler-32s. Steam cuts files
        // into 1 MiB chunks and keeps old chunks at any offset, so a manifest has odd-length chunks too.
        const int MiB = SteamChunkIndex.Window;
        static IEnumerable<(long, int, byte[], uint)> Chunks(byte[] file, params (int Offset, int Length)[] at) =>
            at.Select(c => ((long)c.Offset, c.Length, System.Security.Cryptography.SHA1.HashData(file.AsSpan(c.Offset, c.Length)), SteamChunkIndex.Adler32(file.AsSpan(c.Offset, c.Length))));
        var shippedFile = Noise(60, 3 * MiB + 300_001);
        (int, int)[] cuts = [(0, MiB), (MiB, MiB), (2 * MiB, 300_001), (2 * MiB + 300_001, MiB)];

        test("Chunks Steam shipped are found anywhere in a mod, under any name, from the depot manifests alone", () =>
        {
            var index = new SteamChunkIndex();
            index.AddBuild("553853:1", new DateTime(2024, 2, 8), [("data/00000000000000aa.stream", Chunks(shippedFile, cuts))]);
            var other = Noise(61, MiB);
            index.AddBuild("553853:2", new DateTime(2024, 3, 26), [("data/00000000000000aa.stream", Chunks(shippedFile, cuts)), ("data/00000000000000bb", Chunks(other, (0, MiB)))]);
            var file = Path.Combine(root, "chunks.hfsc");
            index.Save(file);
            index = SteamChunkIndex.Load(file);

            // The game file from part-way into its first chunk, at an odd offset, in a mod file of another name.
            var mod = Concat(Noise(62, 777), shippedFile[300_000..], Noise(63, 1234));
            var found = index.Scan(mod);
            check(found.Count == 3 && found.Sum(m => (long)m.Length) == 2L * MiB + 300_001, $"found {found.Count} chunks, {found.Sum(m => (long)m.Length)} bytes");
            check(found[0].Offset == 777 + MiB - 300_000, $"first chunk at {found[0].Offset}");
            var runs = index.Runs([("data/x.patch_0.stream", mod)]);
            check(runs.Count == 1 && runs[0].GameFile == "data/00000000000000aa.stream" && runs[0].Builds == 2 && runs[0].First == new DateTime(2024, 2, 8) && runs[0].Last == new DateTime(2024, 3, 26),
                string.Join("; ", runs));
            check(index.Locate(found[0].Chunk, index.BuildIndex("553853:2")) == ("data/00000000000000aa.stream", (long)MiB), $"{index.Locate(found[0].Chunk, 1)}");

            // An edit inside a chunk loses that chunk only: the next whole chunk is found, then its neighbour behind it.
            var edited = (byte[])mod.Clone();
            edited[777 + MiB] ^= 0xff;
            found = index.Scan(edited);
            check(found.Count == 2 && found[0].Length == 300_001 && found[1].Length == MiB, $"edited: {string.Join(", ", found.Select(m => m.Length))}");
            check(index.Scan(Concat(Noise(64, 5000), shippedFile[..(MiB - 1)], Noise(65, 5000))).Count == 0, "a partial chunk was matched");
            check(index.Scan(other).Count == 1 && index.Scan(Noise(66, 3 * MiB)).Count == 0, "the other file or noise was wrong");
        });

        test("Bytes still shipped that are chunks Steam shipped hold the mod, even under names no build has", () =>
        {
            var builds = History();
            var map = new NameMap();
            foreach (var (game, _) in builds) map.Add(game);
            var index = Index();
            var report = new MigrationReport();
            report.Files.Add(new MigrationReport.FileLine("data/ee.patch_0.stream", 2 * MiB, 0, 2 * MiB, 0, false));
            report.Resources.Add(new MigrationReport.ResourceLine("data/ee.patch_0.stream", unknownKey, PartKind.Stream, 0, 2 * MiB, 0, 2 * MiB, 0, [], false));
            report.Kept["data/ee.patch_0.stream"] = [(0, 2 * MiB)];
            var clean = MigrationVerdict.Decide(report, index, HistoryCatalog(), 0, null, map);
            check(clean.Kind == VerdictKind.NoGameContent, clean.Text);
            report.Shipped.Add(new SteamChunkIndex.Run("data/ee.patch_0.stream", 4096, MiB, "data/00000000000000aa.stream", new DateTime(2024, 2, 8), new DateTime(2024, 3, 26), 17));
            check(report.ShippedLeft().Bytes == MiB, $"{report.ShippedLeft().Bytes}");
            foreach (var held in new[] { MigrationVerdict.Decide(report, index, HistoryCatalog(), 0, null, map), MigrationVerdict.Decide(report, index, HistoryCatalog(), 0, null) })
                check(held.Kind == VerdictKind.PartlyMigrated && held.Text.Contains("chunks Steam shipped") && held.Text.Contains("2024-02-08"), held.Text);
        });

        test("A kitbash's own edit next to Steam chunks the recipe takes from the game is not counted as shipped game bytes", () =>
        {
            // The resource holds a 2 MiB run of a Steam chunk the recipe copies from the game, and 5,000 bytes of the
            // author's own just after it: only those 5,000 bytes ship, and none of them are the chunk.
            var report = new MigrationReport();
            report.Files.Add(new MigrationReport.FileLine("data/kb.patch_0.stream", 3 * MiB, 2 * MiB, 5000, MiB - 5000, true));
            report.Resources.Add(new MigrationReport.ResourceLine("data/kb.patch_0.stream", skin, PartKind.Stream, 0, 3 * MiB, 2 * MiB, 5000, MiB - 5000, [], true));
            report.Kept["data/kb.patch_0.stream"] = [(2 * MiB, 5000)];
            report.Shipped.Add(new SteamChunkIndex.Run("data/kb.patch_0.stream", 0, 2 * MiB, "data/00000000000000aa.stream", new DateTime(2026, 2, 3), new DateTime(2026, 2, 10), 2));
            check(report.ShippedLeft().Bytes == 0, $"{report.ShippedLeft().Bytes} bytes counted");
            // A chunk run reaching 100 bytes into the edit counts those 100 bytes only.
            report.Shipped.Add(new SteamChunkIndex.Run("data/kb.patch_0.stream", 2 * MiB - MiB + 100, MiB, "data/00000000000000aa.stream", new DateTime(2026, 2, 3), new DateTime(2026, 2, 10), 2));
            check(report.ShippedLeft().Bytes == 100 && report.ShippedLeft().Runs.Count == 1, $"{report.ShippedLeft().Bytes} bytes counted");
        });

        test("A targeted fetch reads only the archives it names, and only the resources at a place in a game file", () =>
        {
            var folder = Path.Combine(root, "targeted");
            Directory.CreateDirectory(folder);
            var tank = new ResourceKey(ResourceTypes.Unit, 0x7777);
            var tankParts = new ResourceParts(Noise(71, 900), Noise(72, 40_000), Noise(73, 9000));
            foreach (var (name, resources) in new[]
            {
                ("00000000000000aa", new Dictionary<ResourceKey, ResourceParts> { [rifle] = new(Noise(4, 4000), rifleGpu, []) }),
                ("00000000000000bb", new Dictionary<ResourceKey, ResourceParts> { [apcKey] = apcParts, [tank] = tankParts }),
            })
            {
                var (archive, gpu, stream) = ResourceArchive.Write(resources);
                File.WriteAllBytes(Path.Combine(folder, name), archive);
                File.WriteAllBytes(Path.Combine(folder, name + ".gpu_resources"), gpu);
                File.WriteAllBytes(Path.Combine(folder, name + ".stream"), stream);
            }
            var files = new HashedFolder(folder);
            var game = new LooseGameResources(files, "553853:9");
            var keys = game.In(new HashSet<string> { "00000000000000bb" }).Select(r => r.Key).ToList();
            check(files.Opened == 1 && keys.Count == 2 && keys.Contains(tank) && keys.Contains(apcKey), $"opened {files.Opened}, keys {keys.Count}");

            var bbStream = File.ReadAllBytes(Path.Combine(folder, "00000000000000bb.stream"));
            var at = bbStream.AsSpan().IndexOf(tankParts.Stream);
            var hit = game.At("00000000000000bb.stream", at + 10, 100).ToList();
            check(hit.Count == 1 && hit[0] == (tank, "00000000000000bb"), string.Join(", ", hit));
            check(game.At("00000000000000bb.stream", 0, bbStream.Length).Count() == 2 && !game.At("00000000000000aa.stream", 0, 10).Any(), "the wrong resources are at those places");

            var index = new SourceIndex();
            index.Add(game, null, new HashSet<string> { "00000000000000bb" }, new HashSet<ResourceKey> { tank });
            check(index.Contents.Count == 3 && index.Contents.All(c => c.Places.All(p => p.Key == tank)), $"{index.Contents.Count} contents");
            index.Add(game, null, new HashSet<string> { "00000000000000bb" }, new HashSet<ResourceKey> { apcKey });
            check(index.Contents.Count == 6 && index.PartialBuilds.Contains("553853:9"), $"{index.Contents.Count} contents after the second fetch");
        });
    }
}
