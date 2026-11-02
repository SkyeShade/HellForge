using System.Buffers.Binary;
using HellForge.Core;
using HellForge.Core.GameData;

// Forge Credits (ForgeCredits): the pickup's textures and the currency icon's sprite built from a synthetic game data folder.
static class ForgeCreditsTests
{
    const string Pickups = "6286516637d5c967", Gui = "4c2203529ff1a9c4";
    const ulong Color = 0x7BAA840C5DFD9490, Normal = 0xBB5DC8F2AED34585, Masks = 0xEC06C3EE0F9ADABE;
    const ulong Atlas = 0x5060BF2573C2531C, Page = 0x610644579AE05635, Sprite = 0x9414AB4F02745820;
    const int PageSize = 256, PageMips = 4, SpriteX = 64, SpriteY = 96, SpriteSize = 120;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Forge Credits: the pickup's textures and the currency icon are rebuilt in place, the rest of the page kept", () =>
        {
            var folder = Path.Combine(root, "forge-credits");
            var gameData = Path.Combine(folder, "game", "data");
            // The pickup's textures as the game has them: 256 x 256, 9 mips, all streamed, the chain's end again in the GPU part.
            // The normal map and masks are flat but for a bump where the old logo sits (OldArt's second rectangle).
            ResourceParts Pickup(int format, byte[] chain, int gpu) => new(IconImage.TextureMain(256, 256, 9, format), chain[^gpu..], chain);
            var colorChain = IconImage.Bc1Mips(256, 256, Solid(256, 256, 10, 20, 30, 255));
            var bumped = Solid(256, 256, 128, 128, 0, 255);
            for (var y = 100; y < 120; y++)
                for (var x = 130; x < 160; x++) (bumped[4 * (y * 256 + x)], bumped[4 * (y * 256 + x) + 1]) = (250, 10);
            var pickups = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.Texture, Color)] = Pickup(71, colorChain, 2744),
                [new(ResourceTypes.Texture, Normal)] = Pickup(83, IconImage.Bc5Mips(256, 256, bumped), 1392),
                [new(ResourceTypes.Texture, Masks)] = Pickup(77, IconImage.Bc3Mips(256, 256, bumped, data: true), 1392),
            });
            // The UI page: grey and opaque everywhere, with the currency sprite's record in an atlas.
            var pageChain = IconImage.Bc3Mips(PageSize, PageSize, Solid(PageSize, PageSize, 90, 90, 90, 255));
            var pageBytes = pageChain[..Enumerable.Range(0, PageMips).Sum(m => (PageSize >> m) / 4 * ((PageSize >> m) / 4) * 16)];
            var record = new byte[4 + 40];
            BinaryPrimitives.WriteUInt32LittleEndian(record, 1);
            BinaryPrimitives.WriteUInt64LittleEndian(record.AsSpan(4), Sprite);
            BinaryPrimitives.WriteUInt64LittleEndian(record.AsSpan(12), Page);
            BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(20), SpriteSize);
            BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(24), SpriteSize);
            BinaryPrimitives.WriteSingleLittleEndian(record.AsSpan(28), SpriteX / (float)PageSize);
            BinaryPrimitives.WriteSingleLittleEndian(record.AsSpan(32), SpriteY / (float)PageSize);
            var gui = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.Texture, Page)] = new(IconImage.TextureMain(PageSize, PageSize, PageMips, 77), pageBytes, []),
                [new(ResourceTypes.Hash("texture_atlas"), Atlas)] = new(record, [], []),
                // The names: US English (the card's name shares its text with another id, a voiced line's subtitle and an
                // unrelated text) and German.
                [new(ResourceTypes.Hash("strings"), 0xE1)] = new(Strings(0x03F97B57, [(0x5B043A3F, "SUPER CREDITS"), (0x9C524C54, "SUPER CREDITS"),
                    (0xC8C088ED, "Super Credits may be purchased from the Super Credits tab"), (0x22222222, "{0.0->1.0}Spend your Super Credits"),
                    (0x11111111, "Medals")]), [], []),
                [new(ResourceTypes.Hash("strings"), 0xDE)] = new(Strings(0xBA39C3EC, [(0x5B043A3F, "ÜBER-CREDITS"), (0x33333333, "Super Credits")]), [], []),
            });
            DataBuilderTests.WriteBundles(gameData, new Dictionary<string, byte[]>
            {
                [Pickups] = pickups.Archive, [Pickups + ".gpu_resources"] = pickups.Gpu, [Pickups + ".stream"] = pickups.Stream,
                [Gui] = gui.Archive, [Gui + ".gpu_resources"] = gui.Gpu,
            });
            var output = Path.Combine(folder, "out");
            using (var game = new GameDataReader(gameData))
            {
                var (files, skipped) = ForgeCredits.Build(game, output);
                check(skipped.Count == 0, "Skipped: " + string.Join(", ", skipped));
                check(files.Count == 5, "Patch files: " + string.Join(", ", files));
            }
            Dictionary<ResourceKey, ResourceParts> Patch(string archive)
            {
                byte[] Part(string suffix) => File.Exists(Path.Combine(output, archive + ".patch_0" + suffix)) ? File.ReadAllBytes(Path.Combine(output, archive + ".patch_0" + suffix)) : [];
                return ResourceArchive.Read(Part(""), Part(".gpu_resources"), Part(".stream"));
            }
            var names = Patch(Gui);
            var english = Texts(names[new(ResourceTypes.Hash("strings"), 0xE1)].Main);
            check(english[0x5B043A3F] == "FORGE CREDITS" && english[0x9C524C54] == "FORGE CREDITS" && english[0x11111111] == "Medals" &&
                english[0x22222222] == "{0.0->1.0}Spend your Super Credits" && english[0xC8C088ED] == "Forge Credits are found in supply caches on missions",
                "English names: " + string.Join(" | ", english.Values));
            var german = Texts(names[new(ResourceTypes.Hash("strings"), 0xDE)].Main);
            check(german[0x5B043A3F] == "FORGE CREDITS" && german[0x33333333] == "Super Credits", "German names: " + string.Join(" | ", german.Values));
            var pickup = Patch(Pickups);
            foreach (var (name, gpu) in new[] { (Color, 2744), (Normal, 1392), (Masks, 1392) })
            {
                var parts = pickup[new(ResourceTypes.Texture, name)];
                check(parts.Main.SequenceEqual(Pickup(name == Color ? 71 : name == Normal ? 83 : 77, colorChain, 0).Main) && parts.Gpu.Length == gpu &&
                    parts.Stream.AsSpan(parts.Stream.Length - gpu).SequenceEqual(parts.Gpu), $"{name:X16} lost its header or its split.");
            }
            check(!pickup[new(ResourceTypes.Texture, Color)].Stream.SequenceEqual(colorChain), "The pickup's colour is the game's.");
            foreach (var name in new[] { Normal, Masks })
            {
                var parts = pickup[new(ResourceTypes.Texture, name)];
                var top = EditorIcons.Decode(new(parts.Stream, 256, 256, name == Normal ? 83 : 77), 145, 110, 1, 1)!;
                check(Math.Abs(top[0] - 128) < 8 && Math.Abs(top[1] - 128) < 8, $"{name:X16} still has the old logo's bump ({top[0]}, {top[1]}).");
            }

            var page = Patch(Gui)[new(ResourceTypes.Texture, Page)].Gpu;
            check(page.Length == pageBytes.Length, "The page changed size.");
            byte[] At(byte[] pixels, int x, int y) => EditorIcons.Decode(new(pixels, PageSize, PageSize, 77), x, y, 1, 1)!;
            // The icon (a white card with the logo cut out of it) where the sprite is: an opaque white texel at its top edge.
            var (iw, ih, icon) = IconImage.ReadPng(File.ReadAllBytes(Path.Combine(Repository(), "data", "forge-credits", "currency-icon.png")));
            check(iw == SpriteSize && ih == SpriteSize, $"The currency icon is {iw} x {ih}.");
            var white = At(page, SpriteX + 60, SpriteY + 4);
            check(white[0] > 240 && white[3] > 240 && icon[4 * (4 * iw + 60) + 3] > 240, "No icon in the sprite.");
            // Outside the blocks the sprite touches, the game's bytes.
            var blocksWide = PageSize / 4;
            for (var by = 0; by < PageSize / 4; by++)
                for (var bx = 0; bx < blocksWide; bx++)
                {
                    if (bx * 4 >= SpriteX && bx * 4 < SpriteX + SpriteSize && by * 4 >= SpriteY && by * 4 < SpriteY + SpriteSize) continue;
                    var at = (by * blocksWide + bx) * 16;
                    if (!page.AsSpan(at, 16).SequenceEqual(pageBytes.AsSpan(at, 16))) { check(false, $"Block ({bx}, {by}) outside the sprite changed."); return; }
                }
        });
    }

    public static void RegisterChance(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Forge Credits chance: saved in the mods settings, 0 to 100 or off, and only then in the loader plan", () =>
        {
            var mods = Path.Combine(root, "forge-credits-chance");
            Directory.CreateDirectory(mods);
            check(ModSettings.ForgeCreditChance(mods) is null, "A new installation changes the game's odds.");
            string? Line() => ModLoaderPlan.Text([], root, EntityProfile.Current, "25480438", ModSettings.ForgeCreditChance(mods))
                .Split('\n').FirstOrDefault(l => l.StartsWith("forge_credits_chance", StringComparison.Ordinal));
            check(Line() is null, "The plan names a chance nobody set.");
            ModSettings.SetForgeCreditChance(mods, 100);
            ModSettings.SetPlayWithStrangers(mods, true); /* another setting's save keeps it */
            check(ModSettings.ForgeCreditChance(mods) == 100 && Line() == "forge_credits_chance\t100", "Plan line: " + Line());
            reject(() => ModSettings.SetForgeCreditChance(mods, 101));
            ModSettings.SetForgeCreditChance(mods, null);
            check(ModSettings.ForgeCreditChance(mods) is null && Line() is null, "Off still changes the odds.");
        });
    }

    // A strings resource as the game's: magic, version, count, language, ids, offsets, then the texts, the same text once.
    static byte[] Strings(uint language, (uint Id, string Text)[] entries)
    {
        var head = 16 + 8 * entries.Length;
        var data = new List<byte>();
        var at = new Dictionary<string, int>();
        var bytes = new byte[head];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 0x3E85F3AE);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)entries.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), language);
        for (var i = 0; i < entries.Length; i++)
        {
            if (!at.TryGetValue(entries[i].Text, out var offset)) { at[entries[i].Text] = offset = head + data.Count; data.AddRange(System.Text.Encoding.UTF8.GetBytes(entries[i].Text)); data.Add(0); }
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16 + 4 * i), entries[i].Id);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16 + 4 * entries.Length + 4 * i), (uint)offset);
        }
        return [.. bytes, .. data];
    }

    static Dictionary<uint, string> Texts(byte[] strings)
    {
        var count = (int)BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(8));
        var texts = new Dictionary<uint, string>();
        for (var i = 0; i < count; i++)
        {
            var offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(16 + 4 * count + 4 * i));
            texts[BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(16 + 4 * i))] =
                System.Text.Encoding.UTF8.GetString(strings, offset, Array.IndexOf(strings, (byte)0, offset) - offset);
        }
        return texts;
    }

    static byte[] Solid(int w, int h, byte r, byte g, byte b, byte a)
    {
        var rgba = new byte[w * h * 4];
        for (var i = 0; i < rgba.Length; i += 4) (rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]) = (r, g, b, a);
        return rgba;
    }

    static string Repository()
    {
        var folder = AppContext.BaseDirectory;
        while (!File.Exists(Path.Combine(folder, "HellForge.slnx"))) folder = Path.GetDirectoryName(folder.TrimEnd(Path.DirectorySeparatorChar))!;
        return folder;
    }
}
