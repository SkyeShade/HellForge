using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.Core.AssetMigration;

namespace HellForge.Launcher;

// Mods that ship a recipe instead of the game's bytes (ModAssets, docs/modding/assets.md): the Mods page rebuilds their
// game patch files from this PC's game in the background as soon as it lists one that is not rebuilt yet, says so while
// it works, and refreshes once it is done. A mod that needs a build this PC does not have stays listed with the reason;
// it is tried again when its recipe changes or Refresh is clicked. Sessions do the same before they stage mods.
public partial class MainWindow
{
    Task? assetRebuild;
    int assetRebuilding;
    readonly Dictionary<string, string> assetProblems = new(StringComparer.OrdinalIgnoreCase);
    // the pending mods already tried (folder and recipe time), so a mod that cannot be rebuilt is not retried on every refresh
    readonly HashSet<string> assetTried = new(StringComparer.OrdinalIgnoreCase);

    static string AssetKey(ModManifest mod)
    {
        try { return mod.Folder + "|" + File.GetLastWriteTimeUtc(Path.Combine(mod.Folder, Recipe.FileName)).Ticks; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return mod.Folder; }
    }

    void RebuildModAssets(IReadOnlyList<ModManifest> mods)
    {
        if (paths.InstallRoot == null || assetRebuild != null) return;
        var pending = mods.Where(m => m.AssetsPending != null && !assetTried.Contains(AssetKey(m))).ToList();
        if (pending.Count == 0) return;
        foreach (var mod in pending) assetTried.Add(AssetKey(mod));
        var install = new InstallLayout(paths.InstallRoot);
        var folders = pending.Select(m => m.Folder).ToList();
        assetRebuilding = pending.Count;
        assetRebuild = Task.Run(() => ModAssets.Prepare(folders, () => GameBuilds.Local(install)));
        assetRebuild.ContinueWith(task => Dispatcher.UIThread.Post(() =>
        {
            foreach (var folder in folders) assetProblems.Remove(Path.GetFullPath(folder));
            if (task is Task<List<(string Folder, string Problem)>> { IsCompletedSuccessfully: true } done)
                foreach (var (folder, problem) in done.Result) assetProblems[folder] = problem;
            else if (task.Exception?.GetBaseException() is { } error)
                foreach (var folder in folders) assetProblems[Path.GetFullPath(folder)] = Privacy.Scrub(error.Message);
            assetRebuild = null;
            assetRebuilding = 0;
            RefreshMods();
        }), TaskScheduler.Default);
    }

    // Refresh tries every pending mod again.
    void ForgetAssetTries() => assetTried.Clear();

    // While the rebuild runs, what it is doing; after it, why a mod is still not ready.
    void AddAssetCard(IReadOnlyList<ModManifest> mods)
    {
        var pending = mods.Where(m => m.AssetsPending != null).ToList();
        if (pending.Count == 0 && assetRebuild == null) return;
        var list = new StackPanel { Spacing = 4 };
        list.Children.Add(new TextBlock { Text = "GAME FILES FOR MODS", Classes = { "h2" }, Margin = new(0) });
        if (assetRebuild != null)
            list.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" }, Text =
                $"Rebuilding the game files of {(assetRebuilding == 1 ? "a mod" : $"{assetRebuilding} mods")} from your own game. " +
                "Their authors ship a recipe instead of Arrowhead's files; HellForge makes the files here and checks each one." });
        foreach (var mod in pending)
        {
            if (assetRebuild != null && !assetProblems.ContainsKey(Path.GetFullPath(mod.Folder))) continue;
            var why = assetProblems.GetValueOrDefault(Path.GetFullPath(mod.Folder)) ?? mod.AssetsPending!;
            list.Children.Add(new TextBlock { Text = $"{mod.Name}: {why}. It does not load until then.", TextWrapping = TextWrapping.Wrap, Foreground = Red });
        }
        if (list.Children.Count > 1) ModsList.Children.Add(new Border { Classes = { "card" }, Child = list, HorizontalAlignment = HorizontalAlignment.Stretch });
    }
}
