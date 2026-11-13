using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// Mods that ship a recipe instead of the game's bytes (ModAssets, docs/modding/assets.md): the Mods page rebuilds their
// game patch files from this PC's game in the background as soon as it lists one that is not rebuilt yet, says so while
// it works, and refreshes once it is done. A mod whose recipe takes content from a build this PC does not have (cut
// content from an older build) says how much it needs, and GET THEM FROM STEAM fetches just that with the player's own
// Steam sign-in (a saved one, else a QR code for the Steam app, then kept: SteamLogins; AssetFetch). Nothing downloads without that click. A mod that
// cannot be rebuilt is tried again when its recipe changes or Refresh is clicked. Sessions rebuild from this PC too.
public partial class MainWindow
{
    Task? assetRebuild;
    int assetRebuilding;
    readonly Dictionary<string, ModAssets.Problem> assetProblems = new(StringComparer.OrdinalIgnoreCase);
    // the pending mods already tried (folder and recipe time), so a mod that cannot be rebuilt is not retried on every refresh
    readonly HashSet<string> assetTried = new(StringComparer.OrdinalIgnoreCase);
    // the fetch from Steam: its sign-in's cancel, its QR code and what it is doing (shown on the card while it runs)
    CancellationTokenSource? assetFetch;
    Geometry? assetQr;
    string assetFetchStatus = "";

    static string AssetKey(ModManifest mod)
    {
        try { return mod.Folder + "|" + File.GetLastWriteTimeUtc(System.IO.Path.Combine(mod.Folder, Recipe.FileName)).Ticks; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return mod.Folder; }
    }

    void RebuildModAssets(IReadOnlyList<ModManifest> mods)
    {
        if (paths.InstallRoot == null || assetRebuild != null || assetFetch != null) return;
        var pending = mods.Where(m => m.AssetsPending != null && !assetTried.Contains(AssetKey(m))).ToList();
        if (pending.Count == 0) return;
        foreach (var mod in pending) assetTried.Add(AssetKey(mod));
        var install = new InstallLayout(paths.InstallRoot);
        var folders = pending.Select(m => m.Folder).ToList();
        assetRebuilding = pending.Count;
        var work = Task.Run(() => ModAssets.Prepare(folders, () => GameBuilds.Local(install)));
        assetRebuild = work;
        work.ContinueWith(task => Dispatcher.UIThread.Post(() =>
        {
            foreach (var folder in folders) assetProblems.Remove(System.IO.Path.GetFullPath(folder));
            if (task.IsCompletedSuccessfully)
                foreach (var problem in task.Result) assetProblems[problem.Folder] = problem;
            else if (task.Exception?.GetBaseException() is { } error)
                foreach (var folder in folders) assetProblems[System.IO.Path.GetFullPath(folder)] = new(folder, Privacy.Scrub(error.Message));
            assetRebuild = null;
            assetRebuilding = 0;
            RefreshMods();
        }), TaskScheduler.Default);
    }

    // Refresh tries every pending mod again.
    void ForgetAssetTries() => assetTried.Clear();

    // While the rebuild runs, what it is doing; after it, why a mod is still not ready, and the way to get what it needs.
    void AddAssetCard(IReadOnlyList<ModManifest> mods)
    {
        var pending = mods.Where(m => m.AssetsPending != null).ToList();
        if (pending.Count == 0 && assetRebuild == null && assetFetch == null) return;
        var list = new StackPanel { Spacing = 6 };
        list.Children.Add(new TextBlock { Text = "GAME FILES FOR MODS", Classes = { "h2" }, Margin = new(0) });
        if (assetRebuild != null)
            list.Children.Add(Muted($"Rebuilding the game files of {(assetRebuilding == 1 ? "a mod" : $"{assetRebuilding} mods")} from your own game. " +
                "Their authors ship a recipe instead of Arrowhead's files; HellForge makes the files here and checks each one."));
        var needSteam = new List<ModManifest>();
        foreach (var mod in pending)
            if (PendingLine(mod, needSteam) is { } line) list.Children.Add(Warning(line));
        if (assetFetch != null) AddAssetFetchView(list);
        else if (needSteam.Count > 0) AddAssetFetchOffer(list, needSteam);
        if (list.Children.Count > 1) ModsList.Children.Add(new Border { Classes = { "card" }, Child = list, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    // Why a pending mod does not load (null while its rebuild may still fix it); adds it to needSteam when Steam has what
    // it needs: a recipe's parts of a build this PC lacks, or a data spec's resources of past builds (nothing to rebuild
    // there, only to fetch).
    string? PendingLine(ModManifest mod, List<ModManifest> needSteam)
    {
        if (assetProblems.TryGetValue(System.IO.Path.GetFullPath(mod.Folder), out var problem))
        {
            if (problem.Missing is { Count: > 0 }) needSteam.Add(mod);
            return $"{mod.Name}: {problem.Reason}. It does not load until then.";
        }
        if (assetRebuild != null) return null;
        if (SpecNeedsSteam(mod)) needSteam.Add(mod);
        return $"{mod.Name}: {mod.AssetsPending}.";
    }

    static bool SpecNeedsSteam(ModManifest mod)
    {
        try { return AssetFetch.SpecNeedsSteam(mod.Folder); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return false; }
    }

    static TextBlock Muted(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Classes = { "muted" } };
    static TextBlock Warning(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = Red };

    // What the mods need from builds this PC lacks, and the button that fetches it (only from outside HellForge's offline
    // network namespace, where Steam can be reached).
    void AddAssetFetchOffer(StackPanel list, List<ModManifest> mods)
    {
        var bytes = mods.Sum(m => assetProblems.GetValueOrDefault(System.IO.Path.GetFullPath(m.Folder))?.Missing?.Sum(x => x.Bytes) ?? 0);
        if (LinuxNetworkBlock.Requested)
        {
            list.Children.Add(Muted("This HellForge cannot reach Steam from here; run the launcher outside the game's network block to fetch them."));
            return;
        }
        var amount = bytes > 0 ? $"about {Math.Max(1, bytes / 1e6):F0} MB of an older build's files" : "a few resources of an older build";
        list.Children.Add(Muted($"Steam can send what {(mods.Count == 1 ? "it needs" : "they need")} ({amount}) to your " +
            "own account, under your own licence: only the pieces the recipe uses. " + char.ToUpperInvariant(SignInWords[0]) + SignInWords[1..] + "."));
        var button = new Button { Content = "GET THEM FROM STEAM", HorizontalAlignment = HorizontalAlignment.Left };
        button.Click += (_, _) => _ = FetchModAssets(mods);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { button } };
        if (SignOutButton() is { } signOut) row.Children.Add(signOut);
        list.Children.Add(row);
    }

    // The sign-in's QR code and what the fetch is doing, with a way to stop it.
    void AddAssetFetchView(StackPanel list)
    {
        if (assetQr != null) list.Children.Add(QrView(assetQr));
        list.Children.Add(Muted(assetFetchStatus));
        var cancel = new Button { Content = "Cancel", Classes = { "flat" }, HorizontalAlignment = HorizontalAlignment.Left };
        cancel.Click += (_, _) => assetFetch?.Cancel();
        list.Children.Add(cancel);
    }

    // A sign-in QR code, black on white, for the Steam app to scan.
    static Border QrView(Geometry qr) => new()
    {
        Background = Brushes.White, Padding = new(8), HorizontalAlignment = HorizontalAlignment.Left,
        Child = new Viewbox { Width = 180, Height = 180, Child = new Avalonia.Controls.Shapes.Path { Data = qr, Fill = Brushes.Black } },
    };

    async Task FetchModAssets(List<ModManifest> mods)
    {
        if (paths.InstallRoot == null || assetFetch != null) return;
        var install = new InstallLayout(paths.InstallRoot);
        assetFetch = new CancellationTokenSource();
        var cancel = assetFetch.Token;
        void Status(string text) => Dispatcher.UIThread.Post(() => { assetFetchStatus = text; DrawMods(); });
        Status("Connecting to Steam...");
        try
        {
            await using var session = await SteamDepotSession.SignInAsync(new AssetPrompt(this), cancel, SteamLogins.Default);
            assetQr = null;
            var steam = SteamBuilds.For(session, install, cancel);
            foreach (var mod in mods)
            {
                Status($"Fetching what {mod.Name} needs from Steam...");
                await Task.Run(() =>
                {
                    if (AssetFetch.SpecNeedsSteam(mod.Folder)) AssetFetch.FetchSpec(mod.Folder, steam, Status);
                    if (ModAssets.HasRecipe(mod.Folder) && !ModAssets.Read(mod.Folder).Ready) AssetFetch.Rebuild(mod.Folder, install, steam, Status);
                }, cancel);
                assetProblems.Remove(System.IO.Path.GetFullPath(mod.Folder));
                Status($"{mod.Name}: ready; {steam.Store.Downloaded / 1e6:F1} MB downloaded so far.");
            }
        }
        catch (Exception error) when (error is SteamSignInException or DepotAccessDeniedException or InvalidDataException or IOException or OperationCanceledException or TimeoutException)
        {
            var reason = error is OperationCanceledException ? "Cancelled." : Privacy.Scrub(error.Message);
            foreach (var mod in mods)
                if (assetProblems.TryGetValue(System.IO.Path.GetFullPath(mod.Folder), out var problem))
                    assetProblems[problem.Folder] = problem with { Reason = $"{problem.Reason} (Steam: {reason})" };
        }
        finally
        {
            assetFetch.Dispose();
            assetFetch = null;
            assetQr = null;
            assetFetchStatus = "";
            ForgetAssetTries();
            RefreshMods();
        }
    }

    // What sign-in shows, on the card (from SteamKit2's threads, so through the UI thread).
    sealed class AssetPrompt(MainWindow window) : ISignInPrompt
    {
        public void ShowQr(string challenge) => Dispatcher.UIThread.Post(() => { window.assetQr = QrGeometry(challenge); window.DrawMods(); });
        public void Status(string text) => Dispatcher.UIThread.Post(() => { window.assetFetchStatus = text; window.DrawMods(); });
    }
}
