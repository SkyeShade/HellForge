using Microsoft.Win32;

namespace HellForge.Core;

public sealed record SteamGame(string ManifestPath, InstallReport Report, bool SupportedBuild);

// Finds HELLDIVERS 2 in the signed-in user's Steam libraries, wherever Steam is installed.
public static class SteamLocator
{
    // Steam records its folder for the current user; the usual location is the fallback.
    public static string? SteamRoot()
    {
        if (OperatingSystem.IsWindows() &&
            Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) is string path && Directory.Exists(path))
            return LocalInstall.FullPath(path);
        if (OperatingSystem.IsLinux() && LinuxHost.SteamRoot() is { } linuxSteam) return LocalInstall.FullPath(linuxSteam);
        var fallback = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
        return Directory.Exists(fallback) ? fallback : null;
    }

    // The installed game, or null when Steam or the game is not installed. Throws when the game is present but not
    // usable as a source (an update in progress, missing files). With HELLDIVERS 2 in several libraries (a copy left on
    // another drive), the best one: the supported build, else one a downpatch converts, else the first usable one.
    public static SteamGame? FindGame(string? steamRoot = null)
    {
        steamRoot ??= SteamRoot();
        if (steamRoot == null) return null;
        var games = new List<SteamGame>();
        Exception? unusable = null;
        foreach (var manifest in LocalInstall.FindManifests(steamRoot))
        {
            try
            {
                var report = LocalInstall.Inspect(manifest);
                games.Add(new(manifest, report, report.Pin.BuildId == SupportedBuild.Current.BuildId &&
                    report.Pin.ExeSha256 == SupportedBuild.Current.ExeSha256 && report.Pin.GameDllSha256 == SupportedBuild.Current.GameDllSha256));
            }
            catch (Exception error) when (error is InvalidDataException or IOException) { unusable ??= error; }
        }
        var best = games.FirstOrDefault(g => g.SupportedBuild) ?? games.FirstOrDefault(g => Downpatcher.FindPack(g.Report.Pin) != null) ??
            games.FirstOrDefault();
        if (best == null && unusable != null) throw unusable;
        return best;
    }
}
