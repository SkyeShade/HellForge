namespace HellForge.Core.GameData;

// Mods' warbond art ("art": {"image": ...}, ModWarbonds): each PNG as a texture of the game's warbond art form (2048 x 1024
// BC1, one mip: a game card's own header, which does not hold its name, and the image's pixels) under a name of its own,
// in a patch of the archive the game's warbond art is in, so it loads when the game's does, and a plain card for warbonds
// without "art". The mod loader points the warbond's entry in the game's table at it. Built from the installation's game
// copy and kept until the images change.
public static class ModWarbondArt
{
    // Part of the cache key: raise it whenever the same images would build different bytes.
    public const int Version = 1;
    // The archive of every warbond card (a package the game loads with the ship), and the Steeled Veterans card, whose
    // header the textures share.
    public const string Archive = "4b7bd0cbf5b59fcf";
    const ulong Template = 0xDA2176742E3F1FB2;
    // The card of a warbond with no "art": one texture HellForge draws (DefaultImage), shared by them all.
    public static readonly ulong DefaultTexture = System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(
        System.Security.Cryptography.SHA256.HashData("hellforge/warbond-art/default"u8));

    static bool Plain(ModWarbonds.Warbond w) => w.ArtImage.Length == 0 && w.ArtFrom == 0;

    // Helldivers Mobilize's card (it has no entry in mappings/warbond-art.json: everyone owns it).
    const ulong MobilizeCard = 0x4BCA007A5214D66F;

    // The card of the game warbond an "art": {"from": ...} names, which the warbond gets a copy of under its own name: the
    // game's own texture, which HellForge's lock (WarbondLocks) does not draw on, so a mod warbond never shows another
    // warbond's padlock. 0 for an id with no card.
    static ulong FromCard(uint id) => id == PremiumContent.FreeWarbond ? MobilizeCard : WarbondLocks.Mapping().FirstOrDefault(w => w.Id == id)?.Textures[0] ?? 0;

    // The warbonds with their texture set where it was built, the patch folder (null when no warbond needs a texture) and a
    // problem for each image that could not be used (its warbond keeps the game's placeholder).
    public static (List<ModWarbonds.Warbond> Warbonds, string? Folder, List<string> Problems) Prepare(InstallLayout install, IReadOnlyList<ModWarbonds.Warbond> warbonds)
    {
        var drawn = warbonds.Where(w => w.ArtImage.Length > 0).ToList();
        var plain = warbonds.Any(Plain);
        var copies = warbonds.Where(w => w.ArtImage.Length == 0 && FromCard(w.ArtFrom) != 0)
            .Select(w => (Name: ModWarbonds.ArtTextureName(w.Mod, w.Key), Source: FromCard(w.ArtFrom))).ToList();
        if (drawn.Count == 0 && !plain && copies.Count == 0) return ([.. warbonds], null, []);
        var hashes = drawn.ToDictionary(w => w, w => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(w.ArtImage))));
        var key = $"{Version}:{SupportedBuild.Current.BuildId}:{(plain ? "default" : "")}:" +
            string.Join(",", drawn.Select(w => $"{ModWarbonds.ArtTextureName(w.Mod, w.Key):x16}={hashes[w]}").Order()) + ":" +
            string.Join(",", copies.Select(c => $"{c.Name:x16}<{c.Source:x16}").Order());
        List<string> problems = [];
        var built = new HashSet<ulong>();
        var folder = OwnPatches.Cached(install, "mod-warbond-art", key, folder =>
        {
            using var game = new GameDataReader(Path.Combine(install.Game, "data"));
            Build(game, drawn, plain, copies, folder, problems);
        });
        foreach (var name in File.ReadAllLines(Path.Combine(folder, Listing))) built.Add(Convert.ToUInt64(name, 16));
        var result = warbonds.Select(w => (w.ArtImage.Length > 0 || w.ArtFrom != 0) && built.Contains(ModWarbonds.ArtTextureName(w.Mod, w.Key))
            ? w with { ArtTexture = ModWarbonds.ArtTextureName(w.Mod, w.Key) }
            : Plain(w) && built.Contains(DefaultTexture) ? w with { ArtTexture = DefaultTexture } : w).ToList();
        problems.AddRange(result.Where(w => w.ArtImage.Length > 0 && w.ArtTexture == 0 && problems.All(p => !p.StartsWith(w.Mod + ": warbond \"" + w.Key + "\"", StringComparison.Ordinal)))
            .Select(w => $"{w.Mod}: warbond \"{w.Key}\" shows the game's placeholder art: its image was not built."));
        return (result, folder, problems);
    }

    // The textures built, one name (hex) a line: kept with the patch, so a cached folder says what it holds.
    const string Listing = "textures.txt";

    // The images as textures in one patch of Archive in output, with the default card when withDefault and each copy of a
    // game card (Name, from Source); an image that cannot be read stays out, with a problem, and a card the game data
    // lacks (in Archive) is not copied, so its warbond shows the game's art as it is.
    public static void Build(GameDataReader game, IReadOnlyList<ModWarbonds.Warbond> warbonds, bool withDefault,
        IReadOnlyList<(ulong Name, ulong Source)> copies, string output, List<string> problems)
    {
        Directory.CreateDirectory(output);
        var templateKey = new ResourceKey(ResourceTypes.Texture, Template);
        var resources = new Dictionary<ResourceKey, ResourceParts>();
        if (game.Find([templateKey]).GetValueOrDefault(templateKey) is not { } at || at.Archive != Archive ||
            game.Read(at) is not { } template || WarbondLocks.Texture(template) is not { Width: ModWarbonds.ArtWidth, Height: ModWarbonds.ArtHeight })
        {
            problems.Add("Mod warbond art: the game's warbond art is not in the form HellForge knows; mods' warbonds show the placeholder.");
            File.WriteAllText(Path.Combine(output, Listing), "");
            return;
        }
        foreach (var warbond in warbonds)
        {
            try
            {
                var (width, height, rgba) = IconImage.ReadPng(File.ReadAllBytes(warbond.ArtImage));
                if (width != ModWarbonds.ArtWidth || height != ModWarbonds.ArtHeight) throw new InvalidDataException($"it is {width} x {height}, not {ModWarbonds.ArtWidth} x {ModWarbonds.ArtHeight}");
                resources[new ResourceKey(ResourceTypes.Texture, ModWarbonds.ArtTextureName(warbond.Mod, warbond.Key))] =
                    template with { Gpu = IconImage.Bc1Level(width, height, rgba), Stream = [] };
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                problems.Add($"{warbond.Mod}: warbond \"{warbond.Key}\" shows the game's placeholder art: its image could not be used ({error.Message}).");
            }
        }
        var sources = game.Find(copies.Select(c => new ResourceKey(ResourceTypes.Texture, c.Source)).Distinct());
        foreach (var (name, source) in copies)
            if (sources.GetValueOrDefault(new ResourceKey(ResourceTypes.Texture, source)) is { } found && found.Archive == Archive && game.Read(found) is { } card)
                resources[new ResourceKey(ResourceTypes.Texture, name)] = card;
        if (withDefault)
            resources[new ResourceKey(ResourceTypes.Texture, DefaultTexture)] =
                template with { Gpu = IconImage.Bc1Level(ModWarbonds.ArtWidth, ModWarbonds.ArtHeight, DefaultImage()), Stream = [] };
        if (resources.Count > 0) OwnPatches.Write(new() { [Archive] = resources }, output);
        File.WriteAllLines(Path.Combine(output, Listing), resources.Keys.Select(k => k.Name.ToString("x16", System.Globalization.CultureInfo.InvariantCulture)));
    }

    // The default card, RGBA: dark steel, lighter at the top, with a band of muted hazard stripes along the bottom.
    public static byte[] DefaultImage()
    {
        const int W = ModWarbonds.ArtWidth, H = ModWarbonds.ArtHeight, Band = 96, Stripe = 64;
        var rgba = new byte[W * H * 4];
        for (var y = 0; y < H; y++)
            for (var x = 0; x < W; x++)
            {
                var shade = 0.18 - 0.11 * y / (H - 1);
                double r = shade, g = shade * 1.04, b = shade * 1.12;
                if (y >= H - Band && (x + y) / Stripe % 2 == 0) { r = 0.42; g = 0.35; b = 0.12; }
                var at = (y * W + x) * 4;
                rgba[at] = (byte)(r * 255);
                rgba[at + 1] = (byte)(g * 255);
                rgba[at + 2] = (byte)(b * 255);
                rgba[at + 3] = 255;
            }
        return rgba;
    }
}
