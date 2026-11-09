using System.Security.Cryptography;

namespace HellForge.Core;

// Copies the supported build's files from an HD2 installation into a HellForge game folder. Each file is hashed while it is
// copied and must match Steam's depot hash. Files written completely are renamed from .partial, and a file already
// present with the right content is kept, so an interrupted copy resumes and running it again repairs a damaged one. Only
// the build's listed files are copied: mods, other tools, logs and depot files HellForge never uses are left behind. The
// source is an installed HD2 (Copy) or the depots Steam downloaded at the build's exact manifests (CopyFromDepots,
// SteamDepotDownload). HellForge's own SteamCMD download is moved instead (MoveFromDepots): each file is checked, then moved.
public static class GameCopier
{
    const int BufferSize = 4 << 20;

    // progress: bytes done, bytes total, file being copied.
    public static void Copy(string source, string destination, SupportedBuild build, Action<long, long, string>? progress = null,
        CancellationToken cancellation = default)
    {
        // Steam's game folder and the game folder being written are protected: links above them are normal, a link at or
        // inside either would copy through it to somewhere else.
        source = LocalInstall.NoLinks(source, source);
        destination = LocalInstall.NoLinks(destination, destination);
        if (LocalInstall.Within(destination, source) || LocalInstall.Within(source, destination))
            throw new InvalidDataException("The game folder must not overlap the Steam installation.");
        if (Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(source)!)!).Equals("steamapps", StringComparison.OrdinalIgnoreCase) &&
            LocalInstall.Within(destination, Path.GetDirectoryName(Path.GetDirectoryName(source)!)!))
            throw new InvalidDataException("The game folder must be outside Steam's steamapps folder.");

        CopyFiles(new(source, file => Path.Combine(source, file.Path), "the installation", "validate the game in Steam"), destination, build, progress, cancellation);
    }

    // From Steam's downloads of the build's depots (<Steam>/steamapps/content/app_553850/depot_<id>).
    public static void CopyFromDepots(string steamRoot, string destination, SupportedBuild build, Action<long, long, string>? progress = null,
        CancellationToken cancellation = default)
    {
        steamRoot = LocalInstall.ResolveLinks(steamRoot);
        destination = LocalInstall.NoLinks(destination, destination);
        if (LocalInstall.Within(destination, Path.Combine(steamRoot, "steamapps")))
            throw new InvalidDataException("The game folder must be outside Steam's steamapps folder.");
        CopyFiles(new(steamRoot, SteamDepotDownload.Locator(steamRoot, build), "Steam's download", "run its download_depot command again"),
            destination, build, progress, cancellation);
    }

    // From SteamCMD's download in HellForge's own folder (SteamCmd): each file is checked against the build's hash, then
    // moved into the game folder (on the same drive, so no second copy needs room). only: just these files of the build.
    public static void MoveFromDepots(string depotRoot, string destination, SupportedBuild build, IReadOnlyCollection<string>? only = null,
        Action<long, long, string>? progress = null, CancellationToken cancellation = default)
    {
        depotRoot = LocalInstall.FullPath(depotRoot);
        destination = LocalInstall.FullPath(destination);
        LocalInstall.NoLinks(depotRoot);
        LocalInstall.NoLinks(destination);
        if (LocalInstall.Within(destination, depotRoot) || LocalInstall.Within(depotRoot, destination))
            throw new InvalidDataException("The game folder must not overlap SteamCMD's folder.");
        CopyFiles(new(depotRoot, SteamDepotDownload.Locator(depotRoot, build), "SteamCMD's download", "start SteamCMD again", Move: true, Only: only),
            destination, build, progress, cancellation);
    }

    // The folder every file of the build is under (Root: a link at or inside it is refused), where each file is (Locate),
    // the source's name in errors (Where), what to do when a file there is wrong (Fix), whether files are moved rather
    // than copied, and which files (Only: all when null).
    sealed record FileSource(string Root, Func<SupportedFile, string> Locate, string Where, string Fix, bool Move = false, IReadOnlyCollection<string>? Only = null);

    static void CopyFiles(FileSource source, string destination, SupportedBuild build, Action<long, long, string>? progress, CancellationToken cancellation)
    {
        var (locate, where, fix) = (source.Locate, source.Where, source.Fix);
        var files = build.Files.Where(file => source.Only == null || source.Only.Contains(file.Path)).ToList();
        var missing = files.Where(file => !File.Exists(locate(file))).Select(file => file.Path).ToList();
        if (missing.Count > 0) throw new InvalidDataException($"{char.ToUpperInvariant(where[0])}{where[1..]} lacks {missing.Count} game files, starting with {missing[0]}; {fix}.");
        Directory.CreateDirectory(destination);
        if (!source.Move) DiskSpace.Require((destination, files.Where(file => !Present(Path.Combine(destination, file.Path), file)).Sum(file => file.Size)));

        long done = 0, total = files.Sum(file => file.Size);
        var buffer = new byte[BufferSize];
        foreach (var file in files)
        {
            cancellation.ThrowIfCancellationRequested();
            var from = LocalInstall.NoLinks(source.Locate(file), source.Root);
            var to = Path.Combine(destination, file.Path);
            if (OperatingSystem.IsLinux()) to = LocalInstall.NoLinks(to, destination);
            if (Matches(to, file))
            {
                done += file.Size;
                progress?.Invoke(done, total, file.Path);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            var partial = source.Move ? null : to + ".partial";
            void Read(int count)
            {
                done += count;
                progress?.Invoke(Math.Min(done, total), total, file.Path);
            }
            if (!Transfer(file, from, partial, buffer, Read, cancellation))
                throw new InvalidDataException($"{file.Path} in {where} is not build {build.BuildId}; {fix}.");
            File.Move(partial ?? from, to, overwrite: true);
        }
    }

    // Reads the file, hashing it, and writes it to partial (none: it is moved afterwards). True when it is the build's
    // file: its size and SHA-1. read: the bytes read, as they are.
    static bool Transfer(SupportedFile file, string from, string? partial, byte[] buffer, Action<int> read, CancellationToken cancellation)
    {
        using var input = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.Read, BufferSize);
        if (input.Length != file.Size) return false;
        using var output = partial == null ? null : new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, BufferSize);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
        int count;
        while ((count = input.Read(buffer, 0, buffer.Length)) > 0)
        {
            cancellation.ThrowIfCancellationRequested();
            hash.AppendData(buffer, 0, count);
            output?.Write(buffer, 0, count);
            read(count);
        }
        output?.Flush(true);
        return Convert.ToHexString(hash.GetHashAndReset()).Equals(file.Sha1, StringComparison.OrdinalIgnoreCase);
    }

    // A file that is in place with the build's content (a swapped upscaler is kept: SupportedBuild.IsReplaceable).
    static bool Matches(string path, SupportedFile file) => SupportedBuild.Accepts(path, file);

    // The cheap test used to estimate free space: present with the right size.
    internal static bool Present(string path, SupportedFile file)
    {
        var info = new FileInfo(path);
        return info.Exists && (info.Length == file.Size || SupportedBuild.IsReplaceable(file.Path));
    }
}
