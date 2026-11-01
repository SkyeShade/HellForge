using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// What a migration found, per file, per resource of the mod's archives, and per piece of the game it reuses.
public sealed class MigrationReport
{
    public sealed record FileLine(string Path, long Size, long FromGame, long Literal, long Fill, bool InRecipe);
    public sealed record ResourceLine(string File, ResourceKey Key, PartKind Part, long Size, long FromGame, long Literal, long Fill, Dictionary<int, long> Sources);

    public List<FileLine> Files { get; } = [];
    public List<ResourceLine> Resources { get; } = [];
    public Dictionary<int, long> SourceBytes { get; } = [];
    // Game parts the mod shares content with that none of the given game folders could supply: their bytes stay in the
    // mod, so it needs a review (or the build they come from).
    public HashSet<int> Unreadable { get; } = [];
}

// Turns a mod into a recipe. Each file's content-defined anchors are looked up in the source index; the game contents
// it shares anchors with (and the contents of the resources its archives replace) are read from the game folders
// and matched against the file at every byte (32-byte windows, sampled every 16 bytes of each content), so a reused
// range is found wherever it sits and however short the edit next to it. Ranges of 48 bytes or more become copies; runs
// of one byte become fills; the rest is the mod's own bytes.
public sealed partial class Migrator(SourceIndex index, IReadOnlyList<IGameResources> games)
{
    const int Window = 32, Step = 16, MinCopy = 48, MinFill = 32, MaxCandidates = 96;
    const long MaxCandidateBytes = 1L << 30;
    const ulong Base = 0x100000001B3;
    static readonly ulong Top = Pow(Base, Window - 1);

    readonly Dictionary<int, byte[]?> loaded = [];

    public (Recipe Recipe, byte[] Literals, MigrationReport Report) Migrate(IReadOnlyList<(string Path, byte[] Bytes)> files, Action<string>? log = null)
    {
        var recipe = new Recipe();
        var literals = new MemoryStream();
        var report = new MigrationReport();
        var sourceIds = new Dictionary<int, int>();
        var layout = Layout(files);
        foreach (var (path, bytes) in files)
        {
            var keys = layout.Where(l => l.File == path).Select(l => l.Key).ToHashSet();
            var (ops, candidates) = Match(bytes, keys, report.Unreadable);
            long fromGame = ops.Where(o => o.Kind == Recipe.Copy).Sum(o => o.Length);
            long fill = ops.Where(o => o.Kind == Recipe.Fill).Sum(o => o.Length);
            var inRecipe = fromGame > 0;
            report.Files.Add(new(path, bytes.Length, fromGame, bytes.Length - fromGame - fill, fill, inRecipe));
            log?.Invoke($"{path}: {bytes.Length:N0} bytes, {fromGame:N0} from the game ({candidates} game parts compared)");
            if (!inRecipe) continue;
            var file = new RecipeFile { Path = path, Size = bytes.Length, Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() };
            long at = 0;
            foreach (var op in ops)
            {
                switch (op.Kind)
                {
                    case Recipe.Copy:
                        if (!sourceIds.TryGetValue(op.Source, out var sid))
                        {
                            sid = recipe.Sources.Count;
                            sourceIds[op.Source] = sid;
                            recipe.Sources.Add(SourceOf(op.Source));
                        }
                        file.Ops.Add([Recipe.Copy, sid, op.Offset, op.Length]);
                        report.SourceBytes[op.Source] = report.SourceBytes.GetValueOrDefault(op.Source) + op.Length;
                        break;
                    case Recipe.Literal:
                        file.Ops.Add([Recipe.Literal, literals.Length, op.Length]);
                        literals.Write(bytes, (int)at, op.Length);
                        break;
                    default:
                        file.Ops.Add([Recipe.Fill, op.Offset, op.Length]);
                        break;
                }
                at += op.Length;
            }
            recipe.Files.Add(file);
            Attribute(report, layout.Where(l => l.File == path), ops);
        }
        return (recipe, literals.ToArray(), report);
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
            }).ToList(),
        };
    }

    // Kind is Copy (Source, Offset), Literal or Fill (Offset holds the byte value).
    public readonly record struct Op(int Kind, int Source, int Offset, int Length);

    (List<Op> Ops, int Candidates) Match(byte[] data, HashSet<ResourceKey> keys, HashSet<int> unreadable)
    {
        var hits = new Dictionary<int, int>();
        foreach (var (_, anchor) in ContentChunks.Anchors(data))
            foreach (var (content, _) in index.Lookup(anchor))
                hits[content] = hits.GetValueOrDefault(content) + 1;
        foreach (var key in keys)
            foreach (var content in index.WithKey(key))
                hits[content] = hits.GetValueOrDefault(content) + 1_000_000;

        var candidates = new List<(int Content, byte[] Bytes)>();
        long total = 0;
        foreach (var (content, _) in hits.OrderByDescending(h => h.Value).ThenBy(h => h.Key))
        {
            if (candidates.Count >= MaxCandidates || total + index.Contents[content].Size > MaxCandidateBytes) continue;
            var bytes = Load(content);
            if (bytes is null) { unreadable.Add(content); continue; }
            candidates.Add((content, bytes));
            total += bytes.Length;
        }

        var table = new List<(ulong Hash, int Candidate, int Offset)>();
        for (var c = 0; c < candidates.Count; c++)
        {
            var src = candidates[c].Bytes;
            if (src.Length < Window) continue;
            var h = HashAt(src, 0);
            for (var i = 0; ; i++)
            {
                if (i % Step == 0 && !ContentChunks.Uniform(src.AsSpan(i, Window))) table.Add((h, c, i));
                if (i + Window >= src.Length) break;
                h = Roll(h, src[i], src[i + Window]);
            }
        }
        table.Sort((a, b) => a.Hash.CompareTo(b.Hash));
        var keysSorted = table.Select(t => t.Hash).ToArray();

        var ops = new List<Op>();
        int literalStart = 0, pos = 0;
        ulong hash = 0;
        var valid = false;
        void Flush(int end)
        {
            if (end > literalStart) ops.Add(new(Recipe.Literal, 0, literalStart, end - literalStart));
        }
        while (pos < data.Length)
        {
            var run = Run(data, pos);
            if (run >= MinFill)
            {
                Flush(pos);
                ops.Add(new(Recipe.Fill, 0, data[pos], run));
                pos += run;
                literalStart = pos;
                valid = false;
                continue;
            }
            if (pos + Window > data.Length) break;
            if (!valid) { hash = HashAt(data, pos); valid = true; }

            int bestLength = 0, bestBack = 0, bestCandidate = -1, bestOffset = 0;
            var at = Array.BinarySearch(keysSorted, hash);
            if (at >= 0)
            {
                while (at > 0 && keysSorted[at - 1] == hash) at--;
                for (var tried = 0; at < keysSorted.Length && keysSorted[at] == hash && tried < 32; at++, tried++)
                {
                    var (_, c, off) = table[at];
                    var src = candidates[c].Bytes;
                    if (!data.AsSpan(pos, Window).SequenceEqual(src.AsSpan(off, Window))) continue;
                    var forward = Window + Common(data.AsSpan(pos + Window), src.AsSpan(off + Window));
                    var back = 0;
                    while (pos - back > literalStart && off - back > 0 && data[pos - back - 1] == src[off - back - 1]) back++;
                    if (forward + back > bestLength) (bestLength, bestBack, bestCandidate, bestOffset) = (forward + back, back, c, off);
                }
            }
            if (bestLength >= MinCopy)
            {
                var start = pos - bestBack;
                Flush(start);
                var content = candidates[bestCandidate].Content;
                var srcStart = bestOffset - bestBack;
                if (ops.Count > 0 && ops[^1] is { Kind: Recipe.Copy } last && last.Source == content && last.Offset + last.Length == srcStart)
                    ops[^1] = last with { Length = last.Length + bestLength };
                else ops.Add(new(Recipe.Copy, content, srcStart, bestLength));
                pos = start + bestLength;
                literalStart = pos;
                valid = false;
                continue;
            }
            if (pos + Window < data.Length) hash = Roll(hash, data[pos], data[pos + Window]);
            else valid = false;
            pos++;
        }
        Flush(data.Length);
        return (ops, candidates.Count);
    }

    static int Common(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b) => a.CommonPrefixLength(b);

    static int Run(byte[] data, int pos)
    {
        var rest = data.AsSpan(pos);
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

    byte[]? Load(int content)
    {
        if (loaded.TryGetValue(content, out var cached)) return cached;
        byte[]? found = null;
        var c = index.Contents[content];
        foreach (var place in c.Places)
        {
            var game = games.FirstOrDefault(g => g.Build == index.Builds[place.Build]);
            var bytes = game?.Read(place.Key, place.Part);
            if (bytes is not null && bytes.Length == c.Size && SHA256.HashData(bytes).AsSpan().SequenceEqual(c.Sha256)) { found = bytes; break; }
        }
        loaded[content] = found;
        return found;
    }

    [GeneratedRegex(@"\.patch_\d+$")]
    private static partial Regex PatchName();

    public sealed record Range(string File, long Start, long Length, ResourceKey Key, PartKind Part);

    // Where each resource part of the mod's archives (<name>.patch_<n> with its .gpu_resources and .stream) lies.
    public static List<Range> Layout(IReadOnlyList<(string Path, byte[] Bytes)> files)
    {
        var ranges = new List<Range>();
        foreach (var (path, a) in files)
        {
            if (!PatchName().IsMatch(path) || a.Length < 72 || BinaryPrimitives.ReadUInt32LittleEndian(a) != 0xF0000011) continue;
            var types = BinaryPrimitives.ReadInt32LittleEndian(a.AsSpan(4));
            var count = BinaryPrimitives.ReadInt32LittleEndian(a.AsSpan(8));
            var start = 72 + 32 * types;
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

    static void Attribute(MigrationReport report, IEnumerable<Range> ranges, List<Op> ops)
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
            report.Resources.Add(new(r.File, r.Key, r.Part, r.Length, fromGame, literal, fill, sources));
        }
    }
}
