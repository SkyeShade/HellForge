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

    // The strict walk Windows keeps (and Setup uses for its OneDrive check): a link at path or any parent is refused.
    public static void NoLinks(string path)
    {
        for (var current = FullPath(path); current is not null; current = Path.GetDirectoryName(current))
            RefuseLink(current);
    }

    // Linux: links above protectedRoot are normal (Fedora Atomic's /home -> /var/home) and are resolved; a link at
    // protectedRoot, the folder or file HellForge guards, or below it on the way to path, is refused. Returns path under
    // the real root, and callers use that path, not the one they were given. Windows: every parent of path is checked,
    // exactly as NoLinks(path) does.
    public static string NoLinks(string path, string protectedRoot)
    {
        var full = FullPath(path);
        if (!OperatingSystem.IsLinux())
        {
            NoLinks(full);
            return full;
        }

        var root = FullPath(protectedRoot);
        var prefix = root.EndsWith(Path.DirectorySeparatorChar) ? root : root + Path.DirectorySeparatorChar;
        if (!full.Equals(root, StringComparison.Ordinal) && !full.StartsWith(prefix, StringComparison.Ordinal))
            throw new ArgumentException("The path must be inside the folder HellForge protects: " + root, nameof(path));
        var realRoot = ResolveParentLinks(root);
        var current = realRoot;
        RefuseLink(current);
        var relative = full.Length == root.Length ? "" : full[(root.EndsWith(Path.DirectorySeparatorChar) ? root.Length : root.Length + 1)..];
        foreach (var part in relative.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, part);
            RefuseLink(current);
        }
        return current;
    }

    // Files under a folder whose entries may be untrusted: a link is skipped, never followed or returned. The root itself
    // must not be a link. Linux resolves links in its parents (the normal Fedora Atomic case); Windows and Wine leave
    // parent links to the OS but this walk never enumerates them. Names returned keep the caller's root spelling.
    public static IEnumerable<string> FilesNoLinks(string root, string pattern = "*", bool recursive = true)
    {
        string? fullRoot = null, safeRoot = null;
        try
        {
            fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
            if (OperatingSystem.IsLinux()) safeRoot = NoLinks(fullRoot, fullRoot);
            else { RefuseLink(fullRoot); safeRoot = fullRoot; }
        }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or InvalidDataException or NotSupportedException)
        {
        }
        if (fullRoot is null || safeRoot is null || !Directory.Exists(safeRoot)) yield break;
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = recursive,
            AttributesToSkip = FileAttributes.ReparsePoint,
            IgnoreInaccessible = true
        };
        foreach (var file in Directory.EnumerateFiles(safeRoot, pattern, options))
            yield return Path.Combine(fullRoot, Path.GetRelativePath(safeRoot, file));
    }

    // GetAttributes also checks dangling links, which File.Exists may report as missing.
    static void RefuseLink(string current)
    {
        FileAttributes attributes;
        try { attributes = File.GetAttributes(current); }
        catch (FileNotFoundException) { return; }
        catch (DirectoryNotFoundException) { return; }
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Linked/reparse paths are not supported: {current}");
    }

    // Refuses a link at root or anywhere below it. NoLinks(path, root) checks one path's components; a tree an untrusted
    // program wrote can hold a link in any entry, before a reader such as ModCatalog.Parse opens a file in it.
    internal static void NoLinksBelow(string root)
    {
        Walk(NoLinks(root, root));

        static void Walk(string folder)
        {
            foreach (var entry in new DirectoryInfo(folder).EnumerateFileSystemInfos())
            {
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException($"Linked/reparse paths are not supported: {entry.FullName}");
                if ((entry.Attributes & FileAttributes.Directory) != 0) Walk(entry.FullName);
            }
        }
    }

    // Linux only: the path with every link in its parent folders resolved and its last component left alone, so a link
    // there is still seen. A missing or unreadable component is left as it is. Windows keeps the path unchanged.
    internal static string ResolveParentLinks(string path)
    {
        var full = FullPath(path);
        if (!OperatingSystem.IsLinux()) return full;
        var name = Path.GetFileName(full);
        var parent = Path.GetDirectoryName(full);
        if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(parent)) return full;
        return Path.Combine(ResolveLinks(parent), name);
    }

    // Linux only: resolves every link component of a path from the filesystem root down, following each link to its final
    // target. A missing or unreadable component is left as it is; the caller's existence checks decide what that means.
    internal static string ResolveLinks(string path)
    {
        var full = FullPath(path);
        if (!OperatingSystem.IsLinux()) return full;
        var root = Path.GetPathRoot(full);
        if (string.IsNullOrEmpty(root)) return full;
        var current = root;
        foreach (var name in full[root.Length..].Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, name);
            try
            {
                var target = Directory.ResolveLinkTarget(current, returnFinalTarget: true);
                if (target != null)
                {
                    var resolved = target.FullName;
                    if (!Path.IsPathRooted(resolved))
                        resolved = Path.Combine(Path.GetDirectoryName(current) ?? root, resolved);
                    current = FullPath(resolved);
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { }
        }
        return current;
    }

    public static IReadOnlyList<string> FindManifests(string steamRoot) =>
        FindLibraries(steamRoot).Select(root => Path.Combine(root, "steamapps", $"appmanifest_{AppId}.acf"))
            .Where(File.Exists).Order(StringComparer.OrdinalIgnoreCase).ToArray();

    // Steam's library folders: its own folder and every one in libraryfolders.vdf.
    public static IReadOnlyList<string> FindLibraries(string steamRoot)
    {
        steamRoot = ResolveLinks(steamRoot);
        var libraries = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { steamRoot };
        var folders = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        if (File.Exists(folders))
        {
            folders = NoLinks(folders, steamRoot);
            var document = ValveData.Parse(File.ReadAllText(folders));
            if (!document.Children.TryGetValue("libraryfolders", out var entries))
                throw new InvalidDataException("Missing libraryfolders object.");
            foreach (var entry in entries.Children.Values)
                libraries.Add(ResolveLinks(FullPath(entry.Required("path"))));
            foreach (var entry in entries.Values.Where(x => int.TryParse(x.Key, out _)))
                libraries.Add(ResolveLinks(FullPath(entry.Value)));
        }
        return libraries.Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public static List<InventoryFile> Inventory(string root)
    {
        root = NoLinks(root, root);
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
        var steamApps = Path.GetDirectoryName(FullPath(manifestPath))!;
        steamApps = NoLinks(steamApps, steamApps);
        manifestPath = NoLinks(Path.Combine(steamApps, Path.GetFileName(manifestPath)), steamApps);
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
        if (!Path.GetFileName(steamApps).Equals("steamapps", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Manifest must be inside a steamapps directory.");
        // The game folder is protected: a link above it (the Steam library, /home) is resolved, but the folder itself and
        // anything below it may not be a link, or the game read would be some other folder.
        var source = FullPath(Path.Combine(steamApps, "common", name));
        source = NoLinks(source, source);
        foreach (var required in new[] { Exe, GameDll })
        {
            var path = NoLinks(Path.Combine(source, required), source);
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
