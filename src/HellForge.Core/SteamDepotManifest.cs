using System.Buffers.Binary;
using System.Text;

namespace HellForge.Core;

public sealed record DepotChunk(string Sha1, uint Crc, ulong Offset, uint OriginalSize, uint CompressedSize);

public sealed record DepotFile(string Path, ulong Size, uint Flags, string Sha1, IReadOnlyList<DepotChunk> Chunks)
{
    public bool IsDirectory => (Flags & 0x40) != 0; // EDepotFileFlag.Directory
}

public sealed record DepotManifest(uint DepotId, ulong ManifestId, bool FilenamesEncrypted, IReadOnlyList<DepotFile> Files);

// Steam's content manifest as cached in <Steam>/depotcache/<depot>_<manifest>.manifest: sections of
// [u32 magic][u32 length][protobuf], ending with an end magic. The payload lists every file with its size, flags,
// SHA-1 of the content and the chunks it is downloaded in; the metadata names the depot and manifest.
public static class SteamDepotManifest
{
    const uint PayloadMagic = 0x71F617D0, MetadataMagic = 0x1F4812BE, EndMagic = 0x32C415AB;

    public static DepotManifest Load(string path) => Parse(File.ReadAllBytes(path));

    public static DepotManifest Parse(ReadOnlySpan<byte> data)
    {
        ReadOnlySpan<byte> payload = default, metadata = default;
        var position = 0;
        while (position + 8 <= data.Length)
        {
            var magic = BinaryPrimitives.ReadUInt32LittleEndian(data[position..]);
            if (magic == EndMagic) break;
            var length = (int)BinaryPrimitives.ReadUInt32LittleEndian(data[(position + 4)..]);
            position += 8;
            if (length < 0 || position + length > data.Length) throw new InvalidDataException("Truncated depot manifest section.");
            if (magic == PayloadMagic) payload = data.Slice(position, length);
            else if (magic == MetadataMagic) metadata = data.Slice(position, length);
            position += length;
        }
        if (payload.IsEmpty || metadata.IsEmpty) throw new InvalidDataException("Not a Steam depot manifest.");

        var (depot, manifest, encrypted) = ReadMetadata(metadata);
        return new(depot, manifest, encrypted, ReadFiles(payload));
    }

    static (uint Depot, ulong Manifest, bool Encrypted) ReadMetadata(ReadOnlySpan<byte> metadata)
    {
        uint depot = 0;
        ulong manifest = 0;
        var encrypted = false;
        for (var reader = new ProtoReader(metadata); reader.Next(out var field, out var type);)
        {
            if (field == 1 && type == 0) depot = (uint)reader.Varint();
            else if (field == 2 && type == 0) manifest = reader.Varint();
            else if (field == 4 && type == 0) encrypted = reader.Varint() != 0;
            else reader.Skip(type);
        }
        return (depot, manifest, encrypted);
    }

    static List<DepotFile> ReadFiles(ReadOnlySpan<byte> payload)
    {
        var files = new List<DepotFile>();
        for (var reader = new ProtoReader(payload); reader.Next(out var field, out var type);)
        {
            if (field == 1 && type == 2) files.Add(ReadFile(reader.Bytes()));
            else reader.Skip(type);
        }
        return files;
    }

    static DepotFile ReadFile(ReadOnlySpan<byte> data)
    {
        string name = "", sha = "";
        ulong size = 0;
        uint flags = 0;
        var chunks = new List<DepotChunk>();
        for (var reader = new ProtoReader(data); reader.Next(out var field, out var type);)
        {
            switch (field)
            {
                case 1 when type == 2: name = Encoding.UTF8.GetString(reader.Bytes()).Replace('\\', '/'); break;
                case 2 when type == 0: size = reader.Varint(); break;
                case 3 when type == 0: flags = (uint)reader.Varint(); break;
                case 5 when type == 2: sha = Convert.ToHexString(reader.Bytes()); break;
                case 6 when type == 2: chunks.Add(ReadChunk(reader.Bytes())); break;
                default: reader.Skip(type); break;
            }
        }
        return new(name, size, flags, sha, chunks);
    }

    static DepotChunk ReadChunk(ReadOnlySpan<byte> data)
    {
        string sha = "";
        uint crc = 0, original = 0, compressed = 0;
        ulong offset = 0;
        for (var reader = new ProtoReader(data); reader.Next(out var field, out var type);)
        {
            switch (field)
            {
                case 1 when type == 2: sha = Convert.ToHexString(reader.Bytes()); break;
                case 2 when type == 5: crc = reader.Fixed32(); break;
                case 3 when type == 0: offset = reader.Varint(); break;
                case 4 when type == 0: original = (uint)reader.Varint(); break;
                case 5 when type == 0: compressed = (uint)reader.Varint(); break;
                default: reader.Skip(type); break;
            }
        }
        return new(sha, crc, offset, original, compressed);
    }

    // The subset of the protobuf wire format the manifests use.
    ref struct ProtoReader(ReadOnlySpan<byte> data)
    {
        readonly ReadOnlySpan<byte> data = data;
        int position;

        public bool Next(out int field, out int type)
        {
            field = type = 0;
            if (position >= data.Length) return false;
            var key = Varint();
            field = (int)(key >> 3);
            type = (int)(key & 7);
            return true;
        }

        public ulong Varint()
        {
            ulong value = 0;
            for (var shift = 0; shift < 64; shift += 7)
            {
                if (position >= data.Length) throw new InvalidDataException("Truncated depot manifest varint.");
                var current = data[position++];
                value |= (ulong)(current & 0x7F) << shift;
                if (current < 0x80) return value;
            }
            throw new InvalidDataException("Invalid depot manifest varint.");
        }

        public uint Fixed32()
        {
            if (position + 4 > data.Length) throw new InvalidDataException("Truncated depot manifest field.");
            var value = BinaryPrimitives.ReadUInt32LittleEndian(data[position..]);
            position += 4;
            return value;
        }

        public ReadOnlySpan<byte> Bytes()
        {
            var length = Varint();
            if (length > (ulong)(data.Length - position)) throw new InvalidDataException("Truncated depot manifest field.");
            var value = data.Slice(position, (int)length);
            position += (int)length;
            return value;
        }

        public void Skip(int type)
        {
            switch (type)
            {
                case 0: Varint(); break;
                case 1: position += 8; break;
                case 2: Bytes(); break;
                case 5: position += 4; break;
                default: throw new InvalidDataException($"Unsupported protobuf wire type {type}.");
            }
            if (position > data.Length) throw new InvalidDataException("Truncated depot manifest field.");
        }
    }
}
