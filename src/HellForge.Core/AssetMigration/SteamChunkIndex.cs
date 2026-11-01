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

    public sealed record Build(string Label, DateTime Time);
    public readonly record struct Match(long Offset, int Length, int Chunk);
    // A run of a mod file that is chunks Steam shipped, back to back: the game file they are from (as first seen) and
    // the builds that have them.
    public sealed record Run(string File, long Offset, long Length, string GameFile, DateTime First, DateTime Last, int Builds);

    public List<Build> Builds { get; } = [];
    // Distinct chunks: SHA-1, Adler-32, length, the builds that have them (a bit per build), and where the first one was
    // seen (a file and the chunk's position in it), for its neighbours.
    readonly List<byte[]> sha = [];
    readonly List<uint> adler = [];
    readonly List<int> length = [];
    readonly List<ulong[]> builds = [];
    readonly List<(int File, int Position)> first = [];
    readonly Dictionary<string, int> bySha = new(StringComparer.Ordinal);
    // Files (each distinct file once: by its name and chunk list): their chunks in order.
    readonly List<(string Name, int[] Chunks)> files = [];
    readonly Dictionary<string, int> fileIds = new(StringComparer.Ordinal);
    // Per build, its files.
    readonly List<int[]> buildFiles = [];
    Dictionary<uint, List<int>>? byAdler;
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
        var b = Builds.Count;
        Builds.Add(new Build(label, time));
        var inBuild = new List<int>();
        foreach (var (name, chunks) in buildFiles)
        {
            var ids = new List<int>();
            foreach (var (_, len, sha1, sum) in chunks.OrderBy(c => c.Offset))
            {
                var hex = Convert.ToHexString(sha1);
                if (!bySha.TryGetValue(hex, out var id))
                {
                    id = sha.Count;
                    bySha[hex] = id;
                    sha.Add(sha1);
                    adler.Add(sum);
                    length.Add(len);
                    builds.Add(new ulong[4]);
                    first.Add((-1, -1));
                }
                builds[id][b / 64] |= 1UL << (b % 64);
                ids.Add(id);
            }
            var key = name + ":" + string.Join(",", ids);
            if (!fileIds.TryGetValue(key, out var file))
            {
                file = files.Count;
                files.Add((name, [.. ids]));
                fileIds[key] = file;
                for (var i = 0; i < ids.Count; i++)
                    if (first[ids[i]].File < 0) first[ids[i]] = (file, i);
            }
            inBuild.Add(file);
        }
        this.buildFiles.Add([.. inBuild]);
        byAdler = null;
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
                    if (seen.Add(c) && !have.Any(h => (builds[c][h / 64] & (1UL << (h % 64))) != 0)) missing += length[c];
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
                if (!have.Any(h => (builds[c][h / 64] & (1UL << (h % 64))) != 0)) missing += length[c];
            }
        return (total, missing);
    }

    public IEnumerable<int> BuildIdsOf(int chunk)
    {
        for (var b = 0; b < Builds.Count; b++)
            if ((builds[chunk][b / 64] & (1UL << (b % 64))) != 0) yield return b;
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

    // Every run of these files that is chunks Steam shipped.
    public List<Run> Runs(IEnumerable<(string Path, byte[] Bytes)> mod)
    {
        var runs = new List<Run>();
        foreach (var (path, bytes) in mod)
        {
            var found = Scan(bytes);
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
            if ((builds[chunk][b / 64] & (1UL << (b % 64))) != 0) yield return Builds[b];
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
    public List<Match> Scan(byte[] data)
    {
        var found = new List<Match>();
        if (data.Length < Window) return found;
        if (byAdler is null)
        {
            byAdler = [];
            for (var i = 0; i < sha.Count; i++)
                if (length[i] == Window)
                {
                    if (!byAdler.TryGetValue(adler[i], out var list)) byAdler[adler[i]] = list = [];
                    list.Add(i);
                }
        }
        var digest = new byte[20];
        long p = 0, matchedEnd = 0;
        uint a = 0, b = 0;
        void Init(long at)
        {
            a = 0; b = 0;
            for (var i = 0; i < Window; i++) { a = (a + data[at + i]) % Mod; b = (b + a) % Mod; }
        }
        bool Is(int chunk, long at)
        {
            if (at < 0 || at + length[chunk] > data.Length) return false;
            SHA1.HashData(data.AsSpan((int)at, length[chunk]), digest);
            return digest.SequenceEqual(sha[chunk]);
        }
        Init(0);
        while (true)
        {
            if (byAdler.TryGetValue((b << 16) | a, out var candidates))
            {
                var hit = -1;
                foreach (var c in candidates)
                    if (Is(c, p)) { hit = c; break; }
                if (hit >= 0)
                {
                    // Neighbours before (down to the previous match) and after.
                    var start = p;
                    var before = new List<Match>();
                    for (var last = hit; ;)
                    {
                        var next = Neighbours(last, -1).FirstOrDefault(c => start - length[c] >= matchedEnd && Is(c, start - length[c]), -1);
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
                        var next = Neighbours(last, 1).FirstOrDefault(c => Is(c, end), -1);
                        if (next < 0) break;
                        found.Add(new Match(end, length[next], next));
                        end += length[next];
                        last = next;
                    }
                    matchedEnd = end;
                    p = end;
                    if (p + Window > data.Length) break;
                    Init(p);
                    continue;
                }
            }
            if (p + Window >= data.Length) break;
            uint outgoing = data[p], incoming = data[p + Window];
            a = (a + Mod - outgoing + incoming) % Mod;
            b = (uint)(((long)b - (long)Window % Mod * outgoing % Mod + Mod * 2L + a) % Mod);
            p++;
        }
        return found;
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
                w.Write(sha[i]);
                w.Write(adler[i]);
                w.Write(length[i]);
                foreach (var m in builds[i]) w.Write(m);
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
            index.bySha[Convert.ToHexString(s)] = i;
            index.sha.Add(s);
            index.adler.Add(r.ReadUInt32());
            index.length.Add(r.ReadInt32());
            index.builds.Add([r.ReadUInt64(), r.ReadUInt64(), r.ReadUInt64(), r.ReadUInt64()]);
            index.first.Add((r.ReadInt32(), r.ReadInt32()));
        }
        count = r.ReadInt32();
        for (var i = 0; i < count; i++)
        {
            var name = r.ReadString();
            var chunks = new int[r.ReadInt32()];
            for (var c = 0; c < chunks.Length; c++) chunks[c] = r.ReadInt32();
            index.fileIds[name + ":" + string.Join(",", chunks)] = index.files.Count;
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
