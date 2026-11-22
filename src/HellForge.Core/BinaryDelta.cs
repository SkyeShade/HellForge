using System.IO.Compression;
using System.IO.MemoryMappedFiles;
using Microsoft.Win32.SafeHandles;

namespace HellForge.Core;

// HellForge's own binary diff, for downpatch files (Downpatch): a target file as runs copied from a source file and runs
// carried as they are, the whole delta Brotli-compressed. Managed code, so Windows and Linux run the same thing and no
// diff tool is downloaded or shipped. Matching is rsync-like: the source's blocks are indexed by a rolling hash, the
// target is scanned at every byte, and each match grows both ways to its exact bytes, so moved or shifted data matches.
// Right after a change, the source position the last copy predicts is tried first, so a small edit costs only itself.
public static unsafe class BinaryDelta
{
    public const int BlockSize = 512;
    const uint Magic = 0x31444648; // "HFD1"
    const ulong Prime = 1099511628211;
    // A match at the predicted position must be this long to be worth an operation (a few bytes of varints).
    const int ShortMatch = 24;
    static readonly ulong PrimeToBlock = Power(Prime, BlockSize);

    // Writes the delta that makes target from source (null: no source, everything carried) into output. progress: target
    // bytes scanned so far.
    public static void Create(string? source, string target, Stream output, int quality = 5, Action<long>? progress = null,
        CancellationToken cancellation = default)
    {
        using var to = MappedFile.Open(target);
        using var from = source == null ? null : MappedFile.Open(source);
        using var brotli = new BrotliStream(output, new BrotliCompressionOptions { Quality = quality }, leaveOpen: true);
        var writer = new OpWriter(brotli);
        writer.Header(to.Length);
        if (from == null || from.Length < BlockSize || to.Length < BlockSize)
        {
            if (to.Length > 0) writer.Literal(to.Pointer, to.Length);
        }
        else new Scanner(from, to, writer, progress, cancellation).Run();
        progress?.Invoke(to.Length);
    }

    // Writes the target into output from source (null when the delta needs none) and the delta (Brotli, as Create wrote
    // it). Anything the delta asks for outside the source or beyond its own length is refused as damaged.
    public static long Apply(SafeFileHandle? source, Stream delta, Stream output, CancellationToken cancellation = default)
    {
        using var brotli = new BufferedStream(new BrotliStream(delta, CompressionMode.Decompress, leaveOpen: true), 1 << 16);
        var reader = new OpReader(brotli);
        if (reader.UInt32() != Magic) throw Damaged("it is not a HellForge delta");
        var length = reader.Count();
        var sourceLength = source == null ? 0 : RandomAccess.GetLength(source);
        var buffer = new byte[1 << 20];
        long written = 0, sourceEnd = 0;
        while (written < length)
        {
            cancellation.ThrowIfCancellationRequested();
            var literal = reader.Count();
            if (literal > length - written) throw Damaged("a run is longer than the file");
            reader.CopyTo(output, literal, buffer);
            written += literal;
            if (written == length) break;
            var copy = reader.Count();
            var start = sourceEnd + reader.Signed();
            if (copy == 0 || copy > length - written || start < 0 || source == null || start > sourceLength - copy)
                throw Damaged("a copy is outside the source file");
            CopyRange(source, start, copy, output, buffer);
            written += copy;
            sourceEnd = start + copy;
        }
        if (brotli.Read(buffer, 0, 1) != 0) throw Damaged("it has data after the file's end");
        return written;
    }

    static void CopyRange(SafeFileHandle source, long start, long count, Stream output, byte[] buffer)
    {
        while (count > 0)
        {
            var read = RandomAccess.Read(source, buffer.AsSpan(0, (int)Math.Min(count, buffer.Length)), start);
            if (read <= 0) throw Damaged("the source file ended early");
            output.Write(buffer, 0, read);
            start += read;
            count -= read;
        }
    }

    static InvalidDataException Damaged(string why) => new($"The delta is damaged: {why}.");

    static ulong Power(ulong value, int exponent)
    {
        ulong result = 1;
        for (var i = 0; i < exponent; i++) result *= value;
        return result;
    }

    static ulong Hash(byte* block)
    {
        ulong hash = 0;
        for (var i = 0; i < BlockSize; i++) hash = hash * Prime + block[i];
        return hash;
    }

    // A read-only view of a whole file (none for an empty one).
    sealed class MappedFile : IDisposable
    {
        MemoryMappedFile? file;
        MemoryMappedViewAccessor? view;
        public byte* Pointer { get; private set; }
        public long Length { get; private set; }

        public static MappedFile Open(string path)
        {
            var mapped = new MappedFile();
            var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            mapped.Length = stream.Length;
            if (mapped.Length == 0)
            {
                stream.Dispose();
                return mapped;
            }
            mapped.file = MemoryMappedFile.CreateFromFile(stream, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, leaveOpen: false);
            mapped.view = mapped.file.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
            byte* pointer = null;
            mapped.view.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
            mapped.Pointer = pointer + mapped.view.PointerOffset;
            return mapped;
        }

        public void Dispose()
        {
            if (view != null)
            {
                view.SafeMemoryMappedViewHandle.ReleasePointer();
                view.Dispose();
            }
            file?.Dispose();
            view = null;
            file = null;
        }
    }

    // The source's blocks by hash: a bit filter that is small enough to stay in the CPU cache (most target positions
    // match nothing and stop there), then an open-addressing table of 32-bit fingerprints and block numbers.
    sealed class BlockIndex
    {
        readonly ulong[] table, bits;
        readonly int tableShift, bitShift;
        readonly long tableMask;

        public BlockIndex(byte* source, long length, CancellationToken cancellation)
        {
            var blocks = length / BlockSize;
            var tableBits = Math.Max(10, Bits(blocks * 2));
            var bitBits = Math.Max(13, Bits(blocks * 8));
            table = new ulong[1L << tableBits];
            bits = new ulong[(1L << bitBits) / 64];
            tableShift = 64 - tableBits;
            bitShift = 64 - bitBits;
            tableMask = table.LongLength - 1;
            for (long block = 0; block < blocks; block++)
            {
                if ((block & 0xFFFF) == 0) cancellation.ThrowIfCancellationRequested();
                Add(Hash(source + block * BlockSize), block);
            }
        }

        static int Bits(long count) => 64 - System.Numerics.BitOperations.LeadingZeroCount((ulong)Math.Max(count - 1, 1));

        static ulong Mix(ulong hash)
        {
            hash = (hash ^ (hash >> 29)) * 0xBF58476D1CE4E5B9;
            return hash ^ (hash >> 32);
        }

        void Add(ulong hash, long block)
        {
            var bit = hash >> bitShift;
            bits[bit >> 6] |= 1UL << (int)(bit & 63);
            var mixed = Mix(hash);
            var fingerprint = (uint)mixed;
            for (var slot = (long)(mixed >> tableShift); ; slot = (slot + 1) & tableMask)
            {
                var entry = table[slot];
                if (entry == 0)
                {
                    table[slot] = (ulong)fingerprint << 32 | (uint)(block + 1);
                    return;
                }
                if ((uint)(entry >> 32) == fingerprint) return; // the same block content seen before (or a rare twin)
            }
        }

        public bool MayHave(ulong hash)
        {
            var bit = hash >> bitShift;
            return (bits[bit >> 6] & 1UL << (int)(bit & 63)) != 0;
        }

        // The source block with this hash whose bytes equal target's, or -1.
        public long Find(ulong hash, byte* source, byte* target)
        {
            var mixed = Mix(hash);
            var fingerprint = (uint)mixed;
            for (var slot = (long)(mixed >> tableShift); ; slot = (slot + 1) & tableMask)
            {
                var entry = table[slot];
                if (entry == 0) return -1;
                if ((uint)(entry >> 32) != fingerprint) continue;
                var block = (long)(uint)entry - 1;
                if (new ReadOnlySpan<byte>(source + block * BlockSize, BlockSize).SequenceEqual(new ReadOnlySpan<byte>(target, BlockSize)))
                    return block;
            }
        }
    }

    // One pass over the target: literal runs and copies, as they are found.
    sealed class Scanner(MappedFile from, MappedFile to, OpWriter writer, Action<long>? progress, CancellationToken cancellation)
    {
        readonly byte* s = from.Pointer, t = to.Pointer;
        readonly long sourceLength = from.Length, targetLength = to.Length;
        long literalStart, sourceEnd;

        public void Run()
        {
            var index = new BlockIndex(s, sourceLength, cancellation);
            long position = 0, reported = 0;
            var hash = Hash(t);
            while (true)
            {
                if (position - reported >= 8 << 20)
                {
                    cancellation.ThrowIfCancellationRequested();
                    progress?.Invoke(position);
                    reported = position;
                }
                if (TryPredicted(position) || index.MayHave(hash) && TryBlock(index, hash, position))
                {
                    position = literalStart;
                    if (position + BlockSize > targetLength) break;
                    hash = Hash(t + position);
                    continue;
                }
                if (position + BlockSize >= targetLength) break;
                hash = hash * Prime + t[position + BlockSize] - t[position] * PrimeToBlock;
                position++;
            }
            if (literalStart < targetLength) writer.Literal(t + literalStart, targetLength - literalStart);
        }

        // The source position just after the last copy, moved along with the literal run: an edit in place resumes there.
        bool TryPredicted(long position)
        {
            var predicted = sourceEnd + (position - literalStart);
            if (predicted + ShortMatch > sourceLength || position + ShortMatch > targetLength) return false;
            if (*(ulong*)(t + position) != *(ulong*)(s + predicted)) return false;
            var length = Common(t + position, s + predicted, Math.Min(targetLength - position, sourceLength - predicted));
            if (length < ShortMatch) return false;
            Emit(position, predicted, length);
            return true;
        }

        bool TryBlock(BlockIndex index, ulong hash, long position)
        {
            var block = index.Find(hash, s, t + position);
            if (block < 0) return false;
            var start = block * BlockSize;
            var length = BlockSize + Common(t + position + BlockSize, s + start + BlockSize,
                Math.Min(targetLength - position - BlockSize, sourceLength - start - BlockSize));
            // grown backwards into the literal run waiting to be written
            while (position > literalStart && start > 0 && t[position - 1] == s[start - 1])
            {
                position--;
                start--;
                length++;
            }
            Emit(position, start, length);
            return true;
        }

        void Emit(long position, long start, long length)
        {
            writer.Literal(t + literalStart, position - literalStart);
            writer.Copy(length, start - sourceEnd);
            literalStart = position + length;
            sourceEnd = start + length;
        }

        static long Common(byte* a, byte* b, long most)
        {
            long done = 0;
            while (done < most)
            {
                var part = (int)Math.Min(most - done, 1 << 20);
                var same = new ReadOnlySpan<byte>(a + done, part).CommonPrefixLength(new ReadOnlySpan<byte>(b + done, part));
                done += same;
                if (same < part) break;
            }
            return done;
        }
    }

    // The delta's operations: a literal run (its length and bytes), then a copy (its length and where it starts, from
    // the end of the previous copy), as varints.
    sealed class OpWriter(Stream output)
    {
        readonly byte[] scratch = new byte[32];

        public void Header(long length)
        {
            BitConverter.TryWriteBytes(scratch, Magic);
            output.Write(scratch, 0, 4);
            Write(Varint((ulong)length));
        }

        public void Literal(byte* data, long length)
        {
            Write(Varint((ulong)length));
            for (long done = 0; done < length;)
            {
                var part = (int)Math.Min(length - done, 1 << 20);
                output.Write(new ReadOnlySpan<byte>(data + done, part));
                done += part;
            }
        }

        public void Copy(long length, long offset)
        {
            var count = Varint((ulong)length, 0);
            count = Varint((ulong)(offset << 1 ^ offset >> 63), count);
            output.Write(scratch, 0, count);
        }

        void Write(int count) => output.Write(scratch, 0, count);

        int Varint(ulong value, int at = 0)
        {
            while (value >= 0x80)
            {
                scratch[at++] = (byte)(value | 0x80);
                value >>= 7;
            }
            scratch[at++] = (byte)value;
            return at;
        }
    }

    sealed class OpReader(Stream input)
    {
        public uint UInt32() => (uint)(Byte() | Byte() << 8 | Byte() << 16 | Byte() << 24);

        public long Count()
        {
            var value = Varint();
            return value <= long.MaxValue ? (long)value : throw Damaged("a length is out of range");
        }

        public long Signed()
        {
            var value = Varint();
            return (long)(value >> 1) ^ -(long)(value & 1);
        }

        ulong Varint()
        {
            ulong value = 0;
            for (var shift = 0; shift < 64; shift += 7)
            {
                var b = Byte();
                value |= (ulong)(b & 0x7F) << shift;
                if (b < 0x80) return value;
            }
            throw Damaged("a number is too long");
        }

        int Byte()
        {
            var b = input.ReadByte();
            return b >= 0 ? b : throw Damaged("it ends early");
        }

        public void CopyTo(Stream output, long count, byte[] buffer)
        {
            while (count > 0)
            {
                var read = input.Read(buffer, 0, (int)Math.Min(count, buffer.Length));
                if (read <= 0) throw Damaged("it ends early");
                output.Write(buffer, 0, read);
                count -= read;
            }
        }
    }
}
