using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core.GameData;

// Forge Credits: HellForge's own currency in place of Super Credits, which buy nothing here. The Super Credits pickup on
// the map wears the Forge Credit art (data/forge-credits/pickup-color.png, HellForge's own, painted flat and wrapped onto
// the game's card atlas: research/super-credits/export/forge_template) on the game's own mesh, and the currency icon
// (data/forge-credits/currency-icon.png) replaces the Super Credits sprite in the UI's atlas. The pickup's normal map and
// masks are the game's with the old logo, barcode and text smoothed out. Everything but HellForge's two pictures comes
// from the installation's game copy and stays on the player's machine; it is built once per game build under
// temp/forge-credits/ and staged as HellForge's own patch for the session (ModStaging), like WarbondLocks.
public static class ForgeCredits
{
    // Part of the cache key: raise it whenever the same game build would build different bytes.
    public const int Version = 1;
    const string DoneFile = "done";

    // The Super Credits pickup's textures (unit 0xBD6F4DE16B9AEDCD, archive 6286516637d5c967): 256 x 256, 9 mips, the
    // whole chain in the stream part and its small end again in the GPU part.
    const ulong PickupColor = 0x7BAA840C5DFD9490;   /* BC1 */
    const ulong PickupNormal = 0xBB5DC8F2AED34585;  /* BC5 */
    const ulong PickupMasks = 0xEC06C3EE0F9ADABE;   /* BC3 */
    const int PickupSize = 256, PickupMips = 9;
    // The currency icon: a 120 x 120 sprite of a BC3 page of the UI's atlas (0x5060BF2573C2531C, page 0x610644579AE05635).
    const ulong CurrencyIcon = 0x9414AB4F02745820;
    const int Bc1 = 71, Bc3 = 77, Bc5 = 83;

    // Where the Super Credits art sits in the pickup's normal map and masks (256 x 256 texels, inclusive): the barcode, the
    // logo, and the "SUPER CREDIT" text above and below it. Each is filled with what surrounds it.
    static readonly (int X0, int Y0, int X1, int Y1)[] OldArt = [(14, 63, 98, 106), (116, 82, 186, 142), (112, 60, 188, 74), (112, 148, 188, 162)];

    // The folder of patch files that dress Super Credits as Forge Credits, built once per game build under
    // temp/forge-credits/ (the last one kept).
    public static string Prepare(InstallLayout install, Action<string>? log = null)
    {
        var root = Path.Combine(install.Temp, "forge-credits");
        var key = $"{Version}:{SupportedBuild.Current.BuildId}";
        var folder = Path.Combine(root, Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key)))[..16].ToLowerInvariant());
        if (File.Exists(Path.Combine(folder, DoneFile))) return folder;
        if (Directory.Exists(root))
            foreach (var old in Directory.GetDirectories(root)) Directory.Delete(old, recursive: true);
        using (var game = new GameDataReader(Path.Combine(install.Game, "data")))
        {
            var (files, skipped) = Build(game, folder);
            foreach (var what in skipped) log?.Invoke($"Forge Credits: {what} stays as the game has it (its form changed).");
            log?.Invoke($"Forge Credits: {files.Count} patch file(s) built.");
        }
        File.WriteAllText(Path.Combine(folder, DoneFile), key + "\n");
        return folder;
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
        var found = game.Find(wanted);
        var byArchive = new Dictionary<string, Dictionary<ResourceKey, ResourceParts>>(StringComparer.Ordinal);
        void Add(ulong name, string what, Func<ResourceParts, ResourceParts?> change)
        {
            var key = new ResourceKey(ResourceTypes.Texture, name);
            if (!found.TryGetValue(key, out var at) || change(game.Read(at)) is not { } parts) { skipped.Add(what); return; }
            if (!byArchive.TryGetValue(at.Archive, out var resources)) byArchive[at.Archive] = resources = [];
            resources[key] = parts;
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
        var files = new List<string>();
        foreach (var (archive, resources) in byArchive)
        {
            var (main, gpu, stream) = ResourceArchive.Write(resources);
            var name = archive + ".patch_0";
            File.WriteAllBytes(Path.Combine(output, name), main);
            File.WriteAllBytes(Path.Combine(output, name + ".gpu_resources"), gpu);
            files.AddRange([name, name + ".gpu_resources"]);
            if (stream.Length == 0) continue;
            File.WriteAllBytes(Path.Combine(output, name + ".stream"), stream);
            files.Add(name + ".stream");
        }
        return (files, skipped);
    }

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
