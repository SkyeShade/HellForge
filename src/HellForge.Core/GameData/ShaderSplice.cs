using System.Numerics;
using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core.GameData;

// A material's own pixel shader (data spec "shader"). A root material's GPU part holds each shader stage as a whole DXBC
// container (SM 5.0, fxc). The copy's stage keeps the game's container with its reflection and signatures, so offsets, the
// engine's metadata and the root signature stay as they are; the code chunk comes from the mod's compiled shader, which must
// use the same bindings (ShaderCheck). A PRIV chunk fills the room left, so the container keeps its size, and it is signed again. The
// game's own digest is checked first: a wrong offset, or another game build, fails here and not in the game. The stage id
// (the top half of the container's hash, after the container and in its program's record), the material's name32 and its
// template ids change too. The engine's shader manager keys a map by template id and a later load replaces an earlier one, so
// a copy keeping them would take over the root it came from, and every other copy of it.
public static class ShaderSplice
{
    const int Header = 32;
    static readonly HashSet<string> Kept = ["RDEF", "ISGN", "ISG1", "OSGN", "OSG1", "OSG5", "PCSG", "PSG1"];

    // Every stage of a material, then the ids: stages that share a game id (the same shader in several programs) must end up
    // the same, and take one new id. An id stands twice for each of its stages: after the stage, and in its program's record.
    public static byte[] ApplyAll(byte[] gpu, IReadOnlyList<(int Offset, byte[] Compiled, string File)> stages)
    {
        var output = gpu.ToArray();
        var ids = new Dictionary<uint, (uint New, int Stages)>();
        foreach (var (offset, compiled, file) in stages)
        {
            var (size, _) = Read(gpu, offset, $"the data at 0x{offset:X}");
            if ((long)offset + size + 4 > gpu.Length) throw new InvalidDataException($"no stage id after the stage at 0x{offset:X}.");
            var old = BitConverter.ToUInt32(gpu, offset + size);
            byte[] result;
            try { result = Stage(output, offset, compiled); }
            catch (InvalidDataException error) { throw new InvalidDataException($"{file}: {error.Message}"); }
            result.CopyTo(output, offset);
            var id = (uint)(EngineHash.Murmur64A(result) >> 32);
            if (ids.TryGetValue(old, out var known) && known.New != id)
                throw new InvalidDataException($"the stages with id 0x{old:X8} are not the same shader after the edit.");
            ids[old] = (id, (known.Stages) + 1);
        }
        foreach (var (old, (id, count)) in ids) Rename(output, old, id, count * 2, "stage id");
        return output;
    }

    // A game resource's stages edited in place (data spec "edit"): each must be the signed container the spec pins, the
    // bytes stay inside it past its chunk table, and it is signed again. Its stage id is kept, unlike a copy's: the edited
    // stage is meant to take the game's place.
    public static byte[] EditStages(byte[] gpu, IReadOnlyList<StageEdit> stages)
    {
        var output = gpu.ToArray();
        foreach (var stage in stages)
        {
            var (size, chunks) = Read(gpu, stage.At, $"the data at 0x{stage.At:X}");
            var container = gpu.AsSpan(stage.At, size);
            if (!container.Slice(4, 16).SequenceEqual(stage.Digest) || !Digest(container).AsSpan().SequenceEqual(stage.Digest))
                throw new InvalidDataException($"the stage at 0x{stage.At:X} is not the signed shader the spec was written for (another game build?).");
            var table = Header + 4 * chunks.Count;
            foreach (var (offset, bytes) in stage.Set)
            {
                if (offset < table || (long)offset + bytes.Length > size)
                    throw new InvalidDataException($"the stage at 0x{stage.At:X} has {size} bytes, its chunks from 0x{table:X}: set 0x{offset:X} (+{bytes.Length}) is outside them.");
                bytes.CopyTo(output, stage.At + offset);
            }
            Digest(output.AsSpan(stage.At, size)).CopyTo(output, stage.At + 4);
        }
        return output;
    }

    // The GPU part the edit was written for, when the spec pins it (data spec "edit" gpu).
    public static byte[] Pinned(byte[] gpu, GpuPin? pin)
    {
        if (pin?.Size is { } size && gpu.Length != size)
            throw new InvalidDataException($"the GPU part has {gpu.Length} bytes, not the {size} the spec pins (another game build?).");
        if (pin?.Sha256 is { } sha && !SHA256.HashData(gpu).AsSpan().SequenceEqual(sha))
            throw new InvalidDataException($"the GPU part's SHA-256 is {Convert.ToHexString(SHA256.HashData(gpu))}, not the {Convert.ToHexString(sha)} the spec pins (another game build?).");
        return gpu;
    }

    // Bytes of a game resource's GPU part outside its shader stages (data spec "edit" bytes), such as the binding table that
    // names a stage's textures. Each must find the game's own bytes: another game build or a wrong offset is refused. Bytes
    // in a stage are refused too: its digest would no longer match and D3D would refuse it; "stages" signs again. So are
    // a stage's id, after its container and in its program's record: the engine finds the stage by it.
    public static byte[] EditBytes(byte[] gpu, IReadOnlyList<BytesEdit> edits)
    {
        if (edits.Count == 0) return gpu;
        var stages = Stages(gpu);
        var ids = StageIds(gpu, stages);
        var output = gpu.ToArray();
        foreach (var edit in edits)
        {
            Clear(gpu.Length, edit, stages, ids);
            if (!gpu.AsSpan(edit.At, edit.Expect.Length).SequenceEqual(edit.Expect))
                throw new InvalidDataException($"the bytes at 0x{edit.At:X} are {Convert.ToHexString(gpu, edit.At, edit.Expect.Length)}, not the expected {Convert.ToHexString(edit.Expect)} (another game build?).");
            edit.Set.CopyTo(output, edit.At);
        }
        return output;
    }

    static void Clear(int length, BytesEdit edit, List<(int At, int Size)> stages, List<int> ids)
    {
        bool Meets(int at, int size) => edit.At < at + size && at < edit.At + edit.Set.Length;
        if (edit.At < 0 || (long)edit.At + edit.Set.Length > length)
            throw new InvalidDataException($"bytes at 0x{edit.At:X} (+{edit.Set.Length}) are outside the GPU part's {length} bytes.");
        if (stages.FirstOrDefault(s => Meets(s.At, s.Size)) is { Size: > 0 } stage)
            throw new InvalidDataException($"bytes at 0x{edit.At:X} (+{edit.Set.Length}) are inside the shader stage at 0x{stage.At:X}: edit it under stages.");
        if (ids.FindIndex(at => Meets(at, 4)) is var k and >= 0)
            throw new InvalidDataException($"bytes at 0x{edit.At:X} (+{edit.Set.Length}) touch the stage id at 0x{ids[k]:X}: an edit keeps the game's stage ids.");
    }

    // Every place a stage's id stands: the u32 after each container, and each other place its value stands (its program's
    // record). A zero after a container is no id.
    static List<int> StageIds(byte[] gpu, List<(int At, int Size)> stages)
    {
        var values = stages.Where(s => (long)s.At + s.Size + 4 <= gpu.Length).Select(s => BitConverter.ToUInt32(gpu, s.At + s.Size)).Where(v => v != 0).ToHashSet();
        var at = new List<int>();
        for (var i = 0; i + 4 <= gpu.Length; i++) if (values.Contains(BitConverter.ToUInt32(gpu, i))) at.Add(i);
        return at;
    }

    // Every DXBC container in the part whose header reads.
    static List<(int At, int Size)> Stages(byte[] gpu)
    {
        var stages = new List<(int At, int Size)>();
        var magic = "DXBC"u8;
        for (var at = gpu.AsSpan().IndexOf(magic); at >= 0;)
        {
            var size = 0;
            try { size = Read(gpu, at, "").Size; } catch (InvalidDataException) { }
            if (size > 0) stages.Add((at, size));
            var next = gpu.AsSpan(at + (size > 0 ? size : 1)).IndexOf(magic);
            at = next < 0 ? -1 : at + (size > 0 ? size : 1) + next;
        }
        return stages;
    }

    static byte[] Stage(byte[] gpu, int offset, byte[] compiled)
    {
        var (size, game) = Read(gpu, offset, "the game's stage");
        var stage = gpu.AsSpan(offset, size);
        if (!Digest(stage).AsSpan().SequenceEqual(stage.Slice(4, 16)))
            throw new InvalidDataException($"the data at 0x{offset:X} is not a signed shader stage: not the shader the spec was written for.");
        var (compiledSize, ours) = Read(compiled, 0, "the compiled shader");
        if (compiledSize != compiled.Length) throw new InvalidDataException("the compiled shader is not one whole DXBC container.");
        if (Chunk(ours, "SHEX") is not { } code) throw new InvalidDataException("the compiled shader has no SHEX chunk (shader model 5).");
        ShaderCheck.Check(game, ours);

        var chunks = new List<(string Name, byte[] Data)>();
        foreach (var (name, data) in game)
            if (name == "SHEX") chunks.Add((name, code));
            else if (name == "STAT") { if (Chunk(ours, "STAT") is { } stat) chunks.Add((name, stat)); }
            else if (Kept.Contains(name)) chunks.Add((name, data));
        var head = Header + 4 * (chunks.Count + 1);
        var used = head + chunks.Sum(c => 8 + c.Data.Length) + 8;
        if (used > size) throw new InvalidDataException($"the compiled shader needs {used} bytes and the stage has {size}: make it smaller.");
        chunks.Add(("PRIV", new byte[size - used]));

        var result = new byte[size];
        Encoding.ASCII.GetBytes("DXBC").CopyTo(result, 0);
        BitConverter.TryWriteBytes(result.AsSpan(20), (ushort)1);
        BitConverter.TryWriteBytes(result.AsSpan(22), (ushort)0);
        BitConverter.TryWriteBytes(result.AsSpan(24), size);
        BitConverter.TryWriteBytes(result.AsSpan(28), chunks.Count);
        var at = head;
        for (var i = 0; i < chunks.Count; i++)
        {
            BitConverter.TryWriteBytes(result.AsSpan(Header + 4 * i), at);
            Encoding.ASCII.GetBytes(chunks[i].Name).CopyTo(result, at);
            BitConverter.TryWriteBytes(result.AsSpan(at + 4), chunks[i].Data.Length);
            chunks[i].Data.CopyTo(result, at + 8);
            at += 8 + chunks[i].Data.Length;
        }
        Digest(result).CopyTo(result, 4);
        return result;
    }

    // The GPU part's name32 (+4), as the copy's name: the top half of its hash.
    public static void RenameMaterial(byte[] gpu, ulong name)
    {
        if (gpu.Length < 8) throw new InvalidDataException("the material has no GPU part.");
        Rename(gpu, BitConverter.ToUInt32(gpu, 4), (uint)(name >> 32), 2, "material name");
    }

    // The template ids (a u32 at +0x30 + 0x58 * i, count at +0x2C), the copy's own: the top half of the hash of its name and the
    // old id. Equal ids stay equal. An id stands twice for each entry: there and in the context table.
    public static void RenameTemplates(byte[] gpu, ulong name)
    {
        if (gpu.Length < 0x30) throw new InvalidDataException("the material has no template table.");
        var count = BitConverter.ToInt32(gpu, 0x2C);
        if (count < 0 || 0x30L + 0x58L * count > gpu.Length) throw new InvalidDataException("the material's template table is broken.");
        var entries = Enumerable.Range(0, count).Select(i => BitConverter.ToUInt32(gpu, 0x30 + 0x58 * i)).ToList();
        foreach (var group in entries.GroupBy(id => id))
        {
            var key = new byte[12];
            BitConverter.TryWriteBytes(key, name);
            BitConverter.TryWriteBytes(key.AsSpan(8), group.Key);
            Rename(gpu, group.Key, (uint)(EngineHash.Murmur64A(key) >> 32), group.Count() * 2, "template id");
        }
    }

    // A value that stands exactly as often as expected in the part, changed everywhere. Anything else is not the reviewed layout.
    static void Rename(byte[] gpu, uint from, uint to, int expected, string what)
    {
        var at = new List<int>();
        for (var i = 0; i + 4 <= gpu.Length; i++) if (BitConverter.ToUInt32(gpu, i) == from) at.Add(i);
        if (at.Count != expected) throw new InvalidDataException($"the {what} 0x{from:X8} stands {at.Count} times, not {expected}: not the layout the shader edit was written for.");
        foreach (var i in at) BitConverter.TryWriteBytes(gpu.AsSpan(i), to);
    }

    static byte[]? Chunk(List<(string Name, byte[] Data)> chunks, string name) => chunks.FirstOrDefault(c => c.Name == name).Data;

    // The container at offset: its size, and its chunks in order.
    internal static (int Size, List<(string Name, byte[] Data)> Chunks) Read(byte[] bytes, int offset, string what)
    {
        if (offset < 0 || (long)offset + Header > bytes.Length || Encoding.ASCII.GetString(bytes, offset, 4) != "DXBC")
            throw new InvalidDataException($"{what} is not a DXBC container.");
        var size = BitConverter.ToInt32(bytes, offset + 24);
        var count = BitConverter.ToInt32(bytes, offset + 28);
        if (size < Header || (long)offset + size > bytes.Length || count < 0 || Header + 4L * count > size)
            throw new InvalidDataException($"{what} has a broken header.");
        var chunks = new List<(string, byte[])>();
        for (var i = 0; i < count; i++)
        {
            var at = BitConverter.ToInt32(bytes, offset + Header + 4 * i);
            if (at < 0 || (long)at + 8 > size) throw new InvalidDataException($"{what} has a broken chunk table.");
            var length = BitConverter.ToInt32(bytes, offset + at + 4);
            if (length < 0 || (long)at + 8 + length > size) throw new InvalidDataException($"{what} has a broken chunk.");
            chunks.Add((Encoding.ASCII.GetString(bytes, offset + at, 4), bytes.AsSpan(offset + at + 8, length).ToArray()));
        }
        return (size, chunks);
    }

    // The container's checksum: MD5 rounds over everything after the digest, with the length words placed DXBC's way.
    public static byte[] Digest(ReadOnlySpan<byte> container)
    {
        var data = container[20..];
        var state = new uint[] { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };
        var whole = data.Length / 64 * 64;
        for (var i = 0; i < whole; i += 64) Transform(state, data.Slice(i, 64));
        var rest = data[whole..];
        var bits = (uint)(data.Length * 8);
        var odd = (bits >> 2) | 1;
        var block = new byte[64];
        if (rest.Length >= 56)
        {
            rest.CopyTo(block);
            block[rest.Length] = 0x80;
            Transform(state, block);
            Array.Clear(block);
            BitConverter.TryWriteBytes(block, bits);
            BitConverter.TryWriteBytes(block.AsSpan(60), odd);
        }
        else
        {
            BitConverter.TryWriteBytes(block, bits);
            rest.CopyTo(block.AsSpan(4));
            block[4 + rest.Length] = 0x80;
            BitConverter.TryWriteBytes(block.AsSpan(60), odd);
        }
        Transform(state, block);
        var digest = new byte[16];
        for (var i = 0; i < 4; i++) BitConverter.TryWriteBytes(digest.AsSpan(4 * i), state[i]);
        return digest;
    }

    static readonly int[] Shift = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];

    static void Transform(uint[] state, ReadOnlySpan<byte> block)
    {
        uint a = state[0], b = state[1], c = state[2], d = state[3];
        for (var i = 0; i < 64; i++)
        {
            uint f;
            int word;
            if (i < 16) { f = (b & c) | (~b & d); word = i; }
            else if (i < 32) { f = (d & b) | (~d & c); word = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; word = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); word = 7 * i % 16; }
            var sum = f + a + (uint)(long)(Math.Abs(Math.Sin(i + 1)) * 4294967296.0) + BitConverter.ToUInt32(block.Slice(word * 4, 4));
            a = d; d = c; c = b;
            b += BitOperations.RotateLeft(sum, Shift[i / 16 * 4 + i % 4]);
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    }
}
