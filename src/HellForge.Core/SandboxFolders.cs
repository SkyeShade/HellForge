namespace HellForge.Core;

// A sandboxed session's copies of the game's own data (docs/steam-bridge.md): the game at low integrity writes only into
// folders made new with a low label, so its user data (the profile's settings and saves, the shader cache) and its
// environment (AppData, temp) are copied into the session's folder at the start and back once the game is gone. Coming
// back, the files were written by the game: only regular files are copied, never through a link or junction, and a
// copy stops at its byte limit.
public static class SandboxFolders
{
    public const long MaxBytes = 4L << 30;
    public const int MaxFiles = 100_000;

    public sealed record Copied(int Files, long Bytes, int Skipped);

    // What a sandboxed session takes of the installation's user data root: the profile's folder, the shader cache and
    // the game's ImGui settings. Logs, dumps and crash data stay in the session's folder.
    public static IEnumerable<string> UserDataParts(string profile) => [Path.Combine("profiles", profile), "shader_cache", "dearimgui.ini"];

    // Copies the parts that exist from one root to the other, overwriting; files only, nothing deleted. fromSession is
    // true for the copy back, whose source the game wrote: on Linux that path opens every entry with openat2 and reads
    // only one regular, singly linked file (SandboxFiles). The copy in reads the installation, which the player owns;
    // it keeps the link checks below, as on Windows.
    public static Copied Copy(string fromRoot, string toRoot, IEnumerable<string> parts, long maxBytes = MaxBytes,
        bool fromSession = false)
    {
        if (OperatingSystem.IsLinux() && fromSession)
        {
            var (linuxFiles, linuxBytes, linuxSkipped) = SandboxFiles.CopyTree(fromRoot, toRoot, parts, maxBytes, MaxFiles);
            return new(linuxFiles, linuxBytes, linuxSkipped);
        }
        int files = 0, skipped = 0;
        long bytes = 0;
        foreach (var part in parts)
        {
            var from = Path.Combine(fromRoot, part);
            if (OperatingSystem.IsLinux())
            {
                try { from = LocalInstall.NoLinks(from, fromRoot); }
                catch (InvalidDataException) { skipped++; continue; }
            }
            if (Linked(from)) { skipped++; continue; }
            if (File.Exists(from)) Take(from, Path.Combine(toRoot, part));
            else if (Directory.Exists(from)) Walk(from, Path.Combine(toRoot, part));
        }
        return new(files, bytes, skipped);

        void Walk(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (var entry in new DirectoryInfo(from).EnumerateFileSystemInfos())
            {
                if (entry.Attributes.HasFlag(FileAttributes.ReparsePoint)) { skipped++; continue; }
                var target = Path.Combine(to, entry.Name);
                if (entry is DirectoryInfo) Walk(entry.FullName, target);
                else Take(entry.FullName, target);
            }
        }

        void Take(string from, string to)
        {
            var length = new FileInfo(from).Length;
            if (files >= MaxFiles || bytes + length > maxBytes) { skipped++; return; }
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(from, to, overwrite: true);
            files++;
            bytes += length;
        }
    }

    // The overlay's upper/work live in <run>/sandbox, outside gameOut and never bound into the view. After the copy-back,
    // stats and evidence copy they are dead state: remove them, including overlayfs's mode-000 work/work, then the empty
    // overlay mount point and hf-sandbox's empty new-root mount point (<run>/root). The game never writes this folder (it
    // is not in gameOut), so this is trusted cleanup, but a link there is still refused.
    public static void RemoveSessionState(string sessionRoot, string? gameOut = null)
    {
        try
        {
            if (string.IsNullOrEmpty(sessionRoot)) return;
            var candidate = Path.Combine(sessionRoot, "sandbox");
            if (!Directory.Exists(candidate)) return;
            var sandbox = LocalInstall.NoLinks(candidate, sessionRoot);
            if (!Directory.Exists(sandbox)) return;
            if (OperatingSystem.IsLinux()) MakeWritable(sandbox);
            Directory.Delete(sandbox, recursive: true);
        }
        finally
        {
            if (!string.IsNullOrEmpty(gameOut)) RemoveEmptyMountPoint(Path.Combine(gameOut, "prefix", "merged"));
            if (!string.IsNullOrEmpty(sessionRoot)) RemoveEmptyMountPoint(Path.Combine(sessionRoot, "root"));
        }
    }

    // Remove the session's .steam links after the domain ended. The sdk32/sdk64 links are all that remain there; recursive
    // deletion removes links themselves and never their Steam targets (measured on Linux).
    public static void RemoveSteamHome(string gameOut)
    {
        if (string.IsNullOrEmpty(gameOut)) return;
        var steam = Path.Combine(gameOut, "home", ".steam");
        if (File.Exists(steam)) { File.Delete(steam); return; }
        if (!Directory.Exists(steam)) return;
        var folder = new DirectoryInfo(steam);
        if ((folder.Attributes & FileAttributes.ReparsePoint) != 0) { File.Delete(steam); return; }
        folder.Delete(recursive: true);
    }

    static void RemoveEmptyMountPoint(string path)
    {
        if (File.Exists(path)) { File.Delete(path); return; }
        if (!Directory.Exists(path)) return;
        var folder = new DirectoryInfo(path);
        if ((folder.Attributes & FileAttributes.ReparsePoint) != 0) { File.Delete(path); return; }
        if (!folder.EnumerateFileSystemInfos().Any()) folder.Delete();
    }

    static void MakeWritable(string folder)
    {
        if (!OperatingSystem.IsLinux()) return;
        File.SetUnixFileMode(folder, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        foreach (var entry in new DirectoryInfo(folder).EnumerateFileSystemInfos())
        {
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) continue;
            if ((entry.Attributes & FileAttributes.Directory) != 0) MakeWritable(entry.FullName);
        }
    }

    static bool Linked(string path) =>
        (File.Exists(path) || Directory.Exists(path)) && File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint);
}
