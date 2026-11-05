using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.SteamDownload;

// A past build of the game read straight from Steam's depots for the asset kit (docs/modding/assets.md): only the
// chunks that are read are fetched, with the player's own sign-in, and kept on disk by their SHA-1 (a Steam chunk's id is
// the SHA-1 of its content), so a recipe that needs a few resources of an old build downloads megabytes, not the build.
// Chunks that a build on this PC shares are read from its files instead (checked by SHA-1 too).
public sealed class ChunkStore(IDepotService steam, uint app, string folder, CancellationToken cancel = default)
{
    readonly Dictionary<uint, byte[]> keys = [];
    readonly Dictionary<string, List<ChunkSource>> local = new(StringComparer.Ordinal);
    readonly Lock gate = new();
    public long Downloaded, FromCache, FromLocal;
    public string Folder { get; } = folder;
    public IDepotService Steam => steam;
    public uint App => app;
    public CancellationToken Cancel => cancel;

    // The installed files of a build whose manifest is known (Steam's install, from its depotcache): where each of its
    // chunks may be read on this PC.
    public void AddLocal(DepotManifest manifest, string installFolder)
    {
        foreach (var file in manifest.Files.Where(f => !f.IsDirectory && !f.Path.Contains("..", StringComparison.Ordinal)))
        {
            var path = Path.Combine(installFolder, file.Path);
            var info = new FileInfo(path);
            if (!info.Exists || (ulong)info.Length != file.Size) continue;
            foreach (var chunk in file.Chunks)
            {
                var key = chunk.Sha1.ToUpperInvariant();
                if (!local.TryGetValue(key, out var places)) local[key] = places = [];
                places.Add(new(path, chunk.Offset));
            }
        }
    }

    public byte[] Key(uint depot)
    {
        lock (gate)
        {
            if (keys.TryGetValue(depot, out var key)) return key;
            return keys[depot] = steam.DepotKeyAsync(app, depot, cancel).GetAwaiter().GetResult();
        }
    }

    // A chunk's bytes: from a local file that holds it, the cache, or Steam, in that order; always the chunk's SHA-1.
    public byte[] Get(uint depot, DepotChunk chunk)
    {
        var id = chunk.Sha1.ToUpperInvariant();
        if (chunk.OriginalSize == 0) return [];
        if (chunk.OriginalSize > DepotPlanner.MaxChunk) throw new InvalidDataException($"Steam's manifest lists a chunk of {chunk.OriginalSize:N0} bytes.");
        foreach (var place in local.GetValueOrDefault(id) ?? [])
            if (ReadAt(place, (int)chunk.OriginalSize) is { } bytes && Matches(bytes, id))
            {
                Interlocked.Add(ref FromLocal, bytes.Length);
                return bytes;
            }
        var cached = Path.Combine(Folder, "chunks", id[..2], id);
        if (File.Exists(cached))
        {
            var bytes = File.ReadAllBytes(cached);
            if (Matches(bytes, id))
            {
                Interlocked.Add(ref FromCache, bytes.Length);
                return bytes;
            }
        }
        var buffer = new byte[chunk.OriginalSize];
        var length = steam.ChunkAsync(app, depot, chunk, Key(depot), buffer, cancel).GetAwaiter().GetResult();
        var got = buffer.AsSpan(0, length).ToArray();
        if (!Matches(got, id)) throw new InvalidDataException($"A chunk Steam sent for depot {depot} is not the one asked for.");
        Interlocked.Add(ref Downloaded, got.Length);
        Directory.CreateDirectory(Path.GetDirectoryName(cached)!);
        var partial = cached + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
        File.WriteAllBytes(partial, got);
        File.Move(partial, cached, overwrite: true);
        return got;
    }

    static byte[]? ReadAt(ChunkSource place, int length)
    {
        try
        {
            using var stream = new FileStream(place.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if ((ulong)stream.Length < place.Offset + (ulong)length) return null;
            stream.Position = (long)place.Offset;
            var bytes = new byte[length];
            stream.ReadExactly(bytes);
            return bytes;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static bool Matches(byte[] bytes, string sha1) => Convert.ToHexString(SHA1.HashData(bytes)) == sha1;

    // Keeps the chunk cache within limit bytes, the least recently written going first.
    public void Trim(long limit)
    {
        var root = Path.Combine(Folder, "chunks");
        if (!Directory.Exists(root)) return;
        var files = new DirectoryInfo(root).EnumerateFiles("*", SearchOption.AllDirectories).OrderByDescending(f => f.LastWriteTimeUtc).ToList();
        long kept = 0;
        foreach (var file in files)
        {
            kept += file.Length;
            if (kept <= limit) continue;
            try { file.Delete(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }
}

// The data folder of a build as its depot manifest lists it: each file read through the chunk store, a chunk at a time.
public sealed class DepotDataFiles : IDataFiles
{
    readonly ChunkStore store;
    readonly Dictionary<string, DepotFile> files = new(StringComparer.Ordinal);
    public DepotManifest Manifest { get; }

    public DepotDataFiles(ChunkStore store, DepotManifest manifest, string folder = "data")
    {
        this.store = store;
        Manifest = manifest;
        foreach (var file in manifest.Files.Where(f => !f.IsDirectory && f.Path.StartsWith(folder + "/", StringComparison.Ordinal)))
        {
            var name = file.Path[(folder.Length + 1)..];
            if (!name.Contains('/')) files[name] = file;
        }
    }

    public string Name => GameBuilds.Label(Manifest.DepotId, Manifest.ManifestId.ToString());
    public bool Exists(string name) => files.ContainsKey(name);
    public IEnumerable<string> List() => files.Keys;
    public string? Hash(string name) => files.TryGetValue(name, out var file) && file.Sha1.Length > 0 ? file.Sha1.ToLowerInvariant() : null;
    public Stream Open(string name) => new DepotStream(store, Manifest.DepotId, files.TryGetValue(name, out var file) ? file : throw new FileNotFoundException(name));
}

// A file of a depot as a seekable stream: each read takes the chunks it covers from the store; the last one is kept.
sealed class DepotStream : Stream
{
    readonly ChunkStore store;
    readonly uint depot;
    readonly DepotChunk[] chunks;
    readonly long length;
    long position;
    int heldIndex = -1;
    byte[] held = [];

    public DepotStream(ChunkStore store, uint depot, DepotFile file) =>
        (this.store, this.depot, chunks, length) = (store, depot, [.. file.Chunks.OrderBy(c => c.Offset)], (long)file.Size);

    public override long Length => length;
    public override long Position { get => position; set => position = value; }
    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        var total = 0;
        while (buffer.Length > 0 && position < length)
        {
            var index = ChunkAt(position);
            if (index < 0) throw new InvalidDataException("Steam's manifest leaves a gap in a file.");
            if (index != heldIndex)
            {
                held = store.Get(depot, chunks[index]);
                heldIndex = index;
            }
            var within = (int)(position - (long)chunks[index].Offset);
            var n = Math.Min(buffer.Length, held.Length - within);
            if (n <= 0) throw new InvalidDataException("Steam's manifest leaves a gap in a file.");
            held.AsSpan(within, n).CopyTo(buffer);
            buffer = buffer[n..];
            position += n;
            total += n;
        }
        return total;
    }

    // The chunk that holds the byte at offset (binary search over the chunks in order), or -1.
    int ChunkAt(long offset)
    {
        int lo = 0, hi = chunks.Length - 1;
        while (lo <= hi)
        {
            var mid = (lo + hi) / 2;
            var start = (long)chunks[mid].Offset;
            if (offset < start) hi = mid - 1;
            else if (offset >= start + chunks[mid].OriginalSize) lo = mid + 1;
            else return mid;
        }
        return -1;
    }

    public override long Seek(long offset, SeekOrigin origin) => position = origin switch
    {
        SeekOrigin.Begin => offset,
        SeekOrigin.Current => position + offset,
        _ => length + offset,
    };

    // read-only, seekable
    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

// Depot manifests by id: from Steam's own depotcache when it has the manifest, else from the asset kit's cache, else
// fetched (with the manifest request code for the branch the depot history names) and cached.
public sealed class DepotManifests(ChunkStore store, string? steamRoot)
{
    static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public DepotManifest Get(uint depot, ulong manifest, string branch = "public")
    {
        if (steamRoot != null && Path.Combine(steamRoot, "depotcache", $"{depot}_{manifest}.manifest") is var steamCopy && File.Exists(steamCopy))
        {
            try { return SteamDepotManifest.Load(steamCopy); }
            catch (InvalidDataException) { }
        }
        var cached = Path.Combine(store.Folder, "manifests", $"{depot}_{manifest}.json.gz");
        if (File.Exists(cached))
        {
            try
            {
                using var input = new GZipStream(File.OpenRead(cached), CompressionMode.Decompress);
                if (JsonSerializer.Deserialize<DepotManifest>(input, Json) is { } known && known.DepotId == depot && known.ManifestId == manifest) return known;
            }
            catch (Exception error) when (error is JsonException or IOException or InvalidDataException) { }
        }
        var code = store.Steam.ManifestRequestCodeAsync(store.App, depot, manifest, branch, store.Cancel).GetAwaiter().GetResult();
        if (code == 0) throw new DepotAccessDeniedException(depot, $"Steam does not serve manifest {manifest} any more");
        var fetched = store.Steam.ManifestAsync(store.App, depot, manifest, code, store.Key(depot), store.Cancel).GetAwaiter().GetResult();
        Directory.CreateDirectory(Path.GetDirectoryName(cached)!);
        var partial = cached + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
        using (var output = new GZipStream(File.Create(partial), CompressionLevel.Fastest))
            JsonSerializer.Serialize(output, fetched, Json);
        File.Move(partial, cached, overwrite: true);
        return fetched;
    }
}

// Builds named by label ("<depot>:<manifest>", GameBuilds) opened from Steam: the depot's data folder, read as its
// layout is. The branch a manifest was published on comes from the depot history (data/depot-history).
public sealed class SteamBuilds(ChunkStore store, string? steamRoot)
{
    readonly DepotManifests manifests = new(store, steamRoot);
    public ChunkStore Store => store;

    public IGameResources Open(string label)
    {
        var (depot, manifest) = Parse(label);
        var branch = DepotHistory.Embedded(depot).FirstOrDefault(h => h.Manifest == manifest)?.Branch ?? "public";
        return GameBuilds.Open(new DepotDataFiles(store, manifests.Get(depot, manifest, branch)), label);
    }

    public DepotManifest Manifest(string label)
    {
        var (depot, manifest) = Parse(label);
        return manifests.Get(depot, manifest, DepotHistory.Embedded(depot).FirstOrDefault(h => h.Manifest == manifest)?.Branch ?? "public");
    }

    public static (uint Depot, ulong Manifest) Parse(string label)
    {
        var parts = label.Split(':');
        return parts.Length == 2 && uint.TryParse(parts[0], out var depot) && ulong.TryParse(parts[1], out var manifest)
            ? (depot, manifest) : throw new InvalidDataException($"{label} is not a build Steam can send (<depot>:<manifest>).");
    }

    // The chunk store for an installation (or the user's asset-kit folder), with Steam's installed build's chunks mapped
    // to its files.
    public static SteamBuilds For(IDepotService steam, InstallLayout? install, CancellationToken cancel = default)
    {
        var store = new ChunkStore(steam, uint.Parse(LocalInstall.AppId), AssetKit.CacheFolder(install), cancel);
        var steamRoot = SteamLocator.SteamRoot();
        if (GameBuilds.Steam() is { } installed && steamRoot != null)
        {
            var (depot, manifest) = Parse(installed.Label);
            var cachedManifest = Path.Combine(steamRoot, "depotcache", $"{depot}_{manifest}.manifest");
            try { if (File.Exists(cachedManifest)) store.AddLocal(SteamDepotManifest.Load(cachedManifest), Path.GetDirectoryName(installed.DataFolder)!); }
            catch (InvalidDataException) { }
        }
        return new(store, steamRoot);
    }
}
