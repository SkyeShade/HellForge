using System.Security.Cryptography;

namespace HellForge.Core;

public sealed record BuildPin(string BuildId, string ExeSha256, string GameDllSha256);
public sealed record InventoryFile(string RelativePath, long Length, long LastWriteTicks);
public sealed record InstallReport(string Source, string ManifestPath, BuildPin Pin,
    int FileCount, long TotalBytes, string[] BinFiles,
    string OwnershipStatus = "not_verified_local_metadata_only",
    string IsolationStatus = "not_implemented", bool ReadyToLaunch = false);

public static class LocalInstall
{
    public const string AppId = "553850";
    public const string Exe = "bin/helldivers2.exe";
    public const string GameDll = "data/game/game.dll";

    public static string Hash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    public static string FullPath(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    public static bool Within(string path, string root) =>
        FullPath(path).Equals(FullPath(root), StringComparison.OrdinalIgnoreCase) ||
        FullPath(path).StartsWith(FullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

    public static void NoLinks(string path)
    {
        for (var current = FullPath(path); current is not null; current = Path.GetDirectoryName(current))
        {
            // GetAttributes also checks dangling links, which File.Exists may report as missing.
            FileAttributes attributes;
            try { attributes = File.GetAttributes(current); }
            catch (FileNotFoundException) { continue; }
            catch (DirectoryNotFoundException) { continue; }
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException($"Linked/reparse paths are not supported: {current}");
        }
    }

    public static IReadOnlyList<string> FindManifests(string steamRoot) =>
        FindLibraries(steamRoot).Select(root => Path.Combine(root, "steamapps", $"appmanifest_{AppId}.acf"))
            .Where(File.Exists).Order(StringComparer.OrdinalIgnoreCase).ToArray();

    // Steam's library folders: its own folder and every one in libraryfolders.vdf.
    public static IReadOnlyList<string> FindLibraries(string steamRoot)
    {
        steamRoot = FullPath(steamRoot);
        NoLinks(steamRoot);
        var libraries = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { steamRoot };
        var folders = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        if (File.Exists(folders))
        {
            NoLinks(folders);
            var document = ValveData.Parse(File.ReadAllText(folders));
            if (!document.Children.TryGetValue("libraryfolders", out var entries))
                throw new InvalidDataException("Missing libraryfolders object.");
            foreach (var entry in entries.Children.Values)
                libraries.Add(FullPath(entry.Required("path")));
            foreach (var entry in entries.Values.Where(x => int.TryParse(x.Key, out _)))
                libraries.Add(FullPath(entry.Value));
        }
        return libraries.Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public static List<InventoryFile> Inventory(string root)
    {
        NoLinks(root);
        var files = new List<InventoryFile>();
        void Walk(string directory)
        {
            foreach (var entry in new DirectoryInfo(directory).EnumerateFileSystemInfos())
            {
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException($"Linked/reparse paths are not supported: {entry.FullName}");
                if ((entry.Attributes & FileAttributes.Directory) != 0) Walk(entry.FullName);
                else files.Add(new(Path.GetRelativePath(root, entry.FullName).Replace('\\', '/'),
                    ((FileInfo)entry).Length, entry.LastWriteTimeUtc.Ticks));
            }
        }
        Walk(root);
        return files.OrderBy(x => x.RelativePath, StringComparer.Ordinal).ToList();
    }

    // Steam's app state (EAppState bits): installed in full (4), and not busy with its files. An update Steam has queued
    // (2) or paused (512) leaves the installed build's files as they are, so they count; one it is downloading, staging
    // or committing does not, nor files it reports missing or corrupt, nor a running game (its files are locked).
    public static string? Unusable(uint flags)
    {
        const uint installed = 4, running = 64, missing = 32, corrupt = 128;
        const uint busy = 256 | 1024 | 2048 | 4096 | 65536 | 131072 | 262144 | 524288 | 1048576 | 2097152 | 4194304 | 8388608;
        if ((flags & installed) == 0) return "Steam does not report HELLDIVERS 2 as fully installed.";
        if ((flags & (missing | corrupt)) != 0) return "Steam reports missing or corrupt game files: verify the game in Steam, then start setup again.";
        if ((flags & busy) != 0) return "Steam is updating, validating or moving HELLDIVERS 2: let it finish, then start setup again.";
        if ((flags & running) != 0) return "HELLDIVERS 2 is running: close it, then start setup again.";
        return null;
    }

    public static InstallReport Inspect(string manifestPath)
    {
        manifestPath = FullPath(manifestPath);
        NoLinks(manifestPath);
        if (!Path.GetFileName(manifestPath).Equals($"appmanifest_{AppId}.acf", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Expected the HELLDIVERS 2 Steam app manifest.");
        var document = ValveData.Parse(File.ReadAllText(manifestPath));
        if (!document.Children.TryGetValue("AppState", out var app))
            throw new InvalidDataException("Missing AppState object.");
        if (app.Required("appid") != AppId) throw new InvalidDataException("Wrong Steam app ID.");
        if (!uint.TryParse(app.Required("StateFlags"), out var flags)) throw new InvalidDataException("Invalid Steam state flags.");
        if (Unusable(flags) is { } why) throw new InvalidDataException(why);
        var build = app.Required("buildid");
        if (!ulong.TryParse(build, out var number) || number == 0) throw new InvalidDataException("Invalid Steam build ID.");
        var name = app.Required("installdir");
        if (name is "." or ".." || name.IndexOfAny(['/', '\\', ':']) >= 0 || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new InvalidDataException("Invalid Steam install directory.");
        var steamApps = Path.GetDirectoryName(manifestPath)!;
        if (!Path.GetFileName(steamApps).Equals("steamapps", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Manifest must be inside a steamapps directory.");
        var source = FullPath(Path.Combine(steamApps, "common", name));
        NoLinks(source);
        foreach (var required in new[] { Exe, GameDll })
        {
            var path = Path.Combine(source, required);
            NoLinks(path);
            if (!File.Exists(path)) throw new InvalidDataException($"Required game file is missing: {required}");
        }
        var inventory = Inventory(source);
        return new(source, manifestPath,
            new(build, Hash(Path.Combine(source, Exe)), Hash(Path.Combine(source, GameDll))),
            inventory.Count, inventory.Sum(x => x.Length),
            inventory.Where(x => x.RelativePath.StartsWith("bin/", StringComparison.OrdinalIgnoreCase))
                .Select(x => x.RelativePath).ToArray());
    }
}
