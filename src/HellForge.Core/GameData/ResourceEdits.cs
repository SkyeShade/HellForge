using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// The edits a data spec makes to a copied resource. Each checks the resource is shaped as expected first and refuses
// anything else (InvalidDataException), as the research builders' pin checks did.
public static class ResourceEdits
{
    // Every u64 equal to `from` becomes `to` (a reference to another resource: a unit's material, a material's
    // texture, an effect's material). With `at`, exactly those offsets, each of which must hold `from`.
    public static int Replace(byte[] data, ulong from, ulong to, IReadOnlyList<int>? at = null)
    {
        var offsets = at?.ToList() ?? [];
        if (at == null)
            for (var o = 0; o + 8 <= data.Length; o++)
                if (BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(o)) == from) offsets.Add(o);
        foreach (var o in offsets)
            if (o < 0 || o + 8 > data.Length || BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(o)) != from)
                throw new InvalidDataException($"0x{from:X16} is not at 0x{o:X}.");
        if (offsets.Count == 0) throw new InvalidDataException($"0x{from:X16} is not referenced.");
        foreach (var o in offsets) BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(o), to);
        return offsets.Count;
    }

    // A particle effect's colours are curves over the particle's life: 10 key times on 0..1 (unused keys 10000, after
    // the used ones) followed by 10 RGB keys on a 0..255 scale (up to 1024 for HDR; artifacts/projectile-registry/
    // colour_curves.py). Each warm key of such a curve (red >= green >= blue, red > blue, brightest channel >= 2) is
    // recoloured; white and every other key stay as they are. Returns the number of keys changed.
    public static int RecolourCurves(byte[] effect, Func<float, float, float, (float R, float G, float B)> recolour)
    {
        var n = effect.Length / 4;
        var f = new float[n];
        for (var k = 0; k < n; k++) f[k] = BinaryPrimitives.ReadSingleLittleEndian(effect.AsSpan(4 * k));
        int changed = 0, i = 0;
        while (i + 40 <= n)
        {
            if (!IsColourCurve(f.AsSpan(i, 40), out var used)) { i++; continue; }
            for (var k = 0; k < used; k++)
            {
                float r = f[i + 10 + 3 * k], g = f[i + 11 + 3 * k], b = f[i + 12 + 3 * k];
                if (Math.Max(r, Math.Max(g, b)) < 2f || !(r >= g && g >= b && r > b)) continue;
                var (nr, ng, nb) = recolour(r, g, b);
                var at = effect.AsSpan(4 * (i + 10 + 3 * k));
                BinaryPrimitives.WriteSingleLittleEndian(at, nr);
                BinaryPrimitives.WriteSingleLittleEndian(at[4..], ng);
                BinaryPrimitives.WriteSingleLittleEndian(at[8..], nb);
                changed++;
            }
            i += 40;
        }
        return changed;
    }

    static bool IsColourCurve(ReadOnlySpan<float> c, out int used)
    {
        used = 0;
        while (used < 10 && c[used] != 10000f) used++;
        if (used is 0 or 10) return false;
        for (var k = used; k < 10; k++) if (c[k] != 10000f) return false;
        for (var k = 0; k < used; k++) if (!(c[k] >= 0f && c[k] <= 1f) || (k > 0 && c[k] < c[k - 1])) return false;
        return ColourKeys(c[10..40]);
    }

    // A curve's 30 colour values: each on 0..1024, the brightest at least 2.
    static bool ColourKeys(ReadOnlySpan<float> values)
    {
        var max = float.MinValue;
        foreach (var v in values)
        {
            if (!(v >= 0f && v <= 1024f)) return false;
            max = Math.Max(max, v);
        }
        return max >= 2f;
    }

    // The Talon research's blue: red and blue swapped, green softened, so yellow-orange (255, 184, 15) is (15, 110, 255).
    public static (float, float, float) WarmToBlue(float r, float g, float b) => (b, (float)(g * 0.6), r);

    // The hue and saturation of `colour` (RGB, any scale), at the key's own brightness.
    public static Func<float, float, float, (float, float, float)> TowardsColour(double[] colour)
    {
        var max = colour.Max();
        return (r, g, b) =>
        {
            var value = Math.Max(r, Math.Max(g, b)) / max;
            return ((float)(colour[0] * value), (float)(colour[1] * value), (float)(colour[2] * value));
        };
    }

    // Sets RGB parameters of a material by their name (IdString32). The parameter records {type, count, name, buffer
    // offset, stride} follow the texture list (0x88 + 12 x textures, the count at 0x40; parameter count at 0x68); the
    // values they point into follow them directly (every one of the game's materials fits so; 16-byte alignment, which an
    // earlier HellForge assumed, overruns a third of them). Every named parameter must be there and be RGB (type 2).
    public static void SetMaterialColours(byte[] material, IReadOnlyDictionary<uint, double[]> colours)
    {
        if (material.Length < 0x88) throw new InvalidDataException("not a material (too short).");
        var textures = BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(0x40));
        var parameters = BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(0x68));
        var records = 0x88 + 12L * textures;
        var buffer = records + 20L * parameters;
        if (textures < 0 || parameters < 0 || buffer > material.Length) throw new InvalidDataException("the material's parameter list is malformed.");
        var written = new HashSet<uint>();
        for (var i = 0; i < parameters; i++)
        {
            var record = material.AsSpan((int)(records + 20 * i), 20);
            var name = BinaryPrimitives.ReadUInt32LittleEndian(record[8..]);
            if (!colours.TryGetValue(name, out var rgb)) continue;
            if (BinaryPrimitives.ReadUInt32LittleEndian(record) != 2) throw new InvalidDataException($"parameter 0x{name:X8} is not an RGB value.");
            var at = buffer + BinaryPrimitives.ReadUInt32LittleEndian(record[12..]);
            if (at + 12 > material.Length) throw new InvalidDataException($"parameter 0x{name:X8} is outside the material.");
            for (var c = 0; c < 3; c++) BinaryPrimitives.WriteSingleLittleEndian(material.AsSpan((int)at + 4 * c), (float)rgb[c]);
            written.Add(name);
        }
        var missing = colours.Keys.Where(k => !written.Contains(k)).Select(k => $"0x{k:X8}").ToList();
        if (missing.Count > 0) throw new InvalidDataException("the material has no parameter " + string.Join(", ", missing) + ".");
    }

    // A material of another game build in the layout of `parent`, the game copy's material it derives from (+0x18): the
    // parent's texture slots and parameters, each taking the source's texture or value of the same name (the parent's own
    // where the source has none), so the shader the game copy has reads every value where it expects it. Slots and
    // parameters the parent's shader does not have are left out. Returns the source unchanged when the layouts match.
    public static byte[] MaterialInLayout(byte[] source, byte[] parent)
    {
        var from = Material.Read(source);
        var layout = Material.Read(parent);
        if (from.Parent == 0) throw new InvalidDataException("the material is a shader of its own (no parent); HellForge cannot port shaders.");
        if (from.Textures.Select(t => t.Name).SequenceEqual(layout.Textures.Select(t => t.Name))
            && from.Parameters.Select(r => (r.Name, r.Type)).OrderBy(r => r.Name).SequenceEqual(layout.Parameters.Select(r => (r.Name, r.Type)).OrderBy(r => r.Name)))
            return source;
        var textures = from.Textures.ToDictionary(t => t.Name, t => t.Id);
        var values = from.Parameters.ToDictionary(r => r.Name);
        var output = new byte[(layout.Values + layout.ValuesSize + 15) / 16 * 16];
        parent.AsSpan(0, layout.Values + layout.ValuesSize).CopyTo(output);
        BinaryPrimitives.WriteInt32LittleEndian(output.AsSpan(0x0C), layout.Values + layout.ValuesSize - 0x18);
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(0x10), BinaryPrimitives.ReadUInt32LittleEndian(source.AsSpan(0x10)));
        BinaryPrimitives.WriteUInt64LittleEndian(output.AsSpan(0x18), from.Parent);
        for (var i = 0; i < layout.Textures.Count; i++)
            if (textures.TryGetValue(layout.Textures[i].Name, out var id))
                BinaryPrimitives.WriteUInt64LittleEndian(output.AsSpan(0x88 + 4 * layout.Textures.Count + 8 * i), id);
        foreach (var target in layout.Parameters)
        {
            if (!values.TryGetValue(target.Name, out var value) || value.Type != target.Type || value.Size != target.Size) continue;
            if (from.Values + value.Offset + value.Size > source.Length) throw new InvalidDataException($"parameter 0x{value.Name:X8} is outside the material.");
            source.AsSpan(from.Values + value.Offset, value.Size).CopyTo(output.AsSpan(layout.Values + target.Offset));
        }
        return output;
    }

    // The parts of a material's main part that a layout conversion reads.
    sealed record Material(ulong Parent, IReadOnlyList<(uint Name, ulong Id)> Textures, IReadOnlyList<MaterialParameter> Parameters, int Values, int ValuesSize)
    {
        public static Material Read(byte[] m)
        {
            if (m.Length < 0x88 || BinaryPrimitives.ReadUInt32LittleEndian(m) != 0x120) throw new InvalidDataException("not a material.");
            var textures = BinaryPrimitives.ReadInt32LittleEndian(m.AsSpan(0x40));
            var parameters = BinaryPrimitives.ReadInt32LittleEndian(m.AsSpan(0x68));
            var size = BinaryPrimitives.ReadInt32LittleEndian(m.AsSpan(0x78));
            if (textures is < 0 or > 256 || parameters is < 0 or > 1024) throw new InvalidDataException("the material's lists are malformed.");
            var records = 0x88 + 12 * textures;
            var values = records + 20 * parameters;
            if (size < 0 || values + size > m.Length) throw new InvalidDataException("the material's values are outside it.");
            var slots = Enumerable.Range(0, textures).Select(i => (BinaryPrimitives.ReadUInt32LittleEndian(m.AsSpan(0x88 + 4 * i)),
                BinaryPrimitives.ReadUInt64LittleEndian(m.AsSpan(0x88 + 4 * textures + 8 * i)))).ToList();
            var list = Enumerable.Range(0, parameters).Select(i =>
            {
                var r = m.AsSpan(records + 20 * i, 20);
                uint type = BinaryPrimitives.ReadUInt32LittleEndian(r), count = BinaryPrimitives.ReadUInt32LittleEndian(r[4..]),
                    stride = BinaryPrimitives.ReadUInt32LittleEndian(r[16..]);
                var bytes = type <= 3 ? 4 * (int)(type + 1) : checked((int)(count * stride));
                return new MaterialParameter(type, BinaryPrimitives.ReadUInt32LittleEndian(r[8..]), (int)BinaryPrimitives.ReadUInt32LittleEndian(r[12..]), bytes);
            }).ToList();
            if (list.Any(p => p.Offset < 0 || p.Offset + p.Size > size)) throw new InvalidDataException("a material parameter is outside its values.");
            return new(BinaryPrimitives.ReadUInt64LittleEndian(m.AsSpan(0x18)), slots, list, values, size);
        }
    }

    sealed record MaterialParameter(uint Type, uint Name, int Offset, int Size);

    public sealed record LutRecolour(int? ZonesFrom, IReadOnlyDictionary<int, double[]> Columns, IReadOnlyDictionary<int, IReadOnlyDictionary<int, double[]>> Rows);

    // A material's colour lookup texture (R16G16B16A16_FLOAT; each row a material zone, each column a parameter: column 0
    // the base colour; hd2_model.py): optionally every row first takes one row's values (the gold Liberator takes the
    // bare-metal zone, row 6, everywhere), then the given columns of every row and the given cells take their RGB
    // (alpha kept), and the mip chain is rebuilt (each lower mip the mean of its 2 x 2 block, kept unrounded between
    // levels). `main` is the texture's main part, whose DDS header gives the format; returns the new GPU part.
    public static byte[] RecolourLut(byte[] main, byte[] gpu, LutRecolour edit)
    {
        var (width, height, mips) = LutSize(main, gpu, edit);
        var level = RecolouredLevel(gpu, width, height, edit);
        using var output = new MemoryStream();
        var half = new byte[2];
        for (int m = 0, w = width, h = height; ; m++)
        {
            WriteLevel(output, level, half);
            if (m + 1 == mips) break;
            int nw = Math.Max(1, w / 2), nh = Math.Max(1, h / 2);
            (level, w, h) = (HalfLevel(level, w, h, nw, nh), nw, nh);
        }
        var bytes = output.ToArray();
        return bytes.Length == gpu.Length ? bytes
            : throw new InvalidDataException($"the rebuilt mip chain ({bytes.Length} bytes) differs in size from the texture's ({gpu.Length}).");
    }

    // The colour lookup texture's size and mip count from its DDS header, checked against its pixels and the edit.
    static (int Width, int Height, int Mips) LutSize(byte[] main, byte[] gpu, LutRecolour edit)
    {
        var dds = main.AsSpan().IndexOf("DDS "u8);
        if (dds < 0 || dds + 148 > main.Length) throw new InvalidDataException("the texture has no DDS header.");
        var height = BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(dds + 12));
        var width = BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(dds + 16));
        var mips = Math.Max(1, BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(dds + 28)));
        var dxgi = BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(dds + 128));
        if (dxgi != 10 || width is < 1 or > 256 || height is < 1 or > 256 || mips > 16)
            throw new InvalidDataException("the texture is not an R16G16B16A16_FLOAT colour lookup texture.");
        if ((long)width * height * 8 > gpu.Length) throw new InvalidDataException("the texture's pixels end early.");
        if (edit.ZonesFrom is { } from && from >= height) throw new InvalidDataException($"the texture has {height} rows, no row {from}.");
        if (edit.Columns.Keys.Concat(edit.Rows.Values.SelectMany(r => r.Keys)).Any(x => x >= width) || edit.Rows.Keys.Any(y => y >= height))
            throw new InvalidDataException($"the texture is {width} x {height}; a recoloured cell is outside it.");
        return (width, height, mips);
    }

    // Mip 0 recoloured: each row from ZonesFrom (or itself), then the edit's columns and cells.
    static double[][][] RecolouredLevel(byte[] gpu, int width, int height, LutRecolour edit)
    {
        double[] Pixel(int x, int y) => [.. Enumerable.Range(0, 4).Select(c => (double)BinaryPrimitives.ReadHalfLittleEndian(gpu.AsSpan((y * width + x) * 8 + 2 * c)))];
        var level = new double[height][][];
        for (var y = 0; y < height; y++)
        {
            level[y] = new double[width][];
            for (var x = 0; x < width; x++)
            {
                var px = Pixel(x, edit.ZonesFrom ?? y);
                if (edit.Columns.TryGetValue(x, out var rgb)) Array.Copy(rgb, px, 3);
                if (edit.Rows.TryGetValue(y, out var row) && row.TryGetValue(x, out rgb)) Array.Copy(rgb, px, 3);
                level[y][x] = px;
            }
        }
        return level;
    }

    static void WriteLevel(MemoryStream output, double[][][] level, byte[] half)
    {
        foreach (var row in level)
            foreach (var px in row)
                foreach (var v in px)
                {
                    BinaryPrimitives.WriteHalfLittleEndian(half, (Half)v);
                    output.Write(half);
                }
    }

    // The next mip (nw x nh) of a w x h one: each texel the mean of its 2 x 2 block, unrounded.
    static double[][][] HalfLevel(double[][][] level, int w, int h, int nw, int nh)
    {
        var next = new double[nh][][];
        for (var y = 0; y < nh; y++)
        {
            next[y] = new double[nw][];
            for (var x = 0; x < nw; x++) next[y][x] = BlockMean(level, w, h, x, y);
        }
        return next;
    }

    static double[] BlockMean(double[][][] level, int w, int h, int x, int y)
    {
        var px = new double[4];
        for (var c = 0; c < 4; c++)
        {
            var sum = 0.0;
            for (var dy = 0; dy < 2; dy++)
                for (var dx = 0; dx < 2; dx++) sum += level[Math.Min(2 * y + dy, h - 1)][Math.Min(2 * x + dx, w - 1)][c];
            px[c] = sum / 4.0;
        }
        return px;
    }
}
