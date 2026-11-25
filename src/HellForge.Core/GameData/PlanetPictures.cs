using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// The planets' pictures (data/planets.json "preview"): the 460x148 banner of each biome the game keeps as a texture, for
// the launcher's Galactic war page. Read from the player's own game data; the launcher keeps them in the installation's
// temp folder, never shipped. A texture's main part is a 0xC0 header, then a DX10 DDS header (height at +12, width at
// +16, DXGI format at +128); mip 0 starts its gpu part.
public static class PlanetPictures
{
    const int Header = 0xC0;

    // RGBA, width and height of each picture the game data has, by its hash; one it has not, or cannot decode, is left out.
    public static Dictionary<ulong, (byte[] Rgba, int Width, int Height)> Read(string gameData, IEnumerable<ulong> hashes)
    {
        var pictures = new Dictionary<ulong, (byte[], int, int)>();
        using var reader = new GameDataReader(gameData);
        foreach (var (key, at) in reader.Find(hashes.Where(h => h != 0).Distinct().Select(h => new ResourceKey(ResourceTypes.Texture, h))))
        {
            var parts = reader.Read(at);
            if (parts.Main.Length < Header + 148) continue;
            var dds = parts.Main.AsSpan(Header);
            if (!dds[..4].SequenceEqual("DDS "u8)) continue;
            int height = BinaryPrimitives.ReadInt32LittleEndian(dds[12..]), width = BinaryPrimitives.ReadInt32LittleEndian(dds[16..]);
            var format = BinaryPrimitives.ReadInt32LittleEndian(dds[128..]);
            var pixels = parts.Gpu.Length > 0 ? parts.Gpu : parts.Stream;
            if (width is <= 0 or > 4096 || height is <= 0 or > 4096 || pixels.Length == 0) continue;
            if (EditorIcons.Decode(new EditorIcons.Page(pixels, width, height, format), 0, 0, width, height) is { } rgba) pictures[key.Name] = (rgba, width, height);
        }
        return pictures;
    }
}
