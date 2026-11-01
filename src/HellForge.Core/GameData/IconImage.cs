using System.Buffers.Binary;
using System.IO.Compression;

namespace HellForge.Core.GameData;

// A stratagem icon from a mod's PNG, as the game's own icons are made (a port of HD2Runtime's sdk/tools/hd2_image.py;
// research/stratagem-icon-family-F5FEE03DCFDB.json there): a 256 x 256 BC1 texture with its full 9-level mip chain, and
// the GUI material that draws it, both under the icon's name. The game crashes on an icon without its material, so the
// two always come together.
public static class IconImage
{
    public const int Size = 256;
    const int Mips = 9;
    const int Bc1Unorm = 71;
    const int Bc3Unorm = 77; // the UI atlas pages' format (the difficulty icons' page, 0x610644579AE05635)
    // The first word of the vanilla icon textures' header; the rest is the "no streamed mips" form.
    const uint IconClass = 0xFCE7DA44;
    const uint IconShader = 0x3461FF0D;
    // The material slot the widget's image property names.
    const uint ImageSlot = 0x3AA8B87E;

    // {type: parts} of an icon named `name`. prepare "raw" uses the pixels as given; "auto" uses a picture already drawn
    // in the game's icon mask convention as given and converts any other (a red-and-white icon) to masks (IconMasks).
    // "image" keeps the picture's colours and transparency in BC3, as the UI's atlas pages are stored: for widgets that
    // draw a texture as it is (a difficulty icon), where BC1 has no alpha.
    public static Dictionary<ResourceKey, ResourceParts> Family(byte[] png, ulong name, string prepare = "raw")
    {
        var (width, height, rgba) = ReadPng(png);
        if (width != Size || height != Size) throw new InvalidDataException($"icon images must be {Size} x {Size} pixels (this one is {width} x {height}).");
        if (prepare == "auto" && !IsIconMasks(rgba)) rgba = IconMasks(rgba);
        else if (prepare is not ("auto" or "raw" or "image")) throw new InvalidDataException($"an icon is prepared auto, raw or image, not {prepare}.");
        var texture = prepare == "image" ? new ResourceParts(TextureMain(Size, Size, Mips, Bc3Unorm), Bc3Mips(Size, Size, rgba), [])
            : new ResourceParts(TextureMain(Size, Size, Mips), Bc1Mips(Size, Size, rgba), []);
        return new()
        {
            [new(ResourceTypes.Texture, name)] = texture,
            [new(ResourceTypes.Material, name)] = new(IconMaterial(name), [], []),
        };
    }

    // The texture's main part: the icon texture header, then a DDS header with the DX10 extension.
    public static byte[] TextureMain(int width, int height, int mips, int dxgi = Bc1Unorm)
    {
        var data = new byte[0xC0 + 148];
        void U32(int at, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(at), value);
        U32(0, IconClass);
        U32(8, 0xFFFFFFFF);
        "DDS "u8.CopyTo(data.AsSpan(0xC0));
        var dds = 0xC4;
        foreach (var (at, value) in new[] { (0, 124u), (4, 0x21007u), (8, (uint)height), (12, (uint)width), (24, (uint)mips),
            (72, 32u), (76, 4u), (80, 0x30315844u), (104, 0x401008u), (124, (uint)dxgi), (128, 3u), (136, 1u) })
            U32(dds + at, value);
        return data;
    }

    // The GUI icon material: header {0x120, 1 (one material, usable by GUIs), body at 0x18, 0x7C}; body: no parent, one
    // texture slot, the icon shader; the slot id at 0x88 and the texture's name at 0x8C. Every vanilla icon material is
    // exactly these bytes for its name.
    public static byte[] IconMaterial(ulong name)
    {
        var data = new byte[160];
        BinaryPrimitives.WriteUInt32LittleEndian(data, 0x120);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(8), 0x18);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(12), 0x7C);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x18 + 0x28), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x18 + 0x68), IconShader);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x88), ImageSlot);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(0x8C), name);
        return data;
    }

    // The most image data ReadPng inflates (4096 x 4096 in 8-bit RGBA, with its row filters).
    const long MaxImageBytes = 4096L * (4096 * 4 + 1);

    public static ReadOnlySpan<byte> PngSignature => [0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A];

    // (width, height, RGBA) of a non-interlaced PNG: 8- and 16-bit, every colour type; anything else is refused.
    public static (int Width, int Height, byte[] Rgba) ReadPng(byte[] data)
    {
        if (data.Length < 8 || !data.AsSpan(0, 8).SequenceEqual(PngSignature)) throw new InvalidDataException("not a PNG file.");
        using var idat = new MemoryStream();
        var format = ReadChunks(data, idat);
        var bits = format.Channels * format.Depth;
        int stride = (format.Width * bits + 7) / 8, step = Math.Max(1, bits / 8);
        // The image data's size follows from the header: checked before inflating, and never inflated past it (a few KB of
        // zlib can claim gigabytes; a mod's PNG may come from another machine, mod sync).
        var expected = (long)format.Height * (stride + 1);
        if (expected > MaxImageBytes) throw new InvalidDataException("the PNG is too large.");
        var raw = Inflate(idat, expected);
        var rgba = new byte[(long)format.Width * format.Height * 4];
        var previous = new byte[stride];
        var samples = new int[format.Width * format.Channels];
        for (var y = 0; y < format.Height; y++)
        {
            var filter = raw[y * (stride + 1)];
            var line = raw.AsSpan(y * (stride + 1) + 1, stride).ToArray();
            Unfilter(line, previous, filter, step);
            previous = line;
            ReadSamples(line, format, samples);
            for (var x = 0; x < format.Width; x++) WritePixel(rgba, (y * format.Width + x) * 4, x, format, samples);
        }
        return (format.Width, format.Height, rgba);
    }

    // What a PNG's header says, and its palette and its transparency (tRNS) when it has them.
    sealed record PngFormat(int Width, int Height, int Depth, int Colour, int Channels, byte[]? Palette, byte[]? Alpha);

    // A PNG's chunks, each checked: the image data (IDAT) into idat, and the header, checked, with its palette.
    static PngFormat ReadChunks(byte[] data, MemoryStream idat)
    {
        byte[]? header = null, palette = null, alpha = null;
        for (var at = 8; at + 8 <= data.Length;)
        {
            var length = BinaryPrimitives.ReadInt32BigEndian(data.AsSpan(at));
            var kind = System.Text.Encoding.ASCII.GetString(data, at + 4, 4);
            if (length < 0 || at + 12L + length > data.Length) throw new InvalidDataException("truncated PNG chunk.");
            if (Crc32(data.AsSpan(at + 4, length + 4)) != BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at + 8 + length)))
                throw new InvalidDataException("PNG chunk checksum mismatch.");
            var body = data.AsSpan(at + 8, length);
            at += 12 + length;
            if (kind == "IHDR") header = body.ToArray();
            else if (kind == "PLTE") palette = body.ToArray();
            else if (kind == "tRNS") alpha = body.ToArray();
            else if (kind == "IDAT") idat.Write(body);
            else if (kind == "IEND") break;
        }
        return Format(header, idat.Length, palette, alpha);
    }

    static PngFormat Format(byte[]? header, long imageData, byte[]? palette, byte[]? alpha)
    {
        if (header is not { Length: 13 } || imageData == 0) throw new InvalidDataException("PNG without image data.");
        var width = BinaryPrimitives.ReadInt32BigEndian(header);
        var height = BinaryPrimitives.ReadInt32BigEndian(header.AsSpan(4));
        int depth = header[8], colour = header[9];
        int channels = colour switch { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => 0 };
        if (channels == 0 || header[10] != 0 || header[11] != 0) throw new InvalidDataException("unsupported PNG colour type or compression.");
        if (header[12] != 0) throw new InvalidDataException("interlaced PNGs are not supported; save without interlacing.");
        if (!(colour is 0 or 3 ? depth is 1 or 2 or 4 or 8 : depth is 8 or 16) || (colour == 3 && depth == 16))
            throw new InvalidDataException($"unsupported PNG bit depth {depth}.");
        if (colour == 3 && palette == null) throw new InvalidDataException("palette PNG without a palette.");
        if (width is < 1 or > 8192 || height is < 1 or > 8192) throw new InvalidDataException("the PNG is too large.");
        return new(width, height, depth, colour, channels, palette, alpha);
    }

    // The image data inflated: exactly expected bytes, or it is refused.
    static byte[] Inflate(MemoryStream idat, long expected)
    {
        var raw = new byte[expected];
        idat.Position = 0;
        using (var inflate = new ZLibStream(idat, CompressionMode.Decompress))
        {
            var read = inflate.ReadAtLeast(raw, raw.Length, throwOnEndOfStream: false);
            if (read != raw.Length || inflate.Read(new byte[1]) != 0) throw new InvalidDataException("PNG image data has the wrong size.");
        }
        return raw;
    }

    // A row's filter undone, against the row above (previous); step: the bytes of a pixel, at least 1.
    static void Unfilter(byte[] line, byte[] previous, int filter, int step)
    {
        for (var i = 0; i < line.Length; i++)
        {
            int a = i >= step ? line[i - step] : 0, b = previous[i], c = i >= step ? previous[i - step] : 0;
            line[i] = (byte)(line[i] + filter switch
            {
                0 => 0,
                1 => a,
                2 => b,
                3 => (a + b) / 2,
                4 => Paeth(a, b, c),
                _ => throw new InvalidDataException($"invalid PNG row filter {filter}.")
            });
        }
    }

    // A row's samples: a pixel's each below 8 bits, every channel's at 8 bits, the high byte of each at 16.
    static void ReadSamples(byte[] line, PngFormat format, int[] samples)
    {
        var depth = format.Depth;
        if (depth < 8)
            for (var x = 0; x < format.Width; x++) samples[x] = line[x * depth / 8] >> (8 - depth - x * depth % 8) & ((1 << depth) - 1);
        else
            for (var s = 0; s < format.Width * format.Channels; s++) samples[s] = line[depth == 16 ? 2 * s : s];
    }

    // Pixel x of a row as RGBA at rgba[o].
    static void WritePixel(byte[] rgba, int o, int x, PngFormat format, int[] samples)
    {
        switch (format.Colour)
        {
            case 3:
                var index = samples[x];
                if (3 * index + 3 > format.Palette!.Length) throw new InvalidDataException("PNG palette index out of range.");
                format.Palette.AsSpan(3 * index, 3).CopyTo(rgba.AsSpan(o));
                rgba[o + 3] = format.Alpha != null && index < format.Alpha.Length ? format.Alpha[index] : (byte)255;
                break;
            case 0:
                var grey = (byte)(format.Depth < 8 ? samples[x] * (255 / ((1 << format.Depth) - 1)) : samples[x]);
                (rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]) = (grey, grey, grey, 255);
                break;
            case 4:
                (rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]) = ((byte)samples[2 * x], (byte)samples[2 * x], (byte)samples[2 * x], (byte)samples[2 * x + 1]);
                break;
            case 2:
                (rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]) = ((byte)samples[3 * x], (byte)samples[3 * x + 1], (byte)samples[3 * x + 2], 255);
                break;
            default:
                for (var c = 0; c < 4; c++) rgba[o + c] = (byte)samples[4 * x + c];
                break;
        }
    }

    static int Paeth(int a, int b, int c)
    {
        var p = a + b - c;
        int pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }

    static readonly uint[] crcTable = Enumerable.Range(0, 256).Select(n =>
    {
        var c = (uint)n;
        for (var k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var b in data) crc = crcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
    }

    // The full BC3 mip chain (largest level first) of straight-alpha RGBA pixels.
    public static byte[] Bc3Mips(int width, int height, byte[] rgba)
    {
        using var output = new MemoryStream();
        foreach (var (w, h, level) in RgbaLevels(width, height, rgba))
            for (var by = 0; by < Math.Max(h, 4); by += 4)
                for (var bx = 0; bx < Math.Max(w, 4); bx += 4)
                    WriteBc3Block(output, level, w, h, bx, by);
        return output.ToArray();
    }

    // The BC3 block at (bx, by) of a w x h level; edge blocks repeat the last row and column.
    static void WriteBc3Block(MemoryStream output, byte[] level, int w, int h, int bx, int by)
    {
        var alpha = new int[16];
        var colours = new (int R, int G, int B)[16];
        for (var k = 0; k < 16; k++)
        {
            var o = 4 * (Math.Min(by + k / 4, h - 1) * w + Math.Min(bx + k % 4, w - 1));
            alpha[k] = level[o + 3];
            colours[k] = (level[o], level[o + 1], level[o + 2]);
        }
        // Fully transparent pixels take a visible pixel's colour, so they do not pull the colour endpoints.
        var visible = Array.FindIndex(alpha, a => a > 0);
        for (var k = 0; k < 16; k++) if (alpha[k] == 0) colours[k] = visible >= 0 ? colours[visible] : (0, 0, 0);
        output.Write(AlphaBlock(alpha));
        output.Write(Block(colours));
    }

    // One BC3 alpha block: the highest and lowest alpha as endpoints, eight-value mode, nearest index per pixel.
    static byte[] AlphaBlock(int[] alpha)
    {
        var block = new byte[8];
        int a0 = alpha.Max(), a1 = alpha.Min();
        (block[0], block[1]) = ((byte)a0, (byte)a1);
        if (a0 == a1) return block;
        var palette = new int[8];
        palette[0] = a0; palette[1] = a1;
        for (var i = 1; i < 7; i++) palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
        ulong indices = 0;
        for (var k = 0; k < 16; k++)
        {
            int nearest = 0, best = int.MaxValue;
            for (var n = 0; n < 8; n++)
                if (Math.Abs(alpha[k] - palette[n]) < best) (nearest, best) = (n, Math.Abs(alpha[k] - palette[n]));
            indices |= (ulong)nearest << (3 * k);
        }
        for (var i = 0; i < 6; i++) block[2 + i] = (byte)(indices >> (8 * i));
        return block;
    }

    // Every mip level (largest first) of straight-alpha RGBA pixels, box-filtered with alpha weighting so transparent pixels
    // do not darken the edges.
    static IEnumerable<(int Width, int Height, byte[] Rgba)> RgbaLevels(int width, int height, byte[] rgba)
    {
        var level = (byte[])rgba.Clone();
        while (true)
        {
            yield return (width, height, level);
            if (width == 1 && height == 1) yield break;
            int w = Math.Max(1, width / 2), h = Math.Max(1, height / 2);
            (level, width, height) = (HalfRgba(level, width, height, w, h), w, h);
        }
    }

    // The next level (w x h) of a width x height level: each pixel the alpha-weighted mean of the 2 x 2 it covers.
    static byte[] HalfRgba(byte[] level, int width, int height, int w, int h)
    {
        var next = new byte[w * h * 4];
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
            {
                int r = 0, g = 0, b = 0, a = 0;
                for (var dy = 0; dy < 2; dy++)
                    for (var dx = 0; dx < 2; dx++)
                    {
                        var o = 4 * (Math.Min(2 * y + dy, height - 1) * width + Math.Min(2 * x + dx, width - 1));
                        int pa = level[o + 3];
                        (r, g, b, a) = (r + level[o] * pa, g + level[o + 1] * pa, b + level[o + 2] * pa, a + pa);
                    }
                var t = 4 * (y * w + x);
                if (a > 0) (next[t], next[t + 1], next[t + 2]) = ((byte)((r + a / 2) / a), (byte)((g + a / 2) / a), (byte)((b + a / 2) / a));
                next[t + 3] = (byte)((a + 2) / 4);
            }
        return next;
    }

    // The full BC1 mip chain (box-filtered, largest level first) of RGBA pixels drawn over black.
    public static byte[] Bc1Mips(int width, int height, byte[] rgba)
    {
        var pixels = new (int R, int G, int B)[width * height];
        for (var i = 0; i < pixels.Length; i++)
        {
            int a = rgba[4 * i + 3];
            pixels[i] = ((rgba[4 * i] * a + 127) / 255, (rgba[4 * i + 1] * a + 127) / 255, (rgba[4 * i + 2] * a + 127) / 255);
        }
        using var output = new MemoryStream();
        while (true)
        {
            WriteBc1Level(output, pixels, width, height);
            if (width == 1 && height == 1) return output.ToArray();
            int w = Math.Max(1, width / 2), h = Math.Max(1, height / 2);
            (pixels, width, height) = (HalfPixels(pixels, width, height, w, h), w, h);
        }
    }

    // A level's BC1 blocks, rows of blocks top to bottom; edge blocks repeat the last row and column.
    static void WriteBc1Level(MemoryStream output, (int R, int G, int B)[] pixels, int width, int height)
    {
        for (var by = 0; by < Math.Max(height, 4); by += 4)
            for (var bx = 0; bx < Math.Max(width, 4); bx += 4)
            {
                var block = new (int, int, int)[16];
                for (var y = 0; y < 4; y++)
                    for (var x = 0; x < 4; x++) block[4 * y + x] = pixels[Math.Min(by + y, height - 1) * width + Math.Min(bx + x, width - 1)];
                output.Write(Block(block));
            }
    }

    // The next level (w x h) of a width x height level: each pixel the mean of the 2 x 2 it covers.
    static (int R, int G, int B)[] HalfPixels((int R, int G, int B)[] pixels, int width, int height, int w, int h)
    {
        var next = new (int, int, int)[w * h];
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
            {
                int r = 0, g = 0, b = 0;
                for (var dy = 0; dy < 2; dy++)
                    for (var dx = 0; dx < 2; dx++)
                    {
                        var p = pixels[Math.Min(2 * y + dy, height - 1) * width + Math.Min(2 * x + dx, width - 1)];
                        (r, g, b) = (r + p.R, g + p.G, b + p.B);
                    }
                next[y * w + x] = ((r + 2) / 4, (g + 2) / 4, (b + 2) / 4);
            }
        return next;
    }

    static int To565((int R, int G, int B) c) => (c.R * 31 + 127) / 255 << 11 | (c.G * 63 + 127) / 255 << 5 | (c.B * 31 + 127) / 255;

    static (int, int, int) Expand(int v)
    {
        int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
        return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
    }

    // One BC1 block: the two most distant colours as endpoints, four-colour mode.
    static byte[] Block((int R, int G, int B)[] pixels)
    {
        var block = new byte[8];
        var distinct = pixels.Distinct().ToList();
        if (distinct.Count == 1)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(block, (ushort)To565(distinct[0]));
            BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(2), (ushort)To565(distinct[0]));
            return block;
        }
        var best = -1;
        var ends = (pixels[0], pixels[0]);
        for (var i = 0; i < distinct.Count; i++)
            for (var j = i + 1; j < distinct.Count; j++)
            {
                var (a, b) = (distinct[i], distinct[j]);
                var d = (a.R - b.R) * (a.R - b.R) + (a.G - b.G) * (a.G - b.G) + (a.B - b.B) * (a.B - b.B);
                if (d > best) (best, ends) = (d, (a, b));
            }
        int c0 = To565(ends.Item1), c1 = To565(ends.Item2);
        BinaryPrimitives.WriteUInt16LittleEndian(block, (ushort)Math.Max(c0, c1));
        BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(2), (ushort)Math.Min(c0, c1));
        if (c0 == c1) return block;
        if (c0 < c1) (c0, c1) = (c1, c0);
        var (p0, p1) = (Expand(c0), Expand(c1));
        (int, int, int)[] palette = [p0, p1, ((2 * p0.Item1 + p1.Item1 + 1) / 3, (2 * p0.Item2 + p1.Item2 + 1) / 3, (2 * p0.Item3 + p1.Item3 + 1) / 3),
            ((p0.Item1 + 2 * p1.Item1 + 1) / 3, (p0.Item2 + 2 * p1.Item2 + 1) / 3, (p0.Item3 + 2 * p1.Item3 + 1) / 3)];
        uint indices = 0;
        for (var k = 0; k < 16; k++)
        {
            var p = pixels[k];
            int nearest = 0, nearestDistance = int.MaxValue;
            for (var n = 0; n < 4; n++)
            {
                var (r, g, b) = palette[n];
                var d = (p.R - r) * (p.R - r) + (p.G - g) * (p.G - g) + (p.B - b) * (p.B - b);
                if (d < nearestDistance) (nearest, nearestDistance) = (n, d);
            }
            indices |= (uint)nearest << (2 * k);
        }
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(4), indices);
        return block;
    }

    // The game's stratagem icon convention: the icon shader treats the texture's channels as masks, each coloured by the
    // UI: R the category-coloured artwork, G the white artwork, B a shadow (unused), A opaque; 0 everywhere is empty.
    const int MaskBlueMax = 128;
    static readonly int[] MaskDark = [25, 3, 1], MaskRed = [220, 100, 85], MaskWhite = [255, 255, 238];

    // Drawn in the mask convention: opaque, and no blue above MaskBlueMax (an edited mask may keep a few bluish edges).
    public static bool IsIconMasks(byte[] rgba)
    {
        for (var i = 0; i < rgba.Length; i += 4)
            if (rgba[i + 2] > MaskBlueMax || rgba[i + 3] != 255) return false;
        return true;
    }

    // A red-and-white icon picture as masks: R its red share, G its white share (least squares against the reference
    // colours over the dark one, each clamped to 0..1 and scaled down together past 1), B 0, A 255.
    public static byte[] IconMasks(byte[] rgba)
    {
        var output = new byte[rgba.Length];
        var cache = new Dictionary<(int, int, int), (byte, byte)>();
        int[] s = [.. Enumerable.Range(0, 3).Select(c => MaskRed[c] - MaskDark[c])], k = [.. Enumerable.Range(0, 3).Select(c => MaskWhite[c] - MaskDark[c])];
        long ss = s.Sum(x => (long)x * x), kk = k.Sum(x => (long)x * x), sk = s[0] * k[0] + s[1] * k[1] + s[2] * k[2];
        var det = ss * kk - sk * sk;
        for (var i = 0; i < rgba.Length; i += 4)
        {
            int alpha = rgba[i + 3];
            var key = ((rgba[i] * alpha + 127) / 255, (rgba[i + 1] * alpha + 127) / 255, (rgba[i + 2] * alpha + 127) / 255);
            if (!cache.TryGetValue(key, out var mask))
            {
                int[] p = [Math.Max(0, key.Item1 - MaskDark[0]), Math.Max(0, key.Item2 - MaskDark[1]), Math.Max(0, key.Item3 - MaskDark[2])];
                long ps = p[0] * s[0] + p[1] * s[1] + p[2] * s[2], pk = p[0] * k[0] + p[1] * k[1] + p[2] * k[2];
                double a = Math.Min(1.0, Math.Max(0.0, (double)(ps * kk - pk * sk) / det)), w = Math.Min(1.0, Math.Max(0.0, (double)(pk * ss - ps * sk) / det));
                if (a + w > 1) (a, w) = (a / (a + w), w / (a + w));
                cache[key] = mask = ((byte)(int)(a * 255 + 0.5), (byte)(int)(w * 255 + 0.5));
            }
            (output[i], output[i + 1], output[i + 2], output[i + 3]) = (mask.Item1, mask.Item2, 0, 255);
        }
        return output;
    }
}
