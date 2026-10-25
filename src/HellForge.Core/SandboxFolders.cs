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

    // Copies the parts that exist from one root to the other, overwriting; files only, nothing deleted.
    public static Copied Copy(string fromRoot, string toRoot, IEnumerable<string> parts, long maxBytes = MaxBytes)
    {
        int files = 0, skipped = 0;
        long bytes = 0;
        foreach (var part in parts)
        {
            var from = Path.Combine(fromRoot, part);
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

    static bool Linked(string path) =>
        (File.Exists(path) || Directory.Exists(path)) && File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint);
}
