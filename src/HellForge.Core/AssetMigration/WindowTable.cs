using System.Numerics;

namespace HellForge.Core.AssetMigration;

public sealed partial class Migrator
{
    // The 32-byte windows (one every 16 bytes) of the game parts that small pieces are matched with, all in one
    // open-addressed hash table, so a position of the mod costs one lookup however many parts it is matched with. A part
    // keeps at most TriesPerSource windows per hash (its first ones), so periodic data (texture blocks) cannot make long
    // chains. Parts are added as stretches need them; when the table would hold more than its budget it is emptied and
    // refilled with what the stretch needs (consecutive stretches mostly need the same parts, so this is rare).
    //
    // A slot is 8 bytes: a tag (32 bits of the window's hash, never 0: 0 is an empty slot) and a value, the part's
    // number in the table (8 bits) and the window's offset / 16 (24 bits, so parts up to 256 MB, the reader's limit).
    // The table is far bigger than the CPU's caches, so windows are added, and looked up, in batches: every batch's
    // slots are read first (independent reads, whose cache misses overlap), then used.
    sealed class WindowTable(long budget)
    {
        const int MaxParts = 255, MaxOffset = 1 << 28;
        public const int Batch = 32;
        ulong[] slots = [];
        int shift = 64;
        public uint Sink;
        long count;
        readonly Dictionary<int, int> numberOf = [];
        readonly List<int> contentOf = [];

        public long Inserted { get; private set; }
        public ulong[] Slots => slots;
        public int Mask => slots.Length - 1;

        public int Home(ulong hash) => (int)((hash * 0x9E3779B97F4A7C15UL) >> shift);
        public static uint Tag(ulong hash) => (uint)hash | 1;

        // Makes sure these parts are in the table; returns each one's number in it.
        public int[] Add(IReadOnlyList<(int Content, byte[] Bytes)> parts)
        {
            long needed = 0;
            var fresh = 0;
            foreach (var (content, bytes) in parts)
                if (!numberOf.ContainsKey(content)) { needed += Windows(bytes.Length); fresh++; }
            if (count + needed > budget || contentOf.Count + fresh > MaxParts)
            {
                Clear();
                needed = parts.Sum(p => Windows(p.Bytes.Length));
            }
            // At most half full, so a lookup that misses stops within a few slots.
            var wanted = (ulong)Math.Max(1024, 2 * Math.Max(budget, count + needed));
            var capacity = 1L << (64 - BitOperations.LeadingZeroCount(wanted - 1));
            if (capacity > slots.Length) Allocate((int)Math.Min(capacity, 1 << 30));
            var numbers = new int[parts.Count];
            for (var i = 0; i < parts.Count; i++)
            {
                var (content, bytes) = parts[i];
                if (numberOf.TryGetValue(content, out var number)) { numbers[i] = number; continue; }
                number = contentOf.Count;
                contentOf.Add(content);
                numberOf[content] = number;
                numbers[i] = number;
                Insert(number, bytes);
            }
            return numbers;
        }

        static long Windows(int length) => length < Window ? 0 : (Math.Min(length, MaxOffset) - Window) / Step + 1;

        void Insert(int number, byte[] src)
        {
            var length = Math.Min(src.Length, MaxOffset);
            if (length < Window) return;
            Span<ulong> hashes = stackalloc ulong[Batch];
            Span<int> offsets = stackalloc int[Batch];
            var n = 0;
            // A window's hash is its halves' hashes put together (the same value as rolling it in byte by byte): each
            // 16-byte half is hashed once, and the halves do not wait on each other, so the CPU works on several at once.
            var next = Half(src, 0);
            for (var i = 0; i + Window <= length; i += Step)
            {
                var first = next;
                next = Half(src, i + Step);
                if (ContentChunks.Uniform(src.AsSpan(i, Window))) continue;
                hashes[n] = first * Pow16 + next;
                offsets[n++] = i;
                if (n == Batch) { Insert(number, hashes, offsets); n = 0; }
            }
            Insert(number, hashes[..n], offsets[..n]);
        }

        static readonly ulong Pow8 = Pow(Base, 8), Pow16 = Pow(Base, 16);

        // The hash of the 16 bytes at at (Step is half of Window), as two independent 8-byte chains.
        static ulong Half(byte[] src, int at)
        {
            ulong x = 0, y = 0;
            for (var j = 0; j < 8; j++)
            {
                x = x * Base + src[at + j];
                y = y * Base + src[at + 8 + j];
            }
            return x * Pow8 + y;
        }

        // In offset order, so a part's first windows of a hash are the ones kept.
        void Insert(int number, ReadOnlySpan<ulong> hashes, ReadOnlySpan<int> offsets)
        {
            var mask = Mask;
            Span<int> homes = stackalloc int[hashes.Length];
            uint touched = 0;
            for (var k = 0; k < hashes.Length; k++) touched |= (uint)slots[homes[k] = Home(hashes[k])];
            Sink |= touched;
            for (var k = 0; k < hashes.Length; k++)
            {
                var tag = Tag(hashes[k]);
                var at = homes[k];
                var same = 0;
                for (ulong slot; (slot = slots[at]) != 0; at = (at + 1) & mask)
                    if ((uint)slot == tag && (int)(slot >> 56) == number && ++same >= TriesPerSource) break;
                if (slots[at] != 0) continue;
                slots[at] = ((ulong)(((uint)number << 24) | (uint)(offsets[k] / Step)) << 32) | tag;
                count++;
                Inserted++;
            }
        }

        void Allocate(int capacity)
        {
            slots = new ulong[capacity];
            shift = 64 - BitOperations.TrailingZeroCount(capacity);
            count = 0;
            numberOf.Clear();
            contentOf.Clear();
        }

        public void Clear()
        {
            Array.Clear(slots);
            count = 0;
            numberOf.Clear();
            contentOf.Clear();
        }

        public void Release()
        {
            slots = [];
            shift = 64;
            count = 0;
            numberOf.Clear();
            contentOf.Clear();
        }
    }
}
