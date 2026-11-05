using System.Security.Cryptography;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;
using HellForge.SteamDownload;

// A recipe mod that needs a build this PC lacks (cut content from an older build): that build is read from Steam's
// depot one chunk at a time (DepotDataFiles, ChunkStore), so only the chunks holding the parts the recipe needs are
// fetched, each checked by SHA-1. Steam here is a fake that serves a synthetic old build cut into small chunks.
static class AssetFetchTests
{
    // A depot of one manifest: the given data files, cut into chunks of chunkSize, served by SHA-1.
    sealed class FakeDepot : IDepotService
    {
        public readonly Dictionary<string, byte[]> Chunks = new(StringComparer.OrdinalIgnoreCase);
        public DepotManifest Manifest = null!;
        public int Served;
        public readonly List<string> Branches = [];

        public static FakeDepot Of(uint depot, ulong manifest, string dataFolder, int chunkSize)
        {
            var fake = new FakeDepot();
            var files = new List<DepotFile>();
            foreach (var path in Directory.GetFiles(dataFolder).Order(StringComparer.Ordinal))
            {
                var bytes = File.ReadAllBytes(path);
                var chunks = new List<DepotChunk>();
                for (var at = 0; at < bytes.Length; at += chunkSize)
                {
                    var piece = bytes.AsSpan(at, Math.Min(chunkSize, bytes.Length - at)).ToArray();
                    var sha = Convert.ToHexString(SHA1.HashData(piece));
                    fake.Chunks[sha] = piece;
                    chunks.Add(new(sha, SteamChunkIndex.Adler32(piece), (ulong)at, (uint)piece.Length, (uint)piece.Length));
                }
                files.Add(new("data/" + Path.GetFileName(path), (ulong)bytes.Length, 0, Convert.ToHexString(SHA1.HashData(bytes)), chunks));
            }
            fake.Manifest = new(depot, manifest, false, files);
            return fake;
        }

        public Task<byte[]> DepotKeyAsync(uint app, uint depot, CancellationToken cancellation) => Task.FromResult(new byte[] { 7 });
        public Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, CancellationToken cancellation) => Task.FromResult(99UL);
        public Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, string branch, CancellationToken cancellation)
        {
            Branches.Add(branch);
            return Task.FromResult(99UL);
        }
        public Task<DepotManifest> ManifestAsync(uint app, uint depot, ulong manifest, ulong requestCode, byte[] depotKey, CancellationToken cancellation) =>
            manifest == Manifest.ManifestId && requestCode == 99 ? Task.FromResult(Manifest) : throw new HttpRequestException("404");
        public Task<int> ChunkAsync(uint app, uint depot, DepotChunk chunk, byte[] depotKey, byte[] buffer, CancellationToken cancellation)
        {
            Served++;
            var bytes = Chunks[chunk.Sha1];
            bytes.CopyTo(buffer, 0);
            return Task.FromResult(bytes.Length);
        }
    }

    static byte[] Noise(int seed, int length)
    {
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A recipe's part from a build this PC lacks is read from Steam's depot, only the chunks that hold it", () =>
        {
            // The old build: a loose archive of the cut vehicle and 40 other resources, cut into 16 KB chunks.
            var oldData = Path.Combine(root, "fetch", "old-build", "data");
            Directory.CreateDirectory(oldData);
            var vehicle = new ResourceKey(ResourceTypes.Unit, 0xC0C0);
            var resources = new Dictionary<ResourceKey, ResourceParts> { [vehicle] = new(Noise(1, 2000), Noise(2, 60_000), []) };
            for (var i = 0; i < 40; i++) resources[new(ResourceTypes.Texture, 0x1000 + (ulong)i)] = new(Noise(10 + i, 400), Noise(100 + i, 50_000), []);
            var (archive, gpu, stream) = ResourceArchive.Write(resources);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000cc"), archive);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000cc.gpu_resources"), gpu);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000cc.stream"), stream);
            var oldLabel = GameBuilds.Label(553853, "1234");
            var oldGame = new LooseGameResources(oldData, oldLabel);

            // The author's mod: the cut vehicle as the old build has it, with their own material; stripped with the old build.
            var mod = Path.Combine(root, "fetch", "mod");
            Directory.CreateDirectory(Path.Combine(mod, "data"));
            var (modArchive, modGpu, modStream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [vehicle] = resources[vehicle],
                [new(ResourceTypes.Material, 0xABAB)] = new(Noise(3, 700), [], []),
            });
            File.WriteAllBytes(Path.Combine(mod, "data", "1111222233334444.patch_0"), modArchive);
            File.WriteAllBytes(Path.Combine(mod, "data", "1111222233334444.patch_0.gpu_resources"), modGpu);
            File.WriteAllBytes(Path.Combine(mod, "data", "1111222233334444.patch_0.stream"), modStream);
            File.WriteAllText(Path.Combine(mod, "hellforge-mod.json"), """{"format":1,"id":"test.cut-vehicle","name":"Cut Vehicle","version":"1.0.0","gameplay":false}""");
            var index = new SourceIndex();
            index.Add(oldGame);
            index.Seal();
            var shipped = AssetKit.Strip(mod, index, [oldGame], Path.Combine(root, "fetch", "shipped")).ShippedFolder!;

            // The player has no build at all here: the rebuild needs the old one, from Steam.
            var problems = ModAssets.Prepare([shipped], () => []);
            check(problems is [{ Missing: [{ Build: var needed }] }] && needed == oldLabel, "the player is told which build is needed");

            var depot = FakeDepot.Of(553853, 1234, oldData, 16 * 1024);
            var store = new ChunkStore(depot, 553850, Path.Combine(root, "fetch", "cache"));
            var steam = new SteamBuilds(store, null);
            var remote = steam.Open(oldLabel);
            check(ModAssets.Ensure(shipped, () => [remote]), "rebuilt from the build read from Steam");
            foreach (var file in Directory.GetFiles(Path.Combine(mod, "data")))
                check(SHA256.HashData(File.ReadAllBytes(file)).SequenceEqual(SHA256.HashData(File.ReadAllBytes(Path.Combine(shipped, "data", Path.GetFileName(file))))),
                    $"{Path.GetFileName(file)} is the author's file");
            var total = depot.Chunks.Count;
            check(depot.Served < total / 3, $"only the chunks that hold the vehicle and the table were fetched: {depot.Served} of {total}");
            check(store.Downloaded > 0 && store.Downloaded < (archive.Length + gpu.Length) / 3, $"downloaded {store.Downloaded:N0} bytes");

            // Again from the cache: nothing is downloaded twice (a rebuilt file lost, so the mod is rebuilt).
            var served = depot.Served;
            File.Delete(Path.Combine(shipped, "data", "1111222233334444.patch_0.gpu_resources"));
            var again = new SteamBuilds(new ChunkStore(depot, 553850, Path.Combine(root, "fetch", "cache")), null);
            check(ModAssets.Ensure(shipped, () => [again.Open(oldLabel)]) && depot.Served == served, "the second rebuild reads the cached chunks and manifest");
        });

        test("Cut content only an old build has is proven by Steam's chunks, fingerprinted from that build alone, and goes into the recipe", () =>
        {
            // The old build: the cut vehicle (3 MB of mesh data, more than a chunk) among other resources, in 1 MiB chunks
            // as Steam cuts them. The build on this PC has none of it.
            var oldData = Path.Combine(root, "history", "old", "data");
            Directory.CreateDirectory(oldData);
            var vehicle = new ResourceKey(ResourceTypes.Unit, 0xCAFE);
            var resources = new Dictionary<ResourceKey, ResourceParts> { [vehicle] = new(Noise(30, 5000), Noise(31, 3_000_000), []) };
            for (var i = 0; i < 6; i++) resources[new(ResourceTypes.Texture, 0x2000 + (ulong)i)] = new(Noise(40 + i, 300), Noise(50 + i, 900_000), []);
            var (archive, gpu, stream) = ResourceArchive.Write(resources);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000ab"), archive);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000ab.gpu_resources"), gpu);
            File.WriteAllBytes(Path.Combine(oldData, "00000000000000ab.stream"), stream);
            var depot = FakeDepot.Of(553853, 4321, oldData, SteamChunkIndex.Window);
            var oldLabel = GameBuilds.Label(553853, "4321");
            var newBuild = new LooseGameResources(Path.Combine(root, "history", "empty"), GameBuilds.Label(553853, "9999"));
            Directory.CreateDirectory(Path.Combine(root, "history", "empty"));

            var mod = Path.Combine(root, "history", "mod");
            Directory.CreateDirectory(Path.Combine(mod, "data"));
            var (modArchive, modGpu, modStream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [vehicle] = resources[vehicle],
                [new(ResourceTypes.Material, 0xBEEF)] = new(Noise(32, 800), [], []),
            });
            File.WriteAllBytes(Path.Combine(mod, "data", "5555666677778888.patch_0"), modArchive);
            File.WriteAllBytes(Path.Combine(mod, "data", "5555666677778888.patch_0.gpu_resources"), modGpu);
            File.WriteAllBytes(Path.Combine(mod, "data", "5555666677778888.patch_0.stream"), modStream);
            var index = new SourceIndex();
            index.Add(newBuild);
            index.Seal();
            var first = AssetKit.Strip(mod, index, [newBuild], null);
            check(first.Outcome == AssetKit.Outcome.NoGameContent && first.Unknown.Count > 0, "the build on this PC does not have the vehicle");

            var steam = new SteamBuilds(new ChunkStore(depot, 553850, Path.Combine(root, "history", "cache")), null);
            var chunks = new SteamChunkIndex();
            chunks.AddBuild(oldLabel, new DateTime(2024, 3, 1), depot.Manifest.Files.Select(f => (f.Path,
                f.Chunks.Select(c => ((long)c.Offset, (int)c.OriginalSize, Convert.FromHexString(c.Sha1), c.Crc)))));
            var past = AssetHistory.Resolve(mod, index, first.Report, chunks, steam);
            check(past.Count == 1 && past[0].Build == oldLabel && index.PartialBuilds.Contains(oldLabel), "the old build is planned and fingerprinted in part");
            var served = depot.Served;
            check(served < depot.Chunks.Count, $"only part of the old build was read: {served} of {depot.Chunks.Count} chunks");

            var second = AssetKit.Strip(mod, index, [newBuild, .. past], Path.Combine(root, "history", "shipped"));
            check(second.Outcome == AssetKit.Outcome.Stripped && second.FromGame >= 3_000_000, second.Summary);
            check(second.Recipe.Sources.Any(src => src.Places.Any(p => p.Build == oldLabel)), "the recipe takes the vehicle from the old build");
            check(ModAssets.Ensure(second.ShippedFolder!, () => [newBuild, steam.Open(oldLabel)]), "a player rebuilds it from that build on Steam");
            check(SHA256.HashData(File.ReadAllBytes(Path.Combine(mod, "data", "5555666677778888.patch_0.gpu_resources")))
                .SequenceEqual(SHA256.HashData(File.ReadAllBytes(Path.Combine(second.ShippedFolder!, "data", "5555666677778888.patch_0.gpu_resources")))), "byte for byte");
        });

        test("A chunk Steam sends that is not the one asked for is refused", () =>
        {
            var data = Path.Combine(root, "fetch-bad", "data");
            Directory.CreateDirectory(data);
            File.WriteAllBytes(Path.Combine(data, "00000000000000dd"), Noise(5, 40_000));
            var depot = FakeDepot.Of(553853, 77, data, 16 * 1024);
            foreach (var key in depot.Chunks.Keys.ToList()) depot.Chunks[key] = Noise(6, depot.Chunks[key].Length);
            var files = new DepotDataFiles(new ChunkStore(depot, 553850, Path.Combine(root, "fetch-bad", "cache")), depot.Manifest);
            check(files.Exists("00000000000000dd") && files.List().Count() == 1, "the depot's data files are listed");
            reject(() => { using var s = files.Open("00000000000000dd"); s.ReadExactly(new byte[100]); });
        });

        test("A past manifest is asked for on the branch the depot history names", () =>
        {
            var history = DepotHistory.Embedded(553853);
            check(history.Count > 100, $"the depot history ships: {history.Count} manifests");
            var legacy = history.First(h => h.Branch != null);
            var data = Path.Combine(root, "fetch-branch", "data");
            Directory.CreateDirectory(data);
            File.WriteAllBytes(Path.Combine(data, "00000000000000ee"), Noise(7, 100));
            var depot = FakeDepot.Of(553853, legacy.Manifest, data, 1024);
            new SteamBuilds(new ChunkStore(depot, 553850, Path.Combine(root, "fetch-branch", "cache")), null).Manifest(GameBuilds.Label(553853, legacy.Manifest.ToString()));
            check(depot.Branches.SequenceEqual([legacy.Branch!]), $"asked on {string.Join(",", depot.Branches)}, not {legacy.Branch}");
        });
    }
}
