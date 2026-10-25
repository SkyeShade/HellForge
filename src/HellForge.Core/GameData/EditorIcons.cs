using System.Buffers.Binary;
using System.Text;
using System.Text.Json;

namespace HellForge.Core.GameData;

// The in-game editor's icons: the game's own HUD sprites (mappings/hud-icons.json names each one), read from the installed
// game on the player's machine and written into the editor's folder (game-icons.bin). A port of HD2Runtime's
// sdk/tools/hd2_hud_icons.py: a sprite is a 40-byte record of a texture_atlas resource (u64 sprite name, u64 page texture
// name, u32 width and height, f32 u0, v0, du, dv); a page is a texture resource whose main part is a 0xC0-byte header and
// a DX10 DDS header, its pixels (mip 0 first) in the .gpu_resources part. The artwork is Arrowhead's: it stays on the
// player's machine and is never published.
//   weapons, throwables  their silhouette: the sprite's red mask, pixel for pixel
//   stratagems           two masks, as the game draws them: the category layer (red, drawn in the category's colour)
//                        and the white layer (green)
//   boosters             the sprite in colour, with its alpha
//   passives             their glyph: the sprite is a black glyph on opaque white, a mask the game draws in a colour
//   factions             the emblem, drawn in the faction's colour: an opaque one's red mask (Terminids, Automatons,
//                        Illuminate: green is the same, blue a soft glow), else its alpha (Super Earth's white emblem)
//   difficulties         a mission difficulty's badge (levels 1 to 10), in colour with its alpha
// Sprites larger than 128 pixels are scaled down (by area, alpha-weighted): the editor draws none larger.
//
// game-icons.bin: "HFIC", u32 version 2, u32 count, then per icon: u8 kind (0 weapon, 1 throwable, 2 stratagem category
// layer, 3 stratagem white layer, 4 booster, 5 armor passive, 7 faction, 8 difficulty; 6 is the editor's own, a mod's image), u8 name
// length, the UTF-8 name, u16 width, u16 height, u8 channels (1: coverage, 0 empty to 255 full; 4: RGBA), then
// width * height * channels bytes, rows top to bottom.
public static class EditorIcons
{
    public const string FileName = "game-icons.bin";
    public const int Version = 2;
    static readonly ulong TextureAtlas = ResourceTypes.Hash("texture_atlas");
    const int TextureHeader = 0xC0, Largest = 128;
    static readonly string[] Groups = ["weapons", "throwables", "stratagems", "boosters", "passives", "factions", "difficulties"];
    // What an installation's icons were made from: the file's format, the groups, and the sprites mappings/hud-icons.json
    // names (BundledMods makes them again when it changes; a change to how a group is extracted adds to Version).
    public static string Stamp { get; } = Version + ":" + string.Join(",", Groups) + ":" + Convert.ToHexString(
        System.Security.Cryptography.SHA256.HashData(typeof(EditorIcons).Assembly.GetManifestResourceStream("HellForge.Core.mappings.hud-icons.json")!));

    public sealed record Icon(int Kind, string Name, int Width, int Height, int Channels, byte[] Pixels);

    // {group: {name: sprite}} from the embedded mappings.
    public static IReadOnlyDictionary<string, IReadOnlyDictionary<string, ulong>> Sprites()
    {
        using var stream = typeof(EditorIcons).Assembly.GetManifestResourceStream("HellForge.Core.mappings.hud-icons.json")!;
        using var json = JsonDocument.Parse(stream);
        var groups = new Dictionary<string, IReadOnlyDictionary<string, ulong>>(StringComparer.Ordinal);
        foreach (var group in Groups)
            groups[group] = json.RootElement.GetProperty(group).EnumerateObject()
                .ToDictionary(p => p.Name, p => Convert.ToUInt64(p.Value.GetString()![2..], 16), StringComparer.Ordinal);
        return groups;
    }

    // Every icon the game data has; missing: names whose sprite no atlas holds.
    public static IReadOnlyList<Icon> Extract(GameDataReader data, out IReadOnlyList<string> missing)
    {
        var records = new Dictionary<ulong, (ulong Page, int W, int H, float U, float V, float Du, float Dv)>();
        foreach (var at in data.All(TextureAtlas).Values)
        {
            var raw = data.Read(at).Main;
            if (raw.Length < 4) continue;
            var count = BinaryPrimitives.ReadUInt32LittleEndian(raw);
            if (4 + 40L * count > raw.Length) continue;
            for (var i = 0; i < count; i++)
            {
                var r = raw.AsSpan(4 + 40 * i, 40);
                records.TryAdd(BinaryPrimitives.ReadUInt64LittleEndian(r), (BinaryPrimitives.ReadUInt64LittleEndian(r[8..]),
                    (int)BinaryPrimitives.ReadUInt32LittleEndian(r[16..]), (int)BinaryPrimitives.ReadUInt32LittleEndian(r[20..]),
                    BinaryPrimitives.ReadSingleLittleEndian(r[24..]), BinaryPrimitives.ReadSingleLittleEndian(r[28..]),
                    BinaryPrimitives.ReadSingleLittleEndian(r[32..]), BinaryPrimitives.ReadSingleLittleEndian(r[36..])));
            }
        }
        var wantedPages = new HashSet<ResourceKey>();
        var sprites = Sprites();
        foreach (var group in sprites.Values)
            foreach (var sprite in group.Values)
                if (records.TryGetValue(sprite, out var rec)) wantedPages.Add(new(ResourceTypes.Texture, rec.Page));
        var pageAt = data.Find(wantedPages);
        var pages = new Dictionary<ulong, (int W, int H, int Format, byte[] Pixels)>();
        var icons = new List<Icon>();
        var lost = new List<string>();
        foreach (var group in Groups)
            foreach (var (name, sprite) in sprites[group].OrderBy(p => p.Key, StringComparer.Ordinal))
            {
                if (!records.TryGetValue(sprite, out var rec) || !pageAt.TryGetValue(new(ResourceTypes.Texture, rec.Page), out var location))
                { lost.Add(name); continue; }
                if (!pages.TryGetValue(rec.Page, out var page))
                {
                    var parts = data.Read(location);
                    var dds = parts.Main.AsSpan(TextureHeader);
                    if (dds.Length < 148 || Encoding.ASCII.GetString(dds[..4]) != "DDS " || Encoding.ASCII.GetString(dds.Slice(84, 4)) != "DX10")
                    { lost.Add(name); continue; }
                    page = ((int)BinaryPrimitives.ReadUInt32LittleEndian(dds[16..]), (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[12..]),
                        (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[128..]), parts.Gpu);
                    pages[rec.Page] = page;
                }
                int x = (int)Math.Round(rec.U * page.W), y = (int)Math.Round(rec.V * page.H);
                if (Math.Abs(rec.Du * page.W - rec.W) > 0.5 || Math.Abs(rec.Dv * page.H - rec.H) > 0.5 || rec.W <= 0 || rec.H <= 0 ||
                    rec.W > 1024 || rec.H > 1024 || Decode(page.Pixels, page.W, page.H, page.Format, x, y, rec.W, rec.H) is not { } decoded)
                { lost.Add(name); continue; }
                var (w, h, rgba) = Shrink(rec.W, rec.H, decoded);
                switch (group)
                {
                    case "factions":
                        var opaque = true;
                        for (var i = 3; i < rgba.Length && opaque; i += 4) opaque = rgba[i] == 255;
                        icons.Add(new(7, name, w, h, 1, opaque ? Channel(rgba, 0, true) : Channel(rgba, 3, false)));
                        break;
                    case "difficulties":
                        icons.Add(new(8, name, w, h, 4, rgba));
                        break;
                    case "weapons" or "throwables":
                        icons.Add(new(group == "weapons" ? 0 : 1, name, w, h, 1, Channel(rgba, 0, false)));
                        break;
                    case "stratagems": /* the masks as the game composes them: each layer where the sprite is opaque */
                        icons.Add(new(2, name, w, h, 1, Channel(rgba, 0, true)));
                        icons.Add(new(3, name, w, h, 1, Channel(rgba, 1, true)));
                        break;
                    case "boosters":
                        icons.Add(new(4, name, w, h, 4, rgba));
                        break;
                    default: /* the glyph is the dark part */
                        var glyph = Channel(rgba, 0, false);
                        for (var i = 0; i < glyph.Length; i++) glyph[i] = (byte)((255 - glyph[i]) * rgba[i * 4 + 3] / 255);
                        icons.Add(new(5, name, w, h, 1, glyph));
                        break;
                }
            }
        missing = lost;
        return icons;
    }

    // One channel of an RGBA image as coverage (times its alpha, when asked).
    static byte[] Channel(byte[] rgba, int channel, bool alpha)
    {
        var output = new byte[rgba.Length / 4];
        for (var i = 0; i < output.Length; i++)
            output[i] = alpha ? (byte)(rgba[i * 4 + channel] * rgba[i * 4 + 3] / 255) : rgba[i * 4 + channel];
        return output;
    }

    // At most Largest pixels a side: each output pixel the alpha-weighted average of the source pixels it covers.
    static (int W, int H, byte[] Rgba) Shrink(int w, int h, byte[] rgba)
    {
        var scale = Math.Max(w, h) / (double)Largest;
        if (scale <= 1) return (w, h, rgba);
        int nw = Math.Max(1, (int)Math.Round(w / scale)), nh = Math.Max(1, (int)Math.Round(h / scale));
        var output = new byte[nw * nh * 4];
        for (var oy = 0; oy < nh; oy++)
            for (var ox = 0; ox < nw; ox++)
            {
                int x0 = ox * w / nw, x1 = Math.Max(x0 + 1, (ox + 1) * w / nw), y0 = oy * h / nh, y1 = Math.Max(y0 + 1, (oy + 1) * h / nh);
                double r = 0, g = 0, b = 0, a = 0;
                var n = (x1 - x0) * (y1 - y0);
                for (var y = y0; y < y1; y++)
                    for (var x = x0; x < x1; x++)
                    {
                        var i = (y * w + x) * 4;
                        double alpha = rgba[i + 3];
                        r += rgba[i] * alpha; g += rgba[i + 1] * alpha; b += rgba[i + 2] * alpha; a += alpha;
                    }
                var o = (oy * nw + ox) * 4;
                if (a > 0) { output[o] = (byte)Math.Round(r / a); output[o + 1] = (byte)Math.Round(g / a); output[o + 2] = (byte)Math.Round(b / a); }
                output[o + 3] = (byte)Math.Round(a / n);
            }
        return (nw, nh, output);
    }

    public static byte[] Serialize(IReadOnlyList<Icon> icons)
    {
        using var output = new MemoryStream();
        using var w = new BinaryWriter(output);
        w.Write("HFIC"u8);
        w.Write((uint)Version);
        w.Write((uint)icons.Count);
        foreach (var icon in icons)
        {
            var name = Encoding.UTF8.GetBytes(icon.Name);
            if (name.Length > 255) throw new InvalidDataException($"icon name too long: {icon.Name}");
            if (icon.Pixels.Length != icon.Width * icon.Height * icon.Channels) throw new InvalidDataException($"icon {icon.Name}: its pixels do not fit its size.");
            w.Write((byte)icon.Kind);
            w.Write((byte)name.Length);
            w.Write(name);
            w.Write((ushort)icon.Width);
            w.Write((ushort)icon.Height);
            w.Write((byte)icon.Channels);
            w.Write(icon.Pixels);
        }
        w.Flush();
        return output.ToArray();
    }

    // Writes the icons of the game data folder into `file` (atomically). Returns how many, and the names left out.
    public static (int Count, IReadOnlyList<string> Missing) Write(string gameData, string file)
    {
        using var data = new GameDataReader(gameData);
        var icons = Extract(data, out var missing);
        var temporary = file + ".partial";
        File.WriteAllBytes(temporary, Serialize(icons));
        File.Move(temporary, file, true);
        return (icons.Count, missing);
    }

    // ---- pixels ----

    // RGBA of the w x h rectangle at (x, y) of a page's mip 0, or null for a format it does not read.
    public static byte[]? Decode(byte[] pixels, int width, int height, int format, int x, int y, int w, int h)
    {
        if (x < 0 || y < 0 || x + w > width || y + h > height) return null;
        var output = new byte[w * h * 4];
        switch (format)
        {
            case 28 or 29 or 87 or 91: /* R8G8B8A8, B8G8R8A8 */
                var bgra = format is 87 or 91;
                for (var row = 0; row < h; row++)
                    for (var col = 0; col < w; col++)
                    {
                        var s = ((y + row) * width + x + col) * 4;
                        if (s + 4 > pixels.Length) return null;
                        var d = (row * w + col) * 4;
                        output[d] = pixels[s + (bgra ? 2 : 0)]; output[d + 1] = pixels[s + 1]; output[d + 2] = pixels[s + (bgra ? 0 : 2)]; output[d + 3] = pixels[s + 3];
                    }
                return output;
            case 71 or 72 or 77 or 78 or 80:
                var size = format is 77 or 78 ? 16 : 8;
                var blocksWide = (width + 3) / 4;
                var texels = new byte[64];
                var alpha = new byte[16];
                for (var by = y / 4; by < (y + h + 3) / 4; by++)
                    for (var bx = x / 4; bx < (x + w + 3) / 4; bx++)
                    {
                        var offset = (by * blocksWide + bx) * size;
                        if (offset + size > pixels.Length) return null;
                        var block = pixels.AsSpan(offset, size);
                        if (format is 71 or 72) ColourBlock(block, false, texels);
                        else if (format is 77 or 78)
                        {
                            ColourBlock(block[8..], true, texels);
                            AlphaBlock(block, alpha);
                            for (var i = 0; i < 16; i++) texels[i * 4 + 3] = alpha[i];
                        }
                        else
                        {
                            AlphaBlock(block, alpha);
                            for (var i = 0; i < 16; i++) { texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = alpha[i]; texels[i * 4 + 3] = 255; }
                        }
                        for (var i = 0; i < 16; i++)
                        {
                            int px = bx * 4 + i % 4 - x, py = by * 4 + i / 4 - y;
                            if (px >= 0 && px < w && py >= 0 && py < h) texels.AsSpan(i * 4, 4).CopyTo(output.AsSpan((py * w + px) * 4));
                        }
                    }
                return output;
        }
        return null;
    }

    static (int, int, int) Rgb565(int c)
    {
        int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
        return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
    }

    static void ColourBlock(ReadOnlySpan<byte> block, bool four, byte[] texels)
    {
        int c0 = BinaryPrimitives.ReadUInt16LittleEndian(block), c1 = BinaryPrimitives.ReadUInt16LittleEndian(block[2..]);
        var bits = BinaryPrimitives.ReadUInt32LittleEndian(block[4..]);
        var (r0, g0, b0) = Rgb565(c0);
        var (r1, g1, b1) = Rgb565(c1);
        var palette = new byte[16];
        void Set(int i, int r, int g, int b, int a) { palette[i * 4] = (byte)r; palette[i * 4 + 1] = (byte)g; palette[i * 4 + 2] = (byte)b; palette[i * 4 + 3] = (byte)a; }
        Set(0, r0, g0, b0, 255);
        Set(1, r1, g1, b1, 255);
        if (four || c0 > c1)
        {
            Set(2, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
            Set(3, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
        }
        else
        {
            Set(2, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
            Set(3, 0, 0, 0, 0);
        }
        for (var i = 0; i < 16; i++) palette.AsSpan((int)(bits >> (2 * i) & 3) * 4, 4).CopyTo(texels.AsSpan(i * 4));
    }

    static void AlphaBlock(ReadOnlySpan<byte> block, byte[] values)
    {
        int a0 = block[0], a1 = block[1];
        ulong bits = 0;
        for (var i = 0; i < 6; i++) bits |= (ulong)block[2 + i] << (8 * i);
        var palette = new int[8];
        palette[0] = a0; palette[1] = a1;
        if (a0 > a1) for (var i = 1; i < 7; i++) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        else { for (var i = 1; i < 5; i++) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5; palette[6] = 0; palette[7] = 255; }
        for (var i = 0; i < 16; i++) values[i] = (byte)palette[(int)(bits >> (3 * i) & 7)];
    }
}
