using System.Text.Json;
using Microsoft.Win32;

namespace HellForge.Core;

public sealed record SteamGame(string ManifestPath, InstallReport Report, bool SupportedBuild);
public sealed record ProtonVersion(string Name, string ScriptPath);

// Finds HELLDIVERS 2 in the signed-in user's Steam libraries, wherever Steam is installed.
public static class SteamLocator
{
    // The folders inside a home where Steam installs itself. .steam/root and .steam/steam are links Steam keeps to the
    // folder it runs from, so they also find an installation elsewhere.
    static readonly string[] HomeSteamFolders =
    [
        ".local/share/Steam",
        ".steam/debian-installation",
        ".steam/steam",
        ".steam/root",
        ".var/app/com.valvesoftware.Steam/.local/share/Steam",
        "snap/steam/common/.local/share/Steam"
    ];

    // Distribution packages such as proton-ge-custom install their tools in these folders.
    static readonly string[] SystemCompatibilityTools =
        ["/usr/share/steam/compatibilitytools.d", "/usr/local/share/steam/compatibilitytools.d"];

    public static string? ProtonScript(string? steamRoot = null)
    {
        if (!OperatingSystem.IsLinux()) return null;
        var configured = Environment.GetEnvironmentVariable("PROTON");
        if (!string.IsNullOrEmpty(configured))
            return File.Exists(configured) ? LocalInstall.FullPath(configured) : null;

        return ProtonVersions(steamRoot).LastOrDefault()?.ScriptPath;
    }

    // Installed Proton scripts, in the same order used to choose the default version: every Steam library's
    // steamapps/common/Proton*, the Steam root's compatibilitytools.d, each folder in STEAM_EXTRA_COMPAT_TOOLS_PATHS, and
    // the system compatibilitytools.d folders. A compatibilitytools.d entry counts only with a proton script and a
    // compatibilitytool.vdf beside it. The same search order is in linux/hf-steam.sh.
    public static IReadOnlyList<ProtonVersion> ProtonVersions(string? steamRoot = null)
    {
        if (!OperatingSystem.IsLinux()) return [];
        steamRoot ??= SteamRoot();
        return ProtonVersionsFrom(steamRoot, ExtraCompatibilityTools(), SystemCompatibilityTools,
            Environment.GetEnvironmentVariable("PROTON"));
    }

    // ProtonVersions with its inputs given, so tests can use a fake tree.
    internal static IReadOnlyList<ProtonVersion> ProtonVersionsFrom(string? steamRoot, IEnumerable<string> extraToolFolders,
        IEnumerable<string> systemToolFolders, string? configured)
    {
        var candidates = new List<string>();
        if (steamRoot != null)
        {
            foreach (var library in LocalInstall.FindLibraries(steamRoot))
            {
                var common = Path.Combine(library, "steamapps", "common");
                if (Directory.Exists(common))
                    candidates.AddRange(Directory.EnumerateDirectories(common, "Proton*")
                        .Select(directory => Path.Combine(directory, "proton")));
            }
            AddCompatibilityTools(Path.Combine(steamRoot, "compatibilitytools.d"));
        }
        foreach (var folder in extraToolFolders) AddCompatibilityTools(folder);
        foreach (var folder in systemToolFolders) AddCompatibilityTools(folder);
        if (!string.IsNullOrEmpty(configured) && File.Exists(configured)) candidates.Add(configured);

        void AddCompatibilityTools(string folder)
        {
            if (!Directory.Exists(folder)) return;
            candidates.AddRange(Directory.EnumerateDirectories(folder)
                .Where(tool => File.Exists(Path.Combine(tool, "compatibilitytool.vdf")))
                .Select(tool => Path.Combine(tool, "proton")));
        }

        return candidates.Where(File.Exists).Select(LocalInstall.FullPath).Distinct(StringComparer.Ordinal)
            .OrderBy(path => path, Comparer<string>.Create(CompareProton))
            .Select(path => new ProtonVersion(Path.GetFileName(Path.GetDirectoryName(path))!, LocalInstall.FullPath(path)))
            .ToArray();
    }

    static IEnumerable<string> ExtraCompatibilityTools()
    {
        var configured = Environment.GetEnvironmentVariable("STEAM_EXTRA_COMPAT_TOOLS_PATHS");
        return string.IsNullOrEmpty(configured)
            ? []
            : configured.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);
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

    // Null when a Proton folder the player picked outside the search can be used: a regular executable file named proton
    // with the runtime's files/ or dist/ folder beside it. Otherwise why not.
    public static string? ProtonProblem(string scriptPath)
    {
        if (string.IsNullOrWhiteSpace(scriptPath)) return "Choose the proton file inside a Proton folder.";
        var full = LocalInstall.FullPath(scriptPath);
        if (Directory.Exists(full)) return "The selected path is a folder, not a proton file: " + full;
        var file = new FileInfo(full);
        if (!file.Exists) return "The selected Proton script does not exist: " + full;
        if (!file.Name.Equals("proton", StringComparison.Ordinal))
            return "Choose the file named proton inside a Proton folder: " + full;
        if (OperatingSystem.IsLinux())
        {
            UnixFileMode mode;
            try { mode = File.GetUnixFileMode(full); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                return "The selected proton file could not be read: " + full + " (" + error.Message + ")";
            }
            if ((mode & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) == 0)
                return "The selected proton file is not executable: " + full;
        }
        var folder = file.DirectoryName!;
        if (!Directory.Exists(Path.Combine(folder, "files")) && !Directory.Exists(Path.Combine(folder, "dist")))
            return "The selected Proton folder has no files or dist folder: " + folder;
        return null;
    }

    public static ProtonVersion ProtonVersionFor(string scriptPath)
    {
        var full = LocalInstall.FullPath(scriptPath);
        return new ProtonVersion(Path.GetFileName(Path.GetDirectoryName(full))!, full);
    }

    // The found versions plus a saved one outside the search order (a Proton the player picked with Other...), once.
    public static IReadOnlyList<ProtonVersion> IncludingSelected(IEnumerable<ProtonVersion> protons, string? scriptPath)
    {
        var list = protons.ToList();
        if (string.IsNullOrEmpty(scriptPath)) return list;
        var full = LocalInstall.FullPath(scriptPath);
        if (list.Any(version => version.ScriptPath == full) || ProtonProblem(full) != null) return list;
        list.Add(ProtonVersionFor(full));
        return list;
    }

    static int CompareProton(string left, string right)
    {
        static (bool Experimental, Version Version, string Name) Key(string path)
        {
            var name = Path.GetFileName(Path.GetDirectoryName(path))!;
            // GE-Proton names its builds 10-25 (10.25): a dash between numbers is read as a dot. A bare major number
            // ("Proton 9") is padded, since Version does not parse one alone.
            var match = System.Text.RegularExpressions.Regex.Match(name, @"\d+(?:[.-]\d+)*");
            var text = match.Value.Replace('-', '.');
            if (text.Length > 0 && !text.Contains('.', StringComparison.Ordinal)) text += ".0";
            return (name.Contains("Experimental", StringComparison.OrdinalIgnoreCase),
                Version.TryParse(text, out var version) ? version : new Version(0, 0), name);
        }

        var a = Key(left);
        var b = Key(right);
        var result = a.Experimental.CompareTo(b.Experimental);
        if (result == 0) result = a.Version.CompareTo(b.Version);
        return result != 0 ? result : StringComparer.Ordinal.Compare(a.Name, b.Name);
    }

    // Steam records its folder for the current user; the usual location is the fallback. An installation may save a
    // folder the player chose (Steam outside the usual places), which sessions and external launches then pass to the
    // wrapper as STEAM_COMPAT_CLIENT_INSTALL_PATH.
    public static string? SteamRoot(InstallLayout? install = null)
    {
        if (OperatingSystem.IsLinux())
        {
            var configured = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH");
            if (ValidSteamRoot(configured)) return ResolveSteamRoot(configured!);
            if (install != null && SelectedSteamRoot(install) is { } saved) return saved;
            return FindSteamRoot(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), null);
        }
        if (OperatingSystem.IsWindows() &&
            Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) is string path && Directory.Exists(path))
            return LocalInstall.FullPath(path);
        var fallback = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
        return Directory.Exists(fallback) ? fallback : null;
    }

    // The folder a player chose for Steam, saved in the installation. Null when it is absent or no longer holds steamapps,
    // so a moved or unplugged Steam falls back to the usual search instead of refusing every session.
    public static string? SelectedSteamRoot(InstallLayout install)
    {
        try
        {
            using var selection = JsonDocument.Parse(File.ReadAllText(install.SteamSelection));
            if (selection.RootElement.ValueKind == JsonValueKind.Object &&
                selection.RootElement.TryGetProperty("steamRoot", out var root) && root.ValueKind == JsonValueKind.String &&
                root.GetString() is { Length: > 0 } path && SteamFolderProblem(path) == null)
                return SteamFolder(path);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { }
        return null;
    }

    public static void SaveSteamSelection(InstallLayout install, string steamRoot)
    {
        if (SteamFolderProblem(steamRoot) is { } problem) throw new InvalidDataException(problem);
        File.WriteAllText(install.SteamSelection, JsonSerializer.Serialize(new { steamRoot = SteamFolder(steamRoot) }) + "\n");
    }

    // Null when the folder is Steam's client folder (the one that holds steamapps); otherwise why not.
    public static string? SteamFolderProblem(string folder)
    {
        if (string.IsNullOrWhiteSpace(folder)) return "Choose the folder Steam is installed in (the one that holds steamapps).";
        var full = LocalInstall.FullPath(folder);
        if (!Directory.Exists(full)) return "The Steam folder does not exist: " + full;
        if (!Directory.Exists(Path.Combine(full, "steamapps")))
            return "This folder has no steamapps folder. Choose the folder Steam is installed in: " + full;
        return null;
    }

    public static string SteamFolder(string folder) => ResolveSteamRoot(folder);

    // The Steam folders in a home, plus one the caller names; the first that holds steamapps. Internal so tests can use
    // a fake home rather than this PC's Steam.
    internal static string? FindSteamRoot(string home, string? configured)
    {
        if (ValidSteamRoot(configured)) return ResolveSteamRoot(configured!);
        foreach (var relative in HomeSteamFolders)
        {
            var path = Path.Combine(home, relative);
            if (ValidSteamRoot(path)) return ResolveSteamRoot(path);
        }
        return null;
    }

    static bool ValidSteamRoot(string? path) =>
        !string.IsNullOrEmpty(path) && Directory.Exists(Path.Combine(path, "steamapps"));

    static string ResolveSteamRoot(string path) => LocalInstall.ResolveLinks(LocalInstall.FullPath(path));

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
