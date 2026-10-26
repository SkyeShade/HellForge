using System.Buffers.Binary;
using System.Text;

namespace HellForge.Core.GameData;

// A resource of the game: its type and name hashes (MurmurHash64A of the names, EngineHash.Murmur64A).
public readonly record struct ResourceKey(ulong Type, ulong Name)
{
    public override string ToString() => $"{ResourceTypes.Describe(Type)} 0x{Name:X16}";
}

// The parts of a resource: main (in the archive), GPU (<archive>.gpu_resources) and stream (<archive>.stream).
public sealed record ResourceParts(byte[] Main, byte[] Gpu, byte[] Stream);

public static class ResourceTypes
{
    public static readonly ulong Unit = Hash("unit"), Material = Hash("material"), Texture = Hash("texture"), Particles = Hash("particles"),
        Physics = Hash("physics"), Lua = Hash("lua");
    static readonly string[] Known = ["unit", "material", "texture", "particles", "wwise_bank", "wwise_dep", "wwise_stream", "animation",
        "state_machine", "bones", "physics", "lua", "package", "font", "strings", "entity", "geometry_group", "prefab", "level"];
    static readonly Dictionary<ulong, string> names = Known.ToDictionary(Hash);

    public static ulong Hash(string name) => EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name));
    public static string Describe(ulong type) => names.TryGetValue(type, out var name) ? name : $"0x{type:X16}";
}

// Read-only access to the resources of the game's data folder as Steam ships it: archives packed into the DSAR bundles
// bundles.<nn>.nxa (stored or LZ4 chunks), listed by the DSAA index bundles.nxa. Each archive starts with its Stingray
// table (0xF0000011, 80-byte entries); parts live in the archive, <archive>.gpu_resources and <archive>.stream. A port of
// HD2Runtime's hd2_game_data.py (itself a port of HD2RuntimeGUI's GameDataReader, after Filediver's documented layout).
// Only what bundles.nxa lists is read, never loose files such as staged mod patches, so the game's own resources are
// what is found. Nothing is written.
public sealed class GameDataReader : IDisposable
{
    public const string IndexFile = "bundles.nxa";
    const uint TableMagic = 0xF0000011;
    const long MaxRead = 256L * 1024 * 1024;

    sealed record Chunk(ulong UncompressedOffset, ulong CompressedOffset, uint UncompressedSize, uint CompressedSize, byte Compression);
    sealed record Bundle(string File, Chunk[] Chunks, Dictionary<ulong, int> ByOffset);
    readonly record struct Entry(uint ArchiveOffset, uint BundleOffset, byte BundleIndex);
    sealed record Item(ulong Size, Entry[] Entries);
    public sealed record Location(string Archive, ulong MainOffset, uint MainSize, ulong StreamOffset, uint StreamSize, ulong GpuOffset, uint GpuSize);

    readonly Bundle[] bundles;
    readonly Dictionary<string, Item> items = new(StringComparer.Ordinal);
    readonly Dictionary<string, FileStream> files = new(StringComparer.OrdinalIgnoreCase);
    // The last chunks decompressed: the resources of one archive are read one after the other from the same chunks.
    readonly Dictionary<(int, int), byte[]> chunkCache = [];
    readonly Queue<(int, int)> chunkOrder = new();

    public string Folder { get; }

    public GameDataReader(string folder)
    {
        Folder = Path.GetFullPath(folder);
        var nxa = Path.Combine(Folder, IndexFile);
        if (!File.Exists(nxa)) throw new InvalidDataException($"{nxa} is missing: not the game's data folder.");
        using var index = new MemoryStream();
        foreach (var chunk in Dsar(nxa)) index.Write(Decompress(ReadRaw(nxa, chunk), chunk));
        var d = index.ToArray();
        if (d.Length < 24 || Encoding.ASCII.GetString(d, 0, 4) != "DSAA") throw new InvalidDataException("bundles.nxa is not a DSAA index.");
        var nxaCount = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(12));
        var itemCount = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(16));
        bundles = new Bundle[nxaCount];
        for (var i = 0; i < nxaCount; i++)
        {
            var file = Path.Combine(Folder, CString(d, BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(24 + 24 * itemCount + 4 * i))));
            var chunks = Dsar(file);
            var byOffset = new Dictionary<ulong, int>();
            for (var c = 0; c < chunks.Length; c++) byOffset.TryAdd(chunks[c].UncompressedOffset, c);
            bundles[i] = new(file, chunks, byOffset);
        }
        for (var i = 0; i < itemCount; i++)
        {
            var h = d.AsSpan(24 + 24 * i, 24);
            var count = BinaryPrimitives.ReadInt32LittleEndian(h[12..]);
            var at = checked((int)BinaryPrimitives.ReadUInt64LittleEndian(h[16..]));
            var entries = new Entry[count];
            for (var e = 0; e < count; e++)
            {
                var row = d.AsSpan(at + 16 * e, 16);
                entries[e] = new(BinaryPrimitives.ReadUInt32LittleEndian(row), BinaryPrimitives.ReadUInt32LittleEndian(row[8..]), row[15]);
            }
            items[CString(d, BinaryPrimitives.ReadInt32LittleEndian(h[8..]))] = new(BinaryPrimitives.ReadUInt64LittleEndian(h), entries);
        }
    }

    public bool HasArchive(string archive) => items.ContainsKey(archive);

    // Where each wanted resource is. Archives are searched in name order and the first that has a resource wins (as
    // hd2_game_data.Data.find); the search stops once every resource is found. Missing resources are left out.
    public Dictionary<ResourceKey, Location> Find(IEnumerable<ResourceKey> wanted)
    {
        var set = wanted.ToHashSet();
        var found = new Dictionary<ResourceKey, Location>();
        if (set.Count == 0) return found;
        foreach (var (archive, table, at) in TableEntries())
        {
            var e = table.AsSpan(at, 80);
            var key = new ResourceKey(BinaryPrimitives.ReadUInt64LittleEndian(e[8..]), BinaryPrimitives.ReadUInt64LittleEndian(e));
            if (!set.Contains(key) || found.ContainsKey(key)) continue;
            found[key] = LocationOf(archive, e);
            if (found.Count == set.Count) break; // before the next archive's table is read
        }
        return found;
    }

    // Every resource of a type, each name once (the first archive in name order that has it, as Find).
    public Dictionary<ulong, Location> All(ulong type)
    {
        var found = new Dictionary<ulong, Location>();
        foreach (var (archive, table, at) in TableEntries())
        {
            var e = table.AsSpan(at, 80);
            if (BinaryPrimitives.ReadUInt64LittleEndian(e[8..]) != type) continue;
            var name = BinaryPrimitives.ReadUInt64LittleEndian(e);
            if (found.ContainsKey(name)) continue;
            found[name] = LocationOf(archive, e);
        }
        return found;
    }

    // The resource table entries of the archives, in archive name order: each archive's table (read as its entries are
    // asked for) and where an 80-byte entry starts in it.
    IEnumerable<(string Archive, byte[] Table, int Entry)> TableEntries()
    {
        foreach (var archive in items.Keys.Where(n => n.Length == 16 && !n.Contains('.')).Order(StringComparer.Ordinal))
        {
            var head = ItemBytes(archive, 0, 72);
            if (head.Length < 72 || BinaryPrimitives.ReadUInt32LittleEndian(head) != TableMagic) continue;
            var start = 72 + 32 * (long)BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(4));
            var count = BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(8));
            var table = ItemBytes(archive, 0, start + 80 * (long)count);
            for (var i = 0; i < count && start + 80 * (i + 1) <= table.Length; i++)
                yield return (archive, table, (int)(start + 80 * i));
        }
    }

    static Location LocationOf(string archive, ReadOnlySpan<byte> e) =>
        new(archive, BinaryPrimitives.ReadUInt64LittleEndian(e[16..]), BinaryPrimitives.ReadUInt32LittleEndian(e[56..]),
            BinaryPrimitives.ReadUInt64LittleEndian(e[24..]), BinaryPrimitives.ReadUInt32LittleEndian(e[60..]),
            BinaryPrimitives.ReadUInt64LittleEndian(e[32..]), BinaryPrimitives.ReadUInt32LittleEndian(e[64..]));

    // A resource's three parts; a part of size 0, or a companion file the game does not have, reads as empty.
    public ResourceParts Read(Location at) => new(Part(at.Archive, at.MainOffset, at.MainSize),
        Part(at.Archive + ".gpu_resources", at.GpuOffset, at.GpuSize), Part(at.Archive + ".stream", at.StreamOffset, at.StreamSize));

    byte[] Part(string item, ulong offset, uint size)
    {
        if (size == 0 || !items.ContainsKey(item)) return [];
        var bytes = ItemBytes(item, (long)offset, size);
        return bytes.Length == size ? bytes : throw new InvalidDataException($"A resource in {item} ends early.");
    }

    // The bytes [start, start + length) of an archive or companion, from the bundle chunks that hold them.
    byte[] ItemBytes(string name, long start, long length)
    {
        var item = items[name];
        length = Math.Min(length, (long)item.Size - start);
        if (length <= 0) return [];
        if (length > MaxRead) throw new InvalidDataException($"A resource in {name} is too large.");
        var output = new byte[length];
        long filled = 0;
        for (var i = 0; i < item.Entries.Length; i++)
        {
            var entry = item.Entries[i];
            var end = i + 1 < item.Entries.Length ? item.Entries[i + 1].ArchiveOffset : (long)item.Size;
            if (end <= start || entry.ArchiveOffset >= start + length) continue;
            filled += CopyEntry(name, entry, end, start, output);
        }
        return filled == length ? output : output[..(int)filled];
    }

    // What one index entry (its archive bytes up to end) holds of [start, start + output.Length), from its bundle's
    // chunks into output: how many bytes.
    long CopyEntry(string name, Entry entry, long end, long start, byte[] output)
    {
        if (entry.BundleIndex >= bundles.Length) throw new InvalidDataException("bundles.nxa names a bundle it does not list.");
        var bundle = bundles[entry.BundleIndex];
        if (!bundle.ByOffset.TryGetValue(entry.BundleOffset, out var chunk)) throw new InvalidDataException("A bundle chunk is missing.");
        long filled = 0;
        for (long at = entry.ArchiveOffset; at < end && at < start + output.Length; chunk++)
        {
            if (chunk >= bundle.Chunks.Length) throw new InvalidDataException($"{name} ends early.");
            var next = at + bundle.Chunks[chunk].UncompressedSize;
            if (next > start)
            {
                var data = ChunkBytes(entry.BundleIndex, chunk);
                long lo = Math.Max(at, start), hi = Math.Min(next, start + output.Length);
                data.AsSpan((int)(lo - at), (int)(hi - lo)).CopyTo(output.AsSpan((int)(lo - start)));
                filled += hi - lo;
            }
            at = next;
        }
        return filled;
    }

    byte[] ChunkBytes(int bundle, int chunk)
    {
        if (chunkCache.TryGetValue((bundle, chunk), out var cached)) return cached;
        var c = bundles[bundle].Chunks[chunk];
        var bytes = Decompress(ReadRaw(bundles[bundle].File, c), c);
        chunkCache[(bundle, chunk)] = bytes;
        chunkOrder.Enqueue((bundle, chunk));
        if (chunkOrder.Count > 64) chunkCache.Remove(chunkOrder.Dequeue());
        return bytes;
    }

    byte[] ReadRaw(string file, Chunk c)
    {
        if (c.CompressedSize > MaxRead || c.UncompressedSize > MaxRead) throw new InvalidDataException("A bundle chunk is too large.");
        if (!files.TryGetValue(file, out var stream))
            files[file] = stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var raw = new byte[c.CompressedSize];
        stream.Position = (long)c.CompressedOffset;
        stream.ReadExactly(raw);
        return raw;
    }

    static byte[] Decompress(byte[] raw, Chunk c) => c.Compression switch
    {
        0 => raw,
        3 => Lz4.DecompressBlock(raw, (int)c.UncompressedSize),
        _ => throw new InvalidDataException($"Unsupported bundle compression {c.Compression}.")
    };

    static Chunk[] Dsar(string file)
    {
        using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var header = new byte[32];
        stream.ReadExactly(header);
        if (Encoding.ASCII.GetString(header, 0, 4) != "DSAR") throw new InvalidDataException($"{Path.GetFileName(file)} is not a DSAR bundle.");
        var count = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(8));
        var raw = new byte[32L * count];
        stream.ReadExactly(raw);
        return Enumerable.Range(0, count).Select(i =>
        {
            var c = raw.AsSpan(32 * i, 32);
            return new Chunk(BinaryPrimitives.ReadUInt64LittleEndian(c), BinaryPrimitives.ReadUInt64LittleEndian(c[8..]),
                BinaryPrimitives.ReadUInt32LittleEndian(c[16..]), BinaryPrimitives.ReadUInt32LittleEndian(c[20..]), c[24]);
        }).ToArray();
    }

    static string CString(byte[] d, int offset) => Encoding.UTF8.GetString(d, offset, Array.IndexOf(d, (byte)0, offset) - offset);

    public void Dispose()
    {
        foreach (var stream in files.Values) stream.Dispose();
        files.Clear();
    }
}

// The LZ4 block format (no frame), as DSAR chunks use it.
public static class Lz4
{
    public static byte[] DecompressBlock(ReadOnlySpan<byte> src, int size)
    {
        try { return Decode(src, size); }
        catch (IndexOutOfRangeException) { throw new InvalidDataException("Corrupt LZ4 block."); }
    }

    static byte[] Decode(ReadOnlySpan<byte> src, int size)
    {
        var dst = new byte[size];
        int s = 0, d = 0;
        while (s < src.Length)
        {
            var token = src[s++];
            var literals = token >> 4;
            if (literals == 15) literals = Extended(src, ref s, literals);
            if (d + literals > size || s + literals > src.Length) throw new InvalidDataException("Corrupt LZ4 block.");
            src.Slice(s, literals).CopyTo(dst.AsSpan(d));
            s += literals;
            d += literals;
            if (s >= src.Length) break;
            var offset = src[s] | src[s + 1] << 8;
            s += 2;
            var match = (token & 15) + 4;
            if ((token & 15) == 15) match = Extended(src, ref s, match);
            if (offset == 0 || offset > d || d + match > size) throw new InvalidDataException("Corrupt LZ4 block.");
            CopyMatch(dst, d, offset, match);
            d += match;
        }
        return d == size ? dst : throw new InvalidDataException("LZ4 block size mismatch.");
    }

    // A match: byte by byte from offset back, so a match longer than its offset repeats the bytes it has just written.
    static void CopyMatch(byte[] dst, int d, int offset, int match)
    {
        for (var i = 0; i < match; i++) dst[d + i] = dst[d + i - offset];
    }

    // A length nibble of 15 goes on in the bytes after it: each adds to the length, up to one that is not 255.
    static int Extended(ReadOnlySpan<byte> src, ref int s, int length)
    {
        byte b;
        do { b = src[s++]; length += b; } while (b == 255);
        return length;
    }
}
