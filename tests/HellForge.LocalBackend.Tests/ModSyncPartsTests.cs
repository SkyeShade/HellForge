using System.Security.Cryptography;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;
using HellForge.LocalBackend.Multiplayer;

// Mod sync from the joiner's own game (docs/multiplayer.md "Mod sync from your own game"): the host lists the resource
// parts of a mod's game patch files, the joiner writes the ones its game has (the same key; any key the cached game index
// knows those bytes under) and downloads the rest. The mod, the game and the link are synthetic; a tap on the joiner's
// link counts what came over it.
static partial class ModSyncTests
{
    sealed class FakeGame(string build, Dictionary<ResourceKey, ResourceParts> resources) : IGameResources
    {
        public string Build => build;
        public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All() => resources.Select(r => (r.Key, "00000000000000ff", (Func<ResourceParts>)(() => r.Value)));
        public byte[]? Read(ResourceKey key, PartKind part) => resources.TryGetValue(key, out var p) ? Parts.Of(p, part) : null;
    }

    // A joiner's link that counts the bytes of mods.chunk frames it receives, and can drop the host's parts flag from
    // offers (a host of an earlier HellForge).
    sealed class Tap(IPeerTransport inner) : IPeerTransport
    {
        public long ChunkBytes;
        public int PartRequests;
        public bool OldHost;
        // rewrites a part list page on its way in (a host whose list does not match its files)
        public Func<Frame, Frame>? PartList;
        public ulong LocalId => inner.LocalId;
        public event Action<ulong, byte[]>? Received;
        public void Start() => inner.Start();
        public void Dispose() => inner.Dispose();
        public bool IsFriend(ulong peer) => inner.IsFriend(peer);
        public void Send(ulong peer, byte[] frame, bool reliable)
        {
            if (Frame.Decode(frame).Type == "mods.parts") Interlocked.Increment(ref PartRequests);
            inner.Send(peer, frame, reliable);
        }
        public void Attach() => inner.Received += (peer, bytes) =>
        {
            var frame = Frame.Decode(bytes);
            if (frame.Type == "mods.chunk" && frame.Text(1) == "ok") Interlocked.Add(ref ChunkBytes, frame.Bytes(3).Length);
            if (frame.Type == "mods.partlist" && PartList != null) bytes = PartList(frame).Encode();
            if (frame.Type == "mods.offer" && OldHost && frame.Count > 3)
            {
                var old = new Frame("mods.offer");
                old.Fields.AddRange(frame.Fields.Take(3));
                bytes = old.Encode();
            }
            Received?.Invoke(peer, bytes);
        };
    }

    public static async Task FromOwnGame(string root, Action<bool, string> check)
    {
        root = Path.Combine(root, "mod-sync-parts");
        if (Directory.Exists(root)) Directory.Delete(root, true);
        static byte[] Noise(int seed, int length)
        {
            var bytes = new byte[length];
            new Random(seed).NextBytes(bytes);
            return bytes;
        }
        // The game: a unit, a texture and 20 other resources. The host's mod overrides the unit (the game's own bytes, same
        // key), carries the texture under a name of its own, and adds a material of the author's.
        var unit = new ResourceKey(ResourceTypes.Unit, 0xA1A1);
        var texture = new ResourceKey(ResourceTypes.Texture, 0xB2B2);
        var resources = new Dictionary<ResourceKey, ResourceParts>
        {
            [unit] = new(Noise(1, 4000), Noise(2, 900_000), []),
            [texture] = new(Noise(3, 600), Noise(4, 1_200_000), Noise(5, 300_000)),
        };
        for (var i = 0; i < 20; i++) resources[new(ResourceTypes.Texture, 0x1000 + (ulong)i)] = new(Noise(10 + i, 300), Noise(40 + i, 20_000), []);
        var game = new FakeGame("553853:42", resources);
        var own = new ResourceKey(ResourceTypes.Material, 0xC3C3);
        var hostMods = Path.Combine(root, "host", "mods");
        var modFolder = Path.Combine(hostMods, "big");
        Directory.CreateDirectory(Path.Combine(modFolder, "data"));
        File.WriteAllText(Path.Combine(modFolder, ModCatalog.ManifestName), """{"format":1,"id":"test.big","name":"Big","version":"1"}""");
        var (archive, gpu, stream) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
        {
            [unit] = resources[unit],
            [new(ResourceTypes.Texture, 0xD4D4)] = resources[texture],
            [own] = new(Noise(6, 50_000), [], []),
        });
        File.WriteAllBytes(Path.Combine(modFolder, "data", "0123456789abcdef.patch_0"), archive);
        File.WriteAllBytes(Path.Combine(modFolder, "data", "0123456789abcdef.patch_0.gpu_resources"), gpu);
        File.WriteAllBytes(Path.Combine(modFolder, "data", "0123456789abcdef.patch_0.stream"), stream);
        var hostSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(hostMods).Mods));
        var mod = hostSet.Mods.Single();
        var gameBytes = resources[unit].Main.Length + resources[unit].Gpu.Length + resources[texture].Main.Length + resources[texture].Gpu.Length + resources[texture].Stream.Length;

        var listed = SyncParts.Of(modFolder, [.. mod.Files.Select(f => f.Path)]);
        check(listed.Count == 6 && listed.All(p => p.File.StartsWith("data/", StringComparison.Ordinal)), $"The host lists the archive's 6 parts: {listed.Count}.");
        var sizes = mod.Files.ToDictionary(f => f.Path, f => f.Size, StringComparer.Ordinal);
        check(SyncParts.Check(listed, sizes) == null, "The host's own list checks out.");
        check(SyncParts.Check([listed[0], listed[0] with { Start = listed[0].Start + 1 }], sizes) != null, "Overlapping parts are refused.");
        check(SyncParts.Check([listed[0] with { Length = sizes[listed[0].File] }], sizes) != null, "A part past its file's end is refused.");
        check(SyncParts.Check([listed[0] with { File = "data/other.patch_0" }], sizes) != null, "A part of a file not listed is refused.");

        var index = new SourceIndex();
        index.Add(game);
        index.Seal();
        // the same key only (no index); with the index, the renamed texture too; an older host: nothing from the game
        var withoutIndex = await PartsDownload(root, "no-index", hostSet, [game], null, check);
        var withIndex = await PartsDownload(root, "index", hostSet, [game], index, check);
        var older = await PartsDownload(root, "old-host", hostSet, [game], index, check, tap => tap.OldHost = true);
        // A list that puts the game's unit where the mod has its texture: the file it made does not check out, and it
        // downloads whole.
        var unitGpu = Convert.ToHexString(SHA256.HashData(resources[unit].Gpu));
        var lying = await PartsDownload(root, "lying-host", hostSet, [game], index, check, tap => tap.PartList = page =>
        {
            for (var at = 4; at + 6 < page.Count; at += 7)
                if (page.Text(at).EndsWith(".gpu_resources", StringComparison.Ordinal) && page.Text(at + 4) == "000000000000d4d4")
                {
                    page.Fields[at + 2] = System.Text.Encoding.UTF8.GetBytes(resources[unit].Gpu.Length.ToString());
                    page.Fields[at + 6] = System.Text.Encoding.UTF8.GetBytes(unitGpu);
                }
            return page;
        });
        check(lying.Log.Any(l => l.Contains("did not check out")), "The file made from a list that does not match is downloaded again whole.");

        // A recipe mod (the asset kit): the host's copy rebuilt and ready. A joiner with the whole game downloads the recipe
        // alone; one whose game lacks the texture (after a game update) makes the rest and downloads that from the host's
        // rebuilt files; from an older host it is told which build it lacks.
        var authored = Path.Combine(root, "author", "big");
        CopyFolder(modFolder, authored);
        var kitMods = Path.Combine(root, "kit-host", "mods");
        var stripped = AssetKit.Strip(authored, index, [game], Path.Combine(kitMods, "big"));
        check(stripped.Outcome == AssetKit.Outcome.Stripped, stripped.Summary);
        ModAssets.Ensure(Path.Combine(kitMods, "big"), () => [game]);
        var kitSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(kitMods).Mods));
        var kit = kitSet.Mods.Single();
        var lacking = new FakeGame("553853:43", resources.Where(r => r.Key != texture).ToDictionary());
        var whole = await PartsDownload(root, "kit-whole", kitSet, [game], null, check);
        var after = await PartsDownload(root, "kit-update", kitSet, [lacking], null, check);
        var texBytes = resources[texture].Main.Length + resources[texture].Gpu.Length + resources[texture].Stream.Length;
        Console.WriteLine($"mod sync of a recipe mod ({kit.Bytes:N0} bytes counted): {whole.ChunkBytes:N0} bytes over the link with the whole game, {after.ChunkBytes:N0} when the game lacks a texture");
        check(whole.ChunkBytes == kit.Bytes, $"With the whole game only the recipe comes over the link ({whole.ChunkBytes:N0} of {kit.Bytes:N0}).");
        // the texture's small main part is in the recipe's literals; its GPU and stream parts come from the host's files
        var texLarge = resources[texture].Gpu.Length + resources[texture].Stream.Length;
        check(after.ChunkBytes >= kit.Bytes + texLarge && after.ChunkBytes <= kit.Bytes + texBytes,
            $"A game without the texture downloads just that from the host's rebuilt files ({after.ChunkBytes - kit.Bytes:N0} bytes; the texture is {texBytes:N0}).");
        var oldKit = await PartsDownload(root, "kit-old-host", kitSet, [lacking], null, check, tap => tap.OldHost = true, expect: "failed");
        check(oldKit.Message.Contains("553853:42"), "From an older host, the joiner is told which build it lacks: " + oldKit.Message);
        var unitBytes = resources[unit].Main.Length + resources[unit].Gpu.Length;
        Console.WriteLine($"mod sync from the joiner's game, a {mod.Bytes:N0}-byte mod: {withoutIndex.ChunkBytes:N0} bytes over the link without the game index, " +
            $"{withIndex.ChunkBytes:N0} with it, {older.ChunkBytes:N0} from an older host");
        check(withoutIndex.ChunkBytes <= mod.Bytes - unitBytes, $"Without the index the unit (same key) came from the game: {withoutIndex.ChunkBytes:N0} of {mod.Bytes:N0} bytes downloaded.");
        check(withoutIndex.ChunkBytes > mod.Bytes - gameBytes + 100_000, "... and the renamed texture did not.");
        check(withIndex.ChunkBytes <= mod.Bytes - gameBytes, $"With the index the renamed texture came from the game too: {withIndex.ChunkBytes:N0} bytes downloaded.");
        check(older.ChunkBytes == mod.Bytes && older.PartRequests == 0, $"An older host's offer asks for no part list, and the mod downloads whole ({older.ChunkBytes:N0}).");
    }

    static void CopyFolder(string from, string to)
    {
        foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
        {
            var target = Path.Combine(to, Path.GetRelativePath(from, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target, overwrite: true);
        }
    }

    static async Task<(long ChunkBytes, int PartRequests, List<string> Log, string Message)> PartsDownload(string root, string name, ModSet hostSet,
        IReadOnlyList<IGameResources> games, SourceIndex? index, Action<bool, string> check, Action<Tap>? setUp = null, string expect = "ready")
    {
        var network = new InMemoryNetwork();
        var tap = new Tap(network.Join(GuestId));
        setUp?.Invoke(tap);
        var host = new Node(network, HostId, Path.Combine(root, name, "host"), hostSet, (_, _) => true);
        var guest = new Node(tap, Path.Combine(root, name, "guest"), new ModSet("none", []), (_, _) => true);
        tap.Attach();
        guest.Sync.LocalGames = () => games;
        guest.Sync.LocalIndex = () => index;
        var offer = Refused(host, guest, check, name, "offer");
        guest.Sync.Answer(["syncanswer", offer.Version.ToString(), "accept"]);
        var ready = guest.Shown.Next(check, "ready", "failed");
        var mod = hostSet.Mods.Single();
        check(ready.State == expect, $"{name}: {ready.State} {ready.Message} | {string.Join(" / ", guest.Log.TakeLast(4))}");
        var stored = SyncedStore.Find(guest.Mods, mod.Id, mod.Hash);
        if (expect == "ready")
        {
            check(stored != null && mod.Files.All(f => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(stored, f.Path)))) == f.Sha256),
                $"{name}: every file stored is the host's, byte for byte.");
            check(!ModAssets.HasRecipe(stored!) || ModAssets.Read(stored!).Ready, $"{name}: a recipe mod is stored rebuilt and ready.");
        }
        await guest.Sync.DisposeAsync();
        await host.Sync.DisposeAsync();
        lock (guest.Log) return (tap.ChunkBytes, tap.PartRequests, [.. guest.Log], ready.Message);
    }
}
