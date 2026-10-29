using System.Globalization;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// Profile > Account sync (HellForge.Core InventorySync): runs the platform's capture (elevated on Windows, native and
// below the installed helper on Linux), opens the normal game through Steam once the proxy listens, and saves the
// player's sealed inventory and progression as the profile's snapshot as soon as the proxy has sealed them. The proxy
// keeps the game connected until it exits. The backend takes premium ownership from the snapshot on every Play; the rest
// is imported as the player chooses (AccountImportFlow). Polled with the rest of the window, once a second, only while a
// sync runs.
public partial class MainWindow
{
    InventorySync.CaptureRun? syncProxy;
    LocalProfile? syncProfile;
    string? syncDir, syncSnapshotDir, syncWorkingFolder;
    bool syncGameOpened, syncStarting, syncTaken, syncClosing;
    DateTime syncStartedUtc;

    static bool NativeLinux => OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine;

    // Busy until the capture is taken; after that the proxy only keeps the game connected, and imports may run.
    bool SyncBusy => syncProxy != null && !syncTaken || syncStarting;

    void InitializeSync()
    {
        SyncCard.IsVisible = InventorySync.FindCaptureProxy(paths.InstallRoot) != null;
        if (NativeLinux)
            SyncDescription.Text = "Brings your own HELLDIVERS 2 account into this profile. Warbonds, Superstore items and DLC gear you bought always follow your latest sync; everything else is imported only as you choose, and the profile is backed up first. Super Credits are never imported or usable. Sync opens the normal game through Steam; log in and pass the title screen. On Linux the first sync needs the capture helper installed once (docs/linux.md); until the normal game exits, two marked /etc/hosts lines point its server name at this PC and a one-day certificate is trusted only in HELLDIVERS 2's Proton prefix. Both are removed when it exits.";
        SyncButton.Click += async (_, _) => await StartSync();
        InitializeAccountImport();
        UpdateSyncStatus();
        var root = paths.InstallRoot;
        if (NativeLinux && root != null && !InventorySync.IsGameRunning() && InventorySync.HasPendingLinuxCleanup(root))
            _ = CleanupLinuxLeftoversAsync(root);
    }

    // A killed sync leaves the helper's hosts lines and its recorded prefix certificate. The next launcher start removes
    // them without a prompt; the sync card is still available if this fails.
    // The startup cleanup holds the same busy flag as a sync, so a Sync pressed while it runs waits instead of having its
    // helper or hosts lines removed by the cleanup.
    async Task CleanupLinuxLeftoversAsync(string installRoot)
    {
        syncStarting = true;
        SyncButton.IsEnabled = false;
        try
        {
            string? problem;
            try { problem = await Task.Run(() => InventorySync.CleanupLinux(installRoot)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException
                or System.ComponentModel.Win32Exception)
            {
                problem = error.Message;
            }
            if (problem != null) Append("Inventory sync: leftover Linux cleanup failed: " + problem, Orange);
        }
        finally
        {
            syncStarting = false;
            SyncButton.IsEnabled = true;
            UpdateSyncStatus();
        }
    }

    void UpdateSyncStatus()
    {
        if (paths.InstallRoot == null || !SyncCard.IsVisible) return;
        var info = InventorySync.ReadMetadata(InventorySync.SnapshotDirectory(paths.InstallRoot, Selected.Name));
        SyncDetailText.Text = info == null ? "No snapshot yet. Sync to copy your real inventory into this profile."
            : !info.Progression ? "Last sync failed " + (info.CapturedUtc?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) ?? "at an unknown time") +
                $": {info.Items:N0} items, no progression. Sync again to get your level, unlocks and ship."
            : "Last synced " + (info.CapturedUtc?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) ?? "at an unknown time") +
              $": {info.Items:N0} items, " + (info.Experience is { } xp ? $"{xp:N0} XP." : "progression captured.");
        UpdateImportControls();
        // While this launcher's capture still keeps the game connected, its hosts line is its own, not an earlier sync's leftover.
        if (SyncBusy || syncProxy != null) return;
        var leftover = InventorySync.HasLeftoverRedirect();
        SyncButton.Content = leftover ? "Clean up" : "Sync Inventory";
        if (leftover) SetSyncStatus("An earlier sync did not finish: the normal game cannot reach its servers until you clean up.", Orange);
    }

    void SetSyncStatus(string text, IBrush brush)
    {
        SyncStatusText.Text = text;
        SyncStatusText.Foreground = brush;
    }

    async Task StartSync()
    {
        if (SyncBusy || syncClosing || paths.InstallRoot == null || InventorySync.FindCaptureProxy(paths.InstallRoot) is not { } proxy) return;
        if (syncProxy != null)
        {
            SetSyncStatus("The last sync still keeps HELLDIVERS 2 connected: close the game first.", Orange);
            return;
        }
        if (InventorySync.HasLeftoverRedirect())
        {
            await CleanUpSync(proxy);
            return;
        }
        if (SyncRefusal() is { } refusal)
        {
            SetSyncStatus(refusal, Orange);
            return;
        }
        var install = new InstallLayout(paths.InstallRoot);
        syncStarting = true;
        SyncButton.IsEnabled = false;
        SetSyncStatus("Checking the signed-in Steam account...", Gold);
        ulong? signedIn;
        try { signedIn = await Task.Run(() => SteamAccount.Current(install)); }
        catch (Exception error) when (error is InvalidOperationException or IOException or UnauthorizedAccessException
            or System.ComponentModel.Win32Exception)
        {
            syncStarting = false;
            SyncButton.IsEnabled = true;
            SetSyncStatus("The signed-in Steam account could not be read: " + error.Message, Orange);
            return;
        }
        if (signedIn is not { } account)
        {
            syncStarting = false;
            SyncButton.IsEnabled = true;
            SetSyncStatus("Sign in to Steam first: the sync is sealed for your Steam account.", Orange);
            return;
        }
        var profile = Selected;
        var snapshot = InventorySync.SnapshotDirectory(install.Root, profile.Name);
        var folder = Path.Combine(install.Logs, "sync-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture));
        syncWorkingFolder = folder;
        SetSyncStatus(SyncStartingText(), Gold);
        InventorySync.CaptureRun? started = null;
        string? failure = null;
        try { started = await Task.Run(() => InventorySync.StartCaptureProxy(install.Root, proxy, folder, snapshot, account, runner.IsRunning)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            failure = error.Message;
        }
        syncStarting = false;
        if (syncClosing)
        {
            syncWorkingFolder = null;
            started?.Dispose();
            return;
        }
        if (started == null)
        {
            syncWorkingFolder = null;
            SyncNotStarted(folder, failure);
            return;
        }
        syncWorkingFolder = null;
        (syncProxy, syncProfile, syncDir, syncSnapshotDir, syncGameOpened, syncTaken) = (started, profile, folder, snapshot, false, false);
        syncStartedUtc = DateTime.UtcNow;
        InventorySync.Note(folder, $"sync started for profile {profile.Name}; capture pid {started.Id}; launcher {Environment.ProcessPath}");
        SetSyncStatus("Starting the capture...", Gold);
        Append("Inventory sync: capture started for profile " + profile.Name + ". HELLDIVERS 2 opens through Steam; take it past the title screen.", LauncherLine);
    }

    static string SyncStartingText() => NativeLinux
        ? "Starting the native capture and the Linux helper..."
        : "Waiting for the administrator prompt...";

    void SyncNotStarted(string folder, string? failure)
    {
        var declined = NativeLinux ? "the capture did not start" : "the administrator prompt was declined";
        InventorySync.Note(folder, "capture did not start: " + (failure ?? declined));
        SyncButton.IsEnabled = true;
        SetSyncStatus(failure ?? (NativeLinux ? "Sync cancelled before the capture started." : "Sync cancelled: it needs the administrator prompt."),
            failure != null ? Red : Orange);
    }

    // Why a sync cannot start now; its Steam account is asked for separately, off the UI thread when the probe is needed.
    string? SyncRefusal()
    {
        if (runner.IsRunning) return "Stop the HellForge session first.";
        if (InventorySync.IsGameRunning()) return "Close HELLDIVERS 2 first: the sync reads its next login.";
        return null;
    }

    void RefreshSync()
    {
        if (syncProxy == null || syncProfile == null || syncDir == null || syncSnapshotDir == null) return;
        if (!syncProxy.HasExited)
        {
            if (!syncGameOpened && InventorySync.IsListening(syncDir)) OpenGameForSync();
            else if (!syncTaken && InventorySync.IsSealed(syncDir)) TakeCapture(syncProfile, syncDir, syncSnapshotDir);
            return;
        }
        var exitCode = syncProxy.ExitCode;
        syncProxy.Dispose();
        syncProxy = null;
        SyncButton.IsEnabled = true;
        var (profile, folder, snapshot) = (syncProfile, syncDir, syncSnapshotDir);
        InventorySync.Note(folder, $"capture exited with code {exitCode} (game opened {(syncGameOpened ? "yes" : "no")}, capture taken {(syncTaken ? "yes" : "no")})");
        foreach (var line in InventorySync.CollectGameLogs(folder, syncStartedUtc)) InventorySync.Note(folder, line);
        if (syncTaken) ProxyClosed(exitCode, folder);
        else if (exitCode == 0) TakeCapture(profile, folder, snapshot);
        else SyncFailed(syncGameOpened ? $"Nothing was captured within {InventorySync.TimeoutMinutes} minutes. Close the game and sync again."
            : "The capture could not start: see " + Path.Combine(folder, "proxy.log") + ".");
        (syncProfile, syncDir, syncSnapshotDir) = (null, null, null);
        syncTaken = false;
        UpdateSyncStatus();
    }

    // The sealed capture becomes the profile's snapshot, while the proxy may still keep the game connected.
    void TakeCapture(LocalProfile profile, string folder, string snapshot)
    {
        syncTaken = true;
        if (InventorySync.FinalizeCapture(folder, snapshot) is { } error)
        {
            InventorySync.Note(folder, "capture not taken: " + error);
            SyncFailed(error);
            return;
        }
        UpdateSyncStatus();
        var info = InventorySync.ReadMetadata(snapshot);
        var partial = info is not { Progression: true };
        InventorySync.Note(folder, $"capture taken into {snapshot}: {info?.Items ?? 0} items, progression {(partial ? "no" : "yes")}, " +
            $"capture {(syncProxy is { HasExited: false } ? "still forwarding the game" : "exited")}");
        if (partial)
        {
            // No progression is a failed sync, not a smaller success: the inventory alone still gives the backend premium
            // ownership, but nothing is imported from half an account.
            SyncFailed("The capture got your inventory but not your progression (see " + Path.Combine(folder, "proxy.log") + "). " +
                "Nothing was imported; sync again.");
            if (syncProxy is { HasExited: false })
                Append("Inventory sync: the game stays connected through the capture until you close it; then the hosts line and certificate are removed.", LauncherLine);
            return;
        }
        Append("Inventory sync complete for profile " + profile.Name + ".", LauncherLine);
        if (syncProxy is { HasExited: false })
            Append("Inventory sync: the game stays connected through the capture until you close it; then the hosts line and certificate are removed.", LauncherLine);
        _ = AfterCaptureAsync(profile);
    }

    void SyncFailed(string error)
    {
        if (syncDir != null) InventorySync.Note(syncDir, "sync failed: " + error);
        SetSyncStatus(error, Red);
        Append("Inventory sync failed: " + error, Red);
    }

    static string ProxyClosedLine(int exitCode, string folder) => exitCode == 0
        ? "Inventory sync: the game closed, and the capture removed its hosts line and certificate."
        : "Inventory sync: the capture ended with an error; see " + Path.Combine(folder, "proxy.log") + ".";

    void ProxyClosed(int exitCode, string folder) => Append(ProxyClosedLine(exitCode, folder), exitCode == 0 ? LauncherLine : Orange);

    void OpenGameForSync()
    {
        syncGameOpened = true;
        try
        {
            InventorySync.OpenVanillaGame();
            if (syncDir != null) InventorySync.Note(syncDir, "capture listening; asked Steam to open the game (steam://rungameid/553850)");
            SetSyncStatus("Log in to HELLDIVERS 2 and pass the title screen. This updates when the capture is done.", Gold);
        }
        catch (System.ComponentModel.Win32Exception error)
        {
            if (syncDir != null) InventorySync.Note(syncDir, "Steam did not open the game: " + error.Message);
            SetSyncStatus("Steam did not open the game: start HELLDIVERS 2 from Steam and pass the title screen.", Orange);
        }
    }

    // Closing the window while a Linux capture runs must not leave the hosts redirect, the root relay and the prefix CA
    // behind. The helper command and reg.exe are bounded; if they do not finish in time the helper's own pid watch and
    // lifetime cap still end the relay, and the next launcher start runs the same cleanup again.
    async Task StopSyncForCloseAsync()
    {
        if (syncClosing) return;
        syncClosing = true;
        syncStarting = true;
        poll.Stop();
        var folder = syncDir ?? syncWorkingFolder;
        var run = syncProxy;
        syncProxy = null;
        SetSyncStatus("Stopping the Linux capture...", Gold);
        if (folder != null)
        {
            var syncFolder = folder;
            try
            {
                var stop = Task.Run(() =>
                {
                    var problem = LinuxCapture.Finish(syncFolder);
                    if (problem != null) InventorySync.Note(syncFolder, "cleanup before closing: " + problem);
                    run?.Dispose();
                });
                await Task.WhenAny(stop, Task.Delay(TimeSpan.FromSeconds(30)));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException
                or System.ComponentModel.Win32Exception)
            {
                if (folder != null) InventorySync.Note(folder, "cleanup before closing failed: " + error.Message);
            }
        }
        else run?.Dispose();
        syncStarting = false;
        closeConfirmed = true;
        Close();
    }

    async Task CleanUpSync(string proxy)
    {
        syncStarting = true;
        SyncButton.IsEnabled = false;
        string? failure = null;
        bool declined = false;
        if (NativeLinux)
        {
            SetSyncStatus("Cleaning up the Linux capture...", Gold);
            var root = paths.InstallRoot;
            failure = root == null ? "The installation folder is missing." : await Task.Run(() => InventorySync.CleanupLinux(root));
        }
        else
        {
            SetSyncStatus("Waiting for the administrator prompt...", Gold);
            using var cleanup = await Task.Run(() => InventorySync.StartCleanup(proxy));
            if (cleanup != null) await cleanup.WaitForExitAsync();
            else declined = true;
        }
        syncStarting = false;
        SyncButton.IsEnabled = true;
        UpdateSyncStatus();
        if (declined) SetSyncStatus("Clean up needs the administrator prompt.", Orange);
        else if (failure != null) SetSyncStatus("Clean up failed: " + failure, Red);
        else if (InventorySync.HasLeftoverRedirect()) SetSyncStatus("Clean up failed: remove the HellForge-CaptureProxy lines from the hosts file.", Red);
        else SetSyncStatus("Cleaned up: the normal game reaches its servers again.", Green);
    }
}
