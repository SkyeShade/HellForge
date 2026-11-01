using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// The asset migrator (HellForge.Core.AssetMigration): a mod built from the game's resources becomes a recipe plus the
// author's own bytes, and the recipe rebuilds the mod byte for byte from the game. Every "game" here is synthetic.
static class AssetMigrationTests
{
    sealed class FakeGame(string build, Dictionary<ResourceKey, ResourceParts> resources) : IGameResources
    {
        public string Build => build;
        public IEnumerable<(ResourceKey Key, ResourceParts Parts)> All() => resources.Select(r => (r.Key, r.Value));
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
