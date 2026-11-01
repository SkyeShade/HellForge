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
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// Setting up an installation: find HELLDIVERS 2 in Steam, confirm ownership, copy the game into the installation,
// place its network block (WFP filters), optionally import earlier HellForge game data and create shortcuts. When Steam's own copy is
// another build (Arrowhead patched the game) or missing, the player signs in to Steam and setup downloads the supported
// build from Steam's servers for their account, copying what Steam's install already holds (HellForge.SteamDownload). When
// that cannot be done, the Steam client downloads the build's depots through its console (SteamDepotDownload) and the game
// is copied from there. Shown instead of the other pages until the game is copied; Settings > Installation runs it again
// to check and repair.
public partial class MainWindow
{
    enum SetupStep { Find, Ownership, Copy, NetworkBlock, Import, Shortcuts }
    static readonly string[] SetupStepNames =
        ["Find HELLDIVERS 2 in Steam", "Confirm you own the game", "Copy the game into this folder", "Block the copy from the internet",
            "Import earlier game data", "Create shortcuts"];

    readonly List<(Border Dot, TextBlock Detail)> setupRows = [];
    CancellationTokenSource? setupCancel;
    bool setupRunning;
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
    // console download holds it, and whether setup downloads it from Steam's servers (copying what Game holds).
    sealed record SetupSource(SupportedBuild Build, string SteamRoot, SteamGame? Game, bool FromDepots, bool FromSteamServers);

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
            // When downloading from Steam, that comes first, so the supported build's steam_api64.dll is there for the
            // ownership check (the newer build's DLL may not work with the probe).
            if (found.FromSteamServers) { step = SetupStep.Copy; await SteamDownloadStep(install, found); }
            var steamApi = SteamApiPath(install, found);
            step = SetupStep.Ownership;
            await OwnershipStep(install, steamApi);
            if (!found.FromSteamServers) { step = SetupStep.Copy; await CopyStep(install, found); }
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

    // Finds HELLDIVERS 2 for setup: the installed supported build or Steam's console download of it; else the download
    // from Steam's servers, copying from Steam's installed game when there is one. Stops while that game is busy.
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
        SteamGame? game;
        string? unusable = null;
        try { game = await Task.Run(() => SteamLocator.FindGame(steamRoot)); }
        catch (Exception error) when (error is InvalidDataException or IOException) { game = null; unusable = error.Message; }
        if (game is { SupportedBuild: true }) { SetRow(SetupStep.Find, true, $"Build {game.Report.Pin.BuildId} in {game.Report.Source}"); return new(build, steamRoot, game, false, false); }
        if (await Task.Run(() => SteamDepotDownload.IsComplete(steamRoot, build)))
        {
            SetRow(SetupStep.Find, true, $"Build {build.BuildId}, downloaded by Steam into {Path.GetDirectoryName(SteamDepotDownload.DepotFolder(steamRoot, build, build.Depots[0].Id))}");
            return new(build, steamRoot, game, true, false);
        }
        // the game is there but busy (Steam updating it, the game running): that passes, so no download is offered.
        // Under Wine it does not pass: Steam's Linux libraries are on the Z: drive, which counts as a linked path
        // ("Linked/reparse paths are not supported: Z:\", GitHub issue 1), so everything is downloaded there.
        if (game == null && unusable != null && !LinuxNetworkBlock.UnderWine)
            throw new SetupStopped($"HELLDIVERS 2 is in Steam, but setup cannot use it right now: {unusable}");
        // Linux (Wine/Proton): setup runs inside HellForge's network namespace, which has no route out, so Steam's
        // servers are out of its reach; the Steam client downloads the build through its console instead.
        if (LinuxNetworkBlock.Requested)
        {
            ShowDownload(steamRoot, SteamHas(game, unusable));
            throw new SetupWaiting($"Steam has {SteamHas(game, unusable)}, but this version of HellForge needs build {build.BuildId}. Setup " +
                "runs inside HellForge's network namespace and cannot reach Steam's servers itself; the Steam client can download that exact " +
                "build for your account: follow the steps below. Setup continues by itself when the download is complete.");
        }
        SetRow(SetupStep.Find, true, game != null
            ? $"Build {game.Report.Pin.BuildId} in {game.Report.Source}. Steam sends build {build.BuildId} for your account; what your install already has is copied, not downloaded."
            : $"No HELLDIVERS 2 setup can copy from. Steam sends build {build.BuildId} (about {build.TotalBytes / 1e9:0} GB) for your account.");
        return new(build, steamRoot, game, false, true);
    }

    // What Steam has, in the console card's words.
    static string SteamHas(SteamGame? game, string? unusable = null) => game != null ? $"HELLDIVERS 2 build {game.Report.Pin.BuildId}"
        : unusable != null && LinuxNetworkBlock.UnderWine ? "a HELLDIVERS 2 setup cannot read from inside Wine" : "no HELLDIVERS 2 setup can copy from";

    // Gets the supported build into the installation the way the player chooses on the card (SteamSignInFlow), on the copy
    // step's row. A full drive asks for space; a refused or failed download shows the Steam app's console download.
    // Files already written are kept.
    async Task SteamDownloadStep(InstallLayout install, SetupSource found)
    {
        var build = found.Build;
        // Only the screen order changes: each step keeps its own row (setupRows by step), so the download's progress
        // and errors show on the copy row. (Swapping the rows here as well put them on "Confirm you own the game",
        // and a second run swapped the screen order back.)
        var copyRow = (Control)setupRows[(int)SetupStep.Copy].Dot.Parent!;
        var ownershipRow = (Control)setupRows[(int)SetupStep.Ownership].Dot.Parent!;
        if (SetupSteps.Children.IndexOf(copyRow) > SetupSteps.Children.IndexOf(ownershipRow))
        {
            SetupSteps.Children.Remove(copyRow);
            SetupSteps.Children.Insert(SetupSteps.Children.IndexOf(ownershipRow), copyRow);
        }
        SetRow(SetupStep.Copy, null, $"Choose below how to get build {build.BuildId} from Steam.", running: true);
        setupCancel = new();
        var token = setupCancel.Token;
        SetupPauseButton.Content = "Cancel";
        SetupPauseButton.IsVisible = true;
        try
        {
            SetRow(SetupStep.Copy, true, await GetBuildFromSteam(install, found, token));
        }
        catch (SteamAppChosen)
        {
            // the player's choice: the Steam app downloads it through its console, and setup copies it from there
            ShowDownload(found.SteamRoot, SteamHas(found.Game));
            throw new SetupWaiting($"The Steam app downloads build {build.BuildId} for your account: follow the steps below. Setup " +
                "continues by itself when the download is complete, and copies it into this folder.");
        }
        catch (Exception error) when (DownloadFailures.Describe(error, build.BuildId) is { } failure)
        {
            if (failure.Fallback == DownloadFallback.SteamConsole) ShowDownload(found.SteamRoot, SteamHas(found.Game));
            throw new SetupWaiting(Privacy.Scrub(failure.Message));
        }
    }

    // The steam_api64.dll the ownership check uses: the downloaded copy, Steam's depot download, or the installed game.
    static string SteamApiPath(InstallLayout install, SetupSource found) => found.FromSteamServers
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
        SignInCard.IsVisible = false;
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
        SetupPauseButton.Content = "Pause copying";
        SetupPauseButton.IsVisible = true;
        setupCancel = new();
        var token = setupCancel.Token;
        var clock = Stopwatch.StartNew();
        var progress = GameFilesProgress(clock);
        await Task.Run(() =>
        {
            if (source != null) GameCopier.Copy(source, install.Game, SupportedBuild.Current, progress, token);
            else GameCopier.CopyFromDepots(steamRoot, install.Game, SupportedBuild.Current, progress, token);
        }, token);
        CopyProgress.Value = 1000;
        CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
    }

    // A copy's (or a move's) progress on the game files card: bytes done, bytes total, the file. At most five updates a
    // second reach the window.
    Action<long, long, string> GameFilesProgress(Stopwatch clock)
    {
        long shown = -1;
        var lastUpdate = TimeSpan.Zero;
        return (done, total, file) =>
        {
            if (clock.Elapsed - lastUpdate < TimeSpan.FromMilliseconds(200) && done < total) return;
            lastUpdate = clock.Elapsed;
            Dispatcher.UIThread.Post(() =>
            {
                if (done == shown) return;
                shown = done;
                CopyCard.IsVisible = true;
                CopyProgress.Value = total > 0 ? done * 1000.0 / total : 0;
                var rate = clock.Elapsed.TotalSeconds > 1 ? done / clock.Elapsed.TotalSeconds / 1e6 : 0;
                CopyText.Text = $"{done / 1e9:0.0} of {total / 1e9:0.0} GB  ·  {rate:0} MB/s  ·  {file}";
            });
        };
    }

    // The download from Steam's servers into the installation, its progress on the game files card. Returns the bytes
    // downloaded and copied from Steam's install.
    async Task<(long Downloaded, long Copied)> DownloadFromSteam(SteamDepotSession steam, InstallLayout install, SetupSource found, CancellationToken token)
    {
        CopyCard.IsVisible = true;
        SetupPauseButton.Content = "Pause download";
        SetupPauseButton.IsVisible = true;
        var clock = Stopwatch.StartNew();
        var lastUpdate = TimeSpan.Zero;
        AssembleProgress? last = null;
        void Progress(AssembleProgress p)
        {
            last = p;
            if (clock.Elapsed - lastUpdate < TimeSpan.FromMilliseconds(200) && p.Done < p.Total) return;
            lastUpdate = clock.Elapsed;
            var rate = clock.Elapsed.TotalSeconds > 1 ? p.Downloaded / clock.Elapsed.TotalSeconds / 1e6 : 0;
            Dispatcher.UIThread.Post(() =>
            {
                CopyProgress.Value = p.Total > 0 ? p.Done * 1000.0 / p.Total : 0;
                CopyText.Text = $"{p.Done / 1e9:0.0} of {p.Total / 1e9:0.0} GB  ·  {p.Downloaded / 1e9:0.00} GB downloaded ({rate:0} MB/s), " +
                    $"{p.Copied / 1e9:0.0} GB copied from your install  ·  {p.File}";
            });
        }
        void Status(string text) => Dispatcher.UIThread.Post(() => CopyText.Text = text);
        var job = new BuildDownloadJob(found.Build, install.Game, found.SteamRoot, found.Game);
        await Task.Run(() => SteamBuildDownload.DownloadAsync(steam, job, Status, Progress, token), token);
        CopyProgress.Value = 1000;
        CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
        return (last?.Downloaded ?? 0, last?.Copied ?? 0);
    }

    // The Steam app's way, on the card (SteamSignInFlow): the console commands for the pinned depot manifests, and the
    // download's progress on the game files card, checked every 2 s. When every depot is complete, setup runs again and
    // copies from the download.
    void ShowDownload(string steamRoot, string have)
    {
        var build = SupportedBuild.Current;
        downloadSteamRoot = steamRoot;
        WireSignIn();
        downloadChoice = DownloadChoice.SteamClient;
        ShowChoice();
        SignInIntro.Text = OfferedChoices.Count > 1
            ? $"Steam has another build of HELLDIVERS 2 than the one this HellForge runs (build {build.BuildId}). Steam can send that build " +
              "to your own account, under your own licence; your normal game and Steam stay as they are."
            : $"Steam has another build of HELLDIVERS 2 than the one this HellForge runs (build {build.BuildId}). On Linux, setup runs " +
              "inside HellForge's network namespace and cannot reach Steam itself, so the Steam app downloads that build for your account.";
        SignInCard.IsVisible = true;
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
            ShowDepotProgress(states, "Steam");
            if (states.All(s => s.Complete))
            {
                downloadTimer?.Stop();
                CopyText.Text = "Download complete. Copying it into this installation...";
                if (!setupRunning) await RunSetup();
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { CopyText.Text = "Could not check the download: " + Privacy.Scrub(error.Message); }
        finally { checkingDownload = false; }
    }

    // A depot download's progress (the Steam app's or SteamCMD's, by who) on the game files card, where every way's
    // progress shows. Near the end (a few files left, which the rounded GB cannot show) it names the files it still waits
    // for: a file not written, or written at another size (another build's), would otherwise hold the setup with no word.
    void ShowDepotProgress(IReadOnlyList<SteamDepotDownload.DepotState> states, string who)
    {
        long done = states.Sum(s => s.Done), total = states.Sum(s => s.Total);
        CopyCard.IsVisible = true;
        CopyProgress.Value = total > 0 ? done * 1000.0 / total : 0;
        CopyText.Text = $"{who} downloads: " + string.Join("   ", states.Select(s => $"depot {s.Depot.Id}: {(s.Complete ? "done" : $"{s.Done / 1e9:0.0} of {s.Total / 1e9:0.0} GB")}"));
        var left = states.Where(s => !s.Complete).SelectMany(s => s.Waiting).ToList();
        if (left.Count is > 0 and <= 5)
            CopyText.Text += "\nWaiting for " + string.Join(", ", left.Select(f => f.Path + (f.Have < 0 ? " (not there yet)"
                : f.Have > f.Size ? $" ({f.Have:N0} bytes, larger than this build's {f.Size:N0}: another build's file; delete it and run the download_depot command again)"
                : $" ({f.Have:N0} of {f.Size:N0} bytes)")));
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

    // An error as setup shows it. A failed download: the cause, not "see inner exception" (the Steam download's own
    // failures are described by DownloadFailures).
    static string Describe(Exception error) => error is HttpRequestException download
        ? $"A download failed: {download.InnerException?.GetBaseException().Message ?? download.Message} " +
          "Something on this PC may be blocking or inspecting HTTPS (an antivirus web shield, a VPN or a proxy); allow it, then try again."
        : error.Message;
}
