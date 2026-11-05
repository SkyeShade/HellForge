using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.Core.AssetMigration;

namespace HellForge.Launcher;

// Mod sync from your own game (docs/multiplayer-mod-sync.md "Mod sync from your own game"): a host's mods' game patch files are
// written from this PC's game where it has the same bytes, and only the rest is downloaded. Resources under their own
// names are found without help; the game index (AssetKit.Index, the asset kit's fingerprints of each build: hashes,
// never game content) finds the game's bytes a mod renamed too. Mod sync never makes it, since it reads the whole game
// and takes gigabytes of memory while the game runs, so it is made here, on request, with no session running.
public partial class MainWindow
{
    // The builds of the game on this PC and whether their index is made: worked out once (and after indexing, or Refresh).
    (List<LocalBuild> Builds, bool Indexed)? syncIndex;
    Task? syncIndexing;
    string syncIndexStatus = "";

    void ForgetSyncIndex() => syncIndex = null;

    (List<LocalBuild> Builds, bool Indexed) SyncIndexState(InstallLayout install)
    {
        if (syncIndex is { } known) return known;
        try
        {
            var builds = GameBuilds.Local(install);
            return (syncIndex = (builds, AssetKit.HasIndex(builds, AssetKit.CacheFolder(install)))).Value;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            return (syncIndex = ([], false)).Value;
        }
    }

    // The MOD SYNC card's row under "When I join": what downloads from hosts take from this PC's game, and the button that
    // indexes it.
    void AddSyncIndexRow(StackPanel panel)
    {
        if (paths.InstallRoot == null) return;
        var install = new InstallLayout(paths.InstallRoot);
        var (builds, indexed) = SyncIndexState(install);
        panel.Children.Add(new TextBlock { Text = "Take the game's files from your own game", TextWrapping = TextWrapping.Wrap });
        string text;
        if (builds.Count == 0) text = "No copy of the game is on this PC to take them from: a host's mods download whole.";
        else if (indexed) text = $"Your game is indexed ({string.Join(", ", builds.Select(b => b.What))}): the parts of a host's mods that are the game's " +
            "own files, under any name, are written from your game, and only the rest downloads.";
        else text = "The parts of a host's mods that are the game's own files under their own names are written from your game, and only the rest downloads. " +
            "Indexing your game once finds the ones a mod renamed too: about a minute per game build and a few GB of memory, while no session runs. " +
            "The index is hashes only, kept in the installation's temp folder.";
        panel.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Classes = { "muted" } });
        if (syncIndexStatus.Length > 0) panel.Children.Add(new TextBlock { Text = syncIndexStatus, TextWrapping = TextWrapping.Wrap, FontSize = 12, Foreground = Light });
        if (builds.Count == 0 || indexed) return;
        var button = new Button
        {
            Content = syncIndexing != null ? "Indexing..." : "Index my game",
            IsEnabled = syncIndexing == null && !modsLocked,
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        ToolTip.SetShowOnDisabled(button, true);
        ToolTip.SetTip(button, modsLocked ? "Waits until the session ends: indexing reads the whole game and takes a few GB of memory."
            : "Fingerprints every resource of " + string.Join(" and ", builds.Select(b => b.What)) + ", once per game build.");
        button.Click += (_, _) => IndexGame(install, builds);
        panel.Children.Add(button);
    }

    void IndexGame(InstallLayout install, List<LocalBuild> builds)
    {
        if (syncIndexing != null || modsLocked) return;
        void Status(string line) => Dispatcher.UIThread.Post(() => { syncIndexStatus = line; RefreshMods(); });
        Status("Starting...");
        syncIndexing = Task.Run(() =>
        {
            try
            {
                AssetKit.Index(builds, AssetKit.CacheFolder(install), Status);
                Status("Indexed. Downloads from hosts now take every part your game has.");
            }
            catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or OutOfMemoryException)
            {
                Status("Not indexed: " + error.Message);
            }
            finally
            {
                Dispatcher.UIThread.Post(() =>
                {
                    syncIndexing = null;
                    ForgetSyncIndex();
                    RefreshMods();
                });
            }
        });
    }
}
