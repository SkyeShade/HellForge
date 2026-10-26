using System.ComponentModel;
using System.Diagnostics;
using System.Net.Http;
using System.Runtime.Versioning;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// Setting up an installation: find HELLDIVERS 2 in Steam, confirm ownership, copy the game into the installation,
// place its network block (WFP filters), optionally import earlier HellForge game data and create shortcuts. When Steam's own copy is
// another build (Arrowhead patched the game) or missing, Steam itself downloads the supported build's depots through
// its console (SteamDepotDownload) and the game is copied from there. Shown instead of the other
// pages until the game is copied; Settings > Installation runs it again to check and repair.
public partial class MainWindow
{
    enum SetupStep { Find, Ownership, Copy, NetworkBlock, Import, Shortcuts }
    static readonly string[] SetupStepNames =
        ["Find HELLDIVERS 2 in Steam", "Confirm you own the game", "Copy the game into this folder", "Block the copy from the internet",
            "Import earlier game data", "Create shortcuts"];

    readonly List<(Border Dot, TextBlock Detail)> setupRows = [];
    CancellationTokenSource? setupCancel;
    bool setupRunning;
    // Build patches (by SHA-256) that failed in this launcher run: Check again uses Steam's download instead, and a
    // re-published pack (another hash) is tried. Restarting the launcher tries a failed one again.
    readonly HashSet<string> failedPatches = new(StringComparer.OrdinalIgnoreCase);
    DispatcherTimer? downloadTimer;
    string? downloadSteamRoot;

    // Where the game kept its user data before installations existed, under the project's name then (HD2 Beyond The Stars).
    static string EarlierGameData => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "HD2BeyondTheStars");

    void InitializeSetup()
    {
        if (paths.InstallRoot == null) return;
        var install = new InstallLayout(paths.InstallRoot);
        if (OperatingSystem.IsWindows()) RenameLegacyShortcuts(install);
        _ = SyncBundledMods(install);
        foreach (var name in SetupStepNames)
        {
            var dot = new Border { Classes = { "dot" }, Background = Pending, VerticalAlignment = VerticalAlignment.Top, Margin = new(0, 5, 14, 0) };
            var title = new TextBlock { Text = name, FontWeight = FontWeight.SemiBold };
            var detail = new TextBlock { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };
            var row = new DockPanel();
            DockPanel.SetDock(dot, Dock.Left);
            row.Children.Add(dot);
            row.Children.Add(new StackPanel { Spacing = 2, Children = { title, detail } });
            SetupSteps.Children.Add(row);
            setupRows.Add((dot, detail));
        }
        ImportBox.IsVisible = Directory.Exists(Path.Combine(EarlierGameData, "profiles")) || File.Exists(Path.Combine(EarlierGameData, "user_settings.config"));
        InstallCard.IsVisible = true;
        InstallText.Text = $"{Privacy.Scrub(install.Root)}  ·  HELLDIVERS 2 build {SupportedBuild.Current.BuildId}";
        SetupStartButton.Click += async (_, _) => await RunSetup();
        SetupPauseButton.Click += (_, _) => setupCancel?.Cancel();
        SetupBackButton.Click += (_, _) => LeaveSetup();
        OpenConsoleButton.Click += (_, _) => OpenSteamConsole();
        DeleteDownloadBox.Content = $"Delete Steam's download after copying (frees about {SupportedBuild.Current.TotalBytes / 1e9:0} GB)";
        RepairButton.Click += (_, _) => EnterSetup(repair: true);
        UninstallButton.Click += (_, _) => AskUninstall(install);
        UninstallConfirmButton.Click += async (_, _) => await Uninstall(install);
        UninstallCancelButton.Click += (_, _) => UninstallConfirm.IsVisible = false;
        if (!SupportedBuild.Current.IsComplete(install.Game)) EnterSetup(repair: false);
    }

    void EnterSetup(bool repair)
    {
        if (runner.IsRunning) return;
        foreach (var page in new Control[] { PlayPage, ProfilePage, ModsPage, ConsolePage, SettingsPage }) page.IsVisible = false;
        SetupPage.IsVisible = true;
        // A first setup has nowhere to go back to; a repair can be left.
        Nav.IsVisible = repair;
        SetupBackButton.IsVisible = repair;
        SetupBackButton.Content = "Back";
        SetBackButtonPrimary(false);
        SetupStartButton.IsVisible = true;
        SetupTitle.Text = repair ? "Check and repair" : "Set up HellForge";
        SetupStartButton.Content = repair ? "Check and repair" : "Start setup";
        for (var i = 0; i < setupRows.Count; i++) SetRow((SetupStep)i, null, "");
    }

    void SetBackButtonPrimary(bool primary)
    {
        SetupBackButton.Classes.Set("primary", primary);
        SetupBackButton.Classes.Set("ghost", !primary);
    }

    void LeaveSetup()
    {
        Nav.IsVisible = true;
        ShowPage(Nav.SelectedIndex);
    }

    void SetRow(SetupStep step, bool? success, string detail, bool running = false)
    {
        var (dot, text) = setupRows[(int)step];
        dot.Background = running ? Accent : success switch { true => Green, false => Red, null => Pending };
        dot.Classes.Set("pulse", running);
        text.Text = detail;
        text.Foreground = success == false ? Red : Grey;
    }

    // The game found for setup: the supported build, Steam's root, Steam's installed game (or null), whether Steam's
    // depot download holds it, and a build patch (with its source) that converts an installed build to the supported one.
    sealed record SetupSource(SupportedBuild Build, string SteamRoot, SteamGame? Game, bool FromDepots, DownpatchPack? DownpatchPack, string? DownpatchSource);

    async Task RunSetup()
    {
        if (setupRunning || runner.IsRunning || paths.InstallRoot == null || !OperatingSystem.IsWindows()) return;
        setupRunning = true;
        SetupStartButton.IsEnabled = false;
        SetupBackButton.IsVisible = false;
        var install = new InstallLayout(paths.InstallRoot);
        var step = SetupStep.Find;
        try
        {
            var found = await FindGameStep(install);
            // When downpatching, copy first so the supported build's steam_api64.dll is available for the ownership check
            // (the newer build's DLL may not work with the probe).
            if (found.DownpatchPack != null) { step = SetupStep.Copy; await DownpatchStep(install, found); }
            var steamApi = SteamApiPath(install, found);
            step = SetupStep.Ownership;
            await OwnershipStep(install, steamApi);
            if (found.DownpatchPack == null) { step = SetupStep.Copy; await CopyStep(install, found); }
            step = SetupStep.NetworkBlock;
            await NetworkBlockStep(install);
            step = SetupStep.Import;
            await ImportStep(install);
            step = SetupStep.Shortcuts;
            await ShortcutsStep(install);
            ReadyToPlay();
        }
        catch (OperationCanceledException)
        {
            SetRow(step, null, "Paused. Start setup again to continue where it stopped.");
            SetupStartButton.Content = "Continue setup";
        }
        catch (SetupWaiting waiting)
        {
            SetRow(step, null, waiting.Message);
            SetupStartButton.Content = "Check again";
            SetupBackButton.IsVisible = Nav.IsVisible;
        }
        catch (Exception error) when (error is SetupStopped or IOException or InvalidDataException or UnauthorizedAccessException or Win32Exception
            or JsonException or InvalidOperationException or HttpRequestException)
        {
            SetRow(step, false, error is Win32Exception { NativeErrorCode: 1223 }
                ? "Windows permission was declined. The network block is required; start setup again and allow it."
                : Describe(error));
            SetupStartButton.Content = "Try again";
            SetupBackButton.IsVisible = Nav.IsVisible;
        }
        finally
        {
            setupRunning = false;
            setupCancel = null;
            SetupStartButton.IsEnabled = true;
            SetupPauseButton.IsVisible = false;
        }
    }

    // Finds HELLDIVERS 2 for setup: the installed supported build, Steam's depot download, or a build patch; else offers
    // Steam's download (throwing SetupWaiting) or stops.
    async Task<SetupSource> FindGameStep(InstallLayout install)
    {
        SetRow(SetupStep.Find, null, "Looking in your Steam libraries...", running: true);
        // Linux (Wine/Proton) outside the namespace, such as the launcher HellForge-Setup.exe opens: the network block
        // step could only fail there (Wine has no Windows Filtering Platform), after the whole copy.
        if (LinuxNetworkBlock.UnderWine && !LinuxNetworkBlock.Requested)
            throw new SetupStopped("On Linux, setup runs inside HellForge's network namespace. Close HellForge and start it with " +
                "app/linux/hellforge-linux.sh (docs/linux.md), then start setup there.");
        if (!LinuxNetworkBlock.Requested) await CheckFilteringEngine(install);
        if (VisualCppRuntimes.Problem("start setup again.") is { } runtimes) throw new SetupStopped(runtimes);
        var build = SupportedBuild.Current;
        var steamRoot = SteamLocator.SteamRoot() ?? throw new SetupStopped("Steam is not installed on this PC. Install Steam and HELLDIVERS 2, then start setup again.");
        // The build patches published since this release (a game update gets one there, not a new HellForge).
        using (var http = new HttpClient()) await Downpatcher.RefreshAsync(http, Path.Combine(install.Temp, "downpatch-index.json"));
        SteamGame? game;
        string? unusable = null;
        try { game = await Task.Run(() => SteamLocator.FindGame(steamRoot)); }
        catch (Exception error) when (error is InvalidDataException or IOException) { game = null; unusable = error.Message; }
        if (game is { SupportedBuild: true }) { SetRow(SetupStep.Find, true, $"Build {game.Report.Pin.BuildId} in {game.Report.Source}"); return new(build, steamRoot, game, false, null, null); }
        if (await Task.Run(() => SteamDepotDownload.IsComplete(steamRoot, build)))
        {
            SetRow(SetupStep.Find, true, $"Build {build.BuildId}, downloaded by Steam into {Path.GetDirectoryName(SteamDepotDownload.DepotFolder(steamRoot, build, build.Depots[0].Id))}");
            return new(build, steamRoot, game, true, null, null);
        }
        if (game != null && Downpatcher.FindPack(game.Report.Pin) is { } downpatch && !failedPatches.Contains(downpatch.Sha256))
        {
            SetRow(SetupStep.Find, true, $"Build {game.Report.Pin.BuildId} in {game.Report.Source}. A {downpatch.Size / 1e9:0.0} GB patch converts it to build {build.BuildId}.");
            return new(build, steamRoot, game, false, downpatch, game.Report.Source);
        }
        // the game is there but busy (Steam updating it, the game running): that passes, so no download is offered.
        // Under Wine it does not pass: Steam's Linux libraries are on the Z: drive, which counts as a linked path
        // ("Linked/reparse paths are not supported: Z:\", GitHub issue 1), so the download is the way there.
        if (game == null && unusable != null && !LinuxNetworkBlock.UnderWine)
            throw new SetupStopped($"HELLDIVERS 2 is in Steam, but setup cannot use it right now: {unusable}");
        OfferDownload(steamRoot, game, unusable, build);
        throw new InvalidOperationException("unreachable"); // OfferDownload always throws
    }

    // Offers Steam's download of the supported build and stops setup until it is complete (throws SetupWaiting).
    void OfferDownload(string steamRoot, SteamGame? game, string? unusable, SupportedBuild build)
    {
        var have = game != null ? $"HELLDIVERS 2 build {game.Report.Pin.BuildId}"
            : unusable != null ? "a HELLDIVERS 2 setup cannot read from inside Wine" : "no HELLDIVERS 2";
        ShowDownload(steamRoot, have);
        var noPatch = game == null ? ""
            : Downpatcher.FindPack(game.Report.Pin) != null ? $" The build patch for build {game.Report.Pin.BuildId} did not work earlier."
            : $" HellForge has no patch for build {game.Report.Pin.BuildId} yet; once one is published, Check again uses it.";
        throw new SetupWaiting($"Steam has {have}, but this version of HellForge needs build {build.BuildId}.{noPatch} Steam can download that exact " +
            "build for your account: follow the steps below. Setup continues by itself when the download is complete.");
    }

    // Applies the build patch into the installation (the copy step's row), offering Steam's download when it will not fit or fails.
    async Task DownpatchStep(InstallLayout install, SetupSource found)
    {
        var build = found.Build;
        var downpatchPack = found.DownpatchPack!;
        // Only the screen order changes: each step keeps its own row (setupRows by step), so the patch's progress
        // and errors show on the copy row. (Swapping the rows here as well put them on "Confirm you own the game",
        // and a second run swapped the screen order back.)
        var copyRow = (Control)setupRows[(int)SetupStep.Copy].Dot.Parent!;
        var ownershipRow = (Control)setupRows[(int)SetupStep.Ownership].Dot.Parent!;
        if (SetupSteps.Children.IndexOf(copyRow) > SetupSteps.Children.IndexOf(ownershipRow))
        {
            SetupSteps.Children.Remove(copyRow);
            SetupSteps.Children.Insert(SetupSteps.Children.IndexOf(ownershipRow), copyRow);
        }
        SetRow(SetupStep.Copy, null, "Downloading and applying the build patch...", running: true);
        try { await DownpatchGame(found.DownpatchSource!, install, downpatchPack); }
        // a full drive: the patch is fine and Steam's download would need the same room, so ask for space; the
        // files already patched are kept
        catch (NotEnoughSpaceException error)
        {
            throw new SetupWaiting($"{error.Message} Free some space on that drive, then click Check again: the files already patched are kept.");
        }
        // the patch failed (its download, Steam's copy, the pack itself): Steam's own download of the build still
        // works, and the files already patched are kept (each one has the build's hash), so it copies only the rest
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or HttpRequestException
            or InvalidOperationException or Win32Exception)
        {
            failedPatches.Add(downpatchPack.Sha256);
            ShowDownload(found.SteamRoot, $"HELLDIVERS 2 build {found.Game!.Report.Pin.BuildId}");
            throw new SetupWaiting($"The build patch did not work: {Describe(error)} Steam can download build {build.BuildId} for your " +
                "account instead: follow the steps below. Setup continues by itself when the download is complete.");
        }
        SetRow(SetupStep.Copy, true, $"{build.Files.Count} files ({build.TotalBytes / 1e9:0.0} GB), patched from build {downpatchPack.SourceBuild}.");
    }

    // The steam_api64.dll the ownership check uses: the patched copy, Steam's depot download, or the installed game.
    static string SteamApiPath(InstallLayout install, SetupSource found) => found.DownpatchPack != null
        ? Path.Combine(install.Game, "bin", "steam_api64.dll")
        : found.FromDepots
            ? SteamDepotDownload.Locate(found.SteamRoot, found.Build, found.Build.Files.Single(f => f.Path == "bin/steam_api64.dll"))
            : Path.Combine(found.Game!.Report.Source, "bin", "steam_api64.dll");

    async Task OwnershipStep(InstallLayout install, string steamApi)
    {
        SetRow(SetupStep.Ownership, null, "Asking Steam...", running: true);
        // the evidence decides (SteamOwnershipCheck.Problem); under Wine, whose powershell.exe runs nothing, the probe
        // runs directly
        var evidence = Path.Combine(install.Logs, "setup-ownership.json");
        var checkStarted = DateTime.UtcNow;
        if (File.Exists(evidence)) File.Delete(evidence);
        int ownershipExit;
        string ownershipError;
        if (LinuxNetworkBlock.UnderWine)
        {
            var probe = Path.GetFullPath(Path.Combine(paths.ScriptsFolder, "..", "steamprobe", "HellForge.SteamProbe.exe"));
            try
            {
                ownershipExit = await Task.Run(() => SteamOwnershipCheck.RunProbe(probe, steamApi, Path.Combine(install.Temp, "ownership"),
                    evidence, TimeSpan.FromSeconds(15)));
                ownershipError = "";
            }
            catch (InvalidDataException error) { ownershipExit = 1; ownershipError = error.Message; }
        }
        else
        {
            var ownership = await RunPowerShell(
                $"-File {Arg(ScriptPath("Test-SteamOwnership.ps1"))} -NoBuild -PassThru -SteamApi {Arg(steamApi)} " +
                $"-WorkRoot {Arg(Path.Combine(install.Temp, "ownership"))} -EvidencePath {Arg(evidence)}", install);
            (ownershipExit, ownershipError) = (ownership.ExitCode, ownership.Error);
        }
        if (ownershipExit == 0 && SteamOwnershipCheck.Problem(evidence, checkStarted) is { } problem) (ownershipExit, ownershipError) = (1, problem + ".");
        // Setup always asks Steam. A pass is recorded for the sessions to reuse for a while (SteamOwnershipRecord); a
        // failure ends the reuse of an earlier one.
        SteamOwnershipRecord.AfterCheck(install, evidence, ownershipExit == 0, checkStarted);
        if (ownershipExit != 0)
            throw new SetupStopped("The ownership check did not pass: " + (ownershipError.Length > 0 ? ownershipError : $"exit code {ownershipExit}.") +
                " Make sure Steam is running and signed in, then start setup again.");
        SetRow(SetupStep.Ownership, true, "Steam confirmed the game on this account.");
    }

    // Copies the game into the installation (from Steam's installed game or its depot download), each file verified.
    async Task CopyStep(InstallLayout install, SetupSource found)
    {
        SetRow(SetupStep.Copy, null, "Copying and verifying the game files...", running: true);
        await CopyGame(found.FromDepots ? null : found.Game!.Report.Source, found.SteamRoot, install);
        var copied = $"{found.Build.Files.Count} files ({found.Build.TotalBytes / 1e9:0.0} GB), each verified against Steam's hashes.";
        if (found.FromDepots && DeleteDownloadBox.IsChecked == true)
        {
            SetRow(SetupStep.Copy, null, copied + " Deleting Steam's download...", running: true);
            await Task.Run(() => SteamDepotDownload.Delete(found.SteamRoot, found.Build));
            copied += " Steam's download was deleted.";
        }
        DownloadCard.IsVisible = false;
        SetRow(SetupStep.Copy, true, copied);
    }

    async Task NetworkBlockStep(InstallLayout install)
    {
        SetRow(SetupStep.NetworkBlock, null, "Checking the network block...", running: true);
        if (await NetworkBlockInPlace(install)) { SetRow(SetupStep.NetworkBlock, true, "Already in place."); return; }
        if (LinuxNetworkBlock.Requested)
        {
            SetRow(SetupStep.NetworkBlock, null, "Checking that this session has no route to the internet...", running: true);
            await LinuxNetworkBlock.Install(install);
        }
        else
        {
            SetRow(SetupStep.NetworkBlock, null, "Windows asks for permission once to place the network block...", running: true);
            await InstallNetworkBlock(install);
        }
        SetRow(SetupStep.NetworkBlock, true, "The copied game can only reach this PC (its local backend). Your normal game is not affected.");
    }

    async Task ImportStep(InstallLayout install)
    {
        if (ImportBox.IsVisible && ImportBox.IsChecked == true)
        {
            var imported = await Task.Run(() => Migration.Run(install, Path.Combine(install.Temp, "no-repository"), EarlierGameData));
            // The folder keeps the project's old name; the message doesn't show it.
            SetRow(SetupStep.Import, true, imported.Copied.Count > 0 ? $"Imported {imported.Copied.Count} item(s) from the earlier HellForge setup's game data; the originals stay where they were."
                : "Nothing new to import.");
        }
        else SetRow(SetupStep.Import, true, "Skipped.");
    }

    [SupportedOSPlatform("windows")]
    async Task ShortcutsStep(InstallLayout install)
    {
        var made = CreateShortcuts(install);
        SetRow(SetupStep.Shortcuts, true, made.Count > 0 ? string.Join(", ", made) : "Skipped.");
        await SyncBundledMods(install); // with the game copied: the editor's icons and fonts, Hell Forge's icon
    }

    // Setup is done: the highlighted button on the right always moves forward, which here is Continue. Checking again
    // stays available under Settings > Installation.
    void ReadyToPlay()
    {
        SetupTitle.Text = "Ready to play";
        SetupStartButton.IsVisible = false;
        SetupBackButton.Content = "Continue";
        SetBackButtonPrimary(true);
        SetupBackButton.IsVisible = true;
        Nav.IsVisible = true;
    }

    // From Steam's installed game (source), or from its depot downloads when source is null.
    async Task CopyGame(string? source, string steamRoot, InstallLayout install)
    {
        CopyCard.IsVisible = true;
        SetupPauseButton.IsVisible = true;
        setupCancel = new();
        var token = setupCancel.Token;
        var clock = Stopwatch.StartNew();
        long shown = -1;
        var lastUpdate = TimeSpan.Zero;
        void Progress(long done, long total, string file)
        {
            // At most five updates a second reach the window.
            if (clock.Elapsed - lastUpdate < TimeSpan.FromMilliseconds(200) && done < total) return;
            lastUpdate = clock.Elapsed;
            Dispatcher.UIThread.Post(() =>
            {
                if (done == shown) return;
                shown = done;
                CopyProgress.Value = total > 0 ? done * 1000.0 / total : 0;
                var rate = clock.Elapsed.TotalSeconds > 1 ? done / clock.Elapsed.TotalSeconds / 1e6 : 0;
                CopyText.Text = $"{done / 1e9:0.0} of {total / 1e9:0.0} GB  ·  {rate:0} MB/s  ·  {file}";
            });
        }
        await Task.Run(() =>
        {
            if (source != null) GameCopier.Copy(source, install.Game, SupportedBuild.Current, Progress, token);
            else GameCopier.CopyFromDepots(steamRoot, install.Game, SupportedBuild.Current, Progress, token);
        }, token);
        CopyProgress.Value = 1000;
        CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
    }

    async Task DownpatchGame(string source, InstallLayout install, DownpatchPack pack)
    {
        CopyCard.IsVisible = true;
        SetupPauseButton.IsVisible = true;
        setupCancel = new();
        var token = setupCancel.Token;
        var clock = Stopwatch.StartNew();
        var lastUpdate = TimeSpan.Zero;
        var downloadStart = TimeSpan.Zero;

        void Update(string text, double fraction)
        {
            if (clock.Elapsed - lastUpdate < TimeSpan.FromMilliseconds(200) && fraction < 1) return;
            lastUpdate = clock.Elapsed;
            Dispatcher.UIThread.Post(() =>
            {
                CopyProgress.Value = fraction * 1000;
                CopyText.Text = text;
            });
        }

        using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        await Task.Run(async () =>
        {
            await Downpatcher.ApplyAsync(source, install.Game, install.Temp, pack, http, new DownpatchProgress(
                Download: (done, total) =>
                {
                    if (downloadStart == TimeSpan.Zero) downloadStart = clock.Elapsed;
                    var elapsed = clock.Elapsed - downloadStart;
                    var rate = elapsed.TotalSeconds > 1 ? done / elapsed.TotalSeconds / 1e6 : 0;
                    Update($"Downloading build patch: {done / 1e9:0.00} of {total / 1e9:0.0} GB  ·  {rate:0} MB/s",
                        total > 0 ? (double)done / total * 0.5 : 0);
                },
                Apply: (done, total, file) =>
                    Update($"Applying: {done / 1e9:0.0} of {total / 1e9:0.0} GB  ·  {file}",
                        0.5 + (total > 0 ? (double)done / total * 0.5 : 0))),
                cancellation: token);
        }, token);

        CopyProgress.Value = 1000;
        CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
    }

    // The download card: the console commands for the pinned depot manifests, and the download's progress, checked every
    // 2 s. When every depot is complete, setup runs again and copies from the download.
    void ShowDownload(string steamRoot, string have)
    {
        var build = SupportedBuild.Current;
        downloadSteamRoot = steamRoot;
        DownloadIntro.Text = $"Steam has {have}; this version of HellForge runs build {build.BuildId}. Steam can still download that exact build " +
            "for your account. Open the Steam console, paste each command below and press Enter, one at a time. Steam then downloads about " +
            $"{build.TotalBytes / 1e9:0} GB into {SteamDepotDownload.ContentFolder(steamRoot, build)} (your normal game is not touched). " +
            "Steam shows no progress in its library for this; the bar below does.";
        // Linux: Steam for Linux downloads into its own folder, which setup cannot read under Wine (Z: counts as a linked
        // path); linux/hellforge-linux.sh moves the download into this prefix's Steam folder.
        if (LinuxNetworkBlock.UnderWine)
            DownloadIntro.Text += " On Linux, Steam puts the download in its own folder instead, and the bar stays empty: once the console " +
                "says \"Depot download complete\" for every command, close HellForge and start it again with app/linux/hellforge-linux.sh, " +
                "which moves the download into this prefix. Then press Check again.";
        DownloadCommands.Children.Clear();
        foreach (var command in SteamDepotDownload.Commands(build))
        {
            var copy = new Button { Content = "Copy", Classes = { "ghost" }, Margin = new(8, 0, 0, 0) };
            var text = new TextBox { Text = command, IsReadOnly = true, FontFamily = new FontFamily("Consolas, monospace") };
            copy.Click += async (_, _) =>
            {
                if (TopLevel.GetTopLevel(this)?.Clipboard is { } clipboard) await clipboard.SetTextAsync(command);
                copy.Content = "Copied";
            };
            var row = new DockPanel();
            DockPanel.SetDock(copy, Dock.Right);
            row.Children.Add(copy);
            row.Children.Add(text);
            DownloadCommands.Children.Add(row);
        }
        DownloadCard.IsVisible = true;
        downloadTimer ??= new DispatcherTimer(TimeSpan.FromSeconds(2), DispatcherPriority.Background, async (_, _) => await CheckDownload());
        downloadTimer.Start();
        _ = CheckDownload();
    }

    bool checkingDownload;

    async Task CheckDownload()
    {
        if (checkingDownload || downloadSteamRoot == null) return;
        checkingDownload = true;
        try
        {
            var root = downloadSteamRoot;
            var states = await Task.Run(() => SteamDepotDownload.Status(root, SupportedBuild.Current));
            long done = states.Sum(s => s.Done), total = states.Sum(s => s.Total);
            DownloadProgress.Value = total > 0 ? done * 1000.0 / total : 0;
            DownloadText.Text = string.Join("   ", states.Select(s => $"depot {s.Depot.Id}: {(s.Complete ? "done" : $"{s.Done / 1e9:0.0} of {s.Total / 1e9:0.0} GB")}"));
            // Near the end (a few files left, which the rounded GB cannot show), the files it still waits for, by name: a file
            // Steam did not write, or wrote at another size (another build's), would otherwise hold the setup with no word.
            var left = states.Where(s => !s.Complete).SelectMany(s => s.Waiting).ToList();
            if (left.Count is > 0 and <= 5)
                DownloadText.Text += "\nWaiting for " + string.Join(", ", left.Select(f => f.Path + (f.Have < 0 ? " (not there yet)"
                    : f.Have > f.Size ? $" ({f.Have:N0} bytes, larger than this build's {f.Size:N0}: another build's file; delete it and run the download_depot command again)"
                    : $" ({f.Have:N0} of {f.Size:N0} bytes)")));
            if (states.All(s => s.Complete))
            {
                downloadTimer?.Stop();
                DownloadText.Text = "Download complete. Copying it into this installation...";
                if (!setupRunning) await RunSetup();
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { DownloadText.Text = "Could not check the download: " + Privacy.Scrub(error.Message); }
        finally { checkingDownload = false; }
    }

    static void OpenSteamConsole()
    {
        try { Process.Start(new ProcessStartInfo("steam://open/console") { UseShellExecute = true }); }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException) { }
    }

    // HellForge's network block is placed in the Windows Filtering Platform, carried by the Base Filtering Engine (BFE)
    // service. Unlike the old firewall rules, it does not need Windows Firewall itself to be on (a security suite that
    // switches the firewall off no longer stops setup). Checked first, before the long copy: exit 3 only when BFE is
    // off, which is rare since it is a core Windows service.
    async Task CheckFilteringEngine(InstallLayout install)
    {
        var check = await RunPowerShell("-Command \"if((Get-Service BFE).Status -ne 'Running'){exit 3}; exit 0\"", install);
        if (check.ExitCode == 3)
            throw new SetupStopped("The Windows Base Filtering Engine (BFE) service is off. HellForge needs it to keep its copy of the game off the internet. " +
                "Turn it back on (Services > Base Filtering Engine, or run 'net start BFE' as administrator), then start setup again.");
    }

    // The block leaves a marker when it was placed; reading WFP filters back needs administrator rights, so setup
    // trusts the marker rather than re-reading them without a prompt.
    // On Windows its check program too, which every session runs (antivirus programs sometimes remove it): without it the
    // block is placed again, which makes it again.
    static Task<bool> NetworkBlockInPlace(InstallLayout install) => Task.FromResult(LinuxNetworkBlock.InPlace(install.Root));

    // The mods HellForge ships (BundledMods: the in-game editor, Hell Forge) are in mods/ from the start, not only once a
    // session has run. In the background: the editor's catalogue takes a moment to write.
    // PLAY waits for it: the session's own sync would write the same files.
    bool bundledSyncing;

    async Task SyncBundledMods(InstallLayout install)
    {
        if (bundledSyncing || runner.IsRunning) return;
        bundledSyncing = true;
        UpdateButton();
        var problems = await Task.Run(() =>
        {
            try { return BundledMods.Sync(install); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { return [error.Message]; }
        });
        bundledSyncing = false;
        UpdateButton();
        foreach (var problem in problems) Append(problem, Orange);
        if (ModsPage.IsVisible) RefreshMods();
    }

    // Uninstall: what HellForge put outside its folder goes first (the network block, elevated, since it outlives the
    // folder; the shortcuts), then a small script outside the folder deletes it once this launcher has exited. Nothing
    // is deleted when Windows' permission is declined, or while anything from the installation still runs.
    void AskUninstall(InstallLayout install)
    {
        UninstallWarning.Text = $"This deletes {install.Root} and everything in it: the game copy (about 24 GB), your HellForge " +
            "profiles and progress, every mod in its mods folder (your own too), saves and logs. Copy anything you want to keep first. " +
            "HellForge closes when it is done.";
        UninstallConfirm.IsVisible = true;
        UninstallText.Text = "";
    }

    async Task Uninstall(InstallLayout install)
    {
        UninstallConfirm.IsVisible = false;
        if (runner.IsRunning || setupRunning) { UninstallText.Text = "Stop the session (or setup) first."; return; }
        // Linux (Wine/Proton): the removal runs Windows PowerShell, which a Wine prefix has only as a stub that does nothing.
        if (LinuxNetworkBlock.UnderWine) { UninstallText.Text = "On Linux, remove HellForge by hand: see \"Removing HellForge\" in docs/linux.md."; return; }
        var running = Process.GetProcesses().Where(p => p.Id != Environment.ProcessId && IsFrom(p, install.Root)).Select(p => p.ProcessName).Distinct().ToList();
        if (running.Count > 0) { UninstallText.Text = "Close these first: " + string.Join(", ", running) + "."; return; }
        try { UninstallSafety(install.Root); }
        catch (InvalidDataException refused) { UninstallText.Text = Privacy.Scrub(refused.Message); return; }
        UninstallButton.IsEnabled = false;
        UninstallText.Text = "Removing the network block: Windows asks for permission...";
        try { await InstallNetworkBlock(install, uninstall: true); }
        catch (Exception error) when (error is SetupStopped or IOException or Win32Exception or InvalidOperationException or JsonException)
        {
            UninstallText.Text = (error is Win32Exception ? "Windows did not give permission" : Privacy.Scrub(error.Message)) + ": nothing was removed.";
            UninstallButton.IsEnabled = true;
            return;
        }
        if (OperatingSystem.IsWindows()) RemoveShortcuts(install);
        var script = Path.Combine(Path.GetTempPath(), $"hellforge-uninstall-{Guid.NewGuid():N}.ps1");
        File.WriteAllText(script,
            "param([string]$Root,[int]$Launcher)\n" +
            "$ErrorActionPreference='SilentlyContinue'\n" +
            "Wait-Process -Id $Launcher -Timeout 120\n" +
            "for($i=0;$i -lt 30 -and (Test-Path -LiteralPath $Root);$i++){Remove-Item -LiteralPath $Root -Recurse -Force;if(Test-Path -LiteralPath $Root){Start-Sleep -Seconds 1}}\n" +
            "Remove-Item -LiteralPath $PSCommandPath -Force\n");
        Process.Start(new ProcessStartInfo(PowerShell, $"-NoProfile -ExecutionPolicy Bypass -File {Arg(script)} -Root {Arg(install.Root)} -Launcher {Environment.ProcessId}")
        { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetTempPath() });
        Close();
    }

    // Deletes only a HellForge installation: its marker, no links in its path, and never a drive, the user's folder or
    // one of its known folders.
    static void UninstallSafety(string root)
    {
        root = LocalInstall.FullPath(root);
        if (!new InstallLayout(root).Exists) throw new InvalidDataException("This folder is not a HellForge installation; nothing was removed.");
        LocalInstall.NoLinks(root);
        var keep = new[] { Path.GetPathRoot(root), Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.Windows) };
        if (keep.Any(folder => !string.IsNullOrEmpty(folder) && string.Equals(LocalInstall.FullPath(folder), root, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException($"HellForge is installed directly in {root}; delete its files by hand instead.");
    }

    // The Desktop and Start menu shortcuts that start this installation.
    [SupportedOSPlatform("windows")]
    static void RemoveShortcuts(InstallLayout install)
    {
        var type = Type.GetTypeFromProgID("WScript.Shell");
        if (type == null) return;
        dynamic shell = Activator.CreateInstance(type)!;
        foreach (var folder in new[] { Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Environment.GetFolderPath(Environment.SpecialFolder.Programs) })
        {
            var link = Path.Combine(folder, "HellForge.lnk");
            if (string.IsNullOrEmpty(folder) || !File.Exists(link)) continue;
            try
            {
                string target = shell.CreateShortcut(link).TargetPath;
                if (LocalInstall.Within(target, install.Root)) File.Delete(link);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Runtime.InteropServices.COMException) { }
        }
    }

    static bool IsFrom(Process process, string root)
    {
        try { return process.MainModule?.FileName is { } file && LocalInstall.Within(file, root); }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException) { return false; }
        finally { process.Dispose(); }
    }

    async Task InstallNetworkBlock(InstallLayout install, bool uninstall = false)
    {
        var report = Path.Combine(install.Logs, "setup-network-block.json");
        if (File.Exists(report)) File.Delete(report);
        var start = new ProcessStartInfo(PowerShell, $"-NoProfile -ExecutionPolicy Bypass -File {Arg(ScriptPath("Install-WfpBlock.ps1"))} " +
            $"-InstallRoot {Arg(install.Root)} -ReportPath {Arg(report)}" + (uninstall ? " -Uninstall" : $" -BackendPort {install.ComputedBackendPort}"))
        { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = install.Root };
        await Task.Run(() =>
        {
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows PowerShell did not start.");
            process.WaitForExit();
        });
        var state = File.Exists(report) ? JsonDocument.Parse(File.ReadAllText(report)).RootElement : default;
        if (state.ValueKind != JsonValueKind.Object || SessionFiles.Text(state, "status") != (uninstall ? "uninstalled" : "installed"))
            throw new SetupStopped((uninstall ? "The network block was not removed: " : "The network block was not installed: ") +
                (state.ValueKind == JsonValueKind.Object ? SessionFiles.Text(state, "failure") : "no report was written."));
    }

    // Shortcuts from before the rename ("HD2 Beyond The Stars.lnk") into this installation become HellForge ones.
    [SupportedOSPlatform("windows")]
    public static void RenameLegacyShortcuts(InstallLayout install)
    {
        var target = Path.Combine(install.Root, "HellForge.exe");
        if (!File.Exists(target)) return;
        foreach (var folder in new[] { Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Environment.GetFolderPath(Environment.SpecialFolder.Programs) })
        {
            var legacy = Path.Combine(folder, "HD2 Beyond The Stars.lnk");
            if (string.IsNullOrEmpty(folder) || !File.Exists(legacy)) continue;
            try
            {
                var type = Type.GetTypeFromProgID("WScript.Shell");
                if (type == null) return;
                dynamic shell = Activator.CreateInstance(type)!;
                dynamic old = shell.CreateShortcut(legacy);
                if (!((string)old.TargetPath).StartsWith(install.Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
                dynamic shortcut = shell.CreateShortcut(Path.Combine(folder, "HellForge.lnk"));
                shortcut.TargetPath = target;
                shortcut.WorkingDirectory = install.Root;
                shortcut.Description = "HellForge";
                shortcut.Save();
                File.Delete(legacy);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Runtime.InteropServices.COMException) { }
        }
    }

    [SupportedOSPlatform("windows")]
    List<string> CreateShortcuts(InstallLayout install)
    {
        var made = new List<string>();
        var target = Path.Combine(install.Root, "HellForge.exe");
        if (!File.Exists(target)) return made;
        foreach (var (box, folder, name) in new[] {
            (DesktopShortcutBox, Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Desktop"),
            (StartMenuShortcutBox, Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Start menu") })
        {
            if (box.IsChecked != true || string.IsNullOrEmpty(folder)) continue;
            var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("Windows Script Host is unavailable.");
            dynamic shell = Activator.CreateInstance(type)!;
            dynamic shortcut = shell.CreateShortcut(Path.Combine(folder, "HellForge.lnk"));
            shortcut.TargetPath = target;
            shortcut.WorkingDirectory = install.Root;
            shortcut.Description = "HellForge";
            shortcut.Save();
            made.Add(name);
        }
        return made;
    }

    string ScriptPath(string name) => Path.Combine(paths.ScriptsFolder, name);

    static string PowerShell => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");

    // A PowerShell single-quoted string, for paths inside a -Command script.
    static string Quote(string value) => "'" + value.Replace("'", "''") + "'";

    // A Windows command-line argument, for powershell.exe -File and its parameters: Windows splits the command line, not
    // PowerShell, so single quotes do not group (a path with spaces broke the ownership check, 2026-10-14). Paths cannot
    // contain double quotes; a trailing backslash is doubled so it does not escape the closing quote.
    static string Arg(string value) => "\"" + (value.EndsWith('\\') ? value + "\\" : value) + "\"";

    // Runs Windows PowerShell hidden with the installation's temp folder; returns the exit code and the last error line.
    static Task<(int ExitCode, string Error)> RunPowerShell(string arguments, InstallLayout install) => Task.Run(() =>
    {
        var start = new ProcessStartInfo(PowerShell, "-NoProfile -ExecutionPolicy Bypass " + arguments)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true, WorkingDirectory = install.Root
        };
        start.Environment["TEMP"] = start.Environment["TMP"] = Directory.CreateDirectory(install.Temp).FullName;
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows PowerShell did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        _ = output.Result;
        var last = error.Split('\n').Select(line => line.Trim()).FirstOrDefault(line => line.Length > 0 && !line.StartsWith("At ", StringComparison.Ordinal)) ?? "";
        return (process.ExitCode, last);
    });

    sealed class SetupStopped(string message) : Exception(message);

    // Setup waits for something outside it (Steam downloading the supported build); not a failure.
    sealed class SetupWaiting(string message) : Exception(message);

    // An error as setup shows it. The build patch's download (the only download setup makes): the cause, not "see inner
    // exception".
    static string Describe(Exception error) => error is HttpRequestException download
        ? $"Could not download the build patch: {download.InnerException?.GetBaseException().Message ?? download.Message} " +
          "Something on this PC may be blocking or inspecting HTTPS (an antivirus web shield, a VPN or a proxy); allow it, then try again."
        : error.Message;
}
