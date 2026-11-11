using System.Globalization;
using Avalonia.Controls;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// Profile > Account sync (HellForge.Core InventorySync): runs the platform's capture (elevated on Windows, native and
// below the installed helper on Linux), opens the normal game through Steam once the proxy listens, and saves the
// player's sealed inventory and progression as the profile's snapshot as soon as the proxy has sealed them. On Windows the
// proxy then closes that game itself (CaptureGameEnd) and removes its hosts line and CA; a game it could not close is the
// player's to close, and the launcher says so (InventorySync.GameAfterCapture). On Linux the proxy keeps the game
// connected until it exits. The backend takes premium ownership from the snapshot on every Play; the rest is imported as
// the player chooses (AccountImportFlow). Polled with the rest of the window, once a second, only while a sync runs.
public partial class MainWindow
{
    InventorySync.CaptureRun? syncProxy;
    LocalProfile? syncProfile;
    string? syncDir, syncSnapshotDir, syncWorkingFolder;
    bool syncGameOpened, syncStarting, syncTaken, syncClosing, syncSlowNoted, syncGameWarned;
    DateTime syncStartedUtc, syncTakenUtc;
    // A capture that has not listened by then is reported (CaptureProblem.NotReady), and still waited for: its own steps
    // before listening take well under half of this.
    static readonly TimeSpan SyncReadyWarning = TimeSpan.FromSeconds(60);
    // What the launcher saw of the game since it asked Steam to open it, for a message that says which step stopped.
    bool syncGameSeen, syncFolderHidden;
    DateTime syncAskedUtc;
    string? syncProblem;

    static bool NativeLinux => OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine;

    // Busy until the capture is taken; after that the proxy only keeps the game connected, and imports may run.
    bool SyncBusy => syncProxy != null && !syncTaken || syncStarting;
    // This launcher's running capture: not a session leftover (MainWindow.ShowLeftoverProcesses).
    int? SyncCaptureId => syncProxy is { HasExited: false } run ? run.Id : null;

    void InitializeSync()
    {
        SyncCard.IsVisible = InventorySync.FindCaptureProxy(paths.InstallRoot) != null;
        if (NativeLinux)
            SyncDescription.Text = "Brings your own HELLDIVERS 2 account into this profile. Warbonds, Superstore items and DLC gear you bought always follow your latest sync; everything else is imported only as you choose, and the profile is backed up first. Super Credits are never imported or usable. Sync opens the normal game through Steam; log in and pass the title screen. On Linux the first sync asks once to install a small root-owned helper, through the desktop's password dialog; until the normal game exits, two marked /etc/hosts lines point its server name at this PC and a one-day certificate is trusted only in HELLDIVERS 2's Proton prefix. Both are removed when it exits.";
        SyncButton.Click += async (_, _) => await StartSync();
        PlaySyncButton.Click += async (_, _) => await StartSync();
        PlayReviewButton.Click += (_, _) => Nav.SelectedIndex = 1;
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
                "your HELLDIVERS 2 account. Sync opens the normal game through Steam and " +
                (NativeLinux ? "the first time asks for your password to install its helper" : "asks for administrator access") +
                "; log in and pass the title screen."
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
        if (SyncBusy || syncClosing || paths.InstallRoot == null) return;
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
        if (SyncRefusal() is { } refusal)
        {
            RefuseSync(refusal);
            return;
        }
        var install = new InstallLayout(paths.InstallRoot);
        if (NativeLinux && !await EnsureLinuxCaptureHelperAsync(install.Root)) return;
        syncStarting = true;
        SyncButton.IsEnabled = false;
        if (await SyncAccountAsync(install) is not { } account) return;
        var profile = Selected;
        var snapshot = InventorySync.SnapshotDirectory(install.Root, profile.Name);
        var folder = Path.Combine(install.Logs, "sync-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture));
        syncWorkingFolder = folder;
        SetSyncStatus(SyncStartingText(), Gold);
        (InventorySync.CaptureRun? Run, CaptureProblem Refused, string? Detail) launch = (null, CaptureProblem.None, null);
        string? failure = null;
        try { launch = await Task.Run(() => InventorySync.StartCaptureProxy(install.Root, proxy, folder, snapshot, account, runner.IsRunning)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            failure = error.Message;
        }
        syncStarting = false;
        syncWorkingFolder = null;
        if (syncClosing)
        {
            launch.Run?.Dispose();
            return;
        }
        if (launch.Run is not { } started)
        {
            await CaptureNotStarted(folder, failure, new CaptureFailure(launch.Refused, launch.Detail), proxy);
            return;
        }
        (syncProxy, syncProfile, syncDir, syncSnapshotDir, syncGameOpened, syncTaken, syncSlowNoted) =
            (started, profile, folder, snapshot, false, false, false);
        (syncGameSeen, syncFolderHidden, syncProblem) = (false, false, null);
        syncStartedUtc = DateTime.UtcNow;
        InventorySync.Note(folder, $"sync started for profile {profile.Name}; capture pid {started.Id}; launcher {Environment.ProcessPath}");
        SetSyncStatus("Starting the capture...", Gold);
        Append("Inventory sync: capture started for profile " + profile.Name + ". HELLDIVERS 2 opens through Steam; take it past the title screen.", LauncherLine);
    }

    // The signed-in Steam account the sync is sealed for, read off the UI thread (native Linux may run its probe); null,
    // with the sync refused and the busy state cleared, when there is none or it cannot be read.
    async Task<ulong?> SyncAccountAsync(InstallLayout install)
    {
        SetSyncStatus("Checking the signed-in Steam account...", Gold);
        ulong? signedIn;
        string? refusal;
        try
        {
            signedIn = await Task.Run(() => SteamAccount.Current(install));
            // The Windows launcher under Wine has no source for the account (only a session's runner is given one), so on
            // Linux it can never sync: say where the sync works instead of asking for a sign-in that is already there.
            refusal = signedIn != null ? null : LinuxNetworkBlock.UnderWine
                ? "Account Sync needs HellForge's Linux launcher: close this window and start HellForge from its desktop shortcut."
                : "Sign in to Steam first: the sync is sealed for your Steam account.";
        }
        catch (Exception error) when (error is InvalidOperationException or IOException or UnauthorizedAccessException
            or System.ComponentModel.Win32Exception)
        {
            (signedIn, refusal) = (null, "The signed-in Steam account could not be read: " + error.Message);
        }
        if (refusal == null) return signedIn;
        syncStarting = false;
        SyncButton.IsEnabled = true;
        RefuseSync(refusal);
        return null;
    }

    // One confirmation, then pkexec's own password dialog. The helper and relay are root-owned and the launcher never
    // keeps root or a password. A failed or cancelled install changes nothing and says why.
    async Task<bool> EnsureLinuxCaptureHelperAsync(string installRoot)
    {
        if (ConfirmOverlay.IsVisible)
        {
            // Nothing to install means nothing to confirm; only a needed install has to wait for the open confirmation.
            if (LinuxCapture.HelperProblem(installRoot) == null) return true;
            SetSyncStatus("Finish the open confirmation first.", Orange);
            return false;
        }
        syncStarting = true;
        SyncButton.IsEnabled = false;
        try
        {
            // The files first; then sudo itself, because a later sudoers rule can ask for a password although every file
            // is right (SteamOS). Either way the installer runs again and writes the rule that sorts last.
            var need = LinuxCapture.HelperProblem(installRoot) ?? await Task.Run(LinuxCapture.HelperCheckProblem);
            if (need == null) return true;
            ConfirmTitle.Text = "Install Account Sync helper?";
            ConfirmText.Text = LinuxCapture.HelperInstallDescription(installRoot) + "\n\nIt is needed because: " + need;
            (ConfirmCancel.Content, ConfirmStop.Content) = ("Cancel", "Install");
            ConfirmButtons.IsVisible = true;
            ConfirmOverlay.IsVisible = true;
            confirm = new TaskCompletionSource<bool>();
            var accepted = await confirm.Task;
            confirm = null;
            ConfirmOverlay.IsVisible = false;
            if (!accepted)
            {
                SetSyncStatus("The capture helper was not installed; nothing changed.", Orange);
                return false;
            }
            SetSyncStatus("Installing the Account Sync helper...", Gold);
            LinuxCapture.HelperSetupResult result;
            try { result = await Task.Run(() => LinuxCapture.InstallHelper(installRoot)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException
                or System.ComponentModel.Win32Exception)
            {
                var text = "The Account Sync helper could not be installed: " + error.Message;
                SetSyncStatus(text, Red);
                Append("Inventory sync: " + text, Red);
                return false;
            }
            if (!result.Succeeded)
            {
                SetSyncStatus(result.Message, result.Cancelled ? Orange : Red);
                Append("Inventory sync: " + result.Message, result.Cancelled ? Orange : Red);
                return false;
            }
            if (LinuxCapture.HelperProblem(installRoot) is { } problem)
            {
                var text = "The helper installer finished, but the installed copies still do not match: " + problem;
                SetSyncStatus(text, Red);
                Append("Inventory sync: " + text, Red);
                return false;
            }
            Append("Inventory sync: " + result.Message, LauncherLine);
            return true;
        }
        finally
        {
            syncStarting = false;
            SyncButton.IsEnabled = true;
        }
    }

    static string SyncStartingText() => NativeLinux
        ? "Starting the native capture and the Linux helper..."
        : "Waiting for the administrator prompt...";

    // A capture that did not start. Native Linux says why by throwing (failure); Windows names what refused the elevated
    // start (CaptureStart).
    async Task CaptureNotStarted(string folder, string? failure, CaptureFailure refused, string proxy)
    {
        if (failure != null || NativeLinux)
        {
            SyncNotStarted(folder, failure);
            return;
        }
        SyncButton.IsEnabled = true;
        await ReportStartFailure(folder, refused, proxy);
    }

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
        // A session this launcher did not start (an earlier launcher's, or one left behind) still holds the installation's
        // Steam bridge and prefix. ShowLeftoverProcesses also shows the card that ends them.
        if (ShowLeftoverProcesses()) return "A HellForge session is still running: end it on the play page, then sync again.";
        return null;
    }

    // A sync that cannot start says why on the sync card and in logs/sync-refused.txt: the refusal comes before the sync's
    // own folder exists, so without this the player finds no log at all.
    void RefuseSync(string reason)
    {
        SetSyncStatus(reason, Orange);
        if (paths.InstallRoot != null) InventorySync.NoteRefusal(paths.InstallRoot, reason);
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
        // A sync folder this launcher cannot read hides everything the capture wrote, its start failure included.
        else if (!InventorySync.CanReadSyncFolder(folder)) SyncFailed(InventorySync.UnreadableFolderMessage);
        // A hosts write the capture could not make (CaptureHosts) names its own cause. Before listening it is also the
        // capture's start failure (capture-failure.json holds the same reason), shown once, with the security program to allow.
        else if (syncGameOpened)
            SyncFailed(CaptureHosts.ReadFailure(folder) ?? InventorySync.NothingCaptured(InventorySync.GameState(true, syncGameSeen, InventorySync.IsGameRunning()),
                InventorySync.TimeoutMinutes, Path.Combine(folder, "proxy.log")));
        else if (NativeLinux) SyncFailed("The capture could not start: see " + Path.Combine(folder, "proxy.log") + ".");
        else if (paths.InstallRoot != null)
            _ = ReportStartFailure(folder, CaptureStart.FromExit(exitCode, CaptureStart.Read(folder), ReadProxyLog(folder), CaptureHosts.ReadFailure(folder)),
                ExpectedProxy(paths.InstallRoot));
        (syncProfile, syncDir, syncSnapshotDir) = (null, null, null);
        (syncTaken, syncGameWarned) = (false, false);
        UpdateSyncStatus();
    }

    // While the capture waits: the game's process since Steam was asked to open it, and a sync folder this launcher cannot
    // read (then the capture's listening state never shows, and the game would never be opened), each said as it happens.
    void WatchSyncProgress(string folder)
    {
        if (!syncGameOpened)
        {
            if (syncFolderHidden || DateTime.UtcNow - syncStartedUtc < TimeSpan.FromSeconds(15) || InventorySync.CanReadSyncFolder(folder)) return;
            syncFolderHidden = true;
            InventorySync.Note(folder, "this launcher cannot read the sync folder; the capture runs as another Windows account");
            SetSyncStatus(InventorySync.UnreadableFolderMessage, Red);
            return;
        }
        var running = InventorySync.IsGameRunning();
        if (running && !syncGameSeen) InventorySync.Note(folder, "the game is running");
        syncGameSeen |= running;
        var state = InventorySync.GameState(true, syncGameSeen, running);
        var problem = InventorySync.WaitingProblem(state, DateTime.UtcNow - syncAskedUtc);
        if (problem == syncProblem) return;
        syncProblem = problem;
        if (problem != null)
        {
            InventorySync.Note(folder, "waiting: game " + (state == SyncGameState.Closed ? "closed before the capture" : "not started by Steam"));
            SetSyncStatus(problem, Orange);
        }
        else SetSyncStatus(WaitingForLogin, Gold);
    }

    const string WaitingForLogin = "Log in to HELLDIVERS 2 and pass the title screen. This updates when the capture is done.";

    // While the capture runs: the game opened once it listens, a slow start reported once, the capture taken once
    // sealed, and after that a game it could not close; until then, what the launcher sees of the game and the folder.
    void WatchCapture(LocalProfile profile, string folder, string snapshot)
    {
        if (!syncGameOpened && InventorySync.IsListening(folder)) OpenGameForSync();
        // not when the folder is hidden from this launcher: that is said already, and is why it seems not to listen
        else if (!syncGameOpened && !syncSlowNoted && !syncFolderHidden && DateTime.UtcNow - syncStartedUtc > SyncReadyWarning && paths.InstallRoot != null)
        {
            syncSlowNoted = true;
            _ = ReportStartFailure(folder, new CaptureFailure(CaptureProblem.NotReady, null), ExpectedProxy(paths.InstallRoot));
        }
        else if (!syncTaken && InventorySync.IsSealed(folder)) TakeCapture(profile, folder, snapshot);
        else if (syncTaken) WarnIfGameStillOpen(folder);
        else WatchSyncProgress(folder);
    }

    // The capture closes the sync's game itself; one still open well after the capture was taken is one it could not end.
    // The Linux capture keeps the game connected on purpose: nothing to warn about there.
    void WarnIfGameStillOpen(string folder)
    {
        if (NativeLinux || syncGameWarned || InventorySync.GameAfterCapture(syncTakenUtc, DateTime.UtcNow, InventorySync.IsGameRunning()) is not { } warning) return;
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
        // Windows' security advice (Windows Security, a quarantine) does not apply to the native Linux capture.
        var message = CaptureStart.Message(failure, antivirus, proxy, folder != null ? Path.Combine(folder, "proxy.log") : null, securityAdvice: !NativeLinux);
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

    // The sync's game has done its job: on Windows the capture closes it, so Easy Anti-Cheat never gets to "Could not
    // connect". The Linux capture (no --close-game) keeps it connected until the player closes it.
    void AppendGameClosing()
    {
        if (syncProxy is not { HasExited: false }) return;
        Append(NativeLinux
            ? "Inventory sync: the game stays connected through the capture until you close it; then the hosts line and certificate are removed."
            : "Inventory sync: HellForge is closing the normal game it opened for the sync; start it from Steam again to play normally.", LauncherLine);
    }

    static string ProxyClosedLine(int exitCode, string folder) => exitCode == 0
        ? "Inventory sync: the game is closed, and the capture removed its hosts line and certificate."
        : "Inventory sync: the capture ended with an error; see " + Path.Combine(folder, "proxy.log") + ".";

    void ProxyClosed(int exitCode, string folder) => Append(ProxyClosedLine(exitCode, folder), exitCode == 0 ? LauncherLine : Orange);

    void OpenGameForSync()
    {
        syncGameOpened = true;
        syncAskedUtc = DateTime.UtcNow;
        try
        {
            InventorySync.OpenVanillaGame();
            if (syncDir != null) InventorySync.Note(syncDir, "capture listening; asked Steam to open the game (steam://rungameid/553850)");
            SetSyncStatus(WaitingForLogin, Gold);
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
        string? failure = null, folder = null;
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
            var logs = Path.Combine(new InstallLayout(paths.InstallRoot!).Logs, "sync-cleanup-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture));
            folder = logs;
            using var cleanup = await Task.Run(() => InventorySync.StartCleanup(proxy, logs));
            if (cleanup != null) await cleanup.WaitForExitAsync();
            else declined = true;
        }
        syncStarting = false;
        SyncButton.IsEnabled = true;
        UpdateSyncStatus();
        if (declined) SetSyncStatus("Clean up needs the administrator prompt.", Orange);
        else if (failure != null) SetSyncStatus("Clean up failed: " + failure, Red);
        else if (InventorySync.HasLeftoverRedirect())
            SetSyncStatus("Clean up failed: " + (folder == null ? "remove the HellForge-CaptureProxy lines from the hosts file."
                : CaptureHosts.ReadFailure(folder) ?? "see " + Path.Combine(folder, "proxy.log") + "."), Red);
        else SetSyncStatus("Cleaned up: the normal game reaches its servers again.", Green);
    }
}
