using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The Mods page's reading of the disk, off the UI thread: the mods folder (dropped archives, the catalogue, the modlist
// code, which hashes every file of each shared gameplay mod), its settings and downloaded mods, each source mod's build
// state (its sources and the SDK's headers hashed), bundled mods, premium ownership and the mods' pictures. With many
// mods that took seconds on the UI thread at every click on the page. RefreshMods reads it all in the background and the
// page keeps what it shows until DrawMods draws the new state, which reads nothing.
public partial class MainWindow
{
    // What a load needs from the window, read on the UI thread.
    sealed record ModsPageRequest(string ModsFolder, string? InstallRoot, string ProgressionFile, string? RunDirectory, string Profile);

    // What the page shows. Error: the mods folder could not be read (the rest is then empty). Fingerprint: all of it that
    // the page draws, so a load that finds the same need not draw the page again.
    sealed record ModsPageData(List<ModManifest> Mods, List<string> Problems, string? Error, string Compared, ModSettings.State Settings,
        int Downloaded, long DownloadedBytes, Dictionary<string, string> Statuses, DateTime StatusesWritten, ModSdk? Sdk, bool ToolsReady,
        bool HasSourceMods, HashSet<string> Bundled, Dictionary<string, (string Text, IBrush Colour)?> Builds, PremiumContent? Premium,
        bool PremiumAtSession, Dictionary<string, Bitmap?> Icons, string Fingerprint = "");

    ModsPageData? modsData;
    bool modsLoading, modsLoadAgain, modsLoadAgainDraws, modsDrawAfterDrag;
    string? modsDrawn; // the fingerprint and page state of what is drawn (DrawnKey)

    // The page loaded again: read in the background, then drawn. A load asked for while one runs follows it once. With
    // onlyIfChanged (showing the page) the page is drawn again only when the load finds something new: clicking the tab
    // shows the page as it was at once, and 150 mods were a quarter of a second to draw.
    void RefreshMods(bool onlyIfChanged = false)
    {
        if (paths.ModsFolder == null) { modsData = null; DrawMods(); return; }
        if (modsLoading) { modsLoadAgain = true; modsLoadAgainDraws |= !onlyIfChanged; return; }
        modsLoading = true;
        var request = new ModsPageRequest(paths.ModsFolder, paths.InstallRoot, paths.ProgressionFile, runDirectory, Selected.Name);
        if (modsData == null) DrawMods(); // the first time: says it is loading
        Task.Run(() => LoadModsPage(request)).ContinueWith(task => Dispatcher.UIThread.Post(() =>
        {
            modsLoading = false;
            // something changed while it read (a switch, a new mod): the next load is drawn, not this one
            if (modsLoadAgain && modsData != null)
            {
                var draws = modsLoadAgainDraws || !onlyIfChanged;
                (modsLoadAgain, modsLoadAgainDraws) = (false, false);
                RefreshMods(!draws);
                return;
            }
            modsData = task.IsCompletedSuccessfully ? task.Result
                : EmptyModsPage("Could not read the mods folder: " + Privacy.Scrub(task.Exception?.GetBaseException().Message ?? "unknown error"));
            if (!onlyIfChanged || modsDrawn != DrawnKey())
            {
                if (modDrag == null) DrawMods();
                else modsDrawAfterDrag = true; // a row being dragged: drawn once it is let go
            }
            if (modsLoadAgain) { (modsLoadAgain, modsLoadAgainDraws) = (false, false); RefreshMods(); }
        }), TaskScheduler.Default);
    }

    // What the drawn page depends on: the load, and the page's own state that a load does not see.
    string DrawnKey() => $"{modsData?.Fingerprint}|{modsLocked}|{LiveSwitches}|{runner.IsRunning}|{modsRefused.Count}";

    // A load that ended during a drag, drawn after it.
    void DrawAfterDrag()
    {
        if (!modsDrawAfterDrag) return;
        modsDrawAfterDrag = false;
        Dispatcher.UIThread.Post(DrawMods, DispatcherPriority.Background);
    }

    static ModsPageData EmptyModsPage(string error) =>
        new([], [], error, "", new ModSettings.State([], [], []), 0, 0, [], default, null, false, false, [], [], null, false, []);

    // Everything the page reads from disk, on a worker thread.
    ModsPageData LoadModsPage(ModsPageRequest request)
    {
        // a .rar or .7z dropped into the mods folder is added as a folder first, as Add mods... adds one; then mod files
        // written over through a staged link since (ModStaging.Notices): reinstalling the mod clears its line
        var dropped = new List<string>();
        if (request.InstallRoot != null)
        {
            var layout = new InstallLayout(request.InstallRoot);
            dropped.AddRange(ModInstaller.TakeDropped(layout));
            dropped.AddRange(ModStaging.Notices(layout));
        }
        List<ModManifest> mods;
        List<string> problems;
        try { (mods, problems) = ModCatalog.Load(request.ModsFolder); problems.InsertRange(0, dropped); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return EmptyModsPage("Could not read the mods folder: " + Privacy.Scrub(error.Message));
        }
        // Local Stats is the in-game editor's (its saved changes, always on, never compared): not listed, so nobody thinks
        // friends must match it
        mods = mods.Where(m => m.Id != ModCatalog.LocalStatsId).ToList();
        var sdk = request.InstallRoot != null ? ModSdk.Find(request.InstallRoot) : null;
        var toolsReady = sdk != null && ModKit.ToolsReady(sdk);
        var (downloaded, bytes) = DownloadedMods(request.ModsFolder);
        var (premium, atSession) = mods.Any(m => m.Premium.Count > 0) ? PremiumOwnership(request) : (null, false);
        var data = new ModsPageData(mods, problems, null, ComparedText(ModCatalog.Active(mods)), ModSettings.Read(request.ModsFolder), downloaded, bytes,
            request.RunDirectory != null ? ModStatus.Read(request.RunDirectory) ?? [] : [],
            request.RunDirectory != null ? ModStatus.WrittenUtc(request.RunDirectory) : default, sdk, toolsReady,
            HasSourceMods(request.ModsFolder), [.. mods.Where(m => BundledMods.ShippedFiles(m.Folder) != null).Select(m => m.Id)],
            mods.Where(m => m.HasSource).ToDictionary(m => m.Id, m => BuildStateOf(m, request.InstallRoot, sdk, toolsReady)),
            premium, atSession, Icons(mods));
        return data with { Fingerprint = Fingerprint(data, request.ModsFolder) };
    }

    // All of a load the page draws, as text: the mods (each one's manifest and picture by their time on disk, so an edit
    // shows), the settings file, the notices, the modlist code, downloads, statuses, builds and premium ownership.
    static string Fingerprint(ModsPageData data, string modsFolder)
    {
        static string Written(string? path) => path != null && File.Exists(path) ? File.GetLastWriteTimeUtc(path).Ticks.ToString() : "-";
        var text = new System.Text.StringBuilder();
        try { text.Append(File.ReadAllText(Path.Combine(modsFolder, ModSettings.FileName))); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { text.Append('-'); }
        foreach (var mod in data.Mods)
            text.Append('\n').Append(mod.Id).Append('|').Append(mod.Enabled).Append('|').Append(mod.Version).Append('|').Append(mod.Folder)
                .Append('|').Append(Written(ModCatalog.ManifestIn(mod.Folder))).Append('|').Append(mod.Icon).Append('|').Append(Written(mod.Icon))
                .Append('|').Append(data.Builds.GetValueOrDefault(mod.Id)?.Text).Append('|').Append(data.Bundled.Contains(mod.Id))
                .Append('|').Append(mod.Premium.Count > 0 && data.Premium != null ? string.Join(",", data.Premium.Missing(mod.Premium)) : "");
        text.Append('\n').AppendJoin('\n', data.Problems).Append('\n').Append(data.Compared).Append('|').Append(data.Downloaded)
            .Append('|').Append(data.DownloadedBytes).Append('|').Append(data.HasSourceMods).Append('|').Append(data.ToolsReady)
            .Append('|').Append(data.PremiumAtSession).Append('\n').AppendJoin(',', data.Statuses.OrderBy(s => s.Key, StringComparer.Ordinal).Select(s => s.Key + "=" + s.Value));
        return text.ToString();
    }

    // What friends compare (ModCatalog.Digest).
    static string ComparedText(List<ModManifest> active)
    {
        var gameplay = active.Count(m => m.Gameplay && ModCatalog.Tier(m) == CoopTier.Shared);
        try
        {
            return gameplay == 0 ? "No shared gameplay mods to compare: you can play with friends who have none."
                : $"Modlist code {ModCatalog.Digest(ModCatalog.LoadOrder(active))[..8]}: friends need the same {gameplay} gameplay mod{(gameplay == 1 ? "" : "s")}; the order is up to you.";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // a file in a mod's folder that another program holds (issue 18): the list still shows
            return "Modlist code unavailable: a mod's file could not be read (" + Privacy.Scrub(error.Message) + "). Close the program using it, then Refresh.";
        }
    }

    // The mods downloaded from hosts (SyncedStore): how many, and their size.
    static (int Count, long Bytes) DownloadedMods(string modsFolder)
    {
        var store = SyncedStore.Root(modsFolder);
        var stored = Directory.Exists(store) ? Directory.GetDirectories(store).Where(d => !Path.GetFileName(d).StartsWith('.')).ToList() : [];
        long bytes = 0;
        try { bytes = stored.Sum(d => Directory.GetFiles(d, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return (stored.Count, bytes);
    }

    // Folder mods with sources (ModKitPage's SourceMods).
    static bool HasSourceMods(string modsFolder) => Directory.Exists(modsFolder) &&
        Directory.EnumerateDirectories(modsFolder).Any(f => !Path.GetFileName(f).StartsWith('.') &&
            File.Exists(Path.Combine(f, ModCatalog.ManifestName)) && Directory.Exists(Path.Combine(f, "src")));

    // What of the mods' premium lists the selected profile's synced account owns (HellForge.Core PremiumContent). On Linux
    // no fresh account is not "locked": the session's ownership check may supply one, so the page says it will check then.
    static (PremiumContent? Ownership, bool AtSession) PremiumOwnership(ModsPageRequest request)
    {
        var account = SteamAccount.Current();
        var atSession = OperatingSystem.IsLinux() && account == null;
        var ownership = File.Exists(request.ProgressionFile) && request.InstallRoot != null
            ? PremiumContent.ForSnapshot(request.ProgressionFile, InventorySync.SnapshotDirectory(request.InstallRoot, request.Profile), account)
            : PremiumContent.Nothing;
        return (ownership, atSession);
    }

    // Each mod's picture, decoded small (Picture keeps them while the files are unchanged).
    Dictionary<string, Bitmap?> Icons(List<ModManifest> mods)
    {
        var icons = new Dictionary<string, Bitmap?>(StringComparer.Ordinal);
        foreach (var icon in mods.Select(m => m.Icon).OfType<string>().Distinct(StringComparer.Ordinal)) icons[icon] = Picture(icon, 80);
        return icons;
    }
}
