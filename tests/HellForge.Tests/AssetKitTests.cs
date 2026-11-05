using System.Security.Cryptography;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

// The asset kit (docs/modding/assets.md): an author strips Arrowhead's bytes out of a mod's game patch files, and the
// player's HellForge rebuilds them into the mod's folder from its own game before the mod loads; friends, mod sync and
// packages count the recipe, never the rebuilt files. Every game here is synthetic.
static class AssetKitTests
{
    sealed class FakeGame(string build, Dictionary<ResourceKey, ResourceParts> resources) : IGameResources
    {
        public string Build => build;
        public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All() => resources.Select(r => (r.Key, "00000000000000ff", (Func<ResourceParts>)(() => r.Value)));
        public byte[]? Read(ResourceKey key, PartKind part) => resources.TryGetValue(key, out var p) ? Parts.Of(p, part) : null;
    }

    static byte[] Noise(int seed, int length)
    {
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }

    static string Sha(string path) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var unit = new ResourceKey(ResourceTypes.Unit, 0xA1A1);
        var texture = new ResourceKey(ResourceTypes.Texture, 0xB2B2);
        var game = new FakeGame("553853:42", new()
        {
            [unit] = new(Noise(11, 3000), Noise(12, 150_000), []),
            [texture] = new(Noise(13, 500), Noise(14, 220_000), Noise(15, 40_000)),
        });
        var index = new SourceIndex();
        index.Add(game);
        index.Seal();
        IReadOnlyList<IGameResources> games = [game];
        var profile = EntityProfile.Current;

        // An author's mod: a game patch holding the unit as the game has it, the texture with a region repainted, and a
        // material of the author's own; a README beside it.
        string Author(string name)
        {
            var folder = Path.Combine(root, "asset-kit", name);
            if (Directory.Exists(folder)) Directory.Delete(folder, true);
            Directory.CreateDirectory(Path.Combine(folder, "data"));
            var repainted = Parts.Of(game.Read(texture, PartKind.Gpu) is { } gpu ? new([], gpu, []) : throw new Exception(), PartKind.Gpu).ToArray();
            Noise(16, 3000).CopyTo(repainted, 100_000);
            var (archive, archiveGpu, archiveStream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [unit] = new(game.Read(unit, PartKind.Main)!, game.Read(unit, PartKind.Gpu)!, []),
                [texture] = new(game.Read(texture, PartKind.Main)!, repainted, game.Read(texture, PartKind.Stream)!),
                [new(ResourceTypes.Material, 0xC3C3)] = new(Noise(17, 900), [], []),
            });
            File.WriteAllBytes(Path.Combine(folder, "data", "0123456789abcdef.patch_0"), archive);
            File.WriteAllBytes(Path.Combine(folder, "data", "0123456789abcdef.patch_0.gpu_resources"), archiveGpu);
            File.WriteAllBytes(Path.Combine(folder, "data", "0123456789abcdef.patch_0.stream"), archiveStream);
            File.WriteAllText(Path.Combine(folder, "hellforge-mod.json"), $$"""{"format":1,"id":"test.{{name}}","name":"Kit {{name}}","version":"1.0.0","gameplay":false}""");
            File.WriteAllText(Path.Combine(folder, "README.txt"), "Made from the game's unit, with my own material.");
            return folder;
        }
        string Out(string name) => Path.Combine(root, "asset-kit-out", name);

        test("Stripping a mod writes it without the game's bytes, and the recipe it ships rebuilds it from the game", () =>
        {
            var mod = Author("strip");
            var result = AssetKit.Strip(mod, index, games, Out("strip"));
            check(result.Outcome == AssetKit.Outcome.Stripped, result.Summary);
            check(result.FromGame > 360_000, $"from the game: {result.FromGame:N0}");
            var shipped = result.ShippedFolder!;
            check(File.Exists(Path.Combine(shipped, Recipe.FileName)) && File.Exists(Path.Combine(shipped, Recipe.LiteralsName)), "the recipe ships");
            check(File.Exists(Path.Combine(shipped, "README.txt")) && File.Exists(Path.Combine(shipped, "hellforge-mod.json")), "the author's files ship");
            check(!Directory.Exists(Path.Combine(shipped, "data")) || Directory.GetFiles(Path.Combine(shipped, "data")).Length == 0, "no patch file ships");
            var size = Directory.GetFiles(shipped, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length);
            check(size < 40_000, $"shared mod is {size:N0} bytes");
            check(Directory.GetFiles(Path.Combine(mod, "data")).Length == 3, "the author's folder is not changed");
            check(result.ReportText.Contains("STRIPPED") && result.ReportText.Contains("553853:42"), "the report says what happened");
        });

        test("A stripped mod is pending until its game files are rebuilt, then loads with the author's exact files", () =>
        {
            var mod = Author("player");
            var shipped = AssetKit.Strip(mod, index, games, Out("player")).ShippedFolder!;
            var pending = ModCatalog.Parse(shipped, profile);
            check(pending.AssetsPending != null, "pending before the rebuild");
            check(pending.DataFiles.SequenceEqual(["0123456789abcdef.patch_0", "0123456789abcdef.patch_0.gpu_resources", "0123456789abcdef.patch_0.stream"]),
                $"its data files are known from the recipe: {string.Join(", ", pending.DataFiles)}");
            var hashBefore = ModSets.ContentHash(pending);
            check(ModAssets.Ensure(shipped, () => games), "rebuilt");
            foreach (var file in Directory.GetFiles(Path.Combine(mod, "data")))
                check(Sha(file) == Sha(Path.Combine(shipped, "data", Path.GetFileName(file))), $"{Path.GetFileName(file)} is the author's file, byte for byte");
            var ready = ModCatalog.Parse(shipped, profile);
            check(ready.AssetsPending == null, "ready after the rebuild");
            check(ModSets.ContentHash(ready) == hashBefore, "friends count the same mod before and after the rebuild");
            check(ModSets.Files(ready).All(f => !f.Path.StartsWith("data/", StringComparison.Ordinal)), "the rebuilt files are not counted or sent");
            check(ModSets.Files(ready).Any(f => f.Path == Recipe.FileName) && ModSets.Files(ready).Any(f => f.Path == Recipe.LiteralsName), "the recipe is");
            check(ModPackage.Files(ready).All(f => !f.StartsWith("data/", StringComparison.Ordinal) && !f.StartsWith('.')), "a package never takes the rebuilt files");
            check(!ModAssets.Ensure(shipped, () => throw new Exception("the game is not opened again")), "nothing to do while the files are as rebuilt");
        });

        test("A rebuilt file that changed is rebuilt again; a file the mod ships itself is never overwritten", () =>
        {
            var shipped = AssetKit.Strip(Author("changed"), index, games, Out("changed")).ShippedFolder!;
            ModAssets.Ensure(shipped, () => games);
            var gpu = Path.Combine(shipped, "data", "0123456789abcdef.patch_0.gpu_resources");
            var good = Sha(gpu);
            File.WriteAllBytes(gpu, Noise(99, 1000));
            check(ModCatalog.Parse(shipped, profile).AssetsPending != null, "pending again after an edit");
            check(ModAssets.Ensure(shipped, () => games) && Sha(gpu) == good, "rebuilt to the recipe's bytes");

            var other = AssetKit.Strip(Author("ships"), index, games, Out("ships")).ShippedFolder!;
            Directory.CreateDirectory(Path.Combine(other, "data"));
            File.WriteAllBytes(Path.Combine(other, "data", "0123456789abcdef.patch_0.gpu_resources"), Noise(98, 100));
            reject(() => ModAssets.Ensure(other, () => games));

            // a copy of a rebuilt mod without its record (copied without dot files): its files are already the recipe's
            File.Delete(Path.Combine(shipped, ModAssets.RecordName));
            check(ModCatalog.Parse(shipped, profile).AssetsPending != null, "pending without the record");
            check(ModAssets.Ensure(shipped, () => games) && Sha(gpu) == good, "files that are the recipe's own are taken back");
            check(ModCatalog.Parse(shipped, profile).AssetsPending == null, "ready again");
        });

        test("A recipe from another PC only rebuilds game patch files inside the mod", () =>
        {
            foreach (var path in new[] { "mod.dll", "hellforge-mod.json", "../0123456789abcdef.patch_0", "data/../../x/0123456789abcdef.patch_0", "/abs/0123456789abcdef.patch_0", "data/.hidden.patch_0" })
            {
                var folder = Path.Combine(root, "asset-kit-bad", Convert.ToHexStringLower(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(path)))[..8]);
                Directory.CreateDirectory(folder);
                new Recipe { Files = [new RecipeFile { Path = path, Size = 1, Sha256 = new string('a', 64), Ops = [[Recipe.Fill, 0, 1]] }] }.Save(folder, []);
                reject(() => ModAssets.Read(folder));
            }
        });

        test("A recipe that needs a build this PC lacks changes nothing and says which build", () =>
        {
            var shipped = AssetKit.Strip(Author("missing"), index, games, Out("missing")).ShippedFolder!;
            var other = new FakeGame("553853:7", new() { [unit] = new(Noise(1, 10), Noise(2, 10), []) });
            try
            {
                ModAssets.Ensure(shipped, () => [other]);
                check(false, "rebuilt without the build");
            }
            catch (ModAssets.MissingBuildsException missing)
            {
                check(missing.Missing.Single().Build == "553853:42" && missing.Missing[0].Bytes > 360_000, missing.Message);
            }
            check(!Directory.Exists(Path.Combine(shipped, "data")) || Directory.GetFiles(Path.Combine(shipped, "data")).Length == 0, "no file written");
            check(!File.Exists(Path.Combine(shipped, ModAssets.RecordName)), "no record written");
            var problems = ModAssets.Prepare([shipped], () => []);
            check(problems.Count == 1 && problems[0].Problem.Contains("553853:42"), "the session's preparation reports it");
        });

        test("A mod with nothing from the game ships as it is; a stripped copy is not stripped again", () =>
        {
            var folder = Path.Combine(root, "asset-kit", "own");
            Directory.CreateDirectory(Path.Combine(folder, "data"));
            var (archive, _, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(ResourceTypes.Material, 0xD4D4)] = new(Noise(21, 5000), [], []) });
            File.WriteAllBytes(Path.Combine(folder, "data", "fedcba9876543210.patch_0"), archive);
            var result = AssetKit.Strip(folder, index, games, null);
            check(result.Outcome == AssetKit.Outcome.NoGameContent && result.Recipe.Files.Count == 0, result.Summary);
            check(result.Unknown.Count > 0 && result.Summary.Contains("none of the builds checked"), "it says what it could not place");
            var shipped = AssetKit.Strip(Author("twice"), index, games, Out("twice")).ShippedFolder!;
            reject(() => AssetKit.Strip(shipped, index, games, Out("twice-again")));
            reject(() => AssetKit.Strip(Author("inside"), index, games, Path.Combine(root, "asset-kit", "inside", "out")));
        });
    }
}
