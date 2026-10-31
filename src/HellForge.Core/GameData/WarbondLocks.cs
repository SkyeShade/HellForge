using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HellForge.Core.GameData;

// The premium warbonds a profile does not own look locked in the game's Acquisitions: their art (mappings/warbond-art.json:
// the untitled art Acquisitions shows as the warbond's card, with the title drawn over it by the game, and the same art
// with the title drawn in) dimmed and greyed, a padlock in the middle and a line in from each corner that stops short of it. The art is read from
// the installation's game copy, drawn on and staged as HellForge's own patch for the session (ModStaging), after the
// mods'. Ownership is the backend's (PremiumContent: the profile's sync, sealed for the Steam account signed in), so what
// looks locked is what stays locked. The game still opens a locked warbond; only its art changes. The art is Arrowhead's:
// it stays on the player's machine and is never published.
public static class WarbondLocks
{
    // Part of the cache key: raise it whenever the same warbonds would build different bytes.
    // 2: the untitled art too, which is what Acquisitions shows.
    public const int Version = 2;
    const string DoneFile = "done";
    const int Bc1 = 71;

    // Dimmed to this share of its brightness, and greyed by this much.
    const float Dim = 0.32f, Grey = 0.65f;

    // A premium warbond's art: its textures, locked when the player does not own it.
    public sealed record Art(uint Id, string Name, ulong[] Textures);

    public static IReadOnlyList<Art> Mapping()
    {
        using var stream = typeof(WarbondLocks).Assembly.GetManifestResourceStream("HellForge.Core.mappings.warbond-art.json")!;
        using var json = JsonDocument.Parse(stream);
        static ulong Name(JsonElement hex) => Convert.ToUInt64(hex.GetString()![2..], 16);
        return [.. json.RootElement.GetProperty("warbonds").EnumerateObject().Select(w => new Art(uint.Parse(w.Name, System.Globalization.CultureInfo.InvariantCulture),
            w.Value.GetProperty("name").GetString()!, [Name(w.Value.GetProperty("list")), .. w.Value.GetProperty("titled").EnumerateArray().Select(Name)]))];
    }

    // The folder of patch files that lock the art of the warbonds ownership lacks, built once per game build and set of
    // warbonds under temp/warbond-locks/ (the last one kept); null when every one is owned.
    public static string? Prepare(InstallLayout install, PremiumContent ownership, Action<string>? log = null)
    {
        var locked = Mapping().Where(w => !ownership.Owns("warbond:" + w.Id)).ToList();
        if (locked.Count == 0) return null;
        var root = Path.Combine(install.Temp, "warbond-locks");
        var key = $"{Version}:{SupportedBuild.Current.BuildId}:" + string.Join(",", locked.Select(w => w.Id).Order());
        var folder = Path.Combine(root, Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key)))[..16].ToLowerInvariant());
        if (File.Exists(Path.Combine(folder, DoneFile))) return folder;
        if (Directory.Exists(root))
            foreach (var old in Directory.GetDirectories(root)) Directory.Delete(old, recursive: true);
        using (var game = new GameDataReader(Path.Combine(install.Game, "data")))
            Build(game, locked.SelectMany(w => w.Textures), folder);
        File.WriteAllText(Path.Combine(folder, DoneFile), key + "\n");
        log?.Invoke($"Warbond art: {locked.Count} premium warbond(s) your synced account does not own are shown locked.");
        return folder;
    }

    // The textures as patch files in output: each 2048 x 1024 BC1 texture with one mip, locked, in a patch of its archive.
    // One the game data lacks, or has in another form, is left as it is.
    public static IReadOnlyList<string> Build(GameDataReader game, IEnumerable<ulong> textures, string output)
    {
        Directory.CreateDirectory(output);
        // Read one after the other (the reader is not shared across threads), then drawn and encoded side by side.
        var read = game.Find(textures.Select(texture => new ResourceKey(ResourceTypes.Texture, texture)))
            .Select(found => (found.Key, found.Value.Archive, Parts: game.Read(found.Value))).ToList();
        var locked = new ResourceParts?[read.Count];
        Parallel.For(0, read.Count, i =>
        {
            if (Texture(read[i].Parts) is not { } page) return;
            var rgba = EditorIcons.Decode(page, 0, 0, page.Width, page.Height)!;
            Draw(rgba, page.Width, page.Height);
            locked[i] = read[i].Parts with { Gpu = IconImage.Bc1Level(page.Width, page.Height, rgba) };
        });
        var byArchive = new Dictionary<string, Dictionary<ResourceKey, ResourceParts>>(StringComparer.Ordinal);
        for (var i = 0; i < read.Count; i++)
        {
            if (locked[i] is not { } parts) continue;
            if (!byArchive.TryGetValue(read[i].Archive, out var resources)) byArchive[read[i].Archive] = resources = [];
            resources[read[i].Key] = parts;
        }
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
        return files;
    }

    // A texture of the warbond art's form: its DDS header after the texture header, BC1, one mip, its pixels all in the
    // GPU part.
    static EditorIcons.Page? Texture(ResourceParts parts)
    {
        if (parts.Main.Length < 0xC0 + 148) return null;
        var dds = parts.Main.AsSpan(0xC0);
        if (!dds[..4].SequenceEqual("DDS "u8) || !dds.Slice(84, 4).SequenceEqual("DX10"u8)) return null;
        int height = (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[12..]), width = (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[16..]);
        var mips = BinaryPrimitives.ReadUInt32LittleEndian(dds[28..]);
        var format = (int)BinaryPrimitives.ReadUInt32LittleEndian(dds[128..]);
        return format == Bc1 && mips == 1 && width % 4 == 0 && height % 4 == 0 && parts.Gpu.Length == width * height / 2
            ? new(parts.Gpu, width, height, format) : null;
    }

    // Locks art: dims and greys it, then draws the padlock and the corner lines.
    public static void Draw(byte[] rgba, int width, int height)
    {
        for (var i = 0; i < width * height * 4; i += 4)
        {
            float r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
            var luma = 0.299f * r + 0.587f * g + 0.114f * b;
            rgba[i] = (byte)((r + (luma - r) * Grey) * Dim);
            rgba[i + 1] = (byte)((g + (luma - g) * Grey) * Dim);
            rgba[i + 2] = (byte)((b + (luma - b) * Grey) * Dim);
        }
        // Sizes follow the art's height: 1024 pixels in the game's warbond art.
        var unit = height / 1024f;
        float cx = width / 2f, cy = height / 2f;
        var clear = 200 * unit; /* the lines stop this far from the middle */
        var stroke = 3.5f * unit;
        foreach (var (x, y) in new[] { (0f, 0f), (width, 0f), (0f, height), (width, (float)height) })
        {
            var length = MathF.Sqrt((cx - x) * (cx - x) + (cy - y) * (cy - y));
            var (ex, ey) = (cx + (x - cx) * clear / length, cy + (y - cy) * clear / length);
            Paint(rgba, width, height, Bounds(x, y, ex, ey, stroke), (px, py) => Segment(px, py, x, y, ex, ey) - stroke, 225, 0.6f);
        }
        // The padlock: a rounded body, a shackle above it, a keyhole cut out of the body.
        float bodyW = 190 * unit, bodyH = 150 * unit, bodyY = cy + 40 * unit, corner = 22 * unit;
        float shackleR = 62 * unit, shackleW = 24 * unit, shackleY = bodyY - bodyH / 2 - 20 * unit, legs = 40 * unit;
        float keyR = 19 * unit, keyY = bodyY - 12 * unit;
        float Lock(float px, float py)
        {
            var body = RoundedBox(px - cx, py - bodyY, bodyW / 2, bodyH / 2, corner);
            var keyhole = MathF.Min(MathF.Sqrt((px - cx) * (px - cx) + (py - keyY) * (py - keyY)) - keyR,
                Box(px - cx, py - (keyY + 32 * unit), 9 * unit, 32 * unit));
            body = MathF.Max(body, -keyhole);
            // The shackle: a half ring over shackleY and its two legs down into the body.
            var dx = px - cx;
            var ring = py <= shackleY
                ? MathF.Abs(MathF.Sqrt(dx * dx + (py - shackleY) * (py - shackleY)) - shackleR) - shackleW / 2
                : Box(MathF.Abs(dx) - shackleR, py - (shackleY + legs / 2), shackleW / 2, legs / 2);
            return MathF.Min(body, ring);
        }
        var reach = shackleR + shackleW;
        Paint(rgba, width, height, ((int)(cx - bodyW), (int)(shackleY - reach), (int)(cx + bodyW), (int)(bodyY + bodyH)), Lock, 235, 0.95f);
    }

    // Blends `grey` over the pixels inside a shape (distance < 0), with a pixel of anti-aliasing at its edge.
    static void Paint(byte[] rgba, int width, int height, (int X0, int Y0, int X1, int Y1) box, Func<float, float, float> distance, byte grey, float opacity)
    {
        for (var y = Math.Max(0, box.Y0); y < Math.Min(height, box.Y1); y++)
            for (var x = Math.Max(0, box.X0); x < Math.Min(width, box.X1); x++)
            {
                var a = Math.Clamp(0.5f - distance(x + 0.5f, y + 0.5f), 0, 1) * opacity;
                if (a <= 0) continue;
                var i = 4 * (y * width + x);
                for (var c = 0; c < 3; c++) rgba[i + c] = (byte)(rgba[i + c] + (grey - rgba[i + c]) * a);
            }
    }

    static (int, int, int, int) Bounds(float x0, float y0, float x1, float y1, float pad) =>
        ((int)(MathF.Min(x0, x1) - pad - 1), (int)(MathF.Min(y0, y1) - pad - 1), (int)(MathF.Max(x0, x1) + pad + 2), (int)(MathF.Max(y0, y1) + pad + 2));

    // Distance from (px, py) to the segment (x0, y0)-(x1, y1).
    static float Segment(float px, float py, float x0, float y0, float x1, float y1)
    {
        float dx = x1 - x0, dy = y1 - y0;
        var t = Math.Clamp(((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy), 0, 1);
        float qx = px - (x0 + t * dx), qy = py - (y0 + t * dy);
        return MathF.Sqrt(qx * qx + qy * qy);
    }

    // Signed distance to a box of half sizes (hw, hh) around the origin.
    static float Box(float x, float y, float hw, float hh)
    {
        float qx = MathF.Abs(x) - hw, qy = MathF.Abs(y) - hh;
        return MathF.Sqrt(MathF.Max(qx, 0) * MathF.Max(qx, 0) + MathF.Max(qy, 0) * MathF.Max(qy, 0)) + MathF.Min(MathF.Max(qx, qy), 0);
    }

    static float RoundedBox(float x, float y, float hw, float hh, float r) => Box(x, y, hw - r, hh - r) - r;
}
