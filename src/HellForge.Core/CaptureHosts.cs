namespace HellForge.Core;

// The capture's line in the hosts file: a marker, then the API host mapped to loopback. Remove drops every line that holds
// the marker, so Add starts a new line first when the file does not end with one; otherwise the marker would join the
// player's last entry and cleanup would delete that entry with it.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureHosts
{
    public const string Marker = "# HellForge-CaptureProxy";

    public static void Add(string hostsPath, string host)
    {
        var text = Marker + Environment.NewLine + Mapping(host) + Environment.NewLine;
        File.AppendAllText(hostsPath, EndsMidLine(hostsPath) ? Environment.NewLine + text : text);
    }

    // True when it removed anything.
    public static bool Remove(string hostsPath, string host)
    {
        if (!File.Exists(hostsPath)) return false;
        var lines = File.ReadAllLines(hostsPath);
        var kept = lines.Where(line => !line.Contains(Marker, StringComparison.OrdinalIgnoreCase) &&
            !line.Trim().Equals(Mapping(host), StringComparison.OrdinalIgnoreCase)).ToArray();
        if (kept.Length == lines.Length) return false;
        File.WriteAllLines(hostsPath, kept);
        return true;
    }

    static string Mapping(string host) => "127.0.0.1 " + host;

    static bool EndsMidLine(string path)
    {
        if (!File.Exists(path)) return false;
        using var file = File.OpenRead(path);
        if (file.Length == 0) return false;
        file.Seek(-1, SeekOrigin.End);
        return file.ReadByte() != '\n';
    }
}
