namespace HellForge.Core;

// The supported build fetched by Steam itself, for when Steam's own HELLDIVERS 2 has moved on to another build (or is not
// installed). The Steam console's "download_depot <app> <depot> <manifest>" downloads one depot at an exact manifest, for
// an account that owns the game, into <Steam>/steamapps/content/app_<app>/depot_<depot>. HellForge shows the commands for the
// manifests it pins (SupportedBuild.Depots), watches those folders and copies the game from them like from an install:
// every file is hashed against the build's list. HellForge downloads nothing itself and ships no game content.
public static class SteamDepotDownload
{
    // Waiting: the depot's files not (yet) present with their listed size, as (path, the size on disk or -1 when missing,
    // the listed size).
    public sealed record DepotState(SupportedDepot Depot, long Done, long Total, IReadOnlyList<(string Path, long Have, long Size)> Waiting)
    {
        public bool Complete => Done == Total;
    }

    public static string ContentFolder(string steamRoot, SupportedBuild build) =>
        Path.Combine(steamRoot, "steamapps", "content", "app_" + build.AppId);

    // Where else download_depot may have put it: the Linux Steam client downloads relative to the folder its program runs
    // from, so <Steam>/ubuntu12_32/steamapps/content (or ubuntu12_64, or steamrt64 for the 64-bit client) instead of
    // <Steam>/steamapps/content (GitHub issue 11).
    static readonly string[] ClientFolders = ["ubuntu12_32", "ubuntu12_64", "steamrt64"];

    // Steam's steamapps folders: its own and the ones under the client folders its download_depot runs from.
    public static IReadOnlyList<string> SteamAppsFolders(string steamRoot) =>
        [Path.Combine(steamRoot, "steamapps"), .. ClientFolders.Select(client => Path.Combine(steamRoot, client, "steamapps"))];

    public static IReadOnlyList<string> ContentFolders(string steamRoot, SupportedBuild build) =>
        [.. SteamAppsFolders(steamRoot).Select(folder => Path.Combine(folder, "content", "app_" + build.AppId))];

    // The depot's folder: the first place Steam put it, else the usual one (while nothing is there yet).
    public static string DepotFolder(string steamRoot, SupportedBuild build, uint depot) =>
        ContentFolders(steamRoot, build).Select(folder => Path.Combine(folder, "depot_" + depot)).FirstOrDefault(Directory.Exists)
        ?? Path.Combine(ContentFolder(steamRoot, build), "depot_" + depot);

    public static IReadOnlyList<string> Commands(SupportedBuild build) =>
        [.. build.Depots.Select(depot => $"download_depot {build.AppId} {depot.Id} {depot.ManifestId}")];

    public static string Locate(string steamRoot, SupportedBuild build, SupportedFile file) =>
        Path.Combine(DepotFolder(steamRoot, build, file.Depot), file.Path);

    // Bytes of each depot's files that are present with their listed size (a quick check while Steam downloads; the copy
    // hashes every file).
    public static IReadOnlyList<DepotState> Status(string steamRoot, SupportedBuild build) =>
        [.. build.Depots.Select(depot =>
        {
            long done = 0, total = 0;
            var waiting = new List<(string, long, long)>();
            foreach (var file in build.Files.Where(f => f.Depot == depot.Id))
            {
                total += file.Size;
                var info = new FileInfo(Locate(steamRoot, build, file));
                if (info.Exists && info.Length == file.Size) done += file.Size;
                else waiting.Add((file.Path, info.Exists ? info.Length : -1, file.Size));
            }
            return new DepotState(depot, done, total, waiting);
        })];

    public static bool IsComplete(string steamRoot, SupportedBuild build) => Status(steamRoot, build).All(depot => depot.Complete);

    // Removes the build's depot folders after the game is copied (and the app folder when nothing else is in it). With
    // callbacks, a failed path is reported and the rest are still attempted; without one, a failure is thrown as before.
    // On Linux a depot folder that is a link is refused, never followed or removed; links inside a depot folder are
    // removed with it, because Directory.Delete(recursive: true) unlinks them without following them.
    public static void Delete(string steamRoot, SupportedBuild build, Action<string, Exception>? failed = null,
        Action<string>? removed = null)
    {
        if (OperatingSystem.IsLinux()) steamRoot = LocalInstall.ResolveLinks(steamRoot);
        foreach (var app in ContentFolders(steamRoot, build))
        {
            foreach (var depot in build.Depots)
                DeleteFolder(steamRoot, Path.Combine(app, "depot_" + depot.Id), failed, removed);
            DeleteEmptyApp(steamRoot, app, failed, removed);
        }
    }

    static void DeleteFolder(string steamRoot, string folder, Action<string, Exception>? failed, Action<string>? removed)
    {
        if (!Directory.Exists(folder)) return;
        try
        {
            RefuseLinks(folder, steamRoot);
            Directory.Delete(folder, recursive: true);
            removed?.Invoke(folder);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            if (failed == null) throw;
            failed(folder, error);
        }
    }

    static void DeleteEmptyApp(string steamRoot, string app, Action<string, Exception>? failed, Action<string>? removed)
    {
        if (!Directory.Exists(app)) return;
        try
        {
            RefuseLinks(app, steamRoot);
            if (Directory.EnumerateFileSystemEntries(app).Any()) return;
            Directory.Delete(app);
            removed?.Invoke(app);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            if (failed == null) throw;
            failed(app, error);
        }
    }

    // Linux uninstall refuses links inside Steam so a depot download can never lead outside Steam's folders; a link above
    // Steam's folder (/home -> /var/home) is normal and is resolved first. Windows keeps its old behaviour.
    static void RefuseLinks(string path, string steamRoot)
    {
        if (OperatingSystem.IsLinux()) LocalInstall.NoLinks(path, steamRoot);
    }
}
