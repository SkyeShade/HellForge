using System.Security.Cryptography;
using System.Text;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

public enum PartKind : byte { Main, Gpu, Stream }

// Where a piece of the game's content is: a build (an index into SourceIndex.Builds), a resource and one of its parts.
public readonly record struct Place(int Build, ResourceKey Key, PartKind Part);

// One distinct resource part of the game, by its SHA-256: the same bytes in two builds (or two resources) are one content
// with several places.
public sealed class Content(byte[] sha256, int size)
{
    public byte[] Sha256 { get; } = sha256;
    public int Size { get; } = size;
    public List<Place> Places { get; } = [];
    public string Hex => Convert.ToHexString(Sha256).ToLowerInvariant();
}

// A game build's resources, as the migrator reads them: every resource with its parts, or one part on request.
public interface IGameResources
{
    string Build { get; }
    IEnumerable<(ResourceKey Key, ResourceParts Parts)> All();
    byte[]? Read(ResourceKey key, PartKind part);
}

public static class Parts
{
    public static byte[] Of(ResourceParts parts, PartKind kind) => kind switch
    {
        PartKind.Main => parts.Main,
        PartKind.Gpu => parts.Gpu,
        _ => parts.Stream,
    };

    public static readonly PartKind[] Kinds = [PartKind.Main, PartKind.Gpu, PartKind.Stream];
}

// A data folder as Steam ships it (bundles.nxa), read through GameDataReader.
public sealed class GameResources(string folder, string build) : IGameResources, IDisposable
{
    readonly GameDataReader reader = new(folder);
    Dictionary<ResourceKey, GameDataReader.Location>? locations;

    public string Build => build;

    // Parts the reader cannot read whole (over its 256 MB limit) are left out and counted.
    public int Skipped { get; private set; }

    public IEnumerable<(ResourceKey Key, ResourceParts Parts)> All()
    {
        foreach (var (key, at) in reader.Resources())
        {
            byte[] Part(GameDataReader.Location only, PartKind kind)
            {
                try { return Parts.Of(reader.Read(only), kind); }
                catch (InvalidDataException) { Skipped++; return []; }
            }
            yield return (key, new ResourceParts(
                Part(at with { GpuSize = 0, StreamSize = 0 }, PartKind.Main),
                Part(at with { MainSize = 0, StreamSize = 0 }, PartKind.Gpu),
                Part(at with { MainSize = 0, GpuSize = 0 }, PartKind.Stream)));
        }
    }

    public byte[]? Read(ResourceKey key, PartKind part)
    {
        locations ??= reader.Resources().ToDictionary(r => r.Key, r => r.At);
        if (!locations.TryGetValue(key, out var at)) return null;
        var only = part switch
        {
            PartKind.Main => at with { GpuSize = 0, StreamSize = 0 },
            PartKind.Gpu => at with { MainSize = 0, StreamSize = 0 },
            _ => at with { MainSize = 0, GpuSize = 0 },
        };
        try { return Parts.Of(reader.Read(only), part); }
        catch (InvalidDataException) { return null; }
    }

    public void Dispose() => reader.Dispose();
}

// The fingerprints of every resource part of one or more game builds: each distinct part's SHA-256, size and places,
// and its content-defined anchors (an 8-byte hash of a 64-byte window and where the window starts). It holds no game content:
// what a resource is made of can be checked against it, never read back out of it.
public sealed class SourceIndex
{
    const string Magic = "HFSI";
    const int Version = 2;
    // An anchor found in more contents than this (shared headers, common tables) is kept only for the first ones.
    const int MaxPerChunk = 16;

    public List<string> Builds { get; } = [];
    public List<Content> Contents { get; } = [];
    public long IndexedBytes { get; private set; }
    readonly Dictionary<string, int> bySha = new(StringComparer.Ordinal);
    List<(ulong Hash, int Content, int Offset)>? pending = [];
    ulong[] hashes = [];
    int[] owners = [];
    int[] offsets = [];
    Dictionary<ResourceKey, List<int>>? byKey;

    public int AnchorCount => hashes.Length;

    public void Add(IGameResources game, Action<long, long>? progress = null)
    {
        if (pending is null) throw new InvalidOperationException("The index is sealed.");
        var build = Builds.Count;
        Builds.Add(game.Build);
        long resources = 0, bytes = 0;
        foreach (var (key, parts) in game.All())
        {
            resources++;
            foreach (var kind in Parts.Kinds)
            {
                var data = Parts.Of(parts, kind);
                if (data.Length == 0) continue;
                bytes += data.Length;
                var sha = SHA256.HashData(data);
                var hex = Convert.ToHexString(sha);
                if (!bySha.TryGetValue(hex, out var id))
                {
                    id = Contents.Count;
                    Contents.Add(new Content(sha, data.Length));
                    bySha[hex] = id;
                    IndexedBytes += data.Length;
                    foreach (var (offset, hash) in ContentChunks.Anchors(data)) pending.Add((hash, id, offset));
                }
                Contents[id].Places.Add(new Place(build, key, kind));
            }
            if (resources % 20000 == 0) progress?.Invoke(resources, bytes);
        }
        progress?.Invoke(resources, bytes);
        byKey = null;
    }

    public void Seal()
    {
        if (pending is null) return;
        pending.Sort((a, b) => a.Hash != b.Hash ? a.Hash.CompareTo(b.Hash) : a.Content != b.Content ? a.Content.CompareTo(b.Content) : a.Offset.CompareTo(b.Offset));
        var kept = new List<(ulong, int, int)>(pending.Count);
        for (int i = 0, run = 0; i < pending.Count; i++)
        {
            run = i > 0 && pending[i].Hash == pending[i - 1].Hash ? run + 1 : 0;
            if (run < MaxPerChunk) kept.Add(pending[i]);
        }
        hashes = kept.Select(k => k.Item1).ToArray();
        owners = kept.Select(k => k.Item2).ToArray();
        offsets = kept.Select(k => k.Item3).ToArray();
        pending = null;
    }

    // The contents (and where in each) an anchor with this hash starts.
    public IEnumerable<(int Content, int Offset)> Lookup(ulong hash)
    {
        int lo = 0, hi = hashes.Length;
        while (lo < hi)
        {
            var mid = (lo + hi) >>> 1;
            if (hashes[mid] < hash) lo = mid + 1; else hi = mid;
        }
        for (var i = lo; i < hashes.Length && hashes[i] == hash; i++) yield return (owners[i], offsets[i]);
    }

    public int? Find(byte[] sha256) => bySha.TryGetValue(Convert.ToHexString(sha256), out var id) ? id : null;

    // The contents that are a part of this resource in some build.
    public IReadOnlyList<int> WithKey(ResourceKey key)
    {
        if (byKey is null)
        {
            byKey = [];
            for (var id = 0; id < Contents.Count; id++)
                foreach (var place in Contents[id].Places)
                {
                    if (!byKey.TryGetValue(place.Key, out var list)) byKey[place.Key] = list = [];
                    if (list.Count == 0 || list[^1] != id) list.Add(id);
                }
        }
        return byKey.TryGetValue(key, out var found) ? found : [];
    }

    public void Save(string path)
    {
        Seal();
        using var file = File.Create(path);
        using var w = new BinaryWriter(file, Encoding.UTF8);
        w.Write(Encoding.ASCII.GetBytes(Magic));
        w.Write(Version);
        w.Write(Builds.Count);
        foreach (var build in Builds) w.Write(build);
        w.Write(Contents.Count);
        foreach (var c in Contents)
        {
            w.Write(c.Sha256);
            w.Write(c.Size);
            w.Write(c.Places.Count);
            foreach (var p in c.Places)
            {
                w.Write((byte)p.Build);
                w.Write(p.Key.Type);
                w.Write(p.Key.Name);
                w.Write((byte)p.Part);
            }
        }
        w.Write(hashes.Length);
        foreach (var h in hashes) w.Write(h);
        foreach (var o in owners) w.Write(o);
        foreach (var o in offsets) w.Write(o);
    }

    public static SourceIndex Load(string path)
    {
        using var file = File.OpenRead(path);
        using var r = new BinaryReader(file, Encoding.UTF8);
        if (Encoding.ASCII.GetString(r.ReadBytes(4)) != Magic || r.ReadInt32() != Version)
            throw new InvalidDataException($"{path} is not a HellForge source index.");
        var index = new SourceIndex { pending = null };
        var builds = r.ReadInt32();
        for (var i = 0; i < builds; i++) index.Builds.Add(r.ReadString());
        var contents = r.ReadInt32();
        for (var i = 0; i < contents; i++)
        {
            var c = new Content(r.ReadBytes(32), r.ReadInt32());
            var places = r.ReadInt32();
            for (var p = 0; p < places; p++)
                c.Places.Add(new Place(r.ReadByte(), new ResourceKey(r.ReadUInt64(), r.ReadUInt64()), (PartKind)r.ReadByte()));
            index.bySha[Convert.ToHexString(c.Sha256)] = i;
            index.Contents.Add(c);
            index.IndexedBytes += c.Size;
        }
        var count = r.ReadInt32();
        index.hashes = new ulong[count];
        index.owners = new int[count];
        index.offsets = new int[count];
        for (var i = 0; i < count; i++) index.hashes[i] = r.ReadUInt64();
        for (var i = 0; i < count; i++) index.owners[i] = r.ReadInt32();
        for (var i = 0; i < count; i++) index.offsets[i] = r.ReadInt32();
        return index;
    }
}

// A data folder in the layout older builds shipped (before bundles.nxa): one Stingray archive per file, named by its
// 16-hex-digit hash, with <name>.gpu_resources and <name>.stream beside it. Each resource key is taken from the first
// archive in name order that has it, as GameDataReader does.
public sealed class LooseGameResources(string folder, string build) : IGameResources
{
    const uint Magic = 0xF0000011;
    Dictionary<ResourceKey, (string Archive, long[] Offsets, int[] Sizes)>? entries;

    public string Build => build;

    Dictionary<ResourceKey, (string Archive, long[] Offsets, int[] Sizes)> Entries()
    {
        if (entries is not null) return entries;
        entries = [];
        foreach (var file in Directory.EnumerateFiles(folder).Where(f => System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(f), "^[0-9a-f]{16}$")).Order(StringComparer.Ordinal))
        {
            using var s = File.OpenRead(file);
            var head = new byte[72];
            if (s.Read(head) < 72 || BitConverter.ToUInt32(head, 0) != Magic) continue;
            var types = BitConverter.ToInt32(head, 4);
            var count = BitConverter.ToInt32(head, 8);
            var table = new byte[80L * count];
            s.Position = 72 + 32L * types;
            if (s.Read(table) < table.Length) continue;
            for (var i = 0; i < count; i++)
            {
                var at = 80 * i;
                var key = new ResourceKey(BitConverter.ToUInt64(table, at + 8), BitConverter.ToUInt64(table, at));
                entries.TryAdd(key, (file,
                    [BitConverter.ToInt64(table, at + 16), BitConverter.ToInt64(table, at + 32), BitConverter.ToInt64(table, at + 24)],
                    [BitConverter.ToInt32(table, at + 56), BitConverter.ToInt32(table, at + 64), BitConverter.ToInt32(table, at + 60)]));
            }
        }
        return entries;
    }

    static byte[] Slice(string file, long offset, int size)
    {
        if (size <= 0 || !File.Exists(file)) return [];
        using var s = File.OpenRead(file);
        var bytes = new byte[size];
        s.Position = offset;
        s.ReadExactly(bytes);
        return bytes;
    }

    static readonly string[] Suffix = ["", ".gpu_resources", ".stream"];

    byte[] Part((string Archive, long[] Offsets, int[] Sizes) e, PartKind kind) =>
        Slice(e.Archive + Suffix[(int)kind], e.Offsets[(int)kind], e.Sizes[(int)kind]);

    public IEnumerable<(ResourceKey Key, ResourceParts Parts)> All()
    {
        foreach (var (key, e) in Entries()) yield return (key, new ResourceParts(Part(e, PartKind.Main), Part(e, PartKind.Gpu), Part(e, PartKind.Stream)));
    }

    public byte[]? Read(ResourceKey key, PartKind part) => Entries().TryGetValue(key, out var e) ? Part(e, part) : null;
}
