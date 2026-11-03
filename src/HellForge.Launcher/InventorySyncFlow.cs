using System.Diagnostics;
using System.Globalization;
using Avalonia.Controls;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// Profile > Account sync (HellForge.Core InventorySync): runs the elevated capture proxy, opens the normal game through Steam
// once the proxy listens, and saves the player's sealed inventory and progression as the profile's snapshot as soon as the
// proxy has sealed them. The proxy then closes that game itself (CaptureGameEnd) and removes its hosts line and CA; a game
// it could not close is the player's to close, and the launcher says so (InventorySync.GameAfterCapture). The backend takes premium ownership from the
// snapshot on every Play; the rest is imported as the player chooses (AccountImportFlow). Polled with the rest of the
// window, once a second, only while a sync runs.
public partial class MainWindow
{
    Process? syncProxy;
    LocalProfile? syncProfile;
    string? syncDir, syncSnapshotDir;
    bool syncGameOpened, syncStarting, syncTaken, syncSlowNoted, syncGameWarned;
    DateTime syncStartedUtc, syncTakenUtc;
    // A capture that has not listened by then is reported (CaptureProblem.NotReady), and still waited for: its own steps
    // before listening take well under half of this.
    static readonly TimeSpan SyncReadyWarning = TimeSpan.FromSeconds(60);

    // Busy until the capture is taken; after that the proxy only keeps the game connected, and imports may run.
    bool SyncBusy => syncProxy != null && !syncTaken || syncStarting;

    void InitializeSync()
    {
        SyncCard.IsVisible = InventorySync.FindCaptureProxy(paths.InstallRoot) != null;
        SyncButton.Click += async (_, _) => await StartSync();
        PlaySyncButton.Click += async (_, _) => await StartSync();
        PlayReviewButton.Click += (_, _) => Nav.SelectedIndex = 1;
        InitializeAccountImport();
        UpdateSyncStatus();
    }

    void UpdateSyncStatus()
    {
        if (paths.InstallRoot == null || !SyncCard.IsVisible) return;
        var snapshot = InventorySync.SnapshotDirectory(paths.InstallRoot, Selected.Name);
        var info = InventorySync.ReadMetadata(snapshot);
        SyncDetailText.Text = info == null ? "No snapshot yet. Sync to copy your real inventory into this profile."
            : !info.Progression ? "Last sync failed " + (info.CapturedUtc?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) ?? "at an unknown time") +
                $": {info.Items:N0} items, no progression. Sync again to get your level, unlocks and ship."
            : "Last synced " + (info.CapturedUtc?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) ?? "at an unknown time") +
              $": {info.Items:N0} items, " + (info.Experience is { } xp ? $"{xp:N0} XP." : "progression captured.");
        UpdateImportControls();
        UpdatePlaySync(snapshot, info);
        // While this launcher's capture still keeps the game connected, its hosts line is its own, not an earlier sync's leftover.
        if (SyncBusy || syncProxy != null) return;
        var leftover = InventorySync.HasLeftoverRedirect();
        SyncButton.Content = leftover ? "Clean up" : "Sync Inventory";
        if (!leftover) return;
        PlaySyncButton.Content = "Clean up";
        SetSyncStatus("An earlier sync did not finish: the normal game cannot reach its servers until you clean up.", Orange);
    }

    // The Play page's card: premium content stays locked in the game until the profile has a sync that holds for the Steam
    // account signed in now (HellForge.LocalBackend AccountSnapshot), so the card says so beside PLAY and syncs from there.
    void UpdatePlaySync(string snapshot, SnapshotInfo? info)
    {
        var synced = InventorySync.HasSnapshot(snapshot);
        var seal = synced ? SnapshotSeal.Verify(snapshot, InventorySync.SealKeyFor(snapshot)) : null;
        var when = (seal?.CapturedUtc ?? info?.CapturedUtc)?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture);
        var last = when != null ? "The last sync (" + when + ")" : "The last sync";
        var locked = !synced ? "Warbonds, Superstore items and DLC gear you own stay locked in the game until you sync this profile with " +
                "your HELLDIVERS 2 account. Sync opens the normal game through Steam and asks for administrator access; log in and pass " +
                "the title screen."
            : seal == null ? last + " cannot be checked on this PC: it changed since, or was made on another PC. Your premium content " +
                "stays locked in the game until you sync again."
            : SteamAccount.Current() is { } account && account != seal.SteamAccount ? last + " is for another Steam account than the " +
                "one signed in. Your premium content stays locked in the game until you sync with this one."
            : null;
        PlaySyncCard.IsVisible = true;
        PlaySyncTitle.Text = locked != null ? "Your premium content is not in this profile" : "Profile synced";
        PlaySyncTitle.Foreground = locked != null ? Orange : Text;
        PlaySyncText.Text = locked ?? "Premium content you owned " + (when != null ? "on " + when : "at the last sync") +
            " is unlocked. Bought or unlocked something since? Update the sync to bring it in.";
        if (locked != null) PlaySyncCard.BorderBrush = Orange;
        else PlaySyncCard.ClearValue(Border.BorderBrushProperty);
        PlaySyncButton.Content = synced ? "Update profile sync" : "Sync profile";
        PlaySyncButton.Classes.Set("primary", locked != null);
        PlaySyncButton.Classes.Set("ghost", locked == null);
    }

    // playText: the Play page's wording, where the Profile page's points at something on that page.
    void SetSyncStatus(string text, IBrush brush, string? playText = null)
    {
        SyncStatusText.Text = text;
        SyncStatusText.Foreground = brush;
        PlaySyncStatus.Text = playText ?? text;
        PlaySyncStatus.Foreground = brush;
        PlaySyncStatus.IsVisible = text.Length > 0;
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
            WatchCapture(syncProfile, syncDir, syncSnapshotDir);
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
        (syncTaken, syncGameWarned) = (false, false);
        UpdateSyncStatus();
    }

    // While the capture runs: the game opened once it listens, a slow start reported once, the capture taken once
    // sealed, and after that a game it could not close.
    void WatchCapture(LocalProfile profile, string folder, string snapshot)
    {
        if (!syncGameOpened && InventorySync.IsListening(folder)) OpenGameForSync();
        else if (!syncGameOpened && !syncSlowNoted && DateTime.UtcNow - syncStartedUtc > SyncReadyWarning && paths.InstallRoot != null)
        {
            syncSlowNoted = true;
            _ = ReportStartFailure(folder, new CaptureFailure(CaptureProblem.NotReady, null), ExpectedProxy(paths.InstallRoot));
        }
        else if (!syncTaken && InventorySync.IsSealed(folder)) TakeCapture(profile, folder, snapshot);
        else if (syncTaken) WarnIfGameStillOpen(folder);
    }

    // The capture closes the sync's game itself; one still open well after the capture was taken is one it could not end.
    void WarnIfGameStillOpen(string folder)
    {
        if (syncGameWarned || InventorySync.GameAfterCapture(syncTakenUtc, DateTime.UtcNow, InventorySync.IsGameRunning()) is not { } warning) return;
        syncGameWarned = true;
        InventorySync.Note(folder, "the game is still running " + InventorySync.CloseGameWait.TotalSeconds + " s after the capture was taken");
        Append("Inventory sync: " + warning, Orange);
    }

    // The sealed capture becomes the profile's snapshot, while the proxy may still keep the game connected.
    void TakeCapture(LocalProfile profile, string folder, string snapshot)
    {
        (syncTaken, syncTakenUtc) = (true, DateTime.UtcNow);
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
            AppendGameClosing();
            return;
        }
        Append("Inventory sync complete for profile " + profile.Name + ".", LauncherLine);
        AppendGameClosing();
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

    // The sync's game has done its job: the capture closes it, so Easy Anti-Cheat never gets to "Could not connect".
    void AppendGameClosing()
    {
        if (syncProxy is { HasExited: false })
            Append("Inventory sync: HellForge is closing the normal game it opened for the sync; start it from Steam again to play normally.", LauncherLine);
    }

    static string ProxyClosedLine(int exitCode, string folder) => exitCode == 0
        ? "Inventory sync: the game is closed, and the capture removed its hosts line and certificate."
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
