using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core.AssetMigration;

// Every chunk Steam has shipped in the game's depots, from the depot manifests alone: each file of each build is a
// sequence of chunks (about 1 MiB), and the manifest gives each chunk's SHA-1 (of its uncompressed bytes) and its
// Adler-32 (rsync's rolling checksum, seeded with 0). Steam itself cuts a new build's files around the chunks it already
// shipped, found at any byte position with that rolling checksum; the scanner does the same over a mod's bytes, so any
// run of the game's bytes as Steam shipped them (about 2 MiB or more, under any name, at any position) is found and
// proven by SHA-1 without downloading anything. Shorter runs and edited content are what the name map and the content
// index are for. Builds whose data is compressed in bundles.*.nxa ship compressed chunks a mod never holds, so this
// covers the loose-archive builds (February 2024 to February 2026), where cut content comes from.
public sealed class SteamChunkIndex
{
    const string Magic = "HFSC";
    const int Version = 2;
    public const int Window = 1 << 20;
    const uint Mod = 65521;
    const int WindowMod = (int)(Window % Mod);

    public sealed record Build(string Label, DateTime Time);

    // A SHA-1 and a set of up to 256 builds, held as values (not an object per chunk).
    readonly record struct Sha1(ulong A, ulong B, uint C)
    {
        public static Sha1 Of(ReadOnlySpan<byte> s) =>
            new(BitConverter.ToUInt64(s), BitConverter.ToUInt64(s[8..]), BitConverter.ToUInt32(s[16..]));
        public void Write(BinaryWriter w) { w.Write(A); w.Write(B); w.Write(C); }
    }

    struct BuildBits
    {
        public ulong A, B, C, D;
        public readonly bool Has(int b) => ((b >> 6) switch { 0 => A, 1 => B, 2 => C, _ => D } & (1UL << (b & 63))) != 0;
        public void Set(int b)
        {
            var bit = 1UL << (b & 63);
            switch (b >> 6) { case 0: A |= bit; break; case 1: B |= bit; break; case 2: C |= bit; break; default: D |= bit; break; }
        }
    }
    public readonly record struct Match(long Offset, int Length, int Chunk);
    // A run of a mod file that is chunks Steam shipped, back to back: the game file they are from (as first seen) and
    // the builds that have them.
    public sealed record Run(string File, long Offset, long Length, string GameFile, DateTime First, DateTime Last, int Builds);

    public List<Build> Builds { get; } = [];
    // Distinct chunks: SHA-1, Adler-32, length, the builds that have them (a bit per build), and where the first one was
    // seen (a file and the chunk's position in it), for its neighbours.
    readonly List<Sha1> sha = [];
    readonly List<uint> adler = [];
    readonly List<int> length = [];
    readonly List<BuildBits> builds = [];
    readonly List<(int File, int Position)> first = [];
    readonly Dictionary<Sha1, int> bySha = [];
    // Files (each distinct file once: by its name and chunk list): their chunks in order.
    readonly List<(string Name, int[] Chunks)> files = [];
    // Each distinct file (its name and chunk list) once: only needed while builds are added.
    Dictionary<string, int>? fileIds;
    // Per build, its files.
    readonly List<int[]> buildFiles = [];
    Dictionary<uint, List<int>>? byAdler;
    // A bit per value of 24 bits of the Adler-32s of the whole-window chunks (2 MB): most positions of a scan stop here.
    ulong[]? adlerBits;
    static int Bit(uint adler) => (int)((adler * 0x9E3779B1u) >> 8);
    readonly Dictionary<int, Dictionary<int, (int File, long Offset)>> located = [];
    // Every place each chunk is in (file and position), across every version of every file: a chunk's neighbours differ
    // between builds, and odd-length chunks are only found as neighbours. Built when first needed.
    int[]? placeStart;
    long[]? places;

    public int ChunkCount => sha.Count;
    public long ChunkBytes => length.Sum(l => (long)l);

    // A build's files, each as its chunks in file order: (offset, uncompressed length, SHA-1, Adler-32).
    public void AddBuild(string label, DateTime time, IEnumerable<(string File, IEnumerable<(long Offset, int Length, byte[] Sha1, uint Adler)> Chunks)> buildFiles)
    {
        if (Builds.Any(b => b.Label == label)) return;
        if (Builds.Count == 256) throw new InvalidOperationException("A Steam chunk index holds at most 256 builds.");
        fileIds ??= KnownFiles();
        var b = Builds.Count;
        Builds.Add(new Build(label, time));
        var inBuild = new List<int>();
        foreach (var (name, chunks) in buildFiles)
        {
            var ids = new List<int>();
            foreach (var (_, len, sha1, sum) in chunks.OrderBy(c => c.Offset))
            {
                var id = ChunkId(Sha1.Of(sha1), sum, len);
                var bits = builds[id];
                bits.Set(b);
                builds[id] = bits;
                ids.Add(id);
            }
            inBuild.Add(FileId(name, ids));
        }
        this.buildFiles.Add([.. inBuild]);
        byAdler = null;
    }

    // Each distinct file the index has, by its name and chunk list.
    Dictionary<string, int> KnownFiles()
    {
        var known = new Dictionary<string, int>(StringComparer.Ordinal);
        for (var f = 0; f < files.Count; f++) known[files[f].Name + ":" + string.Join(",", files[f].Chunks)] = f;
        return known;
    }

    // The chunk with this SHA-1, a new one (in no build yet) when the index does not have it.
    int ChunkId(Sha1 digest, uint sum, int len)
    {
        if (bySha.TryGetValue(digest, out var id)) return id;
        id = sha.Count;
        bySha[digest] = id;
        sha.Add(digest);
        adler.Add(sum);
        length.Add(len);
        builds.Add(default);
        first.Add((-1, -1));
        return id;
    }

    // The file with this name and these chunks, a new one when no build had it (its chunks first seen there, unless
    // seen before).
    int FileId(string name, List<int> ids)
    {
        var key = name + ":" + string.Join(",", ids);
        if (fileIds!.TryGetValue(key, out var file)) return file;
        file = files.Count;
        files.Add((name, [.. ids]));
        fileIds[key] = file;
        for (var i = 0; i < ids.Count; i++)
            if (first[ids[i]].File < 0) first[ids[i]] = (file, i);
        return file;
    }

    public long Verified;
    // Positions the scans looked at.
    public long Scanned;
    public int FileCount => files.Count;
    public long PlaceCount => files.Sum(f => (long)f.Chunks.Length);
    public int UniformChunks;

    // The distinct whole-window chunks by the kind of game file they were first seen in.
    public Dictionary<string, int> WindowChunkKinds()
    {
        var kinds = new Dictionary<string, int>();
        for (var i = 0; i < sha.Count; i++)
        {
            if (length[i] != Window) continue;
            var kind = Kind(FileOf(i));
            kinds[kind] = kinds.GetValueOrDefault(kind) + 1;
        }
        return kinds;
    }

    // A game data file's kind: a loose archive (main, gpu, stream), a packed bundle, or other.
    public static string Kind(string file)
    {
        var name = file[(file.LastIndexOf('/') + 1)..];
        var dot = name.IndexOf('.');
        var stem = dot < 0 ? name : name[..dot];
        var loose = stem.Length == 16 && stem.All(char.IsAsciiHexDigitLower);
        return loose ? dot < 0 ? "loose main" : "loose " + name[(dot + 1)..] : name.StartsWith("bundles.", StringComparison.Ordinal) ? "packed bundle" : "other";
    }

    public int BuildIndex(string label) => Builds.FindIndex(b => b.Label == label);

    // A build's distinct chunk bytes, and those of them no build in have has: what it costs to rebuild that build's
    // files from the have builds' files (read locally) plus downloads, the way a Steam update does.
    // The distinct chunk bytes these builds have between them that no build in have has.
    public long MissingUnion(IEnumerable<int> wanted, IReadOnlyCollection<int> have)
    {
        var seen = new HashSet<int>();
        long missing = 0;
        foreach (var build in wanted)
            foreach (var file in buildFiles[build])
                foreach (var c in files[file].Chunks)
                    if (seen.Add(c) && !have.Any(builds[c].Has)) missing += length[c];
        return missing;
    }

    public (long Total, long Missing) Delta(int build, IReadOnlyCollection<int> have)
    {
        var seen = new HashSet<int>();
        long total = 0, missing = 0;
        foreach (var file in buildFiles[build])
            foreach (var c in files[file].Chunks)
            {
                if (!seen.Add(c)) continue;
                total += length[c];
                if (!have.Any(builds[c].Has)) missing += length[c];
            }
        return (total, missing);
    }

    public IEnumerable<int> BuildIdsOf(int chunk)
    {
        for (var b = 0; b < Builds.Count; b++)
            if (builds[chunk].Has(b)) yield return b;
    }

    // Where a build has this chunk: the file and the chunk's offset in it (the first place, if it has it more than once).
    public (string File, long Offset)? Locate(int chunk, int build)
    {
        if (!located.TryGetValue(build, out var map))
        {
            located[build] = map = [];
            foreach (var file in buildFiles[build])
            {
                long offset = 0;
                foreach (var c in files[file].Chunks)
                {
                    map.TryAdd(c, (file, offset));
                    offset += length[c];
                }
            }
        }
        return map.TryGetValue(chunk, out var at) ? (files[at.File].Name, at.Offset) : null;
    }

    // Every run of these files that is chunks Steam shipped. With only, just the runs that have a whole chunk over a
    // byte of those ranges of each file (the bytes a mod still ships), and files with none are not read.
    public List<Run> Runs(IEnumerable<(string Path, byte[] Bytes)> mod) => Runs(mod.Select(f => (f.Path, (Func<byte[]>)(() => f.Bytes))), null);

    public List<Run> Runs(IEnumerable<(string Path, Func<byte[]> Read)> mod, IReadOnlyDictionary<string, List<(long Start, long Length)>>? only)
    {
        var runs = new List<Run>();
        foreach (var (path, read) in mod)
        {
            List<(long Start, long Length)>? ranges = null;
            if (only is not null && (!only.TryGetValue(path, out ranges) || ranges.Count == 0)) continue;
            var bytes = read();
            var found = Scan(bytes, ranges);
            for (var i = 0; i < found.Count;)
            {
                var j = i + 1;
                while (j < found.Count && found[j].Offset == found[j - 1].Offset + found[j - 1].Length) j++;
                var ids = found.Skip(i).Take(j - i).SelectMany(m => BuildIdsOf(m.Chunk)).Distinct().Select(b => Builds[b]).ToList();
                runs.Add(new Run(path, found[i].Offset, found[j - 1].Offset + found[j - 1].Length - found[i].Offset, FileOf(found[i].Chunk),
                    ids.Min(b => b.Time), ids.Max(b => b.Time), ids.Count));
                i = j;
            }
        }
        return runs;
    }

    public IEnumerable<Build> BuildsOf(int chunk)
    {
        for (var b = 0; b < Builds.Count; b++)
            if (builds[chunk].Has(b)) yield return Builds[b];
    }

    public string FileOf(int chunk) => first[chunk].File >= 0 ? files[first[chunk].File].Name : "?";

    public static uint Adler32(ReadOnlySpan<byte> data)
    {
        uint a = 0, b = 0;
        foreach (var x in data)
        {
            a = (a + x) % Mod;
            b = (b + a) % Mod;
        }
        return (b << 16) | a;
    }

    void Places()
    {
        if (places is not null) return;
        var count = new int[sha.Count + 1];
        foreach (var (_, chunks) in files) foreach (var c in chunks) count[c + 1]++;
        for (var i = 0; i < sha.Count; i++) count[i + 1] += count[i];
        var at = (int[])count.Clone();
        var list = new long[count[sha.Count]];
        for (var f = 0; f < files.Count; f++)
            for (var i = 0; i < files[f].Chunks.Length; i++) list[at[files[f].Chunks[i]]++] = ((long)f << 32) | (uint)i;
        placeStart = count;
        places = list;
    }

    // The chunks next to this one (step +1 after it, -1 before it) in any version of any file that has it.
    IEnumerable<int> Neighbours(int chunk, int step)
    {
        Places();
        var seen = new HashSet<int>();
        for (var k = placeStart![chunk]; k < placeStart[chunk + 1]; k++)
        {
            var (file, position) = ((int)(places![k] >> 32), (int)(uint)places[k] + step);
            if (position >= 0 && position < files[file].Chunks.Length && seen.Add(files[file].Chunks[position])) yield return files[file].Chunks[position];
        }
    }

    // The runs of data that are chunks Steam shipped: 1 MiB chunks found by the rolling checksum at any position and
    // proven by SHA-1, then their neighbours (any length, in any version of a file that has them) checked in place.
    // Adler-32 is weak over a 1 MiB window (about one position in 10,000 of any data matches some chunk's), so finding
    // the positions, each proven by SHA-1, runs on every core; the runs are then followed in order. Chunks of the packed
    // bundles are left out: they are compressed, and a mod never holds them.
    // With only, the runs are those with a whole chunk over a byte of those ranges (found from it in both directions).
    public List<Match> Scan(byte[] data, IReadOnlyList<(long Start, long Length)>? only = null)
    {
        var found = new List<Match>();
        if (data.Length < Window) return found;
        if (byAdler is null) IndexAdler();

        // 1. Every position whose window is a chunk (over a byte of the ranges asked for), in parallel pieces.
        long positions = data.Length - Window + 1;
        var wanted = Wanted(only, positions);
        Scanned += wanted.Sum(w => w.Hi - w.Lo);
        var piece = Math.Max(8L << 20, wanted.Sum(w => w.Hi - w.Lo) / Environment.ProcessorCount + 1);
        var pieces = wanted.SelectMany(w => Enumerable.Range(0, (int)((w.Hi - w.Lo + piece - 1) / piece))
            .Select(i => (Lo: w.Lo + i * piece, Hi: Math.Min(w.Hi, w.Lo + (i + 1) * piece)))).ToList();
        var hits = new System.Collections.Concurrent.ConcurrentBag<(long At, int Chunk)>();
        Parallel.ForEach(pieces, p =>
        {
            foreach (var hit in Positions(data, p.Lo, p.Hi)) hits.Add(hit);
        });

        // 2. In order: a hit that is not inside a run already found starts one, followed through its neighbours.
        var digest = new byte[20];
        long matchedEnd = 0;
        foreach (var (p, hit) in hits.OrderBy(h => h.At).ThenBy(h => h.Chunk))
        {
            if (p < matchedEnd) continue;
            matchedEnd = Follow(data, p, hit, matchedEnd, found, digest);
        }
        return found;
    }

    // The whole-window chunks by Adler-32, and their bits.
    void IndexAdler()
    {
        byAdler = [];
        adlerBits = new ulong[(1 << 24) / 64];
        // A chunk of one byte value (padding) says nothing about where bytes came from, and in a long run of that
        // byte every position would match it.
        var uniform = new HashSet<int>();
        var fill = new byte[Window];
        for (var v = 0; v < 256; v++)
        {
            Array.Fill(fill, (byte)v);
            if (bySha.TryGetValue(Sha1.Of(SHA1.HashData(fill)), out var id)) uniform.Add(id);
        }
        UniformChunks = uniform.Count;
        for (var i = 0; i < sha.Count; i++)
            if (length[i] == Window && !uniform.Contains(i) && Kind(FileOf(i)) != "packed bundle")
            {
                if (!byAdler.TryGetValue(adler[i], out var list)) byAdler[adler[i]] = list = [];
                list.Add(i);
                var bit = Bit(adler[i]);
                adlerBits[bit >> 6] |= 1UL << bit;
            }
    }

    // The window positions to scan, in order and merged: those whose window has a byte of the ranges asked for (all
    // of them without ranges).
    static List<(long Lo, long Hi)> Wanted(IReadOnlyList<(long Start, long Length)>? only, long positions)
    {
        var wanted = new List<(long Lo, long Hi)>();
        IEnumerable<(long Start, long Length)> asked = only is null ? [(0L, positions)] : only.OrderBy(r => r.Start);
        foreach (var (start, length) in asked)
        {
            long lo = Math.Max(0, start - Window + 1), hi = Math.Min(positions, start + length);
            if (hi <= lo) continue;
            if (wanted.Count > 0 && lo <= wanted[^1].Hi) wanted[^1] = (wanted[^1].Lo, Math.Max(wanted[^1].Hi, hi));
            else wanted.Add((lo, hi));
        }
        return wanted;
    }

    // The run a hit starts: the hit's chunk, its neighbours before it (back to the end of the run before) and after it,
    // added to found in order. Returns where the run ends.
    long Follow(byte[] data, long p, int hit, long matchedEnd, List<Match> found, byte[] digest)
    {
        var start = p;
        var before = new List<Match>();
        for (var last = hit; ;)
        {
            var next = Neighbours(last, -1).FirstOrDefault(c => start - length[c] >= matchedEnd && Is(data, c, start - length[c], digest), -1);
            if (next < 0) break;
            start -= length[next];
            before.Add(new Match(start, length[next], next));
            last = next;
        }
        before.Reverse();
        found.AddRange(before);
        found.Add(new Match(p, Window, hit));
        var end = p + Window;
        for (var last = hit; ;)
        {
            var next = Neighbours(last, 1).FirstOrDefault(c => Is(data, c, end, digest), -1);
            if (next < 0) break;
            found.Add(new Match(end, length[next], next));
            end += length[next];
            last = next;
        }
        return end;
    }

    // The positions in [lo, hi) whose 1 MiB window is a chunk (the first one that proves by SHA-1).
    List<(long At, int Chunk)> Positions(byte[] data, long lo, long hi)
    {
        var found = new List<(long, int)>();
        var bits = adlerBits!;
        var digest = new byte[20];
        uint x = 0, y = 0;
        for (var i = 0; i < Window; i++) { x = (x + data[lo + i]) % Mod; y = (y + x) % Mod; }
        int a = (int)x, b = (int)y;
        for (var p = lo; ; p++)
        {
            var sum = ((uint)b << 16) | (uint)a;
            var bit = Bit(sum);
            if ((bits[bit >> 6] & (1UL << bit)) != 0) Prove(data, p, sum, digest, found);
            if (p + 1 >= hi) break;
            // Rolled by one byte: a gains the incoming byte and loses the outgoing one; b loses Window times the outgoing
            // byte (Window % Mod is 240, so 240 * 255 stays under Mod) and gains the new a. One correction each.
            int outgoing = data[p], incoming = data[p + Window];
            a = Wrap(a + incoming - outgoing);
            b = Wrap(b + a - WindowMod * outgoing);
        }
        return found;
    }

    // A sum rolled by one byte back into [0, Mod): it is at most one Mod outside.
    static int Wrap(int v) => v < 0 ? v + (int)Mod : v >= Mod ? v - (int)Mod : v;

    // The first chunk with this Adler-32 that the window at p is (by SHA-1), if any.
    void Prove(byte[] data, long p, uint sum, byte[] digest, List<(long, int)> found)
    {
        if (!byAdler!.TryGetValue(sum, out var candidates)) return;
        foreach (var c in candidates)
            if (Is(data, c, p, digest)) { found.Add((p, c)); break; }
    }

    bool Is(byte[] data, int chunk, long at, byte[] digest)
    {
        if (at < 0 || at + length[chunk] > data.Length) return false;
        Interlocked.Increment(ref Verified);
        SHA1.HashData(data.AsSpan((int)at, length[chunk]), digest);
        return Sha1.Of(digest) == sha[chunk];
    }

    public void Save(string path)
    {
        var temp = path + ".tmp";
        using (var w = new BinaryWriter(File.Create(temp), Encoding.UTF8))
        {
            w.Write(Encoding.ASCII.GetBytes(Magic));
            w.Write(Version);
            w.Write(Builds.Count);
            foreach (var build in Builds) { w.Write(build.Label); w.Write(build.Time.ToBinary()); }
            w.Write(sha.Count);
            for (var i = 0; i < sha.Count; i++)
            {
                sha[i].Write(w);
                w.Write(adler[i]);
                w.Write(length[i]);
                w.Write(builds[i].A); w.Write(builds[i].B); w.Write(builds[i].C); w.Write(builds[i].D);
                w.Write(first[i].File);
                w.Write(first[i].Position);
            }
            w.Write(files.Count);
            foreach (var (name, chunks) in files)
            {
                w.Write(name);
                w.Write(chunks.Length);
                foreach (var c in chunks) w.Write(c);
            }
            foreach (var list in buildFiles)
            {
                w.Write(list.Length);
                foreach (var f in list) w.Write(f);
            }
        }
        File.Move(temp, path, true);
    }

    public static SteamChunkIndex Load(string path)
    {
        using var r = new BinaryReader(File.OpenRead(path), Encoding.UTF8);
        if (Encoding.ASCII.GetString(r.ReadBytes(4)) != Magic || r.ReadInt32() != Version) throw new InvalidDataException($"{path} is not a HellForge Steam chunk index.");
        var index = new SteamChunkIndex();
        var count = r.ReadInt32();
        for (var i = 0; i < count; i++) index.Builds.Add(new Build(r.ReadString(), DateTime.FromBinary(r.ReadInt64())));
        count = r.ReadInt32();
        for (var i = 0; i < count; i++)
        {
            var s = r.ReadBytes(20);
            var key = Sha1.Of(s);
            index.bySha[key] = i;
            index.sha.Add(key);
            index.adler.Add(r.ReadUInt32());
            index.length.Add(r.ReadInt32());
            index.builds.Add(new BuildBits { A = r.ReadUInt64(), B = r.ReadUInt64(), C = r.ReadUInt64(), D = r.ReadUInt64() });
            index.first.Add((r.ReadInt32(), r.ReadInt32()));
        }
        count = r.ReadInt32();
        for (var i = 0; i < count; i++)
        {
            var name = r.ReadString();
            var chunks = new int[r.ReadInt32()];
            for (var c = 0; c < chunks.Length; c++) chunks[c] = r.ReadInt32();
            index.files.Add((name, chunks));
        }
        foreach (var _ in index.Builds)
        {
            var list = new int[r.ReadInt32()];
            for (var f = 0; f < list.Length; f++) list[f] = r.ReadInt32();
            index.buildFiles.Add(list);
        }
        return index;
    }
}
