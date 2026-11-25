using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// What a migration found, per file, per resource of the mod's archives, and per piece of the game it reuses.
public sealed class MigrationReport
{
    public sealed record FileLine(string Path, long Size, long FromGame, long Literal, long Fill, bool InRecipe);
    // InIndex: some indexed build has a resource with this key and part (with these bytes or others).
    // Sha256: of the mod's bytes of this part (null when the range runs past the file's end).
    public sealed record ResourceLine(string File, ResourceKey Key, PartKind Part, long Start, long Size, long FromGame, long Literal, long Fill, Dictionary<int, long> Sources, bool InIndex, byte[]? Sha256 = null);

    public List<FileLine> Files { get; } = [];
    public List<ResourceLine> Resources { get; } = [];
    public Dictionary<int, long> SourceBytes { get; } = [];
    // Game parts the mod shares content with that none of the given game folders could supply: their bytes stay in the
    // mod, so it needs a review (or the build they come from).
    public HashSet<int> Unreadable { get; } = [];
    // The byte ranges of each file the mod still ships: its literal bytes, or the whole file when it ships as it is.
    public Dictionary<string, List<(long Start, long Length)>> Kept { get; } = new(StringComparer.Ordinal);
    // Runs of the mod's files that are chunks Steam shipped (from the Steam chunk index, when one was given).
    public List<SteamChunkIndex.Run> Shipped { get; } = [];

    // The bytes the mod still ships that are chunks Steam shipped (exactly: the runs' overlap with the kept ranges), and
    // those runs: game content, whatever the resource is called and whether or not an indexed build has a resource by
    // that name. A run whose bytes the recipe already takes from the game is not counted.
    public (long Bytes, List<(SteamChunkIndex.Run Run, long Bytes)> Runs) ShippedLeft()
    {
        long total = 0;
        var runs = new List<(SteamChunkIndex.Run, long)>();
        foreach (var run in Shipped)
        {
            if (!Kept.TryGetValue(run.File, out var kept)) continue;
            var bytes = kept.Sum(k => Math.Max(0, Math.Min(k.Start + k.Length, run.Offset + run.Length) - Math.Max(k.Start, run.Offset)));
            if (bytes == 0) continue;
            total += bytes;
            runs.Add((run, bytes));
        }
        return (total, runs.OrderByDescending(r => r.Item2).ToList());
    }
}

// Turns a mod into a recipe, one stretch of a file at a time (a resource part of its archives, or the bytes between):
//   1. Diagonals. The stretch's content-defined anchors are looked up in the source index. Each hit says a game part has
//      the same 64 bytes at some offset, so it may hold the stretch's bytes shifted by (its offset - the stretch's); a
//      part of the resource the stretch replaces is tried unshifted too. Along each such diagonal every run of 48 equal
//      bytes or more is a copy, found with vector compares and no tables: that takes the long reused ranges.
//   2. Small pieces. What no diagonal covers is matched at every byte against the parts the diagonals found (and the
//      replaced resource's parts), with 32-byte windows sampled every 16 bytes of each part (a table per part, built
//      once): that takes a short piece at any offset, however short the edit next to it.
//   3. The copies that reach furthest are taken; runs of one byte become fills; the rest is the mod's own bytes.
// Game parts are read as needed and kept within a memory budget.
public sealed partial class Migrator(SourceIndex index, IReadOnlyList<IGameResources> games)
{
    // Step is half of Window (the window table hashes a window from its two halves).
    const int Window = 32, Step = 16, MinCopy = 48, MinFill = 32, MaxCandidates = 96, TriesPerSource = 8;
    const ulong Base = 0x100000001B3;
    static readonly ulong Top = Pow(Base, Window - 1);

    // Game parts read (null: none of the given game folders has it), newest use first, within CacheBytes.
    readonly Dictionary<int, LinkedListNode<(int Content, byte[]? Bytes)>> cache = [];
    readonly LinkedList<(int Content, byte[]? Bytes)> recent = new();
    long cachedBytes;
    // The windows of the game parts small pieces are matched with, within TableBudget windows (8 bytes each, twice that
    // many slots).
    WindowTable? windowTable;
    WindowTable Windows => windowTable ??= new(TableBudget);

    // Game parts kept between stretches (a stretch keeps the parts it compares with until it is done, so more can be
    // held for a while). 384 MB costs a mod of 2 GB about 2 s of reading parts again against 768 MB, and saves ~700 MB.
    public long CacheBytes { get; init; } = 384L << 20;
    public long TableBudget { get; init; } = 8L << 20;
    // Diagonals compared over a stretch's whole uncovered part, most anchors first; parts its small pieces are matched with.
    public int MaxDiagonals { get; init; } = 8192;
    public int Neighbours { get; init; } = 64;

    // Where a migration spends its time and memory (printed by the command line).
    public sealed class Profile
    {
        public readonly System.Diagnostics.Stopwatch Load = new(), Anchors = new(), Diagonals = new(), Small = new(), Tables = new();
        public long Segments, Diagonal, DiagonalBytes, LoadedBytes, LoadedParts, TableEntries, SmallPositions, MaxCachedBytes;
        public override string ToString() =>
            $"{Segments:N0} segments; anchors {Anchors.Elapsed.TotalSeconds:F1} s; " +
            $"read {LoadedParts:N0} game parts ({LoadedBytes / 1e9:F2} GB) in {Load.Elapsed.TotalSeconds:F1} s, at most {MaxCachedBytes / 1e6:F0} MB kept; " +
            $"{Diagonal:N0} diagonals ({DiagonalBytes / 1e9:F2} GB compared) in {Diagonals.Elapsed.TotalSeconds:F1} s; " +
            $"small pieces at {SmallPositions / 1e6:F1}M positions in {Small.Elapsed.TotalSeconds:F1} s ({TableEntries / 1e6:F1}M windows added in {Tables.Elapsed.TotalSeconds:F1} s)";
    }
    public Profile Stats { get; } = new();

    public (Recipe Recipe, byte[] Literals, MigrationReport Report) Migrate(IReadOnlyList<(string Path, byte[] Bytes)> files, Action<string>? log = null)
    {
        var literals = new MemoryStream();
        var (recipe, report) = Migrate(ModFiles.InMemory(files), literals, log);
        return (recipe, literals.ToArray(), report);
    }

    // Migrates the mod one file at a time (only that file is held), writing the literal bytes to the given stream.
    public (Recipe Recipe, MigrationReport Report) Migrate(ModFiles mod, Stream literals, Action<string>? log = null)
    {
        var output = new Output(new Recipe(), new MigrationReport(), literals);
        var report = output.Report;
        var layout = Layout(mod).ToLookup(l => l.File, StringComparer.Ordinal);
        foreach (var path in mod.Paths)
        {
            var bytes = mod.Read(path);
            var ops = new List<Op>();
            var candidates = MatchFile(bytes, layout[path], report.Unreadable, ops);
            Pad(bytes, ops);
            long fromGame = ops.Where(o => o.Kind == Recipe.Copy).Sum(o => o.Length);
            long fill = ops.Where(o => o.Kind == Recipe.Fill).Sum(o => o.Length);
            var inRecipe = fromGame > 0;
            report.Files.Add(new(path, bytes.Length, fromGame, bytes.Length - fromGame - fill, fill, inRecipe));
            report.Kept[path] = inRecipe ? LiteralRanges(ops) : [(0, bytes.Length)];
            log?.Invoke($"{path}: {bytes.Length:N0} bytes, {fromGame:N0} from the game ({candidates} game parts compared)");
            // Every archive's resources are reported, with game content in it or not: one with none may still hold
            // resources the game has by name (cut content from a build the index does not hold).
            Attribute(report, layout[path], bytes, ops);
            if (!inRecipe) continue;
            output.Recipe.Files.Add(RecipeFileOf(output, path, bytes, ops));
        }
        return (output.Recipe, report);
    }

    // Where a migration writes: the recipe, the report and the literals (with how many bytes are written so far), and
    // each game part's number among the recipe's sources.
    sealed class Output(Recipe recipe, MigrationReport report, Stream literals)
    {
        public readonly Recipe Recipe = recipe;
        public readonly MigrationReport Report = report;
        public readonly Stream Literals = literals;
        public readonly Dictionary<int, int> SourceIds = [];
        public long Written;
    }

    // An archive is matched one resource part at a time (with that resource's own candidates), and the bytes between
    // parts (the table, padding) on their own; any other file is matched whole. Returns how many game parts it compared.
    int MatchFile(byte[] bytes, IEnumerable<Range> ranges, HashSet<int> unreadable, List<Op> ops)
    {
        var candidates = 0;
        long cursor = 0;
        foreach (var r in ranges.Where(l => l.Start + l.Length <= bytes.Length).OrderBy(l => l.Start))
        {
            if (r.Start < cursor) continue;
            if (r.Start > cursor) candidates += Match(bytes, (int)cursor, (int)r.Start, [], unreadable, ops);
            candidates += Match(bytes, (int)r.Start, (int)(r.Start + r.Length), [r.Key], unreadable, ops);
            cursor = r.Start + r.Length;
        }
        if (cursor < bytes.Length) candidates += Match(bytes, (int)cursor, bytes.Length, [], unreadable, ops);
        return candidates;
    }

    // The byte ranges of the file that its literal operations hold.
    static List<(long Start, long Length)> LiteralRanges(List<Op> ops)
    {
        var kept = new List<(long Start, long Length)>();
        long position = 0;
        foreach (var op in ops)
        {
            if (op.Kind == Recipe.Literal) kept.Add((position, op.Length));
            position += op.Length;
        }
        return kept;
    }

    // The file's recipe entry. Its literal bytes go to the literals stream, and each game part it copies from becomes a
    // source of the recipe the first time any file uses it.
    RecipeFile RecipeFileOf(Output output, string path, byte[] bytes, List<Op> ops)
    {
        var file = new RecipeFile { Path = path, Size = bytes.Length, Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() };
        long at = 0;
        foreach (var op in ops)
        {
            switch (op.Kind)
            {
                case Recipe.Copy:
                    if (!output.SourceIds.TryGetValue(op.Source, out var sid))
                    {
                        sid = output.Recipe.Sources.Count;
                        output.SourceIds[op.Source] = sid;
                        output.Recipe.Sources.Add(SourceOf(op.Source));
                    }
                    file.Ops.Add([Recipe.Copy, sid, op.Offset, op.Length]);
                    output.Report.SourceBytes[op.Source] = output.Report.SourceBytes.GetValueOrDefault(op.Source) + op.Length;
                    break;
                case Recipe.Literal:
                    file.Ops.Add([Recipe.Literal, output.Written, op.Length]);
                    output.Literals.Write(bytes, (int)at, op.Length);
                    output.Written += op.Length;
                    break;
                default:
                    file.Ops.Add([Recipe.Fill, op.Offset, op.Length]);
                    break;
            }
            at += op.Length;
        }
        return file;
    }

    RecipeSource SourceOf(int content)
    {
        var c = index.Contents[content];
        return new RecipeSource
        {
            Sha256 = c.Hex,
            Size = c.Size,
            // Any place with these bytes will do (the SHA-256 is checked), so a few are enough: one per build first.
            Places = c.Places.GroupBy(p => p.Build).SelectMany(g => g.Select((p, i) => (p, i))).OrderBy(x => x.i).Take(8).Select(x => x.p).Select(p => new RecipePlace
            {
                Build = index.Builds[p.Build],
                Type = p.Key.Type.ToString("x16"),
                Name = p.Key.Name.ToString("x16"),
                Part = p.Part.ToString().ToLowerInvariant(),
                Archive = p.Archive >= 0 && p.Archive < index.Archives.Count ? index.Archives[p.Archive] : null,
            }).ToList(),
        };
    }

    // Kind is Copy (Source, Offset), Literal or Fill (Offset holds the byte value).
    public readonly record struct Op(int Kind, int Source, int Offset, int Length);

    // Literal runs of one byte value (the padding between resources, shorter than a fill would otherwise take) become
    // fills: an op either way, and no byte of them is anyone's content.
    static void Pad(byte[] bytes, List<Op> ops)
    {
        var at = 0;
        for (var i = 0; i < ops.Count; i++)
        {
            var op = ops[i];
            if (op.Kind == Recipe.Literal && op.Length >= 2 && bytes.AsSpan(at + 1, op.Length - 1).IndexOfAnyExcept(bytes[at]) < 0)
                ops[i] = new Op(Recipe.Fill, 0, bytes[at], op.Length);
            at += op.Length;
        }
    }

    // Matches data[from, to) and appends its operations; returns how many game parts it compared.
    int Match(byte[] data, int from, int to, ResourceKey[] keys, HashSet<int> unreadable, List<Op> ops)
    {
        Stats.Segments++;
        var (hits, chosen, support, replaced) = Candidates(data, from, to, keys);
        var stretch = new Stretch(data, from, to);
        Diagonals(stretch, support, unreadable);

        // Small pieces, in what is left: against the parts found here (most bytes first), the replaced resource's, then
        // the other candidates (most anchors first), which may hold a piece no anchor of this stretch fell in.
        var gaps = stretch.Covered.Gaps(from, to);
        if (gaps.Count > 0)
        {
            Stats.Small.Start();
            var near = stretch.Intervals.GroupBy(i => i.Content).OrderByDescending(g => g.Sum(i => (long)i.End - i.Start)).Select(g => g.Key)
                .Concat(replaced).Concat(hits.Where(h => chosen.Contains(h.Key)).OrderByDescending(h => h.Value).ThenBy(h => h.Key).Select(h => h.Key))
                .Where(stretch.Parts.ContainsKey).Distinct().Take(Neighbours).ToList();
            if (near.Count > 0) Small(stretch, gaps, near);
            Stats.Small.Stop();
        }

        Cover(data, from, to, stretch.Intervals, ops);
        return stretch.Parts.Count;
    }

    // The stretch's candidates, from its anchors: per game part the anchors it shares with the stretch (a part of a
    // replaced resource counts a million more), the MaxCandidates parts with most, the anchors on each of their
    // diagonals (a replaced resource's parts unshifted, counted a million more), and the replaced resource's parts.
    (Dictionary<int, int> Hits, HashSet<int> Chosen, Dictionary<(int Content, int Diagonal), int> Support, List<int> Replaced) Candidates(
        byte[] data, int from, int to, ResourceKey[] keys)
    {
        Stats.Anchors.Start();
        var hits = new Dictionary<int, int>();
        var anchorHits = new List<(int Content, int Diagonal)>();
        foreach (var (offset, anchor) in ContentChunks.Anchors(data.AsSpan(from, to - from)))
            foreach (var (content, at) in index.Lookup(anchor))
            {
                hits[content] = hits.GetValueOrDefault(content) + 1;
                anchorHits.Add((content, at - (from + offset)));
            }
        var replaced = keys.SelectMany(index.WithKey).Distinct().ToList();
        foreach (var content in replaced) hits[content] = hits.GetValueOrDefault(content) + 1_000_000;
        var chosen = hits.OrderByDescending(h => h.Value).ThenBy(h => h.Key).Take(MaxCandidates).Select(h => h.Key).ToHashSet();
        var support = new Dictionary<(int Content, int Diagonal), int>();
        foreach (var hit in anchorHits)
            if (chosen.Contains(hit.Content)) support[hit] = support.GetValueOrDefault(hit) + 1;
        foreach (var content in replaced) support[(content, -from)] = support.GetValueOrDefault((content, -from)) + 1_000_000;
        Stats.Anchors.Stop();
        return (hits, chosen, support, replaced);
    }

    // Compares the stretch along the diagonals, most anchors first, until it is covered or MaxDiagonals are done.
    void Diagonals(Stretch stretch, Dictionary<(int Content, int Diagonal), int> support, HashSet<int> unreadable)
    {
        int from = stretch.From, to = stretch.To;
        Stats.Diagonals.Start();
        foreach (var ((content, diagonal), _) in support.OrderByDescending(s => s.Value).ThenBy(s => s.Key.Content).ThenBy(s => s.Key.Diagonal).Take(MaxDiagonals))
        {
            if (stretch.Covered.Covers(from, to)) break;
            var src = Part(content, stretch.Parts, unreadable);
            if (src is null) continue;
            Stats.Diagonal++;
            // The stretch's positions p that the diagonal maps into the part: 0 <= p + diagonal < its length.
            int lo = Math.Max(from, -diagonal), hi = (int)Math.Min(to, (long)src.Length - diagonal);
            if (hi - lo < MinCopy) continue;
            var along = new Along(src, content, diagonal, lo, hi);
            foreach (var (g0, g1) in stretch.Covered.Gaps(lo, hi))
                Diagonal(stretch, along, g0, g1);
        }
        Stats.Diagonals.Stop();
    }

    // A stretch being matched, data[From, To): the copies found in it (each with its game part and diagonal), the bytes
    // they cover, and the game parts it compares with (kept until it is done).
    sealed class Stretch(byte[] data, int from, int to)
    {
        public readonly byte[] Data = data;
        public readonly int From = from, To = to;
        public readonly List<(int Start, int End, int Content, int Diagonal)> Intervals = [];
        public readonly Coverage Covered = new();
        public readonly Dictionary<int, byte[]> Parts = [];

        public void Add(int start, int end, int content, int diagonal)
        {
            Intervals.Add((start, end, content, diagonal));
            Covered.Add(start, end);
        }
    }

    // One diagonal of a game part: the stretch's byte p is compared with Src[p + Diagonal], for Lo <= p < Hi.
    readonly record struct Along(byte[] Src, int Content, int Diagonal, int Lo, int Hi);

    // Every run of MinCopy equal bytes or more along one diagonal that has a byte in [g0, g1); runs may reach outside it
    // (back to lo, on to hi), so a short gap between two other copies is taken too when the bytes are the same.
    // Such a run has MinCopy equal bytes within MinCopy - 1 bytes of the gap, and so a whole equal 16-byte block at a
    // multiple of 16 from wherever the search starts there: the search compares 16 bytes at a time and follows only
    // blocks that are equal throughout (unrelated bytes have single equal bytes everywhere, but almost never 16).
    void Diagonal(Stretch stretch, in Along along, int g0, int g1)
    {
        var data = stretch.Data;
        var (src, content, diagonal, lo, hi) = along;
        Stats.DiagonalBytes += g1 - g0;
        int s0 = Math.Max(lo, g0 - (MinCopy - 1)), s1 = Math.Min(hi, g1 + (MinCopy - 1));
        var p = s0;
        while (p + 16 <= s1)
        {
            var skip = EqualBlock(data.AsSpan(p, s1 - p), src.AsSpan(p + diagonal, s1 - p));
            if (skip < 0) return;
            p += skip;
            var back = 0;
            while (p - back > lo && data[p - back - 1] == src[p + diagonal - back - 1]) back++;
            var forward = data.AsSpan(p, hi - p).CommonPrefixLength(src.AsSpan(p + diagonal, hi - p));
            int start = p - back, end = p + forward;
            if (end - start >= MinCopy && end > g0 && start < g1) stretch.Add(start, end, content, diagonal);
            p = end + 1;
        }
    }

    // The first multiple of 16 where the two spans hold the same 16 bytes, or -1.
    static int EqualBlock(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b)
    {
        var n = Math.Min(a.Length, b.Length) - 16;
        for (var i = 0; i <= n; i += 16)
            if (System.Runtime.Intrinsics.Vector128.Create(a.Slice(i, 16)) == System.Runtime.Intrinsics.Vector128.Create(b.Slice(i, 16))) return i;
        return -1;
    }

    // Matches every position of the gaps against the windows of these parts (one lookup per position). Of equally long
    // matches the earlier part in near wins, then the earlier offset.
    void Small(Stretch stretch, List<(int Start, int End)> gaps, List<int> near)
    {
        var chosen = SmallParts(near, stretch.Parts);
        if (chosen.Count == 0) return;
        Stats.Tables.Start();
        var table = Windows;
        var numbers = table.Add(chosen);
        Stats.TableEntries = table.Inserted;
        Stats.Tables.Stop();
        var lookup = new WindowLookup(stretch, table, chosen, numbers);
        // Positions are looked up a batch at a time: their hashes and home slots first (the slots read together, so
        // their cache misses overlap), then each in turn. A copy found skips the rest of the batch.
        Span<ulong> hashes = stackalloc ulong[WindowTable.Batch];
        Span<int> homes = stackalloc int[WindowTable.Batch];
        foreach (var (g0, g1) in gaps) SmallGap(stretch, lookup, (g0, g1), hashes, homes);
    }

    // The parts in order, up to four budgets' worth of windows (a stretch matched with that much is rare).
    List<(int Content, byte[] Bytes)> SmallParts(List<int> near, Dictionary<int, byte[]> parts)
    {
        var chosen = new List<(int Content, byte[] Bytes)>();
        long windows = 0;
        foreach (var content in near)
        {
            var bytes = parts[content];
            if (bytes.Length < Window) continue;
            if (chosen.Count > 0 && windows + bytes.Length / Step > 4 * TableBudget) break;
            chosen.Add((content, bytes));
            windows += bytes.Length / Step;
        }
        return chosen;
    }

    // The small pieces over one gap [g0, g1), in batches of positions.
    void SmallGap(Stretch stretch, in WindowLookup lookup, (int G0, int G1) gap, Span<ulong> hashes, Span<int> homes)
    {
        var data = stretch.Data;
        int from = stretch.From, to = stretch.To, g0 = gap.G0, g1 = gap.G1;
        var table = lookup.Table;
        var slots = lookup.Slots;
        // A copy of MinCopy bytes or more over a byte of the gap holds a sampled window of its part that starts at
        // most MinCopy - 1 bytes before that byte: start there, and keep what reaches into the gap.
        var pos = Math.Max(from, g0 - (MinCopy - 1));
        while (pos < g1 && pos + Window <= to)
        {
            var n = Math.Min(WindowTable.Batch, Math.Min(g1, to - Window + 1) - pos);
            var hash = HashAt(data, pos);
            uint touched = 0;
            for (var k = 0; k < n; k++)
            {
                hashes[k] = hash;
                touched |= (uint)slots[homes[k] = table.Home(hash)];
                if (pos + k + Window < to) hash = Roll(hash, data[pos + k], data[pos + k + Window]);
            }
            table.Sink |= touched;
            for (var k = 0; k < n; k++)
            {
                Stats.SmallPositions++;
                var best = lookup.Best(pos, homes[k], WindowTable.Tag(hashes[k]));
                if (best.Length >= MinCopy && pos - best.Back + best.Length > g0)
                {
                    int start = pos - best.Back, end = start + best.Length;
                    stretch.Add(start, end, lookup.Contents[best.Number], best.Offset - pos);
                    pos = end;
                    break;
                }
                pos++;
            }
        }
    }

    // The longest copy found at a position: its length, how far before the position it starts, the part's number in
    // the window table and the offset in the part that the position's window has.
    readonly record struct SmallMatch(int Length, int Back, int Number, int Offset);

    // The window table as one stretch's small pieces look it up: per part number in the table, the part's rank in near
    // (-1: not one of this stretch's parts), its bytes and its content.
    readonly struct WindowLookup
    {
        public readonly WindowTable Table;
        public readonly ulong[] Slots;
        public readonly int Mask;
        public readonly int[] Rank, Contents;
        public readonly byte[][] Sources;
        readonly byte[] data;
        readonly int from, to;

        public WindowLookup(Stretch stretch, WindowTable table, List<(int Content, byte[] Bytes)> chosen, int[] numbers)
        {
            (data, from, to) = (stretch.Data, stretch.From, stretch.To);
            Table = table;
            Rank = new int[256];
            Array.Fill(Rank, -1);
            Sources = new byte[256][];
            Contents = new int[256];
            for (var i = 0; i < chosen.Count; i++) (Rank[numbers[i]], Sources[numbers[i]], Contents[numbers[i]]) = (i, chosen[i].Bytes, chosen[i].Content);
            Slots = table.Slots;
            Mask = table.Mask;
        }

        // The longest copy of the stretch's bytes at pos that starts with a window of the chain from home with this tag
        // (Length 0 when there is none). Of equally long ones the part of lower rank wins, then the earlier offset.
        public SmallMatch Best(int pos, int home, uint tag)
        {
            var (slots, mask, rank, sources) = (Slots, Mask, Rank, Sources);
            int bestLength = 0, bestBack = 0, bestRank = int.MaxValue, bestNumber = -1, bestOffset = 0;
            for (var at = home; slots[at] is var slot && slot != 0; at = (at + 1) & mask)
            {
                if ((uint)slot != tag) continue;
                var value = (uint)(slot >> 32);
                var number = (int)(value >> 24);
                var r = rank[number];
                if (r < 0) continue;
                var src = sources[number];
                var off = (int)(value & 0xFFFFFF) * Step;
                if (!data.AsSpan(pos, Window).SequenceEqual(src.AsSpan(off, Window))) continue;
                var (length, back) = Extend(pos, src, off);
                if (length > bestLength || length == bestLength && (r < bestRank || r == bestRank && off < bestOffset))
                    (bestLength, bestBack, bestRank, bestNumber, bestOffset) = (length, back, r, number, off);
            }
            return new SmallMatch(bestLength, bestBack, bestNumber, bestOffset);
        }

        // How far the copy whose window at pos equals src's window at off reaches: on to the stretch's end and back to
        // its start (and the part's): its length, and how far before pos it starts.
        (int Length, int Back) Extend(int pos, byte[] src, int off)
        {
            var forward = Window + data.AsSpan(pos + Window, to - pos - Window).CommonPrefixLength(src.AsSpan(off + Window));
            var back = 0;
            while (pos - back > from && off - back > 0 && data[pos - back - 1] == src[off - back - 1]) back++;
            return (forward + back, back);
        }
    }

    // Takes the copies that reach furthest (the whole of what they cover between them), then fills and literals.
    void Cover(byte[] data, int from, int to, List<(int Start, int End, int Content, int Diagonal)> intervals, List<Op> ops)
    {
        intervals.Sort((a, b) => a.Start.CompareTo(b.Start));
        var cursor = from;
        var k = 0;
        (int End, int Content, int Diagonal) reach = (int.MinValue, -1, 0);
        while (cursor < to)
        {
            for (; k < intervals.Count && intervals[k].Start <= cursor; k++)
                if (intervals[k].End > reach.End) reach = (intervals[k].End, intervals[k].Content, intervals[k].Diagonal);
            if (reach.End > cursor)
            {
                var end = Math.Min(reach.End, to);
                AddCopy(ops, reach.Content, cursor + reach.Diagonal, end - cursor);
                cursor = end;
                continue;
            }
            var next = k < intervals.Count ? Math.Min(intervals[k].Start, to) : to;
            Uncovered(data, cursor, next, ops);
            cursor = next;
        }
    }

    // A copy; one that goes on from the copy before it (the same part, its next bytes) lengthens that one.
    static void AddCopy(List<Op> ops, int content, int srcStart, int length)
    {
        if (ops.Count > 0 && ops[^1] is { Kind: Recipe.Copy } last && last.Source == content && last.Offset + last.Length == srcStart)
            ops[^1] = last with { Length = last.Length + length };
        else ops.Add(new(Recipe.Copy, content, srcStart, length));
    }

    // Bytes [cursor, next) that no copy covers: runs of one byte value of MinFill bytes or more become fills, the rest
    // literals.
    static void Uncovered(byte[] data, int cursor, int next, List<Op> ops)
    {
        var literalStart = cursor;
        while (cursor < next)
        {
            var run = Run(data, cursor, next);
            if (run >= MinFill)
            {
                if (cursor > literalStart) ops.Add(new(Recipe.Literal, 0, literalStart, cursor - literalStart));
                ops.Add(new(Recipe.Fill, 0, data[cursor], run));
                cursor += run;
                literalStart = cursor;
            }
            else cursor += run;
        }
        if (next > literalStart) ops.Add(new(Recipe.Literal, 0, literalStart, next - literalStart));
    }

    // The covered ranges of a stretch, kept sorted and merged.
    sealed class Coverage
    {
        readonly List<(int Start, int End)> spans = [];

        public void Add(int start, int end)
        {
            var i = spans.FindIndex(s => s.End >= start);
            if (i < 0) { spans.Add((start, end)); return; }
            var j = i;
            while (j < spans.Count && spans[j].Start <= end)
            {
                start = Math.Min(start, spans[j].Start);
                end = Math.Max(end, spans[j].End);
                j++;
            }
            spans.RemoveRange(i, j - i);
            spans.Insert(i, (start, end));
        }

        public bool Covers(int lo, int hi) => spans.Any(s => s.Start <= lo && s.End >= hi);

        public List<(int Start, int End)> Gaps(int lo, int hi)
        {
            var gaps = new List<(int, int)>();
            var at = lo;
            foreach (var (start, end) in spans)
            {
                if (end <= at) continue;
                if (start >= hi) break;
                if (start > at) gaps.Add((at, start));
                at = Math.Max(at, end);
                if (at >= hi) break;
            }
            if (at < hi) gaps.Add((at, hi));
            return gaps;
        }
    }

    static int Common(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b) => a.CommonPrefixLength(b);

    static int Run(byte[] data, int pos, int to)
    {
        var rest = data.AsSpan(pos, to - pos);
        var other = rest.IndexOfAnyExcept(rest[0]);
        return other < 0 ? rest.Length : other;
    }

    static ulong HashAt(byte[] data, int pos)
    {
        ulong h = 0;
        for (var i = 0; i < Window; i++) h = h * Base + data[pos + i];
        return h;
    }

    static ulong Roll(ulong h, byte outgoing, byte incoming) => (h - outgoing * Top) * Base + incoming;

    static ulong Pow(ulong b, int e)
    {
        ulong r = 1;
        for (var i = 0; i < e; i++) r *= b;
        return r;
    }

    // Drops the game parts and tables kept between stretches (once the matching is done).
    public void ReleaseCaches()
    {
        cache.Clear();
        recent.Clear();
        cachedBytes = 0;
        windowTable?.Release();
    }

    // A game part this stretch compares with (kept for the stretch, so the cache may drop it meanwhile).
    byte[]? Part(int content, Dictionary<int, byte[]> parts, HashSet<int> unreadable)
    {
        if (parts.TryGetValue(content, out var bytes)) return bytes;
        bytes = Load(content);
        if (bytes is null) { unreadable.Add(content); return null; }
        return parts[content] = bytes;
    }

    byte[]? Load(int content)
    {
        if (cache.TryGetValue(content, out var node))
        {
            recent.Remove(node);
            recent.AddFirst(node);
            return node.Value.Bytes;
        }
        Stats.Load.Start();
        byte[]? found = null;
        var c = index.Contents[content];
        foreach (var place in c.Places)
        {
            var game = games.FirstOrDefault(g => g.Build == index.Builds[place.Build]);
            if (game is null) continue;
            found = game.ReadIn(place.Key, place.Part, index.Archives[place.Archive])
                .FirstOrDefault(bytes => bytes.Length == c.Size && SHA256.HashData(bytes).AsSpan().SequenceEqual(c.Sha256));
            if (found is not null) break;
        }
        Stats.Load.Stop();
        Stats.LoadedParts++;
        Stats.LoadedBytes += found?.Length ?? 0;
        cache[content] = recent.AddFirst((content, found));
        cachedBytes += found?.Length ?? 0;
        Stats.MaxCachedBytes = Math.Max(Stats.MaxCachedBytes, cachedBytes);
        while (cachedBytes > CacheBytes && recent.Count > 1)
        {
            var old = recent.Last!.Value;
            recent.RemoveLast();
            cache.Remove(old.Content);
            cachedBytes -= old.Bytes?.Length ?? 0;
        }
        return found;
    }

    [GeneratedRegex(@"\.patch_\d+$")]
    private static partial Regex PatchName();

    public sealed record Range(string File, long Start, long Length, ResourceKey Key, PartKind Part);

    // Where each resource part of the mod's archives (<name>.patch_<n> with its .gpu_resources and .stream) lies.
    public static List<Range> Layout(IReadOnlyList<(string Path, byte[] Bytes)> files) => Layout(ModFiles.InMemory(files));

    // From each archive's table alone (its head), so no archive is read whole.
    public static List<Range> Layout(ModFiles mod)
    {
        var ranges = new List<Range>();
        foreach (var path in mod.Paths)
        {
            if (!PatchName().IsMatch(path) || mod.Size(path) < 72) continue;
            var a = mod.Head(path, 72);
            if (BinaryPrimitives.ReadUInt32LittleEndian(a) != 0xF0000011) continue;
            var types = BinaryPrimitives.ReadInt32LittleEndian(a.AsSpan(4));
            var count = BinaryPrimitives.ReadInt32LittleEndian(a.AsSpan(8));
            if (types is < 0 or > 1 << 16 || count is < 0 or > 1 << 24) continue;
            var start = 72 + 32 * types;
            a = mod.Head(path, (int)Math.Min(mod.Size(path), Math.Min(int.MaxValue, start + 80L * count)));
            for (var i = 0; i < count && start + 80 * (i + 1) <= a.Length; i++)
            {
                var e = start + 80 * i;
                var key = new ResourceKey(BinaryPrimitives.ReadUInt64LittleEndian(a.AsSpan(e + 8)), BinaryPrimitives.ReadUInt64LittleEndian(a.AsSpan(e)));
                void Add(string file, int offsetAt, int sizeAt, PartKind part)
                {
                    var size = BinaryPrimitives.ReadUInt32LittleEndian(a.AsSpan(e + sizeAt));
                    if (size > 0) ranges.Add(new(file, (long)BinaryPrimitives.ReadUInt64LittleEndian(a.AsSpan(e + offsetAt)), size, key, part));
                }
                Add(path, 16, 56, PartKind.Main);
                Add(path + ".gpu_resources", 32, 64, PartKind.Gpu);
                Add(path + ".stream", 24, 60, PartKind.Stream);
            }
        }
        return ranges;
    }

    void Attribute(MigrationReport report, IEnumerable<Range> ranges, byte[] bytes, List<Op> ops)
    {
        foreach (var r in ranges)
        {
            long fromGame = 0, literal = 0, fill = 0, at = 0;
            var sources = new Dictionary<int, long>();
            foreach (var op in ops)
            {
                var overlap = Math.Min(at + op.Length, r.Start + r.Length) - Math.Max(at, r.Start);
                at += op.Length;
                if (overlap <= 0) continue;
                switch (op.Kind)
                {
                    case Recipe.Copy: fromGame += overlap; sources[op.Source] = sources.GetValueOrDefault(op.Source) + overlap; break;
                    case Recipe.Literal: literal += overlap; break;
                    default: fill += overlap; break;
                }
            }
            var inIndex = index.WithKey(r.Key).Any(c => index.Contents[c].Places.Any(p => p.Key == r.Key && p.Part == r.Part));
            var sha = r.Start + r.Length <= bytes.Length ? SHA256.HashData(bytes.AsSpan((int)r.Start, (int)r.Length)) : null;
            report.Resources.Add(new(r.File, r.Key, r.Part, r.Start, r.Length, fromGame, literal, fill, sources, inIndex, sha));
        }
    }
}
