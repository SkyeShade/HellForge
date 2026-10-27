using System.Text.Json;
using Microsoft.Win32;

namespace HellForge.Core;

public sealed record SteamGame(string ManifestPath, InstallReport Report, bool SupportedBuild);
public sealed record ProtonVersion(string Name, string ScriptPath);

// Finds HELLDIVERS 2 in the signed-in user's Steam libraries, wherever Steam is installed.
public static class SteamLocator
{
    public static string? ProtonScript(string? steamRoot = null)
    {
        if (!OperatingSystem.IsLinux()) return null;
        var configured = Environment.GetEnvironmentVariable("PROTON");
        if (!string.IsNullOrEmpty(configured))
            return File.Exists(configured) ? LocalInstall.FullPath(configured) : null;

        return ProtonVersions(steamRoot).LastOrDefault()?.ScriptPath;
    }

    // Installed Proton scripts, in the same order used to choose the default version.
    public static IReadOnlyList<ProtonVersion> ProtonVersions(string? steamRoot = null)
    {
        if (!OperatingSystem.IsLinux()) return [];
        steamRoot ??= SteamRoot();
        var candidates = new List<string>();
        if (steamRoot != null)
        {
            foreach (var library in LocalInstall.FindLibraries(steamRoot))
            {
                var common = Path.Combine(library, "steamapps", "common");
                var compatibilityTools = Path.Combine(library, "compatibilitytools.d");
                if (Directory.Exists(common))
                    candidates.AddRange(Directory.EnumerateDirectories(common, "Proton*").Select(directory => Path.Combine(directory, "proton")));
                if (Directory.Exists(compatibilityTools))
                    candidates.AddRange(Directory.EnumerateDirectories(compatibilityTools).Select(directory => Path.Combine(directory, "proton")));
            }
        }
        var configured = Environment.GetEnvironmentVariable("PROTON");
        if (!string.IsNullOrEmpty(configured) && File.Exists(configured)) candidates.Add(LocalInstall.FullPath(configured));
        return candidates.Where(File.Exists).Distinct(StringComparer.Ordinal)
            .OrderBy(path => path, Comparer<string>.Create(CompareProton))
            .Select(path => new ProtonVersion(Path.GetFileName(Path.GetDirectoryName(path))!, LocalInstall.FullPath(path)))
            .ToArray();
    }

    public static string? SelectedProtonScript(InstallLayout install)
    {
        try
        {
            using var selection = JsonDocument.Parse(File.ReadAllText(install.ProtonSelection));
            return selection.RootElement.ValueKind == JsonValueKind.Object &&
                selection.RootElement.TryGetProperty("scriptPath", out var path) && path.ValueKind == JsonValueKind.String
                    ? path.GetString() : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    public static void SaveProtonSelection(InstallLayout install, string scriptPath)
    {
        if (!File.Exists(scriptPath)) throw new FileNotFoundException("The selected Proton script is missing.", scriptPath);
        File.WriteAllText(install.ProtonSelection, JsonSerializer.Serialize(new { scriptPath = LocalInstall.FullPath(scriptPath) }) + "\n");
    }

    static int CompareProton(string left, string right)
    {
        static (bool Experimental, Version Version, string Name) Key(string path)
        {
            var name = Path.GetFileName(Path.GetDirectoryName(path))!;
            var match = System.Text.RegularExpressions.Regex.Match(name, @"\d+(?:\.\d+)*");
            return (name.Contains("Experimental", StringComparison.OrdinalIgnoreCase),
                Version.TryParse(match.Value, out var version) ? version : new Version(0, 0), name);
        }

        var a = Key(left);
        var b = Key(right);
        var result = a.Experimental.CompareTo(b.Experimental);
        if (result == 0) result = a.Version.CompareTo(b.Version);
        return result != 0 ? result : StringComparer.Ordinal.Compare(a.Name, b.Name);
    }

    // Steam records its folder for the current user; the usual location is the fallback.
    public static string? SteamRoot()
    {
        if (OperatingSystem.IsLinux())
        {
            var configured = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH");
            if (!string.IsNullOrEmpty(configured) && Directory.Exists(Path.Combine(configured, "steamapps")))
                return LocalInstall.FullPath(configured);
            foreach (var steamPath in new[]
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share", "Steam"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".steam", "debian-installation"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".steam", "steam"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "snap", "steam", "common", ".local", "share", "Steam")
            })
                if (Directory.Exists(Path.Combine(steamPath, "steamapps"))) return LocalInstall.FullPath(steamPath);
            return null;
        }
        if (OperatingSystem.IsWindows() &&
            Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) is string path && Directory.Exists(path))
            return LocalInstall.FullPath(path);
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
