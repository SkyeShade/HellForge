using System.Buffers.Binary;
using System.Text;

namespace HellForge.Core.GameData;

// Forge Credits: HellForge's own currency in place of Super Credits, which buy nothing here. The Super Credits pickup on
// the map wears the Forge Credit art (data/forge-credits/pickup-color.png, HellForge's own, painted flat and wrapped onto
// the game's card atlas: research/super-credits/export/forge_template) on the game's own mesh, and the currency icon
// (data/forge-credits/currency-icon.png) replaces the Super Credits sprite in the UI's atlas. The pickup's normal map and
// masks are the game's with the old logo, barcode and text smoothed out. The game's "Super Credits" names say Forge
// Credits (Names, in every language; the English texts that mention them too). Everything but HellForge's two pictures comes
// from the installation's game copy and stays on the player's machine; it is built once per game build under
// temp/forge-credits/ and staged as HellForge's own patch for the session (ModStaging), like WarbondLocks.
public static class ForgeCredits
{
    // Part of the cache key: raise it whenever the same game build would build different bytes. 2: the card tinted red.
    // 3: the names. 4: the XAML pages' colours. 5: every copy of each resource, the premium token's colours.
    public const int Version = 6; // 6: the "WARBOND LOCKED" label

    // The Super Credits pickup's textures (unit 0xBD6F4DE16B9AEDCD, archive 6286516637d5c967): 256 x 256, 9 mips, the
    // whole chain in the stream part and its small end again in the GPU part.
    const ulong PickupColor = 0x7BAA840C5DFD9490;   /* BC1 */
    const ulong PickupNormal = 0xBB5DC8F2AED34585;  /* BC5 */
    const ulong PickupMasks = 0xEC06C3EE0F9ADABE;   /* BC3 */
    const int PickupSize = 256, PickupMips = 9;
    // The currency icon: a 120 x 120 sprite of a BC3 page of the UI's atlas (0x5060BF2573C2531C, page 0x610644579AE05635).
    const ulong CurrencyIcon = 0x9414AB4F02745820;
    const int Bc1 = 71, Bc3 = 77, Bc5 = 83;

    // The currency's names (strings ids, the same in every language: 0x5B043A3F is the pickup card's, game.dll 0x198BAE6),
    // in every language, since Forge Credits is a name.
    static readonly Dictionary<uint, string> Names = new()
    {
        [0x4F6FCA2E] = "FORGE CREDITS", [0x5B043A3F] = "FORGE CREDITS", [0x9C524C54] = "FORGE CREDITS", [0xA9A92F7C] = "Forge Credits",
        [0x2EA7DFD5] = "BONUS FORGE CREDITS", [0xE99CA12E] = "INSUFFICIENT FORGE CREDITS",
    };
    // The English languages (US, UK): their other texts that name Super Credits name Forge Credits, but for voiced lines'
    // subtitles ("{start->end}...", which the voice still says) and the store's.
    static readonly HashSet<uint> English = [0x03F97B57, 0x6F4515CB];
    // English texts HellForge says differently: Forge Credits are found, not bought.
    static readonly Dictionary<uint, string> EnglishTexts = new() { [0xC8C088ED] = "Forge Credits are found in supply caches on missions" };
    static readonly HashSet<uint> StoreTexts = [0xECF5FCD6, 0x246CCF47, 0xA1490676, 0x2CE16182, 0x2EFEB825, 0x365A11B5, 0x56D3BD14];

    // The XAML pages' Super Credits colours, HellForge red instead (research/forge-credits-tint, "UI colours, round 2"):
    // svg_ui_elements.xaml's Icons_Sec (the pause menu's wallet, acquisitions, price widgets: only Super Credits use it)
    // and currency_icons.xaml's CurrencySupercredits fill #FF90D5F6; color_styles.xaml's SecondaryCTA (#FF71F5FF, only the
    // premium warbond purchase popups use it); currency_icons.xaml's CurrencyWarbondtokenPremium (light blue #FF82DFFB on
    // dark teal #FF003145: warbonds are bought with Forge Credits). Each colour is the same length, so it is written in place.
    static readonly (ulong Resource, string From, string To)[] XamlColours =
    [
        (0x04A4245928EA3906, """Fill="#FF90D5F6""", """Fill="#FFE5392B"""),
        (0x7DB31550D264126F, """Fill="#FF90D5F6""", """Fill="#FFE5392B"""),
        (0x7DB31550D264126F, """Fill="#FF82DFFB""", """Fill="#FFE5392B"""),
        (0x7DB31550D264126F, """V40.8574Z" Fill="#FF003145""", """V40.8574Z" Fill="#FF410A08"""),
        (0x633F3353EB3F56F9, """x:Key="SecondaryCTA">#FF71F5FF<""", """x:Key="SecondaryCTA">#FFE5392B<"""),
    ];

    // Where the Super Credits art sits in the pickup's normal map and masks (256 x 256 texels, inclusive): the barcode, the
    // logo, and the "SUPER CREDIT" text above and below it. Each is filled with what surrounds it.
    static readonly (int X0, int Y0, int X1, int Y1)[] OldArt = [(14, 63, 98, 106), (116, 82, 186, 142), (112, 60, 188, 74), (112, 148, 188, 162)];

    // The folder of patch files that dress Super Credits as Forge Credits, built once per game build under
    // temp/forge-credits/ (the last one kept).
    public static string Prepare(InstallLayout install, Action<string>? log = null)
    {
        return OwnPatches.Cached(install, "forge-credits", $"{Version}:{SupportedBuild.Current.BuildId}", folder =>
        {
            using var game = new GameDataReader(Path.Combine(install.Game, "data"));
            var (files, skipped) = Build(game, folder);
            foreach (var what in skipped) log?.Invoke($"Forge Credits: {what} stays as the game has it (its form changed).");
            log?.Invoke($"Forge Credits: {files.Count} patch file(s) built.");
        });
    }

    static byte[] Picture(string name)
    {
        using var stream = typeof(ForgeCredits).Assembly.GetManifestResourceStream("HellForge.Core.forge-credits." + name)!;
        using var copy = new MemoryStream();
        stream.CopyTo(copy);
        return copy.ToArray();
    }

    // The patch files in output; skipped: what the game data lacks or has in another form, left as it is.
    public static (IReadOnlyList<string> Files, IReadOnlyList<string> Skipped) Build(GameDataReader game, string output)
    {
        Directory.CreateDirectory(output);
        var skipped = new List<string>();
        var sprite = EditorIcons.SpriteRecords(game).TryGetValue(CurrencyIcon, out var record) ? record : default;
        var wanted = new List<ResourceKey> { new(ResourceTypes.Texture, PickupColor), new(ResourceTypes.Texture, PickupNormal), new(ResourceTypes.Texture, PickupMasks) };
        if (sprite.Page != 0) wanted.Add(new(ResourceTypes.Texture, sprite.Page));
        else skipped.Add("the currency icon");
        // Every copy of each resource: packages carry their own, and the game may load any of them.
        var found = game.FindEvery(wanted);
        var byArchive = new Dictionary<string, Dictionary<ResourceKey, ResourceParts>>(StringComparer.Ordinal);
        void Put(string archive, ResourceKey key, ResourceParts parts)
        {
            if (!byArchive.TryGetValue(archive, out var resources)) byArchive[archive] = resources = [];
            resources[key] = parts;
        }
        void Add(ulong name, string what, Func<ResourceParts, ResourceParts?> change)
        {
            var key = new ResourceKey(ResourceTypes.Texture, name);
            var changed = 0;
            foreach (var at in found.GetValueOrDefault(key) ?? [])
                if (change(game.Read(at)) is { } parts) { Put(at.Archive, key, parts); changed++; }
            if (changed == 0) skipped.Add(what);
        }
        Add(PickupColor, "the pickup's colour", parts =>
        {
            var (width, height, rgba) = IconImage.ReadPng(Picture("pickup-color.png"));
            return width == PickupSize && height == PickupSize && Pickup(parts, Bc1) is not null
                ? WithChain(parts, IconImage.Bc1Mips(width, height, rgba)) : null;
        });
        Add(PickupNormal, "the pickup's normal map", parts => Pickup(parts, Bc5) is { } page ? WithChain(parts, IconImage.Bc5Mips(PickupSize, PickupSize, WithoutOldArt(page))) : null);
        Add(PickupMasks, "the pickup's masks", parts => Pickup(parts, Bc3) is { } page ? WithChain(parts, IconImage.Bc3Mips(PickupSize, PickupSize, WithoutOldArt(page), data: true)) : null);
        if (sprite.Page != 0) Add(sprite.Page, "the currency icon", parts => Icon(parts, sprite));
        var stringsType = ResourceTypes.Hash("strings");
        var renamed = 0;
        foreach (var (name, copies) in game.Every(stringsType))
            foreach (var at in copies)
                if (Renamed(game.Read(at)) is { } parts) { Put(at.Archive, new(stringsType, name), parts); renamed++; }
        if (renamed == 0) skipped.Add("the names");
        var xaml = ResourceTypes.Hash("xaml");
        var pages = game.FindEvery(XamlColours.Select(c => new ResourceKey(xaml, c.Resource)).Distinct());
        var texts = new Dictionary<(string Archive, ResourceKey Key), (ResourceParts Parts, string Text)>();
        foreach (var (resource, from, to) in XamlColours)
        {
            var key = new ResourceKey(xaml, resource);
            var changed = 0;
            foreach (var at in pages.GetValueOrDefault(key) ?? [])
            {
                // A page with several colours takes each on top of the ones before.
                var (parts, text) = texts.TryGetValue((at.Archive, key), out var done) ? done : (game.Read(at), "");
                if (text.Length == 0) text = Encoding.UTF8.GetString(parts.Main);
                if (!text.Contains(from, StringComparison.Ordinal)) continue;
                text = text.Replace(from, to, StringComparison.Ordinal);
                texts[(at.Archive, key)] = (parts, text);
                Put(at.Archive, key, parts with { Main = Encoding.UTF8.GetBytes(text) });
                changed++;
            }
            if (changed == 0) skipped.Add($"a colour of XAML page {resource:X16}");
        }
        return (OwnPatches.Write(byArchive, output), skipped);
    }

    // A strings resource (u32 magic, version, count, language; u32 ids[count]; u32 offsets[count] into the resource of
    // NUL-terminated UTF-8, shared between ids with the same text) with the Super Credits names changed: the new texts are
    // added at its end and only their ids' offsets move, so every other byte stays the game's. Null when nothing changes.
    static ResourceParts? Renamed(ResourceParts parts)
    {
        var d = parts.Main;
        if (d.Length < 16) return null;
        var count = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(8));
        var language = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(12));
        if (count == 0 || 16 + 8L * count > d.Length) return null;
        var main = (byte[])d.Clone();
        using var added = new MemoryStream();
        for (var i = 0; i < (int)count; i++)
        {
            var id = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(16 + 4 * i));
            var at = 16 + 4 * (int)count + 4 * i;
            var offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at));
            if (offset < 16 + 8 * (int)count || offset >= d.Length) continue;
            var end = Array.IndexOf(d, (byte)0, offset);
            var text = Encoding.UTF8.GetString(d, offset, (end < 0 ? d.Length : end) - offset);
            string? changed = Names.TryGetValue(id, out var name) ? name
                : English.Contains(language) && EnglishTexts.TryGetValue(id, out var own) ? own
                : English.Contains(language) && !StoreTexts.Contains(id) && !text.StartsWith('{') ? Rename(text) : null;
            if (changed == null || changed == text) continue;
            BinaryPrimitives.WriteUInt32LittleEndian(main.AsSpan(at), (uint)(d.Length + added.Position));
            added.Write(Encoding.UTF8.GetBytes(changed));
            added.WriteByte(0);
        }
        byte[] result = [.. main, .. added.ToArray()];
        // The language's warbond buttons get HellForge's "WARBOND LOCKED" beside "BUY PREMIUM WARBOND".
        var ids = Enumerable.Range(0, (int)count).Select(i => BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(16 + 4 * i))).ToHashSet();
        if (ids.Contains(BuyPremiumWarbond) && !ids.Contains(WarbondLockedLabel)) result = WithText(result, WarbondLockedLabel, "WARBOND LOCKED");
        return result.Length == d.Length && added.Length == 0 ? null : parts with { Main = result };
    }

    // "BUY PREMIUM WARBOND", and HellForge's own label for a premium warbond the live account does not own (the mod loader's
    // locked warbonds, forge_credits.c): EngineHash.Id32("hellforge_warbond_locked").
    const uint BuyPremiumWarbond = 0xD1CA2FF4;
    public const uint WarbondLockedLabel = 0xF8AD8A16;

    // A strings resource with one more text: its id in order among the ids (the game looks them up in order), its offset
    // after them, the old offsets moved by the 8 bytes the two arrays grow, the text added at the end.
    static byte[] WithText(byte[] strings, uint id, string text)
    {
        var count = (int)BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(8));
        var entries = Enumerable.Range(0, count).Select(i => (Id: BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(16 + 4 * i)),
            Offset: BinaryPrimitives.ReadUInt32LittleEndian(strings.AsSpan(16 + 4 * count + 4 * i)) + 8)).ToList();
        entries.Add((id, (uint)(strings.Length + 8)));
        entries.Sort((a, b) => a.Id.CompareTo(b.Id));
        var output = new byte[strings.Length + 8];
        strings.AsSpan(0, 16).CopyTo(output);
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8), (uint)(count + 1));
        for (var i = 0; i <= count; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(16 + 4 * i), entries[i].Id);
            BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(16 + 4 * (count + 1) + 4 * i), entries[i].Offset);
        }
        strings.AsSpan(16 + 8 * count).CopyTo(output.AsSpan(16 + 8 * (count + 1)));
        return [.. output, .. Encoding.UTF8.GetBytes(text), 0];
    }

    static string? Rename(string text) => text.Contains("Super Credit", StringComparison.OrdinalIgnoreCase)
        ? text.Replace("SUPER CREDITS", "FORGE CREDITS").Replace("Super Credits", "Forge Credits").Replace("super credits", "Forge Credits")
            .Replace("SUPER CREDIT", "FORGE CREDIT").Replace("Super Credit", "Forge Credit")
        : null;

    // A pickup texture of the expected form (256 x 256, 9 mips, the format given, the GPU part the end of the streamed
    // chain), as its top level; null otherwise.
    static EditorIcons.Page? Pickup(ResourceParts parts, int format)
    {
        if (Dds(parts) is not { } dds || dds.Width != PickupSize || dds.Height != PickupSize || dds.Mips != PickupMips || dds.Format != format) return null;
        if (parts.Gpu.Length > parts.Stream.Length || !parts.Stream.AsSpan(parts.Stream.Length - parts.Gpu.Length).SequenceEqual(parts.Gpu)) return null;
        return new(parts.Stream, PickupSize, PickupSize, format);
    }

    // The parts with a new mip chain, split as the game's: all of it streamed, its end again in the GPU part.
    static ResourceParts? WithChain(ResourceParts parts, byte[] chain) =>
        chain.Length == parts.Stream.Length ? parts with { Stream = chain, Gpu = chain[(chain.Length - parts.Gpu.Length)..] } : null;

    // The top level's RGBA with each OldArt rectangle filled with the median of the texels in a 3-texel ring around it.
    static byte[] WithoutOldArt(EditorIcons.Page page)
    {
        var rgba = EditorIcons.Decode(page, 0, 0, page.Width, page.Height)!;
        foreach (var (x0, y0, x1, y1) in OldArt)
        {
            var ring = new List<int>[4] { [], [], [], [] };
            for (var y = y0 - 3; y <= y1 + 3; y++)
                for (var x = x0 - 3; x <= x1 + 3; x++)
                {
                    if (x >= x0 && x <= x1 && y >= y0 && y <= y1 || x < 0 || y < 0 || x >= page.Width || y >= page.Height) continue;
                    for (var c = 0; c < 4; c++) ring[c].Add(rgba[4 * (y * page.Width + x) + c]);
                }
            var fill = ring.Select(values => { values.Sort(); return (byte)values[values.Count / 2]; }).ToArray();
            for (var y = y0; y <= y1; y++)
                for (var x = x0; x <= x1; x++) fill.CopyTo(rgba, 4 * (y * page.Width + x));
        }
        return rgba;
    }

    // The atlas page with the currency icon's sprite redrawn at every mip level: only the blocks the sprite touches are
    // encoded again, so the rest of the page keeps the game's bytes. Null for a page of another form.
    static ResourceParts? Icon(ResourceParts parts, (ulong Page, int W, int H, float U, float V, float Du, float Dv) sprite)
    {
        if (Dds(parts) is not { } dds || dds.Format != Bc3 || parts.Stream.Length != 0) return null;
        var (width, height, icon) = IconImage.ReadPng(Picture("currency-icon.png"));
        if (width != sprite.W || height != sprite.H) return null;
        int x = (int)MathF.Round(sprite.U * dds.Width), y = (int)MathF.Round(sprite.V * dds.Height);
        if (x % 4 != 0 || y % 4 != 0 || x + width > dds.Width || y + height > dds.Height) return null;
        var gpu = (byte[])parts.Gpu.Clone();
        var levels = IconImage.MipLevels(width, height, icon);
        int offset = 0, w = dds.Width, h = dds.Height;
        for (var mip = 0; mip < dds.Mips; mip++, w = Math.Max(1, w / 2), h = Math.Max(1, h / 2))
        {
            var size = Math.Max(1, (w + 3) / 4) * Math.Max(1, (h + 3) / 4) * 16;
            if (offset + size > gpu.Length) return null;
            if (mip < levels.Count)
            {
                var (iw, ih, pixels) = levels[mip];
                int sx = x >> mip, sy = y >> mip;
                // The blocks the sprite touches at this level, decoded, the sprite drawn in, encoded again.
                int bx0 = sx / 4 * 4, by0 = sy / 4 * 4, bx1 = Math.Min(w, (sx + iw + 3) / 4 * 4), by1 = Math.Min(h, (sy + ih + 3) / 4 * 4);
                var level = new EditorIcons.Page(gpu.AsSpan(offset, size).ToArray(), w, h, Bc3);
                if (EditorIcons.Decode(level, bx0, by0, bx1 - bx0, by1 - by0) is not { } region) return null;
                var rw = bx1 - bx0;
                for (var row = 0; row < ih; row++)
                    pixels.AsSpan(4 * row * iw, 4 * iw).CopyTo(region.AsSpan(4 * ((sy - by0 + row) * rw + sx - bx0)));
                var blocks = IconImage.Bc3Level(rw, by1 - by0, region);
                var blocksWide = (w + 3) / 4;
                for (var by = 0; by < (by1 - by0) / 4; by++)
                    blocks.AsSpan(by * rw / 4 * 16, rw / 4 * 16).CopyTo(gpu.AsSpan(offset + ((by0 / 4 + by) * blocksWide + bx0 / 4) * 16));
            }
            offset += size;
        }
        return offset == gpu.Length ? parts with { Gpu = gpu } : null;
    }

    // A texture's size, mip count and DXGI format from its DDS header (after the texture header, with the DX10 extension).
    static (int Width, int Height, int Mips, int Format)? Dds(ResourceParts parts)
    {
        if (parts.Main.Length < 0xC0 + 148) return null;
        var dds = parts.Main.AsSpan(0xC0);
        if (!dds[..4].SequenceEqual("DDS "u8) || !dds.Slice(84, 4).SequenceEqual("DX10"u8)) return null;
        return ((int)BinaryPrimitives.ReadUInt32LittleEndian(dds[16..]), (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[12..]),
            (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[28..]), (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[128..]));
    }
}
