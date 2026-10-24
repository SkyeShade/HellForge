using System.Diagnostics;

namespace HellForge.Core;

// Native Linux host: the launcher, the CLI and the backend run as Linux programs, and only the Windows pieces (the session
// runner, the Steam ownership probe) run through Proton. hellforge-linux.sh puts everything in the offline namespace first
// and passes the Proton script, the prefix and the Steam folder in the environment.
public static class LinuxHost
{
    public static bool IsNative => OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine;

    public static string? Proton => Environment.GetEnvironmentVariable("PROTON") is { Length: > 0 } p && File.Exists(p) ? p : null;
    public static string? Prefix => Environment.GetEnvironmentVariable("STEAM_COMPAT_DATA_PATH") is { Length: > 0 } p ? p : null;

    // Steam's own folder: what the environment says, else the usual places.
    public static string? SteamRoot()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string?[] candidates =
        [
            Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH"),
            Path.Combine(home, ".local/share/Steam"), Path.Combine(home, ".steam/debian-installation"), Path.Combine(home, ".steam/steam"),
            Path.Combine(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam"), Path.Combine(home, "snap/steam/common/.local/share/Steam")
        ];
        foreach (var candidate in candidates)
            if (!string.IsNullOrEmpty(candidate) && Directory.Exists(Path.Combine(candidate, "steamapps")))
                return Path.GetFullPath(candidate);
        return null;
    }

    // The installation a native launcher manages: HELLFORGE_INSTALL, else the one in HellForge's own prefix.
    public static InstallLayout? FindInstall()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string?[] candidates =
        [
            Environment.GetEnvironmentVariable("HELLFORGE_INSTALL"),
            Path.Combine(home, ".local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge")
        ];
        foreach (var candidate in candidates)
            if (!string.IsNullOrEmpty(candidate) && InstallLayout.Find(candidate) is { } install) return install;
        return null;
    }

    // A Linux path as the Windows program sees it: C:\ inside the prefix's drive_c, else Wine's Z: drive.
    public static string WindowsPath(string path)
    {
        path = Path.GetFullPath(path);
        if (Prefix is { } prefix)
        {
            var drive = Path.Combine(Path.GetFullPath(prefix), "pfx", "drive_c");
            if (path.StartsWith(drive + "/", StringComparison.Ordinal) || path == drive)
            {
                var rest = path[drive.Length..].Replace('/', '\\');
                return "C:" + (rest.Length == 0 ? "\\" : rest);
            }
        }
        return "Z:" + path.Replace('/', '\\');
    }

    // The reverse: a Windows path the session runner wrote down (play-session.json) as a Linux path. Anything that is not
    // a drive path is returned as it is.
    public static string NativePath(string path)
    {
        if (!OperatingSystem.IsLinux() || path.Length < 3 || path[1] != ':' || path[2] != '\\' || !char.IsAsciiLetter(path[0])) return path;
        var rest = path[3..].Replace('\\', '/');
        if (char.ToUpperInvariant(path[0]) == 'Z') return "/" + rest;
        return Prefix is { } prefix && char.ToUpperInvariant(path[0]) == 'C' ? Path.Combine(Path.GetFullPath(prefix), "pfx", "drive_c", rest) : path;
    }

    // Starts a Windows program through Proton and waits for it (proton's own waitforexitandrun verb). Arguments that are
    // Linux paths are converted by the caller with WindowsPath.
    public static ProcessStartInfo Windows(string exe, IEnumerable<string> arguments, string? workingDirectory = null)
    {
        var proton = Proton ?? throw new InvalidOperationException("Proton was not found: start HellForge with linux/hellforge-linux.sh (it sets PROTON).");
        var start = new ProcessStartInfo(proton) { UseShellExecute = false, CreateNoWindow = true };
        if (workingDirectory != null) start.WorkingDirectory = workingDirectory;
        start.ArgumentList.Add("waitforexitandrun");
        start.ArgumentList.Add(exe);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        return start;
    }
}
