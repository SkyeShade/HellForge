namespace HellForge.Core.GameData;

// Where a build's data files are read from: a folder on disk, or (for the asset migrator) a build's Steam depot,
// streamed. Names are relative to the data folder ("bundles.nxa", "9ba626afa44a3aa3.gpu_resources"); streams are
// seekable and read-only.
public interface IDataFiles
{
    string Name { get; }
    bool Exists(string name);
    Stream Open(string name);
    IEnumerable<string> List();
    // The file's hash when the source knows it without reading the file (a Steam depot manifest lists each file's
    // SHA-1); null when it does not.
    string? Hash(string name) => null;
}

public sealed class FolderFiles(string folder) : IDataFiles
{
    public string Folder { get; } = Path.GetFullPath(folder);
    public string Name => Folder;
    public bool Exists(string name) => File.Exists(Path.Combine(Folder, name));
    public Stream Open(string name) => new FileStream(Path.Combine(Folder, name), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
    public IEnumerable<string> List() => Directory.Exists(Folder) ? Directory.EnumerateFiles(Folder).Select(Path.GetFileName).OfType<string>() : [];
}
