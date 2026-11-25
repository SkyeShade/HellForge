namespace HellForge.Core.AssetMigration;

// A mod's files, read one at a time: a mod can be far bigger than the memory the migrator should use (the largest
// uploads are over 2 GB), so only the file being matched is held. Paths are relative, with '/', in ordinal order.
public sealed class ModFiles
{
    readonly Dictionary<string, long> sizes;
    readonly Func<string, byte[]> read;
    readonly Func<string, int, byte[]> head;

    ModFiles(IEnumerable<(string Path, long Size)> files, Func<string, byte[]> read, Func<string, int, byte[]> head)
    {
        sizes = files.ToDictionary(f => f.Path, f => f.Size, StringComparer.Ordinal);
        Paths = sizes.Keys.Order(StringComparer.Ordinal).ToList();
        this.read = read;
        this.head = head;
    }

    public IReadOnlyList<string> Paths { get; }
    public string? Folder { get; private init; }
    public long Size(string path) => sizes[path];
    public long TotalSize => sizes.Values.Sum();
    public byte[] Read(string path) => read(path);
    // The first bytes of a file (all of it when it is shorter).
    public byte[] Head(string path, int length) => head(path, length);
    // Each file in turn, read when it is reached.
    public IEnumerable<(string Path, byte[] Bytes)> Each()
    {
        foreach (var path in Paths) yield return (path, Read(path));
    }

    public static ModFiles InMemory(IReadOnlyList<(string Path, byte[] Bytes)> files)
    {
        var bytes = files.ToDictionary(f => f.Path, f => f.Bytes, StringComparer.Ordinal);
        return new(files.Select(f => (f.Path, (long)f.Bytes.Length)), path => bytes[path], (path, length) => bytes[path][..Math.Min(length, bytes[path].Length)]);
    }

    public static ModFiles InFolder(string folder) => InFolder(folder,
        Directory.EnumerateFiles(Path.GetFullPath(folder), "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(Path.GetFullPath(folder), f).Replace('\\', '/')));

    // Just these files of the folder (relative paths with '/').
    public static ModFiles InFolder(string folder, IEnumerable<string> paths)
    {
        folder = Path.GetFullPath(folder);
        string Full(string path) => Path.Combine(folder, path);
        return new(paths.Select(p => (p, new FileInfo(Full(p)).Length)),
            path => File.ReadAllBytes(Full(path)),
            (path, length) =>
            {
                using var stream = File.OpenRead(Full(path));
                var buffer = new byte[(int)Math.Min(length, stream.Length)];
                stream.ReadExactly(buffer);
                return buffer;
            })
        { Folder = folder };
    }

    public string FullPath(string path) => Folder is null ? throw new InvalidOperationException("These files are not in a folder.") : Path.Combine(Folder, path);
}
