using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core;

// The backend folder's digest, compiled into HellForge.Session when it is built with its backend (scripts/Build-Engine.ps1
// Publish-Aot), so a session refuses to start a backend that was swapped on its own (PlaySession.StartBackend). It is a
// local check: whoever replaces the session too, or edits the pin inside it, gets past it. One function for the build and
// the check: each file's path from the folder, '/'-separated with its case, in ordinal order, with its SHA-256.
public static class BackendPin
{
    public const string Match = "match", Refused = "refused", None = "none";

    public static string Digest(string folder)
    {
        var held = Hold(folder, out var digest);
        foreach (var file in held) file.Dispose();
        return digest;
    }

    // The folder's files opened for reading only, so nothing changes them until they are disposed, and their digest.
    public static List<FileStream> Hold(string folder, out string digest)
    {
        var root = Path.GetFullPath(folder);
        var files = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .Select(path => (Path: path, Name: Path.GetRelativePath(root, path).Replace('\\', '/')))
            .OrderBy(file => file.Name, StringComparer.Ordinal).ToList();
        var held = new List<FileStream>();
        var lines = new StringBuilder();
        try
        {
            foreach (var (path, name) in files)
            {
                var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                held.Add(stream);
                lines.Append(Convert.ToHexString(SHA256.HashData(stream))).Append(' ').Append(name).Append('\n');
                stream.Position = 0;
            }
        }
        catch
        {
            foreach (var file in held) file.Dispose();
            throw;
        }
        digest = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(lines.ToString())));
        return held;
    }

    // Whether a pinned folder may start: None without a pin (a development build checks nothing), else Match or Refused.
    // The files stay held (FileShare.Read) from the hash to the start; the caller disposes them when the session ends.
    public static string Check(string folder, string pin, List<FileStream> held)
    {
        if (pin.Length == 0) return None;
        held.AddRange(Hold(folder, out var digest));
        return string.Equals(digest, pin, StringComparison.OrdinalIgnoreCase) ? Match : Refused;
    }

    public const string Refusal = "The local backend's files are not the ones this HellForge was built with, so the session " +
        "will not start it: install HellForge again (its Windows installer, or the Linux package), or build the session and " +
        "backend together (scripts/Build-Engine.ps1).";
}
