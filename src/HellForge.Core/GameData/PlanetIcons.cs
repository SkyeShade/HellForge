using System.Buffers.Binary;
using System.Numerics;
using System.Text;

namespace HellForge.Core.GameData;

// Round planet icons for the launcher's Galactic war page, drawn from each planet's galaxy map material (data/planets.json
// "hologram"; all are children of the hologram planet material 3e3b80de588e993f) as its shader draws it, onto a lit sphere
// in software. Read from the player's own game data; the launcher keeps the icons in the installation's temp folder,
// never shipped. HD2Mods research/planet-modifiers/planet-icons has how the shader was read and the prototype.
//
// The material: its texture_map (512x256 BC3, equirectangular: R grey terrain, G land and cloud mask, A city lines) and
// parameter values, the parent's where the child has none. Planets that share a material show another face of it.
public static class PlanetIcons
{
    const int TextureHeader = 0xC0;
    static readonly string[] ParamNames = ["cloud_opacity", "use_channel_r_for_water", "water_contrast", "ambient_amount", "hologram_color",
        "cloud_contrast", "hologram_color_01", "color_tint", "uv_rotate_01", "diffuse_intensity", "city_emissive_color", "destruction_amount",
        "sun_col", "city_emissive_amount", "cloud_rotate", "cloud_color", "uv_rotate_02", "water_color", "water_opacity", "water_rotate"];
    static readonly Dictionary<uint, string> Names = ParamNames.ToDictionary(Thin);
    static readonly uint TextureMap = Thin("texture_map");
    static uint Thin(string name) => (uint)(EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name)) >> 32);

    sealed record Material(ulong TextureMap, Dictionary<string, float[]> Values);

    // Each planet's icon as size x size RGBA (straight alpha), by its index; a planet whose material or map the game data
    // lacks is left out.
    public static Dictionary<int, byte[]> Render(string gameData, IEnumerable<(int Index, ulong Material)> planets, int size)
    {
        var wanted = planets.Where(p => p.Material != 0).ToList();
        var materials = new Dictionary<ulong, Material?>();
        var maps = new Dictionary<ulong, Map?>();
        var jobs = new List<(int Index, Material Material, Map Map)>();
        var located = new Dictionary<ulong, GameDataReader.Location>();
        using (var reader = new GameDataReader(gameData))
        {
            // one look-up for every material (each scans the index); their parents follow one by one
            foreach (var (key, at) in reader.Find(wanted.Select(p => new ResourceKey(ResourceTypes.Material, p.Material)).Distinct())) located[key.Name] = at;
            foreach (var (index, name) in wanted)
            {
                var material = ReadMaterial(reader, name, located, materials, 0);
                if (material == null) continue;
                if (!maps.TryGetValue(material.TextureMap, out var map)) maps[material.TextureMap] = map = ReadMap(reader, material.TextureMap);
                if (map != null) jobs.Add((index, material, map));
            }
        }
        // the reading is done; the drawing is pure arithmetic, one planet a thread
        var icons = new System.Collections.Concurrent.ConcurrentDictionary<int, byte[]>();
        Parallel.ForEach(jobs, job => icons[job.Index] = Draw(job.Material, job.Map, size, job.Index == 0 ? 0 : job.Index * 0.618f % 1));
        return new Dictionary<int, byte[]>(icons);
    }

    // Texture count at 0x40 and parameter count at 0x68; slot ids from 0x88, then the textures; then 20-byte parameter
    // records (type, count, name, offset, stride) and their values. A child takes its parent's (0x18) where it has none.
    // located: the materials Render found in one look-up; read: those parsed already (a parent is shared by many).
    static Material? ReadMaterial(GameDataReader reader, ulong name, Dictionary<ulong, GameDataReader.Location> located, Dictionary<ulong, Material?> read, int depth)
    {
        if (read.TryGetValue(name, out var known)) return known;
        var material = depth > 8 ? null : ParseMaterial(reader, name, located, read, depth);
        read[name] = material;
        return material;
    }

    static Material? ParseMaterial(GameDataReader reader, ulong name, Dictionary<ulong, GameDataReader.Location> located, Dictionary<ulong, Material?> read, int depth)
    {
        if (!located.TryGetValue(name, out var location))
        {
            var found = reader.Find([new ResourceKey(ResourceTypes.Material, name)]);
            if (found.Count == 0) return null;
            location = found.Values.First();
        }
        var main = reader.Read(location).Main;
        if (main.Length < 0x88) return null;
        int textures = BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(0x40)), parameters = BinaryPrimitives.ReadInt32LittleEndian(main.AsSpan(0x68));
        var records = 0x88 + 12 * textures;
        if (textures is < 0 or > 64 || parameters is < 0 or > 512 || records + 20 * parameters > main.Length) return null;
        var map = MapSlot(main, textures);
        var values = Values(main, records, parameters);
        var parent = BinaryPrimitives.ReadUInt64LittleEndian(main.AsSpan(0x18));
        if (parent != 0 && ReadMaterial(reader, parent, located, read, depth + 1) is { } from)
        {
            if (map == 0) map = from.TextureMap;
            foreach (var (k, v) in from.Values) values.TryAdd(k, v);
        }
        return map == 0 ? null : new Material(map, values);
    }

    // The texture in the material's texture_map slot, 0 none.
    static ulong MapSlot(byte[] main, int textures)
    {
        for (var i = 0; i < textures; i++)
            if (BinaryPrimitives.ReadUInt32LittleEndian(main.AsSpan(0x88 + 4 * i)) == TextureMap) return BinaryPrimitives.ReadUInt64LittleEndian(main.AsSpan(0x88 + 4 * textures + 8 * i));
        return 0;
    }

    // The parameters the icon uses (float to float4), by name.
    static Dictionary<string, float[]> Values(byte[] main, int records, int parameters)
    {
        var values = new Dictionary<string, float[]>();
        var start = records + 20 * parameters;
        for (var i = 0; i < parameters; i++)
        {
            var record = main.AsSpan(records + 20 * i);
            uint type = BinaryPrimitives.ReadUInt32LittleEndian(record), key = BinaryPrimitives.ReadUInt32LittleEndian(record[8..]);
            var at = start + (int)BinaryPrimitives.ReadUInt32LittleEndian(record[12..]);
            if (type > 3 || !Names.TryGetValue(key, out var parameter) || at + 4 * (type + 1) > main.Length) continue;
            values[parameter] = [.. Enumerable.Range(0, (int)type + 1).Select(c => BinaryPrimitives.ReadSingleLittleEndian(main.AsSpan(at + 4 * c)))];
        }
        return values;
    }

    // The map's mip 0 as floats. A streamed texture's stream part holds the whole mip chain, an unstreamed one's gpu part;
    // a shorter part holds only the chain's tail.
    static Map? ReadMap(GameDataReader reader, ulong name)
    {
        var found = reader.Find([new ResourceKey(ResourceTypes.Texture, name)]);
        if (found.Count == 0) return null;
        var parts = reader.Read(found.Values.First());
        if (parts.Main.Length < TextureHeader + 148) return null;
        var dds = parts.Main.AsSpan(TextureHeader);
        if (!dds[..4].SequenceEqual("DDS "u8)) return null;
        int height = BinaryPrimitives.ReadInt32LittleEndian(dds[12..]), width = BinaryPrimitives.ReadInt32LittleEndian(dds[16..]);
        int mips = Math.Max(1, BinaryPrimitives.ReadInt32LittleEndian(dds[28..])), format = BinaryPrimitives.ReadInt32LittleEndian(dds[128..]);
        if (width is <= 0 or > 4096 || height is <= 0 or > 4096 || mips > 16) return null;
        long chain = 0;
        for (int i = 0, w = width, h = height; i < mips; i++, w = Math.Max(1, w / 2), h = Math.Max(1, h / 2)) chain += LevelSize(format, w, h);
        var all = parts.Stream.Length >= chain ? parts.Stream : parts.Gpu;
        var offset = all.Length - chain;
        var top = LevelSize(format, width, height);
        if (offset < 0 || offset + top > all.Length) return null;
        var rgba = EditorIcons.Decode(new EditorIcons.Page(all.AsSpan((int)offset, (int)top).ToArray(), width, height, format), 0, 0, width, height);
        return rgba == null ? null : new Map(width, height, [.. rgba.Select(b => b / 255f)]);
    }

    static long LevelSize(int format, int width, int height)
    {
        var block = format switch { 71 or 72 or 80 or 81 => 8, 77 or 78 or 83 or 84 => 16, _ => 0 };
        return block > 0 ? (long)((width + 3) / 4) * ((height + 3) / 4) * block : (long)width * height * 4;
    }

    // Bilinear, u wraps, v clamps.
    sealed class Map(int width, int height, float[] rgba)
    {
        public Vector4 Sample(float u, float v)
        {
            var x = u * width - 0.5f;
            var y = Math.Clamp(v * height - 0.5f, 0, height - 1);
            int x0 = (int)MathF.Floor(x), y0 = (int)MathF.Floor(y), y1 = Math.Min(y0 + 1, height - 1);
            float fx = x - x0, fy = y - y0;
            int xa = (x0 % width + width) % width, xb = (xa + 1) % width;
            return Vector4.Lerp(Vector4.Lerp(At(xa, y0), At(xb, y0), fx), Vector4.Lerp(At(xa, y1), At(xb, y1), fx), fy);
        }

        Vector4 At(int x, int y)
        {
            var i = (y * width + x) * 4;
            return new(rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]);
        }
    }

    static Vector3 V3(float[] f) => f.Length >= 3 ? new(f[0], f[1], f[2]) : new(f[0]);
    static float Sat(float x) => Math.Clamp(x, 0, 1);
    static readonly Vector3 Luminance = new(0.212f, 0.715f, 0.072f);
    // The icon's own look around the shader: a light from the upper left, a softer terminator, some ambient, the north pole
    // tilted 18 degrees towards the viewer, a thin halo (8% of the radius) and 3x3 samples a pixel.
    const float Ambient = 0.22f, Terminator = 0.35f, Tilt = 18f, Pad = 0.08f, Exposure = 1.25f;
    const int Samples = 3;
    static readonly Vector3 Light = Vector3.Normalize(new(-0.62f, 0.55f, 0.56f));

    static byte[] Draw(Material m, Map map, int size, float longitude)
    {
        var shade = new Shader(m, map);
        float tilt = Tilt * MathF.PI / 180f, ct = MathF.Cos(tilt), st = MathF.Sin(tilt), radius = 1f / (1f + Pad);
        var output = new byte[size * size * 4];
        for (var py = 0; py < size; py++)
            for (var px = 0; px < size; px++)
            {
                var acc = Vector3.Zero;
                float alpha = 0;
                for (var sy = 0; sy < Samples; sy++)
                    for (var sx = 0; sx < Samples; sx++)
                    {
                        var (colour, a) = Sample(((px + (sx + 0.5f) / Samples) / size * 2 - 1) / radius, -((py + (sy + 0.5f) / Samples) / size * 2 - 1) / radius);
                        acc += colour * a;
                        alpha += a;
                    }
                if (alpha <= 0) continue;
                var c = acc / alpha * Exposure;
                var i = (py * size + px) * 4;
                (output[i], output[i + 1], output[i + 2]) = (Encode(c.X), Encode(c.Y), Encode(c.Z));
                output[i + 3] = (byte)Math.Clamp(MathF.Round(alpha / (Samples * Samples) * 255), 0, 255);
            }
        return output;

        // one sample of the disc (x and y from -1 to 1, y up): the sphere's colour, or the halo's with its coverage
        (Vector3 Colour, float Alpha) Sample(float x, float y)
        {
            var r2 = x * x + y * y;
            if (r2 > 1) return shade.Halo(x, y, MathF.Sqrt(r2), radius);
            var n = new Vector3(x, y, MathF.Sqrt(1 - r2));
            var o = new Vector3(n.X, n.Y * ct - n.Z * st, n.Y * st + n.Z * ct);
            var u = MathF.Atan2(o.X, o.Z) / (2 * MathF.PI) + 0.5f + longitude;
            var v = 0.5f - MathF.Asin(Math.Clamp(o.Y, -1, 1)) / MathF.PI;
            return (shade.Surface(n, u, v), 1);
        }
    }

    // Linear colour through a soft shoulder above 0.8, to sRGB.
    static byte Encode(float x)
    {
        x = MathF.Max(x, 0);
        if (x > 0.8f) x = 0.8f + 0.2f * (1 - MathF.Exp(-(x - 0.8f) / 0.2f));
        x = Sat(x);
        x = x <= 0.0031308f ? 12.92f * x : 1.055f * MathF.Pow(x, 1 / 2.4f) - 0.055f;
        return (byte)Math.Clamp(MathF.Round(x * 255), 0, 255);
    }

    // The hologram planet shader's colour steps with one material's values (water and land, clouds, lighting with its
    // sun colour, the fresnel rim, the tint, city lights and destruction), plus the icon's atmosphere.
    sealed class Shader
    {
        readonly Map map;
        readonly Vector3 low, high, water, tint, tintMul, cloudColour, city, sun, rim, atmosphere;
        readonly float waterOpacity, waterContrast, cloudOpacity, cloudContrast, rotateWater, rotateCloud, rotate1, rotate2, cityAmount, diffuse, destruction, ambient, tintAmount;
        readonly bool waterFromRed;

        public Shader(Material m, Map map)
        {
            this.map = map;
            float F(string n, float d = 0) => m.Values.TryGetValue(n, out var v) ? v[0] : d;
            Vector3 C(string n, Vector3 d) => m.Values.TryGetValue(n, out var v) ? V3(v) : d;
            (high, low) = (C("hologram_color", new(0.5f)), C("hologram_color_01", new(0.5f)));
            (water, tint, cloudColour, city, sun) = (C("water_color", Vector3.Zero), C("color_tint", Vector3.One), C("cloud_color", Vector3.One),
                C("city_emissive_color", Vector3.One), C("sun_col", Vector3.One));
            (waterOpacity, waterContrast, cloudOpacity, cloudContrast) = (F("water_opacity"), F("water_contrast", 1), F("cloud_opacity"), F("cloud_contrast", 1));
            (rotateWater, rotateCloud, rotate1, rotate2) = (F("water_rotate"), F("cloud_rotate"), F("uv_rotate_01"), F("uv_rotate_02"));
            (cityAmount, diffuse, destruction, waterFromRed) = (F("city_emissive_amount"), F("diffuse_intensity", 1), F("destruction_amount"), F("use_channel_r_for_water") > 0.5f);
            ambient = MathF.Max(F("ambient_amount", 0.1f), Ambient);
            var tintLum = Vector3.Dot(tint, Luminance);
            var k = tintLum * 0.64f + 0.01f;
            (tintMul, tintAmount) = (Vector3.Lerp(tint, Vector3.One, k), (1 - k) * (1 - Sat(tintLum)));
            rim = (high + low) * 0.25f;
            var glow = rim * 2 + water * 0.5f + new Vector3(0.05f);
            atmosphere = glow / MathF.Max(MathF.Max(glow.X, MathF.Max(glow.Y, glow.Z)), 1e-3f) * 0.9f;
        }

        // A thin halo outside the disc, brighter towards the light.
        public (Vector3 Colour, float Alpha) Halo(float x, float y, float r, float radius)
        {
            var t = (r - 1) / (Pad / radius * 0.9f);
            if (t >= 1) return (Vector3.Zero, 0);
            var side = Sat(0.35f + 0.65f * (x * Light.X + y * Light.Y) / r);
            return (atmosphere, MathF.Pow(1 - t, 3) * 0.55f * side);
        }

        public Vector3 Surface(Vector3 n, float u, float v)
        {
            var s = map.Sample(u + rotateWater, v);
            var cl = map.Sample(u + rotateCloud, v);
            var a = map.Sample(u + rotate1, v);
            var b = map.Sample(u + rotate2, v);
            var land = Sat((s.Y * waterContrast - 0.5f) * waterOpacity + 0.5f);
            var ground = low + (a.Y - b.X) * (high - low);
            var sea = (waterFromRed ? new Vector3(s.X) : new Vector3(s.X, s.Y, s.Z)) * water;
            var col = Vector3.Lerp(sea, ground, land);
            col = Vector3.Max(Vector3.Lerp(col, cloudColour, Sat((cl.Y * cloudOpacity - 0.5f) * cloudContrast + 0.5f)), Vector3.Zero);
            var ndl = Sat((Vector3.Dot(n, Light) + Terminator) / (1 + Terminator));
            ndl = ndl * ndl * (3 - 2 * ndl);
            var lit = col * ndl * diffuse * sun;
            col = lit + ambient * Vector3.Max(col - lit, Vector3.Zero);
            var f5 = MathF.Pow(1 - MathF.Abs(n.Z), 5);
            col += rim * f5 * (0.6f + 0.4f * ndl);
            col = Vector3.Lerp(col, atmosphere, f5 * 0.5f * (0.3f + 0.7f * ndl));
            col = Vector3.Lerp(col, tint, Sat(Vector3.Dot(col, Luminance)) * tintAmount);
            col = col * tintMul + Sat(a.W - b.Z) * cityAmount * city;
            if (destruction > 0)
            {
                var hot = Sat(s.Y + 0.2f - (1 - s.X)) * cl.X;
                col = Vector3.Lerp(col, new Vector3(12 * hot, 2 * hot, 0) + col * 0.5f, destruction);
            }
            return col;
        }
    }
}
