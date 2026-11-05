using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// Prepare for sharing (a mod's ⋮ menu; docs/modding/assets.md): the mod is built when it has sources, its game patch
// files are stripped of Arrowhead's bytes (AssetSharing.Package), and it is zipped into the installation's exports
// folder. A card says what happened: what was taken out and how players get it back, and the report. When some bytes
// matched no build on this PC, CHECK OLDER BUILDS prepares it again against every build since release, with the author's
// own Steam sign-in (a QR code on the card; nothing kept), so cut content from an older build is taken out too.
public partial class MainWindow
{
    sealed class Sharing(ModManifest mod, bool history)
    {
        public readonly ModManifest Mod = mod;
        public readonly bool History = history;
        public CancellationTokenSource? Running = new();
        public string Status = history ? "Checking the mod against the builds on this PC first..." : "Checking the mod's game files...";
        public Geometry? Qr;
        public AssetSharing.Packaged? Done;
        public string? Failure;
    }

    Sharing? sharing;

    // The mod built (when it has sources), stripped and zipped, in the background; the card follows it.
    void ShareMod(ModManifest mod, bool history = false)
    {
        if (modKitBusy || paths.InstallRoot == null) return;
        var sdk = Sdk;
        if (mod.HasSource && (sdk == null || modsLocked)) return;
        modKitBusy = true;
        var share = sharing = new Sharing(mod, history);
        Append($"Preparing {mod.Id} for sharing...", LauncherLine);
        RefreshMods();
        var install = new InstallLayout(paths.InstallRoot);
        void Status(string line) => Dispatcher.UIThread.Post(() => { share.Status = line; RefreshMods(); });
        var context = new AssetSharing.Context(install, history ? new SharePrompt(this, share) : null, Status, share.Running!.Token);
        Task.Run(() =>
        {
            try
            {
                if (mod.HasSource) ModBuilds.Ensure(install, sdk!, mod, SupportedBuild.Current.BuildId, line => Dispatcher.UIThread.Post(() => Append(line, LauncherLine)));
                var packaged = AssetSharing.Package(mod, install.Exports, context);
                Dispatcher.UIThread.Post(() =>
                {
                    share.Done = packaged;
                    Append($"{mod.Id} packed: {packaged.Zip} ({new FileInfo(packaged.Zip).Length / 1024} KB). Share that file; players drop it into their mods folder.", Green);
                    if (OperatingSystem.IsWindows()) ShowFile(packaged.Zip);
                });
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception
                or SteamSignInException or DepotAccessDeniedException or OperationCanceledException or TimeoutException)
            {
                var reason = failure is OperationCanceledException ? "Cancelled." : Privacy.Scrub(failure.Message);
                Dispatcher.UIThread.Post(() => { share.Failure = reason; Append($"{mod.Id} was not packed: {reason}", Red); });
            }
            finally
            {
                Dispatcher.UIThread.Post(() =>
                {
                    share.Running?.Dispose();
                    share.Running = null;
                    share.Qr = null;
                    modKitBusy = false;
                    RefreshMods();
                });
            }
        });
    }

    // The card of the last Prepare for sharing: what it is doing, why it failed, or what it made.
    void AddShareCard()
    {
        if (sharing is not { } share) return;
        var list = new StackPanel { Spacing = 6 };
        list.Children.Add(new TextBlock { Text = $"SHARING {share.Mod.Name.ToUpperInvariant()}", Classes = { "h2" }, Margin = new(0) });
        if (share.Running != null)
        {
            if (share.Qr != null) list.Children.Add(QrView(share.Qr));
            list.Children.Add(Muted(share.Status));
            var cancel = new Button { Content = "Cancel", Classes = { "flat" }, HorizontalAlignment = HorizontalAlignment.Left };
            cancel.Click += (_, _) => share.Running?.Cancel();
            list.Children.Add(cancel);
        }
        else if (share.Done is { } done) AddShareResult(list, share, done);
        else list.Children.Add(Warning($"Not packed: {share.Failure}"));
        if (share.Running == null)
        {
            var close = new Button { Content = "Close", Classes = { "flat" }, HorizontalAlignment = HorizontalAlignment.Left };
            close.Click += (_, _) => { sharing = null; RefreshMods(); };
            list.Children.Add(close);
        }
        ModsList.Children.Add(new Border { Classes = { "card" }, Child = list, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    // What the zip holds of the game, and the ways on: the zip, the report, and the older builds on Steam.
    void AddShareResult(StackPanel list, Sharing share, AssetSharing.Packaged done)
    {
        list.Children.Add(Muted($"Packed {Path.GetFileName(done.Zip)} ({MegaBytes(new FileInfo(done.Zip).Length)}). Share that file; players drop it into their mods folder."));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var show = new Button { Content = "Show the zip" };
        show.Click += (_, _) => ShowFile(done.Zip);
        buttons.Children.Add(show);
        if (done.Assets is { } assets)
        {
            foreach (var line in ShareLines(assets, share.History)) list.Children.Add(Muted(line));
            var report = new Button { Content = "Open the report" };
            report.Click += (_, _) => OpenFile(assets.ReportPath);
            buttons.Children.Add(report);
            if (!share.History && assets.Result.Unknown.Count > 0) buttons.Children.Add(OlderBuildsButton(share.Mod));
        }
        list.Children.Add(buttons);
    }

    // In plain words: what was taken out, from which builds, and what stays in the zip as the author's own.
    static IEnumerable<string> ShareLines(AssetSharing.Checked assets, bool history)
    {
        var result = assets.Result;
        yield return result.Outcome == Core.AssetMigration.AssetKit.Outcome.Stripped
            ? $"{MegaBytes(result.FromGame)} of Arrowhead's content is out of it: each player's HellForge rebuilds it from their own game, checked byte for byte."
            : "Its game files hold nothing of the game's builds checked: they ship as they are.";
        if (assets.PastBuilds.Count > 0) yield return "Older builds read from Steam: " + string.Join("; ", assets.PastBuilds) + ".";
        var unknown = result.Unknown.Sum(r => r.Literal);
        if (unknown == 0) yield break;
        yield return history
            ? $"{MegaBytes(unknown)} is in no build of the game since release: your own work, kept in the zip."
            : $"{MegaBytes(unknown)} matched no build of the game on this PC and stays in the zip. If some of it is cut content from an older build, " +
              "CHECK OLDER BUILDS finds it on Steam and takes it out too (sign in with the Steam app on your phone; nothing of the sign-in is kept).";
    }

    Button OlderBuildsButton(ModManifest mod)
    {
        var button = new Button { Content = "CHECK OLDER BUILDS", IsEnabled = !LinuxNetworkBlock.Requested && !modKitBusy };
        ToolTip.SetShowOnDisabled(button, true);
        ToolTip.SetTip(button, LinuxNetworkBlock.Requested
            ? "This HellForge cannot reach Steam from here; run the launcher outside the game's network block."
            : "Checks what the mod still ships against every build of the game since release, from Steam's depot manifests (hashes only), and prepares the zip again.");
        button.Click += (_, _) => ShareMod(mod, history: true);
        return button;
    }

    void ShowFile(string path)
    {
        if (OperatingSystem.IsWindows())
            try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = false }); return; }
            catch (System.ComponentModel.Win32Exception) { }
        OpenFolder(Path.GetDirectoryName(path)!);
    }

    async void OpenFile(string path)
    {
        try { if (!await this.Launcher.LaunchFileInfoAsync(new FileInfo(path))) Append("Could not open " + path, Red); }
        catch (Exception error) when (error is InvalidOperationException or IOException or System.ComponentModel.Win32Exception) { Append("Could not open " + path + ": " + error.Message, Red); }
    }

    static string MegaBytes(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F1} MB" : $"{Math.Max(1, bytes / 1000)} KB";

    // What sign-in shows, on the sharing card (from SteamKit2's threads, so through the UI thread).
    sealed class SharePrompt(MainWindow window, Sharing share) : ISignInPrompt
    {
        public void ShowQr(string challenge) => Dispatcher.UIThread.Post(() => { share.Qr = QrGeometry(challenge); window.RefreshMods(); });
        public void Status(string text) => Dispatcher.UIThread.Post(() => { share.Status = text; window.RefreshMods(); });
    }
}
