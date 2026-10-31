using System.Diagnostics;
using System.Globalization;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// Profile > Account sync (HellForge.Core InventorySync): runs the elevated capture proxy, opens the normal game through Steam
// once the proxy listens, and saves the player's sealed inventory and progression as the profile's snapshot as soon as the
// proxy has sealed them. The proxy keeps the game connected until it exits. The backend takes premium ownership from the
// snapshot on every Play; the rest is imported as the player chooses (AccountImportFlow). Polled with the rest of the
// window, once a second, only while a sync runs.
public partial class MainWindow
{
    Process? syncProxy;
    LocalProfile? syncProfile;
    string? syncDir, syncSnapshotDir;
    bool syncGameOpened, syncStarting, syncTaken, syncSlowNoted;
    DateTime syncStartedUtc;
    // A capture that has not listened by then is reported (CaptureProblem.NotReady), and still waited for: its own steps
    // before listening take well under half of this.
    static readonly TimeSpan SyncReadyWarning = TimeSpan.FromSeconds(60);

    // Busy until the capture is taken; after that the proxy only keeps the game connected, and imports may run.
    bool SyncBusy => syncProxy != null && !syncTaken || syncStarting;

    void InitializeSync()
    {
        SyncCard.IsVisible = InventorySync.FindCaptureProxy(paths.InstallRoot) != null;
        SyncButton.Click += async (_, _) => await StartSync();
        InitializeAccountImport();
        UpdateSyncStatus();
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
        if (SyncBusy || paths.InstallRoot == null) return;
        if (InventorySync.FindCaptureProxy(paths.InstallRoot) is not { } proxy)
        {
            // There when the launcher started, gone now: a security program's quarantine.
            await ReportStartFailure(null, new CaptureFailure(CaptureProblem.Missing, null), ExpectedProxy(paths.InstallRoot));
            return;
        }
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
        if (SyncRefusal(out var account) is { } refusal)
        {
            SetSyncStatus(refusal, Orange);
            return;
        }
        var install = new InstallLayout(paths.InstallRoot);
        var profile = Selected;
        var snapshot = InventorySync.SnapshotDirectory(install.Root, profile.Name);
        var folder = Path.Combine(install.Logs, "sync-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture));
        syncStarting = true;
        SyncButton.IsEnabled = false;
        SetSyncStatus("Waiting for the administrator prompt...", Gold);
        (Process? Process, CaptureProblem Refused, string? Detail) launch = (null, CaptureProblem.None, null);
        string? failure = null;
        try { launch = await Task.Run(() => InventorySync.StartCaptureProxy(proxy, folder, snapshot, account)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { failure = error.Message; }
        syncStarting = false;
        if (launch.Process is not { } started)
        {
            SyncButton.IsEnabled = true;
            if (failure != null)
            {
                InventorySync.Note(folder, "capture did not start: " + failure);
                SetSyncStatus(failure, Red);
            }
            else await ReportStartFailure(folder, new CaptureFailure(launch.Refused, launch.Detail), proxy);
            return;
        }
        (syncProxy, syncProfile, syncDir, syncSnapshotDir, syncGameOpened, syncTaken, syncSlowNoted) =
            (started, profile, folder, snapshot, false, false, false);
        syncStartedUtc = DateTime.UtcNow;
        InventorySync.Note(folder, $"sync started for profile {profile.Name}; capture pid {started.Id}; launcher {Environment.ProcessPath}");
        SetSyncStatus("Starting the capture...", Gold);
        Append("Inventory sync: capture started for profile " + profile.Name + ". HELLDIVERS 2 opens through Steam; take it past the title screen.", LauncherLine);
    }

    // Why a sync cannot start now; null with the signed-in Steam account it will be sealed for.
    string? SyncRefusal(out ulong account)
    {
        account = 0;
        if (runner.IsRunning) return "Stop the HellForge session first.";
        if (InventorySync.IsGameRunning()) return "Close HELLDIVERS 2 first: the sync reads its next login.";
        if (SteamAccount.Current() is not { } signedIn) return "Sign in to Steam first: the sync is sealed for your Steam account.";
        account = signedIn;
        return null;
    }

    void RefreshSync()
    {
        if (syncProxy == null || syncProfile == null || syncDir == null || syncSnapshotDir == null) return;
        if (!syncProxy.HasExited)
        {
            if (!syncGameOpened && InventorySync.IsListening(syncDir)) OpenGameForSync();
            else if (!syncGameOpened && !syncSlowNoted && DateTime.UtcNow - syncStartedUtc > SyncReadyWarning && paths.InstallRoot != null)
            {
                syncSlowNoted = true;
                _ = ReportStartFailure(syncDir, new CaptureFailure(CaptureProblem.NotReady, null), ExpectedProxy(paths.InstallRoot));
            }
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
        // A hosts write the capture could not make (CaptureHosts) names its own cause. Before listening it is also the
        // capture's start failure (capture-failure.json holds the same reason), shown once, with the security program to allow.
        else if (syncGameOpened)
            SyncFailed(CaptureHosts.ReadFailure(folder) ?? $"Nothing was captured within {InventorySync.TimeoutMinutes} minutes. Close the game and sync again.");
        else if (paths.InstallRoot != null)
            _ = ReportStartFailure(folder, CaptureStart.FromExit(exitCode, CaptureStart.Read(folder), ReadProxyLog(folder), CaptureHosts.ReadFailure(folder)),
                ExpectedProxy(paths.InstallRoot));
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

    static string ExpectedProxy(string installRoot) => Path.Combine(new InstallLayout(installRoot).App, "capture", InventorySync.ProxyExe);

    static string? ReadProxyLog(string folder)
    {
        try { return File.ReadAllText(Path.Combine(folder, "proxy.log")); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    // A capture that did not start (or is slow to): what was in its way, and for what a security program commonly causes,
    // the security programs Windows knows of (asked only now, off the UI thread) and the exact file to allow.
    async Task ReportStartFailure(string? folder, CaptureFailure failure, string proxy)
    {
        IReadOnlyList<string> antivirus = CaptureStart.SecurityProgramLikely(failure.Problem) ? await Task.Run(SecurityProducts.ThirdPartyAntivirus) : [];
        var message = CaptureStart.Message(failure, antivirus, proxy, folder != null ? Path.Combine(folder, "proxy.log") : null);
        if (folder != null)
            InventorySync.Note(folder, $"capture did not start: {failure.Problem}" + (failure.Detail != null ? " (" + failure.Detail + ")" : "") +
                "; security programs: " + (antivirus.Count == 0 ? "none besides Microsoft Defender, or not asked" : string.Join(", ", antivirus)));
        // A later sync's status is never overwritten; a slow capture's warning shows only while that capture is still starting.
        var current = failure.Problem == CaptureProblem.NotReady ? syncProxy != null && !syncGameOpened && syncDir == folder
            : syncProxy == null && !syncStarting;
        if (!current) return;
        var brush = failure.Problem is CaptureProblem.Declined or CaptureProblem.NotReady ? Orange : Red;
        SetSyncStatus(message, brush);
        if (failure.Problem != CaptureProblem.Declined) Append("Inventory sync: " + message, brush);
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

    async Task CleanUpSync(string proxy)
    {
        syncStarting = true;
        SyncButton.IsEnabled = false;
        SetSyncStatus("Waiting for the administrator prompt...", Gold);
        var folder = Path.Combine(new InstallLayout(paths.InstallRoot!).Logs, "sync-cleanup-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture));
        using var cleanup = await Task.Run(() => InventorySync.StartCleanup(proxy, folder));
        if (cleanup != null) await cleanup.WaitForExitAsync();
        syncStarting = false;
        SyncButton.IsEnabled = true;
        UpdateSyncStatus();
        if (cleanup == null) SetSyncStatus("Clean up needs the administrator prompt.", Orange);
        else if (InventorySync.HasLeftoverRedirect())
            SetSyncStatus("Clean up failed: " + (CaptureHosts.ReadFailure(folder) ?? "see " + Path.Combine(folder, "proxy.log") + "."), Red);
        else SetSyncStatus("Cleaned up: the normal game reaches its servers again.", Green);
    }
}
