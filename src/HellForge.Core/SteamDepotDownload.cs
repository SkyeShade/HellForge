namespace HellForge.Core;

// The supported build fetched by Steam itself, for when Steam's own HELLDIVERS 2 has moved on to another build (or is not
// installed). The Steam console's "download_depot <app> <depot> <manifest>" downloads one depot at an exact manifest, for
// an account that owns the game, into <Steam>/steamapps/content/app_<app>/depot_<depot>. HellForge shows the commands for the
// manifests it pins (SupportedBuild.Depots), watches those folders and copies the game from them like from an install:
// every file is hashed against the build's list. On this path the Steam client downloads, not HellForge, and HellForge ships no
// game content. Setup offers it as a choice ("Use the Steam app") and as the fallback of the other ways. SteamCMD's
// download_depot (HellForge.SteamDownload's SteamCmd) writes the same folders under SteamCMD's own folder, read the same way.
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
    // <Steam>/steamapps/content (GitHub issue 11). SteamCMD for Linux does the same from its linux32 folder.
    static readonly string[] ClientFolders = ["ubuntu12_32", "ubuntu12_64", "steamrt64", "linux32"];

    static IEnumerable<string> ContentFolders(string steamRoot, SupportedBuild build) =>
        [ContentFolder(steamRoot, build), .. ClientFolders.Select(client => Path.Combine(steamRoot, client, "steamapps", "content", "app_" + build.AppId))];

    // The depot's folder: of the places Steam may have put it, the one holding most of it (the first on a tie), else the
    // usual one (while nothing is there yet). An empty depot folder left by an earlier download must not hide the real
    // one under a client folder (GitHub issue 112; the same as linux-native's #181).
    public static string DepotFolder(string steamRoot, SupportedBuild build, uint depot)
    {
        var found = ContentFolders(steamRoot, build).Select(folder => Path.Combine(folder, "depot_" + depot))
            .Where(Directory.Exists).ToList();
        if (found.Count == 0) return Path.Combine(ContentFolder(steamRoot, build), "depot_" + depot);
        if (found.Count == 1) return found[0];
        var files = build.Files.Where(file => file.Depot == depot).ToList();
        return found.MaxBy(folder => PresentBytes(folder, files))!;
    }

    // Bytes of files already in folder, each counted up to its listed size.
    static long PresentBytes(string folder, IReadOnlyList<SupportedFile> files) =>
        files.Sum(file =>
        {
            var info = new FileInfo(Path.Combine(folder, file.Path));
            return info.Exists ? Math.Min(info.Length, file.Size) : 0;
        });

    public static IReadOnlyList<string> Commands(SupportedBuild build) =>
        [.. build.Depots.Select(depot => $"download_depot {build.AppId} {depot.Id} {depot.ManifestId}")];

    public static string Locate(string steamRoot, SupportedBuild build, SupportedFile file) =>
        Path.Combine(DepotFolder(steamRoot, build, file.Depot), file.Path);

    // Locate for every file, with each depot's folder chosen once.
    public static Func<SupportedFile, string> Locator(string steamRoot, SupportedBuild build)
    {
        var folders = build.Depots.ToDictionary(depot => depot.Id, depot => DepotFolder(steamRoot, build, depot.Id));
        return file => Path.Combine(folders[file.Depot], file.Path);
    }

    // Bytes of each depot's files that are present with their listed size (a quick check while Steam downloads; the copy
    // hashes every file).
    public static IReadOnlyList<DepotState> Status(string steamRoot, SupportedBuild build) =>
        [.. build.Depots.Select(depot =>
        {
            long done = 0, total = 0;
            var waiting = new List<(string, long, long)>();
            var folder = DepotFolder(steamRoot, build, depot.Id);
            foreach (var file in build.Files.Where(f => f.Depot == depot.Id))
            {
                total += file.Size;
                var info = new FileInfo(Path.Combine(folder, file.Path));
                if (info.Exists && info.Length == file.Size) done += file.Size;
                else waiting.Add((file.Path, info.Exists ? info.Length : -1, file.Size));
            }
            return new DepotState(depot, done, total, waiting);
        })];

    public static bool IsComplete(string steamRoot, SupportedBuild build) => Status(steamRoot, build).All(depot => depot.Complete);

    // Removes the build's depot folders after the game is copied (and the app folder when nothing else is in it).
    public static void Delete(string steamRoot, SupportedBuild build)
    {
        foreach (var app in ContentFolders(steamRoot, build))
        {
            foreach (var depot in build.Depots)
            {
                var folder = Path.Combine(app, "depot_" + depot.Id);
                if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
            }
            if (Directory.Exists(app) && !Directory.EnumerateFileSystemEntries(app).Any()) Directory.Delete(app);
        }
    }
}
