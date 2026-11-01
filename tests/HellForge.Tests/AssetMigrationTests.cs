using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// The asset migrator (HellForge.Core.AssetMigration): a mod built from the game's resources becomes a recipe plus the
// author's own bytes, and the recipe rebuilds the mod byte for byte from the game. Every "game" here is synthetic.
static class AssetMigrationTests
{
    sealed class FakeGame(string build, Dictionary<ResourceKey, ResourceParts> resources) : IGameResources
    {
        public string Build => build;
        public IEnumerable<(ResourceKey Key, ResourceParts Parts, string Archive)> All() => resources.Select(r => (r.Key, r.Value, "00000000000000ff"));
        public byte[]? Read(ResourceKey key, PartKind part) => resources.TryGetValue(key, out var p) ? Parts.Of(p, part) : null;
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

        test("The same piece at any byte offset is found", () =>
        {
            for (var shift = 0; shift < 20; shift++)
            {
                var file = Concat(Noise(10 + shift, shift), skinGpu[50_000..60_000], Noise(40 + shift, 7));
                var (_, _, report) = new Migrator(Index(), [oldBuild, newBuild]).Migrate([("x", file)]);
                check(report.Files[0].FromGame == 10_000, $"shift {shift}: {report.Files[0].FromGame}");
            }
        });
    }
}
