using System.Security.Cryptography;
using System.Text;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

public enum PartKind : byte { Main, Gpu, Stream }

// Where a piece of the game's content is: a build (an index into SourceIndex.Builds), a resource, one of its parts, and
// the archive that holds it (an index into SourceIndex.Archives: the 16-hex-digit archive name), so a player's launcher
// knows which of an old build's files it needs.
public readonly record struct Place(int Build, ResourceKey Key, PartKind Part, int Archive);

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
    // Every resource with the archive that holds it, its parts read only when asked for. Resources come grouped by
    // archive.
    IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All();
    // The resources of just these archives, each archive with every entry it has (a loose build reads only their tables,
    // where All reads every table of the build first).
    IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> In(IReadOnlySet<string> archives) =>
        All().Where(r => archives.Contains(r.Archive));
    // The resources whose bytes lie in this range of one of the build's data files (by its name in the data folder), or
    // none when the layout does not say.
    IEnumerable<(ResourceKey Key, string Archive)> At(string file, long offset, long length) => [];
    // A hash of the archive's files known without reading them (a depot manifest's SHA-1s), or null: an archive with
    // the same hash in an earlier build holds the same resources, so the index takes them without reading.
    string? ArchiveHash(string archive) => null;
    // A hash of just the file that holds the archive's table (its main file), or null: the name map needs only that,
    // so an archive whose textures or sounds changed but whose table did not is not read again.
    string? TableHash(string archive) => ArchiveHash(archive);
    // Each archive with its resource keys, read only when asked for (a loose build reads just that archive's table).
    IEnumerable<(string Archive, Func<IReadOnlyList<ResourceKey>> Keys)> Tables() =>
        All().GroupBy(r => r.Archive).Select(g => (g.Key, (Func<IReadOnlyList<ResourceKey>>)(() => g.Select(r => r.Key).ToList())));
    byte[]? Read(ResourceKey key, PartKind part);
    // The part as this archive holds it: a build can have one resource in several archives (or, in a transition build,
    // both packed and loose) with different bytes. Every copy the build has there, for the caller to check by hash;
    // without an archive, or for a layout that does not say, the build's own copy.
    IReadOnlyList<byte[]> ReadIn(ResourceKey key, PartKind part, string? archive) => Read(key, part) is { } bytes ? [bytes] : [];
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
public sealed class GameResources(IDataFiles files, string build) : IGameResources, IDisposable
{
    readonly GameDataReader reader = new(files);

    public GameResources(string folder, string build) : this(new FolderFiles(folder), build) { }
    Dictionary<ResourceKey, GameDataReader.Location>? locations;

    public string Build => build;

    // Parts the reader cannot read whole (over its 256 MB limit) are left out and counted.
    public int Skipped { get; private set; }

    public bool HasArchive(string archive) => reader.HasArchive(archive);

    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> In(IReadOnlySet<string> archives) => Entries(reader.ResourcesIn(archives));

    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All() => Entries(reader.Resources());

    IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> Entries(IEnumerable<(ResourceKey Key, GameDataReader.Location At)> resources)
    {
        foreach (var (key, at) in resources)
        {
            byte[] Part(GameDataReader.Location only, PartKind kind)
            {
                try { return Parts.Of(reader.Read(only), kind); }
                catch (InvalidDataException) { Skipped++; return []; }
            }
            yield return (key, at.Archive, () => new ResourceParts(
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
        return ReadAt(at, part);
    }

    byte[]? ReadAt(GameDataReader.Location at, PartKind part)
    {
        var only = part switch
        {
            PartKind.Main => at with { GpuSize = 0, StreamSize = 0 },
            PartKind.Gpu => at with { MainSize = 0, StreamSize = 0 },
            _ => at with { MainSize = 0, GpuSize = 0 },
        };
        try { return Parts.Of(reader.Read(only), part); }
        catch (InvalidDataException) { return null; }
    }

    readonly Dictionary<string, Dictionary<ResourceKey, GameDataReader.Location>> archiveTables = new(StringComparer.Ordinal);

    public IReadOnlyList<byte[]> ReadIn(ResourceKey key, PartKind part, string? archive)
    {
        if (archive is null || !reader.HasArchive(archive)) return Read(key, part) is { } own ? [own] : [];
        if (!archiveTables.TryGetValue(archive, out var table))
        {
            archiveTables[archive] = table = [];
            foreach (var (k, at) in reader.ResourcesIn([archive])) table.TryAdd(k, at);
        }
        return table.TryGetValue(key, out var location) && ReadAt(location, part) is { } bytes ? [bytes] : [];
    }

    public void Dispose() => reader.Dispose();
}

// The fingerprints of every resource part of one or more game builds: each distinct part's SHA-256, size and places,
// and its content-defined anchors (an 8-byte hash of a 64-byte window and where the window starts). It holds no game content:
// what a resource is made of can be checked against it, never read back out of it.
public sealed class SourceIndex
{
    const string Magic = "HFSI";
    const int Version = 5;
    // An anchor found in more contents than this (shared headers, common tables) is kept only for the first ones.
    const int MaxPerChunk = 16;

    public List<string> Builds { get; } = [];
    public List<string> Archives { get; } = [];
    readonly Dictionary<string, int> archiveIds = new(StringComparer.Ordinal);
    public List<Content> Contents { get; } = [];
    public long IndexedBytes { get; private set; }
    readonly Dictionary<string, int> bySha = new(StringComparer.Ordinal);
    List<(ulong Hash, int Content, int Offset)>? pending = [];
    ulong[] hashes = [];
    int[] owners = [];
    int[] offsets = [];
    Dictionary<ResourceKey, List<int>>? byKey;
    // What each archive (by ArchiveHash) was found to hold: its resources' parts, as contents.
    readonly Dictionary<string, List<(ResourceKey Key, PartKind Part, int Content)>> archiveMemo = new(StringComparer.Ordinal);
    public long ArchivesReused { get; private set; }

    public int AnchorCount => hashes.Length;

    // Builds only some of whose archives were indexed (the ones a held upload needed): not counted as covered.
    public HashSet<string> PartialBuilds { get; } = new(StringComparer.Ordinal);

    // Adds a build; with archives, only those archives (a partial build, for targeted indexing). A partial build added
    // again takes the archives it did not have yet.
    // With keys too, only those resources of those archives: the archives are not counted as done.
    public void Add(IGameResources game, Action<long, long>? progress = null, IReadOnlySet<string>? archives = null, IReadOnlySet<ResourceKey>? keys = null)
    {
        if (archives is not null || PartialBuilds.Contains(game.Build))
        {
            AddPartial(game, archives, keys, progress);
            return;
        }
        if (Builds.Contains(game.Build)) throw new InvalidOperationException($"Build {game.Build} is already in the index.");
        Unseal();
        var build = Builds.Count;
        Builds.Add(game.Build);
        long resources = 0, bytes = 0;
        string? current = null;
        var archive = -1;
        List<(ResourceKey, PartKind, int)>? recording = null;
        string? recordingHash = null;
        var reuse = false;
        void Finish()
        {
            if (recording is not null && recordingHash is not null) archiveMemo[recordingHash] = recording;
            recording = null;
        }
        foreach (var (key, archiveName, read) in game.All())
        {
            resources++;
            if (archiveName != current)
            {
                Finish();
                current = archiveName;
                archive = ArchiveId(archiveName);
                recordingHash = game.ArchiveHash(archiveName);
                reuse = Reuse(recordingHash, build, archive);
                if (!reuse && recordingHash is not null) recording = [];
            }
            if (reuse) continue;
            bytes += AddParts(read(), build, key, archive, recording, partial: false);
            if (resources % 20000 == 0) progress?.Invoke(resources, bytes);
        }
        Finish();
        progress?.Invoke(resources, bytes);
        byKey = null;
    }

    // The number of an archive name, a new one when it has none yet.
    int ArchiveId(string name)
    {
        if (archiveIds.TryGetValue(name, out var archive)) return archive;
        archive = Archives.Count;
        Archives.Add(name);
        archiveIds[name] = archive;
        return archive;
    }

    // Whether an archive with this hash was indexed before; if so, its parts take this place too, without reading.
    bool Reuse(string? archiveHash, int build, int archive)
    {
        if (archiveHash is null || !archiveMemo.TryGetValue(archiveHash, out var memo)) return false;
        ArchivesReused++;
        foreach (var (k, part, content) in memo) Contents[content].Places.Add(new Place(build, k, part, archive));
        return true;
    }

    // Fingerprints a resource's parts (bytes indexed before are the same content) and adds the place to each, and to
    // the recording when there is one; returns how many bytes the parts have. In a partial build a place the content
    // already has is not added again.
    long AddParts(ResourceParts parts, int build, ResourceKey key, int archive, List<(ResourceKey, PartKind, int)>? recording, bool partial)
    {
        long bytes = 0;
        foreach (var kind in Parts.Kinds)
        {
            var data = Parts.Of(parts, kind);
            if (data.Length == 0) continue;
            bytes += data.Length;
            var id = ContentOf(data);
            var place = new Place(build, key, kind, archive);
            if (!partial || !Contents[id].Places.Contains(place)) Contents[id].Places.Add(place);
            recording?.Add((key, kind, id));
        }
        return bytes;
    }

    // The content with these bytes; a new one, with its anchors, when no part indexed before has them.
    int ContentOf(byte[] data)
    {
        var sha = SHA256.HashData(data);
        var hex = Convert.ToHexString(sha);
        if (bySha.TryGetValue(hex, out var id)) return id;
        id = Contents.Count;
        Contents.Add(new Content(sha, data.Length));
        bySha[hex] = id;
        IndexedBytes += data.Length;
        foreach (var (offset, hash) in ContentChunks.Anchors(data)) pending!.Add((hash, id, offset));
        return id;
    }

    readonly Dictionary<string, HashSet<string>> partialArchives = new(StringComparer.Ordinal);

    void AddPartial(IGameResources game, IReadOnlySet<string>? archives, IReadOnlySet<ResourceKey>? keys, Action<long, long>? progress)
    {
        Unseal();
        var build = Builds.IndexOf(game.Build);
        if (build < 0)
        {
            build = Builds.Count;
            Builds.Add(game.Build);
            PartialBuilds.Add(game.Build);
        }
        if (!partialArchives.TryGetValue(game.Build, out var done)) partialArchives[game.Build] = done = new(StringComparer.Ordinal);
        long resources = 0, bytes = 0;
        foreach (var (key, archiveName, read) in archives is null ? game.All() : game.In(archives))
        {
            if (Skipped(archiveName, archives, done)) continue;
            if (keys is not null && !keys.Contains(key)) continue;
            resources++;
            var archive = ArchiveId(archiveName);
            bytes += AddParts(read(), build, key, archive, null, partial: true);
            progress?.Invoke(resources, bytes);
        }
        if (archives is not null && keys is null) done.UnionWith(archives);
        byKey = null;
    }

    // Whether a partial build leaves this archive out: it is not one of those asked for, or (with none asked for) it was
    // indexed already.
    static bool Skipped(string archive, IReadOnlySet<string>? archives, HashSet<string> done) =>
        archives is not null && !archives.Contains(archive) || done.Contains(archive) && (archives is null || !archives.Contains(archive));

    // A loaded (sealed) index takes more builds: its anchors go back to the unsorted list.
    void Unseal()
    {
        if (pending is not null) return;
        pending = new List<(ulong, int, int)>(hashes.Length);
        for (var i = 0; i < hashes.Length; i++) pending.Add((hashes[i], owners[i], offsets[i]));
        hashes = [];
        owners = [];
        offsets = [];
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
    public IReadOnlyList<int> WithKey(ResourceKey key) => ByKey().TryGetValue(key, out var found) ? found : [];

    // Per resource, the contents that are a part of it (built when first needed).
    Dictionary<ResourceKey, List<int>> ByKey()
    {
        if (byKey is not null) return byKey;
        byKey = [];
        for (var id = 0; id < Contents.Count; id++)
            foreach (var place in Contents[id].Places)
            {
                if (!byKey.TryGetValue(place.Key, out var list)) byKey[place.Key] = list = [];
                if (list.Count == 0 || list[^1] != id) list.Add(id);
            }
        return byKey;
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
        w.Write(Archives.Count);
        foreach (var archive in Archives) w.Write(archive);
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
                w.Write(p.Archive);
            }
        }
        w.Write(partialArchives.Count);
        foreach (var (label, set) in partialArchives)
        {
            w.Write(label);
            w.Write(set.Count);
            foreach (var name in set) w.Write(name);
        }
        w.Write(archiveMemo.Count);
        foreach (var (hash, entries) in archiveMemo)
        {
            w.Write(hash);
            w.Write(entries.Count);
            foreach (var (k, part, content) in entries)
            {
                w.Write(k.Type);
                w.Write(k.Name);
                w.Write((byte)part);
                w.Write(content);
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
        var archives = r.ReadInt32();
        for (var i = 0; i < archives; i++)
        {
            index.archiveIds[r.ReadString()] = i;
        }
        index.Archives.AddRange(index.archiveIds.OrderBy(a => a.Value).Select(a => a.Key));
        var contents = r.ReadInt32();
        for (var i = 0; i < contents; i++)
        {
            var c = new Content(r.ReadBytes(32), r.ReadInt32());
            var places = r.ReadInt32();
            for (var p = 0; p < places; p++)
                c.Places.Add(new Place(r.ReadByte(), new ResourceKey(r.ReadUInt64(), r.ReadUInt64()), (PartKind)r.ReadByte(), r.ReadInt32()));
            index.bySha[Convert.ToHexString(c.Sha256)] = i;
            index.Contents.Add(c);
            index.IndexedBytes += c.Size;
        }
        index.LoadPartials(r);
        index.LoadMemos(r);
        var count = r.ReadInt32();
        index.hashes = new ulong[count];
        index.owners = new int[count];
        index.offsets = new int[count];
        for (var i = 0; i < count; i++) index.hashes[i] = r.ReadUInt64();
        for (var i = 0; i < count; i++) index.owners[i] = r.ReadInt32();
        for (var i = 0; i < count; i++) index.offsets[i] = r.ReadInt32();
        return index;
    }

    // The partial builds, each with the archives it has.
    void LoadPartials(BinaryReader r)
    {
        var partials = r.ReadInt32();
        for (var i = 0; i < partials; i++)
        {
            var label = r.ReadString();
            var set = new HashSet<string>(StringComparer.Ordinal);
            var n = r.ReadInt32();
            for (var e = 0; e < n; e++) set.Add(r.ReadString());
            partialArchives[label] = set;
            PartialBuilds.Add(label);
        }
    }

    // What each archive (by ArchiveHash) was found to hold.
    void LoadMemos(BinaryReader r)
    {
        var memos = r.ReadInt32();
        for (var i = 0; i < memos; i++)
        {
            var hash = r.ReadString();
            var n = r.ReadInt32();
            var entries = new List<(ResourceKey, PartKind, int)>(n);
            for (var e = 0; e < n; e++) entries.Add((new ResourceKey(r.ReadUInt64(), r.ReadUInt64()), (PartKind)r.ReadByte(), r.ReadInt32()));
            archiveMemo[hash] = entries;
        }
    }
}

// A data folder in the layout older builds shipped (before bundles.nxa): one Stingray archive per file, named by its
// 16-hex-digit hash, with <name>.gpu_resources and <name>.stream beside it. Each resource key is taken from the first
// archive in name order that has it, as GameDataReader does.
public sealed class LooseGameResources(IDataFiles files, string build) : IGameResources
{
    public LooseGameResources(string folder, string build) : this(new FolderFiles(folder), build) { }

    const uint Magic = 0xF0000011;
    Dictionary<ResourceKey, (string Archive, long[] Offsets, int[] Sizes)>? entries;

    public string Build => build;

    IEnumerable<string> ArchiveFiles() =>
        files.List().Where(f => System.Text.RegularExpressions.Regex.IsMatch(f, "^[0-9a-f]{16}$")).Order(StringComparer.Ordinal);

    // One archive's resource table: only its first bytes are read.
    List<(ResourceKey Key, long[] Offsets, int[] Sizes)> TableOf(string file)
    {
        var rows = new List<(ResourceKey, long[], int[])>();
        using var s = files.Open(file);
        var head = new byte[72];
        if (s.Read(head) < 72 || BitConverter.ToUInt32(head, 0) != Magic) return rows;
        var types = BitConverter.ToInt32(head, 4);
        var count = BitConverter.ToInt32(head, 8);
        if (count < 0 || count > 1_000_000 || types < 0 || types > 10_000) return rows;
        var table = new byte[80L * count];
        s.Position = 72 + 32L * types;
        if (s.Read(table) < table.Length) return rows;
        for (var i = 0; i < count; i++)
        {
            var at = 80 * i;
            rows.Add((new ResourceKey(BitConverter.ToUInt64(table, at + 8), BitConverter.ToUInt64(table, at)),
                [BitConverter.ToInt64(table, at + 16), BitConverter.ToInt64(table, at + 32), BitConverter.ToInt64(table, at + 24)],
                [BitConverter.ToInt32(table, at + 56), BitConverter.ToInt32(table, at + 64), BitConverter.ToInt32(table, at + 60)]));
        }
        return rows;
    }

    Dictionary<ResourceKey, (string Archive, long[] Offsets, int[] Sizes)> Entries()
    {
        if (entries is not null) return entries;
        entries = [];
        foreach (var file in ArchiveFiles())
            foreach (var (key, offsets, sizes) in TableOf(file)) entries.TryAdd(key, (file, offsets, sizes));
        return entries;
    }

    public IEnumerable<(string Archive, Func<IReadOnlyList<ResourceKey>> Keys)> Tables() =>
        ArchiveFiles().Select(f => (f, (Func<IReadOnlyList<ResourceKey>>)(() => TableOf(f).Select(r => r.Key).ToList())));

    byte[] Slice(string file, long offset, int size)
    {
        if (size <= 0 || !files.Exists(file)) return [];
        using var s = files.Open(file);
        var bytes = new byte[size];
        s.Position = offset;
        s.ReadExactly(bytes);
        return bytes;
    }

    static readonly string[] Suffix = ["", ".gpu_resources", ".stream"];

    byte[] Part((string Archive, long[] Offsets, int[] Sizes) e, PartKind kind) =>
        Slice(e.Archive + Suffix[(int)kind], e.Offsets[(int)kind], e.Sizes[(int)kind]);

    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All()
    {
        foreach (var (key, e) in Entries())
            yield return (key, e.Archive, () => new ResourceParts(Part(e, PartKind.Main), Part(e, PartKind.Gpu), Part(e, PartKind.Stream)));
    }

    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> In(IReadOnlySet<string> archives)
    {
        foreach (var archive in archives.Where(files.Exists).Order(StringComparer.Ordinal))
            foreach (var (key, offsets, sizes) in TableOf(archive))
            {
                var e = (archive, offsets, sizes);
                yield return (key, archive, () => new ResourceParts(Part(e, PartKind.Main), Part(e, PartKind.Gpu), Part(e, PartKind.Stream)));
            }
    }

    public IEnumerable<(ResourceKey Key, string Archive)> At(string file, long offset, long length)
    {
        var kind = Array.FindLastIndex(Suffix, x => file.EndsWith(x, StringComparison.Ordinal));
        var archive = file[..^Suffix[kind].Length];
        if (!System.Text.RegularExpressions.Regex.IsMatch(archive, "^[0-9a-f]{16}$") || !files.Exists(archive)) yield break;
        foreach (var (key, offsets, sizes) in TableOf(archive))
            if (sizes[kind] > 0 && offsets[kind] < offset + length && offset < offsets[kind] + sizes[kind]) yield return (key, archive);
    }

    public string? TableHash(string archive) => files.Hash(archive) is { } h ? "main:" + h : null;

    public string? ArchiveHash(string archive)
    {
        var hashes = Suffix.Select(x => archive + x).Where(files.Exists).Select(files.Hash).ToList();
        return hashes.Count == 0 || hashes.Any(h => h is null) ? null : string.Join("+", hashes);
    }

    public byte[]? Read(ResourceKey key, PartKind part) => Entries().TryGetValue(key, out var e) ? Part(e, part) : null;

    readonly Dictionary<string, Dictionary<ResourceKey, (long[] Offsets, int[] Sizes)>> archiveTables = new(StringComparer.Ordinal);

    public bool HasArchive(string archive) => files.Exists(archive);

    // Reads only that archive's table, where Read reads every table of the build first.
    public IReadOnlyList<byte[]> ReadIn(ResourceKey key, PartKind part, string? archive)
    {
        if (archive is null || !files.Exists(archive)) return Read(key, part) is { } own ? [own] : [];
        if (!archiveTables.TryGetValue(archive, out var table))
        {
            archiveTables[archive] = table = [];
            foreach (var (k, offsets, sizes) in TableOf(archive)) table.TryAdd(k, (offsets, sizes));
        }
        if (!table.TryGetValue(key, out var e)) return [];
        var bytes = Part((archive, e.Offsets, e.Sizes), part);
        return bytes.Length > 0 ? [bytes] : [];
    }
}

// A data folder with both layouts (the transition builds of December 2025 to January 2026): what bundles.nxa lists, then
// the loose archives it does not.
public sealed class MixedGameResources(IDataFiles files, string build) : IGameResources, IDisposable
{
    readonly GameResources packed = new(files, build);
    readonly LooseGameResources loose = new(files, build);

    public string Build => build;

    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> All()
    {
        var seen = new HashSet<ResourceKey>();
        foreach (var r in packed.All()) if (seen.Add(r.Key)) yield return r;
        foreach (var r in loose.All()) if (seen.Add(r.Key)) yield return r;
    }

    public string? ArchiveHash(string archive) => ((IGameResources)loose).ArchiveHash(archive);
    public string? TableHash(string archive) => loose.TableHash(archive);

    // Each archive from every side that has it, reading only its table: a transition build can hold an archive both in
    // bundles.nxa and as loose files, with different contents (a chunk the Steam scan finds is in the loose files).
    public IEnumerable<(ResourceKey Key, string Archive, Func<ResourceParts> Read)> In(IReadOnlySet<string> archives) =>
        packed.In(archives.Where(packed.HasArchive).ToHashSet(StringComparer.Ordinal)).Concat(loose.In(archives));

    public IEnumerable<(ResourceKey Key, string Archive)> At(string file, long offset, long length) => loose.At(file, offset, length);

    public byte[]? Read(ResourceKey key, PartKind part) => packed.Read(key, part) ?? loose.Read(key, part);

    public IReadOnlyList<byte[]> ReadIn(ResourceKey key, PartKind part, string? archive)
    {
        if (archive is null || !packed.HasArchive(archive) && !loose.HasArchive(archive)) return Read(key, part) is { } own ? [own] : [];
        return [.. packed.HasArchive(archive) ? packed.ReadIn(key, part, archive) : [], .. loose.HasArchive(archive) ? loose.ReadIn(key, part, archive) : []];
    }

    public void Dispose() => packed.Dispose();
}
