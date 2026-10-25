using System.Security.Cryptography;

namespace HellForge.Core;

// Copies the supported build's files from an HD2 installation into a HellForge game folder. Each file is hashed while it is
// copied and must match Steam's depot hash. Files written completely are renamed from .partial, and a file already
// present with the right content is kept, so an interrupted copy resumes and running it again repairs a damaged one. Only
// the build's listed files are copied: mods, other tools, logs and depot files HellForge never uses are left behind. The
// source is an installed HD2 (Copy) or the depots Steam downloaded at the build's exact manifests (CopyFromDepots,
// SteamDepotDownload).
public static class GameCopier
{
    const int BufferSize = 4 << 20;

    // progress: bytes done, bytes total, file being copied.
    public static void Copy(string source, string destination, SupportedBuild build, Action<long, long, string>? progress = null,
        CancellationToken cancellation = default)
    {
        source = LocalInstall.FullPath(source);
        destination = LocalInstall.FullPath(destination);
        LocalInstall.NoLinks(source);
        LocalInstall.NoLinks(destination);
        if (LocalInstall.Within(destination, source) || LocalInstall.Within(source, destination))
            throw new InvalidDataException("The game folder must not overlap the Steam installation.");
        if (Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(source)!)!).Equals("steamapps", StringComparison.OrdinalIgnoreCase) &&
            LocalInstall.Within(destination, Path.GetDirectoryName(Path.GetDirectoryName(source)!)!))
            throw new InvalidDataException("The game folder must be outside Steam's steamapps folder.");

        CopyFiles(file => Path.Combine(source, file.Path), destination, build, "the installation", "validate the game in Steam", progress, cancellation);
    }

    // From Steam's downloads of the build's depots (<Steam>/steamapps/content/app_553850/depot_<id>).
    public static void CopyFromDepots(string steamRoot, string destination, SupportedBuild build, Action<long, long, string>? progress = null,
        CancellationToken cancellation = default)
    {
        steamRoot = LocalInstall.FullPath(steamRoot);
        destination = LocalInstall.FullPath(destination);
        LocalInstall.NoLinks(destination);
        if (LocalInstall.Within(destination, Path.Combine(steamRoot, "steamapps")))
            throw new InvalidDataException("The game folder must be outside Steam's steamapps folder.");
        CopyFiles(file => SteamDepotDownload.Locate(steamRoot, build, file), destination, build, "Steam's download",
            "run its download_depot command again", progress, cancellation);
    }

    // where: names the source in errors; fix: what to do when a file there is wrong.
    static void CopyFiles(Func<SupportedFile, string> locate, string destination, SupportedBuild build, string where, string fix,
        Action<long, long, string>? progress, CancellationToken cancellation)
    {
        var missing = build.Files.Where(file => !File.Exists(locate(file))).Select(file => file.Path).ToList();
        if (missing.Count > 0) throw new InvalidDataException($"{char.ToUpperInvariant(where[0])}{where[1..]} lacks {missing.Count} game files, starting with {missing[0]}; {fix}.");
        Directory.CreateDirectory(destination);
        DiskSpace.Require((destination, build.Files.Where(file => !Present(Path.Combine(destination, file.Path), file)).Sum(file => file.Size)));

        long done = 0, total = build.TotalBytes;
        var buffer = new byte[BufferSize];
        foreach (var file in build.Files)
        {
            cancellation.ThrowIfCancellationRequested();
            var from = locate(file);
            var to = Path.Combine(destination, file.Path);
            LocalInstall.NoLinks(from);
            if (Matches(to, file))
            {
                done += file.Size;
                progress?.Invoke(done, total, file.Path);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            var partial = to + ".partial";
            using (var input = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.Read, BufferSize))
            using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, BufferSize))
            using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA1))
            {
                if (input.Length != file.Size)
                    throw new InvalidDataException($"{file.Path} in {where} is not build {build.BuildId}; {fix}.");
                int read;
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    cancellation.ThrowIfCancellationRequested();
                    hash.AppendData(buffer, 0, read);
                    output.Write(buffer, 0, read);
                    done += read;
                    progress?.Invoke(Math.Min(done, total), total, file.Path);
                }
                output.Flush(true);
                if (!Convert.ToHexString(hash.GetHashAndReset()).Equals(file.Sha1, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"{file.Path} in {where} is not build {build.BuildId}; {fix}.");
            }
            File.Move(partial, to, overwrite: true);
        }
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
