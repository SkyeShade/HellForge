using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// A texture of a mod's own from PNG files (a data spec's "texture"; docs/modding/textures.md): one PNG, or one per layer
// of an array texture, in a format the game's own textures use. Every game texture is UNORM (shaders decode colour
// themselves), so none is sRGB. The texture is written whole, without streamed mips, as half of the game's textures
// are (up to 4096 x 4096).
public static class TextureImage
{
    public static readonly IReadOnlyDictionary<string, int> Formats = new Dictionary<string, int>
    {
        ["rgba8"] = 28, ["bc1"] = 71, ["bc3"] = 77, ["bc4"] = 80, ["bc5"] = 83,
    };

    // The game's largest unstreamed textures, and the most ReadPng inflates.
    public const int MaxSize = 4096;

    // The first header word names the texture's category, the thin hash of core/texture_categories/<name>; 0, which
    // textures of every format use, is none.
    public static uint Category(string name) => EngineHash.Id32("core/texture_categories/" + name);

    public static ResourceParts Build(IReadOnlyList<byte[]> pngs, string format, uint category = 0)
    {
        if (!Formats.TryGetValue(format, out var dxgi)) throw new InvalidDataException($"a texture's format is {string.Join(", ", Formats.Keys)}, not {format}.");
        if (pngs.Count == 0) throw new InvalidDataException("a texture needs a PNG.");
        var layers = pngs.Select(IconImage.ReadPng).ToList();
        var (width, height, _) = layers[0];
        if (layers.Any(l => l.Width != width || l.Height != height)) throw new InvalidDataException("an array texture's layers are all the same size.");
        if (width > MaxSize || height > MaxSize) throw new InvalidDataException($"a texture is at most {MaxSize} x {MaxSize} pixels.");
        var mips = MipCount(width, height);
        using var gpu = new MemoryStream();
        foreach (var layer in layers) gpu.Write(Chain(format, width, height, layer.Rgba));
        return new(Main(width, height, mips, dxgi, category, layers.Count), gpu.ToArray(), []);
    }

    // Halving (rounding down) to 1 x 1, as the game's 23 x 8 colour lookups have 5 levels.
    public static int MipCount(int width, int height)
    {
        var n = 1;
        for (var s = Math.Max(width, height); s > 1; s /= 2) n++;
        return n;
    }

    // A layer's full mip chain, largest level first. Each channel is filtered on its own (a box filter, as the game's
    // mask arrays are), since these textures hold data such as normals and masks rather than colour over alpha.
    static byte[] Chain(string format, int width, int height, byte[] rgba)
    {
        switch (format)
        {
            case "bc1": return IconImage.Bc1Mips(width, height, Opaque(rgba));
            case "bc3": return IconImage.Bc3Mips(width, height, rgba, data: true);
            case "bc5": return IconImage.Bc5Mips(width, height, rgba);
        }
        using var output = new MemoryStream();
        foreach (var (w, h, level) in IconImage.RgbaLevels(width, height, rgba, data: true))
            if (format == "rgba8") output.Write(level);
            else WriteBc4Level(output, w, h, level);
        return output.ToArray();
    }

    static byte[] Opaque(byte[] rgba)
    {
        var copy = (byte[])rgba.Clone();
        for (var i = 3; i < copy.Length; i += 4) copy[i] = 255;
        return copy;
    }

    // BC4 keeps the red channel; edge blocks repeat the last row and column.
    static void WriteBc4Level(MemoryStream output, int w, int h, byte[] level)
    {
        for (var by = 0; by < Math.Max(h, 4); by += 4)
            for (var bx = 0; bx < Math.Max(w, 4); bx += 4)
            {
                var values = new int[16];
                for (var k = 0; k < 16; k++) values[k] = level[4 * (Math.Min(by + k / 4, h - 1) * w + Math.Min(bx + k % 4, w - 1))];
                output.Write(IconImage.AlphaBlock(values));
            }
    }

    // The main part as the game's unstreamed textures have it: {category, 0, -1 (no streamed mips)} in a 0xC0 header,
    // then a DDS header with the DX10 extension. RGBA8 textures give their row pitch; a single level drops the mip flags.
    public static byte[] Main(int width, int height, int mips, int dxgi, uint category, int layers)
    {
        var data = new byte[0xC0 + 148];
        void U32(int at, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(at), value);
        U32(0, category);
        U32(8, 0xFFFFFFFF);
        "DDS "u8.CopyTo(data.AsSpan(0xC0));
        var dds = 0xC4;
        var pitch = dxgi == Formats["rgba8"] ? (uint)width * 4 : 0;
        var mipped = mips > 1;
        foreach (var (at, value) in new[] { (0, 124u), (4, mipped ? 0x21007u : 0x1007u), (8, (uint)height), (12, (uint)width), (16, pitch), (24, (uint)mips),
            (72, 32u), (76, 4u), (80, 0x30315844u), (104, mipped ? 0x401008u : 0x1000u), (124, (uint)dxgi), (128, 3u), (136, (uint)layers) })
            U32(dds + at, value);
        return data;
    }
}
