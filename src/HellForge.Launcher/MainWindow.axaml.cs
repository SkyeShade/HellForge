using System.Collections.ObjectModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using Avalonia;
using HellForge.Core;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Shape = Avalonia.Controls.Shapes.Path;

namespace HellForge.Launcher;

public sealed class LogLine(string text, IBrush brush)
{
    public string Text { get; } = text;
    public IBrush Brush { get; } = brush;
}

enum Phase { Idle, Busy, Attention, Running, Ended, Failed }

// Front end for the engine's session runner, HellForge.Session.exe play (see SessionRunner). Polls the session files once a second
// and shows status, steps, the local profile and live backend requests.
public partial class MainWindow : Window
{
    const int MaxLogLines = 2000;
    static readonly string[] StepNames = ["Build mods", "Verify files", "Network block", "Launching", "Running"];
    const int BuildStep = 0, VerifyStep = 1, NetworkStep = 2, LaunchStep = 3, RunStep = 4;

    // HellForge's colours (App.axaml): its icon's red for steps done and actions, the editor's gold for work under way.
    static readonly IBrush Accent = Solid("#E5392B"), Gold = Solid("#FFC72C"), Green = Solid("#5FD08A"), Red = Solid("#FF6054"),
        Orange = Solid("#FFA235"), Grey = Solid("#9AA0A8"), Light = Solid("#C9CDD3"), Text = Solid("#E6E6E0"),
        Pending = Solid("#3A3E46"), White = Solid("#FFFFFF"), LauncherLine = Solid("#F08A7E"), ActiveFill = Solid("#26E5392B");

    readonly LauncherPaths paths;
    readonly SessionRunner runner = new();
    readonly LineTail requests = new();
    readonly ObservableCollection<LogLine> lines = [];
    readonly DispatcherTimer poll = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly List<(Border Circle, TextBlock Number, Shape Glyph, TextBlock Label)> steps = [];
    readonly List<ProgressBar> connectors = []; /* each step's track, to the next step: its progress */
    readonly List<ProfileTemplate> templates;
    List<LocalProfile> profiles = [];
    bool loadingProfiles;
    SelectableTextBlock? runPathText, profilePathText;
    TaskCompletionSource<bool>? confirm;
    string? runDirectory;
    DateTime launchedUtc;
    // The first launch of a new installation can stop in the cryo pod: the client asks for /SeasonPass and then never
    // reaches its Pes requests (HellForge's startup research notes). The next launch goes through.
    static readonly TimeSpan IntroStallAfter = TimeSpan.FromSeconds(45);
    DateTime? seasonPassUtc;
    bool reachedShip;
    bool modProblemsShown; // this session's mod problems (result.json mods.problems) went to the console
    bool stopRequested, closeConfirmed, scrollQueued;
    (int Index, bool Failed) shownStep = (-2, false);
    double? stepProgress; /* the current step's progress (0 .. 1), or null while it cannot be told */

    public MainWindow() : this(LauncherPaths.Find() ?? LauncherPaths.ForRepository(AppContext.BaseDirectory)) { }

    internal MainWindow(LauncherPaths paths)
    {
        this.paths = paths;
        InitializeComponent();
        BuildSteps();
        BuildPaths();
        ConsoleList.ItemsSource = lines;
        templates = ProfileStore.LoadTemplates(paths);
        TemplateCombo.ItemsSource = templates;
        TemplateCombo.SelectedItem = templates.FirstOrDefault(t => t.Name == ProfileStore.FallbackTemplate) ?? templates.FirstOrDefault();
        ShowTemplateDescription();
        LoadProfiles(ProfileStore.LoadSelected(paths));

        InitializeMods();
        InitializeWeaponLevels();
        InitializeSettingsImport();
        Nav.SelectionChanged += (_, _) => ShowPage(Nav.SelectedIndex);
        DiscordButton.Click += (_, _) => OpenLink(DiscordInvite);
        ToolTip.SetTip(DiscordButton, "Support, news and mods: the HellForge Discord (" + DiscordInvite + ")");
        PlayButton.Click += (_, _) => { if (runner.IsRunning) RequestStop(); else Start(); };
        ProfileCombo.SelectionChanged += (_, _) => OnProfileSelected();
        TemplateCombo.SelectionChanged += (_, _) => ShowTemplateDescription();
        NewProfileButton.Click += (_, _) => ShowNewProfileForm(!NewProfileForm.IsVisible);
        CancelProfileButton.Click += (_, _) => ShowNewProfileForm(false);
        CreateProfileButton.Click += (_, _) => CreateProfile();
        NewProfileName.TextChanged += (_, _) => NewProfileError.IsVisible = false;
        NewProfileName.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter) CreateProfile();
            else if (e.Key == Key.Escape) ShowNewProfileForm(false);
        };
        OpenProfileButton.Click += (_, _) => OpenFolder(Selected.Folder);
        ConsoleLogsButton.Click += (_, _) => OpenFolder(runDirectory ?? paths.RunsFolder);
        ShareLogsButton.Click += (_, _) => SaveLogsToDesktop();
        SettingsLogsButton.Click += (_, _) => OpenFolder(runDirectory ?? paths.RunsFolder);
        OpenRootButton.Click += (_, _) => OpenFolder(paths.Root);
        ClearButton.Click += (_, _) => lines.Clear();
        AutoScroll.IsCheckedChanged += (_, _) => QueueScroll();
        ConfirmStop.Click += (_, _) => confirm?.TrySetResult(true);
        ConfirmCancel.Click += (_, _) => confirm?.TrySetResult(false);
        poll.Tick += (_, _) => Refresh();
        LocalPlayFabBox.IsChecked = ProfileStore.LoadLocalPlayFab(paths);
        LocalPlayFabBox.IsCheckedChanged += (_, _) => ProfileStore.SaveLocalPlayFab(paths, LocalPlayFabBox.IsChecked == true);
        SdkShimsBox.IsChecked = ProfileStore.LoadSdkShims(paths);
        SdkShimsBox.IsCheckedChanged += (_, _) => ProfileStore.SaveSdkShims(paths, SdkShimsBox.IsChecked == true);
        // Play with strangers lives in the installation's mods settings (ModSettings), which the backend reads: off by default.
        StrangersBox.IsEnabled = paths.ModsFolder != null;
        StrangersBox.IsChecked = paths.ModsFolder != null && ModSettings.PlayWithStrangers(paths.ModsFolder);
        StrangersWarning.IsVisible = StrangersBox.IsChecked == true;
        StrangersBox.IsCheckedChanged += (_, _) =>
        {
            StrangersWarning.IsVisible = StrangersBox.IsChecked == true;
            if (paths.ModsFolder == null) return;
            try { ModSettings.SetPlayWithStrangers(paths.ModsFolder, StrangersBox.IsChecked == true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save Play with strangers: " + error.Message, Red); }
        };
        // The sandbox (docs/steam-bridge.md): off unless chosen, and only with an engine built with the Steam bridge.
        var steamBridge = SessionRunner.HasSteamBridge(paths);
        SandboxBox.IsEnabled = steamBridge;
        SandboxBox.IsChecked = steamBridge && ProfileStore.LoadSandbox(paths);
        SandboxUnavailable.IsVisible = !steamBridge;
        SandboxBox.IsCheckedChanged += (_, _) => ProfileStore.SaveSandbox(paths, SandboxBox.IsChecked == true);
        var war = ProfileStore.LoadWar(paths);
        WarLiveRadio.IsChecked = war == ProfileStore.WarLive;
        WarFrozenRadio.IsChecked = war == ProfileStore.WarFrozen;
        WarOpenRadio.IsChecked = war == ProfileStore.WarOpen;
        foreach (var (radio, mode) in new[] { (WarLiveRadio, ProfileStore.WarLive), (WarFrozenRadio, ProfileStore.WarFrozen), (WarOpenRadio, ProfileStore.WarOpen) })
            radio.IsCheckedChanged += (_, _) => { if (radio.IsChecked == true) ProfileStore.SaveWar(paths, mode); };
        // the all-planets war's subfaction seed: typed (a whole number) or rolled, saved as it changes
        SubfactionSeedBox.Text = ProfileStore.LoadSubfactionSeed(paths).ToString(System.Globalization.CultureInfo.InvariantCulture);
        SubfactionSeedBox.TextChanged += (_, _) =>
        {
            if (uint.TryParse(SubfactionSeedBox.Text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var seed))
                ProfileStore.SaveSubfactionSeed(paths, seed);
        };
        SubfactionRandomiseButton.Click += (_, _) =>
            SubfactionSeedBox.Text = ((uint)System.Security.Cryptography.RandomNumberGenerator.GetInt32(1, int.MaxValue)).ToString(System.Globalization.CultureInfo.InvariantCulture);
        EngineLogBox.IsChecked = ProfileStore.LoadEngineLog(paths);
        EngineLogBox.IsCheckedChanged += (_, _) => ProfileStore.SaveEngineLog(paths, EngineLogBox.IsChecked == true);
        CrashDumpBox.IsChecked = ProfileStore.LoadCrashDump(paths);
        CrashDumpBox.IsCheckedChanged += (_, _) => ProfileStore.SaveCrashDump(paths, CrashDumpBox.IsChecked == true);
        LaunchOptionsBox.Text = ProfileStore.LoadLaunchOptions(paths);
        ShowLaunchOptionsProblem();
        LaunchOptionsBox.TextChanged += (_, _) =>
        {
            ProfileStore.SaveLaunchOptions(paths, LaunchOptionsBox.Text ?? "");
            ShowLaunchOptionsProblem();
        };

        // Development: HELLFORGE_LAUNCHER_SHOT=<file.png> shows the play page as a session builds its mods (2 of 5), saves
        // it as a picture and closes (a look at the launcher without a session). HELLFORGE_LAUNCHER_SHOT_PAGE=<1-5> shows that
        // page instead (3: Mods).
        if (Environment.GetEnvironmentVariable("HELLFORGE_LAUNCHER_SHOT") is { Length: > 0 } shot)
            Opened += async (_, _) =>
            {
                if (int.TryParse(Environment.GetEnvironmentVariable("HELLFORGE_LAUNCHER_SHOT_PAGE"), out var page) && page is >= 1 and <= 5)
                    Nav.SelectedIndex = page - 1;
                RenderSteps(BuildStep, false, 0.4);
                SetStatus("Compiling mods (3 of 5)...", Phase.Busy, "Building Stratagem Compendium from its sources.");
                BootProgress.IsVisible = true;
                BootProgress.Value = 400;
                await Task.Delay(1500);
                var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
                using var bitmap = new Avalonia.Media.Imaging.RenderTargetBitmap(size);
                bitmap.Render(this);
                bitmap.Save(shot);
                Close();
            };

        PlatformText.Text = $"{RuntimeInformation.OSDescription} · .NET {Environment.Version}";
        VersionText.Text = $"HellForge {HellForge.Core.AppVersion.Text}\nHELLDIVERS 2 build {HellForge.Core.SupportedBuild.Current.BuildId}";
        if (SessionRunner.IsSupported)
            SetStatus("Ready. Steam must be running and signed in.", Phase.Idle,
                "Press PLAY to verify the game files and launch the game.");
        else
        {
            PlatformNote.Text = SessionRunner.UnsupportedMessage;
            PlatformNote.IsVisible = true;
            SetStatus(SessionRunner.UnsupportedMessage, Phase.Idle, "Profile, console and settings still work on this system.");
        }
        RenderSteps(-1, false);
        UpdateButton();
        ShowProfile();
        InitializeSetup();
        poll.Start();
        PruneSessionLogs();
    }

    // Only the newest SessionLogs.Keep sessions' logs stay (crash dumps and engine logs added up to gigabytes): when the
    // launcher opens and when a session ends, never while one runs. In the background: the first time can be many GB.
    void PruneSessionLogs()
    {
        if (runner.IsRunning) return;
        var sessions = paths.RunsFolder;
        _ = Task.Run(() =>
        {
            try { return HellForge.Core.SessionLogs.Prune(sessions); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { return null; }
        }).ContinueWith(done =>
        {
            if (done.Result is { Removed: > 0 } pruned)
                Dispatcher.UIThread.Post(() => Append($"Removed the logs of {pruned.Removed} older session(s) ({pruned.Bytes / 1e6:0} MB); the newest {HellForge.Core.SessionLogs.Keep} stay.", LauncherLine));
        }, TaskScheduler.Default);
    }

    // modSync: start with a host's mods after a mod sync (the backend stopped the last session for it).
    void Start(string? modSync = null)
    {
        if (runner.IsRunning) return;
        runDirectory = null;  // Forget the previous run so its final state does not overwrite this one.
        launchedUtc = default;
        NoteSessionEditor();
        if (!SessionRunner.IsSupported)
        {
            SetStatus(SessionRunner.UnsupportedMessage, Phase.Idle);
            return;
        }
        foreach (var (part, file) in new[] { ("Local backend", paths.Backend), ("Session runner", paths.Session) })
            if (!File.Exists(file))
            {
                SetStatus(part + " is missing.", Phase.Failed,
                    failure: paths.InstallRoot != null ? "Reinstall HellForge: " + file + " is missing." : "Sessions run in installations made with HellForge-Setup.");
                return;
            }
        // Without Local PlayFab the game's own lobby search would list vanilla players' Steam lobbies: the session runner
        // refuses multiplayer then, and so does the launcher, with the switch to change.
        if (SdkShimsBox.IsChecked == true && LocalPlayFabBox.IsChecked != true)
        {
            SetStatus("Multiplayer needs Local PlayFab.", Phase.Failed, failure: "Turn on Experiments > Local PlayFab (or turn off the PlayFab SDK stand-ins) and press PLAY again.");
            return;
        }
        // A name typed into the open new-profile form is created and used rather than dropped.
        if (NewProfileForm.IsVisible && !string.IsNullOrWhiteSpace(NewProfileName.Text) && !CreateProfile()) return;
        var profile = Selected;
        ShowNewProfileForm(false);
        requests.Reset();
        seasonPassUtc = null;
        reachedShip = false;
        stopRequested = false;
        launchedUtc = DateTime.UtcNow;
        lines.Clear();
        modProblemsShown = false;
        Append("Starting play session.", LauncherLine);
        Append($"Profile: {profile.Name} (template {profile.Template}) in {profile.Folder}", LauncherLine);
        // both on by default (Settings > Multiplayer): only a switch turned off is worth a line
        if (LocalPlayFabBox.IsChecked != true) Append("Local PlayFab is off (Settings > Multiplayer): co-op with friends needs it.", Orange);
        if (SdkShimsBox.IsChecked != true) Append("Lobby and session through Steam friends is off (Settings > Multiplayer): co-op with friends needs it.", Orange);
        List<string> gameOptions;
        try { gameOptions = HellForge.Core.GameLaunchOptions.Parse(LaunchOptionsBox.Text); }
        catch (InvalidDataException error)
        {
            launchedUtc = default;
            SetStatus("The launch options cannot be used.", Phase.Failed, failure: error.Message + " (Settings, Launch options)");
            return;
        }
        if (gameOptions.Count > 0) Append("Launch options: " + HellForge.Core.GameLaunchOptions.CommandLine(gameOptions), LauncherLine);
        if (SandboxBox.IsChecked == true) Append("Sandboxed (Settings > Sandbox): the game runs at low integrity, with Steam through the Steam broker.", LauncherLine);
        try
        {
            runner.Start(paths, profile, new(LocalPlayFabBox.IsChecked == true, SdkShimsBox.IsChecked == true, ProfileStore.LoadWar(paths),
                EngineLogBox.IsChecked == true, CrashDumpBox.IsChecked == true, gameOptions, modSync, SandboxBox.IsChecked == true));
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or PlatformNotSupportedException)
        {
            launchedUtc = default;
            SetStatus("Could not start the session runner.", Phase.Failed, failure: error.Message);
            return;
        }
        RenderSteps(BuildStep, false);
        SetStatus("Preparing the session...", Phase.Busy, "Building mods that ship their sources, then checking the copied game.");
        UpdateButton();
    }

    void RequestStop()
    {
        if (!runner.IsRunning || stopRequested) return;
        stopRequested = true;
        if (runDirectory != null)
        {
            try
            {
                SessionRunner.SignalStop(runDirectory);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                stopRequested = false;
                Append("Could not write the stop signal: " + error.Message, Red);
                return;
            }
            SetStatus("Stopping: closing the game...", Phase.Busy);
        }
        else
        {
            runner.Kill();
            RenderSteps(-1, false);
            SetStatus("Cancelled before launch.", Phase.Idle, "Nothing was started.");
        }
        UpdateButton();
    }

    void Refresh()
    {
        try { RefreshState(); }
        catch (Exception error) { Append("Launcher status error: " + error.Message, Red); }  // Keep the window usable.
        UpdateButton();
    }

    void RefreshState()
    {
        ShowProfile();
        if (runDirectory == null && launchedUtc != default) FindRun();
        if (runDirectory != null)
        {
            foreach (var line in requests.ReadNewLines(Path.Combine(runDirectory, "backend", "requests.jsonl"))) AppendRequest(line);
            if (SessionFiles.ReadJson(Path.Combine(runDirectory, "result.json")) is { } state) ShowState(state);
            // Mods are built before the script writes its first result.json: their progress shows from the start.
            else if (!stopRequested && ShowModBuilds(out var built)) RenderSteps(BuildStep, false, built);
            if (runner.IsRunning) ShowModSync(runDirectory);
            else ModSyncCard.IsVisible = false;
        }
        var runnerExited = runner.CollectExited();
        if (runnerExited) PruneSessionLogs();
        // Mod sync: the backend stopped the session so that it starts again with the host's mods it downloaded.
        if (runnerExited && runDirectory != null && !stopRequested && paths.InstallRoot != null &&
            SessionFiles.ReadJson(Path.Combine(runDirectory, "result.json")) is { } final && SessionFiles.Text(final, "restart") == "mod-sync")
        {
            Append("Restarting with the host's mods (mod sync). Once the game is on the ship, the host's invitation to their lobby waits in game.", LauncherLine);
            Start(HellForge.Core.ModSyncSession.PathIn(new HellForge.Core.InstallLayout(paths.InstallRoot)));
            return;
        }
        if (runnerExited && runDirectory == null)
        {
            if (stopRequested)
            {
                RenderSteps(-1, false);
                SetStatus("Cancelled before launch.", Phase.Idle);
            }
            else
            {
                FailCurrentStep();
                SetStatus("The session runner exited before starting.", Phase.Failed, paths.InstallRoot != null ? "See " + Path.Combine(paths.InstallRoot, "logs", "session-runner.log") + " for why." : "Sessions run in installations made with HellForge-Setup.");
            }
            launchedUtc = default;
        }
    }

    void FindRun()
    {
        if (SessionRunner.RunSince(paths, launchedUtc) is not { } run) return;
        runDirectory = run;
        if (runPathText != null) runPathText.Text = run;
    }

    // Mods the session left out (did not compile, data not built, the compiler not installed...): once, in the console,
    // so a player sees why a mod is missing in game.
    void ShowModProblems(JsonElement state)
    {
        if (modProblemsShown || !state.TryGetProperty("mods", out var mods) || mods.ValueKind != JsonValueKind.Object ||
            !mods.TryGetProperty("problems", out var problems) || problems.ValueKind != JsonValueKind.Array) return;
        modProblemsShown = true;
        foreach (var problem in problems.EnumerateArray())
            if (problem.ValueKind == JsonValueKind.String) Append("Mod left out of this session: " + problem.GetString(), Orange);
    }

    void ShowState(JsonElement state)
    {
        var stopped = !runner.IsRunning;
        ShowModProblems(state);
        switch (SessionFiles.Text(state, "status"))
        {
            case "preparing":
                if (!stopRequested && ShowModBuilds(out var built)) { RenderSteps(BuildStep, false, built); break; }
                RenderSteps(VerifyStep, false);
                if (!stopRequested) SetStatus("Verifying game files...", Phase.Busy, "Checking the copied game against its pinned hashes.");
                break;
            case "awaiting_network_block_install":
                RenderSteps(NetworkStep, false);
                if (!stopRequested) SetStatus("Approve the Windows prompt to install the network block.", Phase.Attention,
                    "Asked once: the network block keeps the copied game off the internet. Later sessions need no prompt.");
                break;
            case "awaiting_windows_firewall_consent":
                RenderSteps(NetworkStep, false);
                if (!stopRequested) SetStatus("Approve the Windows prompt to place a network block for this session.", Phase.Attention,
                    "It keeps the copied game off the internet until the session ends.");
                break;
            case "patching":
                RenderSteps(LaunchStep, false);
                if (!stopRequested) SetStatus("Launching the game...", Phase.Busy, "Starting the copied game against the local backend.");
                break;
            case "observing":
                RenderSteps(RunStep, false);
                if (stopRequested) break;
                if (!reachedShip && DateTime.UtcNow - seasonPassUtc > IntroStallAfter)
                    SetStatus("The game seems stuck in the cryo pod.", Phase.Attention,
                        "This can happen on the first launch of a new installation. Press STOP, then PLAY again: the next launch goes through.");
                else SetStatus("Running", Phase.Running, "Press STOP or close the game to end the session.");
                break;
            case "observed":
                RenderSteps(SessionFiles.Text(state, "gameLaunched") == "true" ? StepNames.Length : -1, false);
                if (stopped) SetStatus("Session ended.", Phase.Ended, Cleanup(state) + "Press PLAY to start another session.");
                else SetStatus("Ending the session...", Phase.Busy);
                break;
            case "failed":
                FailCurrentStep();
                var failure = SessionFiles.Text(state, "failure");
                SetStatus(stopped ? "Session failed." : "Session failed. Cleaning up...", Phase.Failed,
                    stopped ? "Open the session logs for details." : "", failure.Length > 0 ? failure : "unknown");
                break;
            case "cleanup_failed":
                FailCurrentStep();
                SetStatus("Cleanup failed.", Phase.Failed, "", "Cleanup failed. Check the session logs before playing again.");
                break;
        }
    }

    // The network block an installation keeps (WFP, the Linux namespace, persistent rules) stays; only a session that placed
    // rules of its own for itself (older runners: firewallCleanup) reports taking them down.
    static string Cleanup(JsonElement state) => !state.TryGetProperty("firewallCleanup", out _) ? ""
        : "The session's network block removed: " + (state.TryGetProperty("firewallCleanup", out var cleanup) && SessionFiles.Text(cleanup, "rulesRemoved") == "true" ? "yes" : "unconfirmed") + ". ";

    LocalProfile Selected => ProfileCombo.SelectedItem as LocalProfile ?? profiles[0];

    // Reloads the profile list and selects `name` (else the first profile).
    void LoadProfiles(string name)
    {
        loadingProfiles = true;
        profiles = ProfileStore.List(paths);
        ProfileCombo.ItemsSource = profiles;
        ProfileCombo.SelectedItem = profiles.FirstOrDefault(p => p.Name == name) ?? profiles[0];
        loadingProfiles = false;
        ShowSelectedProfile();
    }

    void OnProfileSelected()
    {
        if (loadingProfiles || ProfileCombo.SelectedItem is not LocalProfile profile) return;
        ProfileStore.SaveSelected(paths, profile.Name);
        ShowSelectedProfile();
    }

    void ShowSelectedProfile()
    {
        var profile = Selected;
        ProfileTitle.Text = profile.Name;
        ProfilePathText.Text = profile.ProfileFile;
        if (profilePathText != null) profilePathText.Text = profile.Folder;
        TemplateText.Text = profile.Template;
        TemplateBadge.IsVisible = profile.IsDev;
        TemplateCaption.Text = "Applied when the profile is first created";
        ImportSettingsStatus.Text = "";
        ShowProfile();
        UpdateSettingsImport();
    }

    void ShowTemplateDescription()
    {
        var template = TemplateCombo.SelectedItem as ProfileTemplate;
        TemplateDescription.Text = template?.Summary ?? "No templates found in " + paths.TemplatesFile + ".";
        CreateProfileButton.IsEnabled = template != null && !runner.IsRunning;
    }

    void ShowNewProfileForm(bool show)
    {
        if (show && runner.IsRunning) return;
        NewProfileForm.IsVisible = show;
        StepsCard.IsVisible = !show;
        NewProfileError.IsVisible = false;
        if (!show) return;
        NewProfileName.Text = "";
        NewProfileName.Focus();
    }

    // Returns false (showing why) when no profile was created.
    bool CreateProfile()
    {
        if (runner.IsRunning) return false;
        var name = (NewProfileName.Text ?? "").Trim();
        var error = TemplateCombo.SelectedItem is ProfileTemplate template ? ProfileStore.Create(paths, name, template.Name) : "Pick a template.";
        if (error != null)
        {
            NewProfileError.Text = error;
            NewProfileError.IsVisible = true;
            return false;
        }
        Append($"Created profile {name} (template {((ProfileTemplate)TemplateCombo.SelectedItem!).Name}). The backend fills it on the first session.", LauncherLine);
        LoadProfiles(name);
        ProfileStore.SaveSelected(paths, name);
        ShowNewProfileForm(false);
        return true;
    }

    void ShowProfile()
    {
        var profile = Selected;
        if (SessionFiles.ReadJson(profile.ProfileFile) is not { ValueKind: JsonValueKind.Object } data ||
            !data.TryGetProperty("progression", out var progression) || progression.ValueKind != JsonValueKind.Object)
        {
            ProfileNote.Text = $"Not used yet. The backend creates it from the \"{profile.Template}\" template on the first session.";
            XpText.Text = ShipText.Text = InventoryText.Text = OnboardingStateText.Text = OnboardingFlagsText.Text = "-";
            OnboardingFlagsHex.Text = "";
            ShowWeaponLevels(null);
            return;
        }
        ShowWeaponLevels(data);
        ProfileNote.Text = "Stored on this computer and served by the local backend.";
        InventoryText.Text = data.TryGetProperty("inventory", out var inventory) && inventory.ValueKind == JsonValueKind.Object &&
            inventory.TryGetProperty("items", out var items) && items.ValueKind == JsonValueKind.Array
            ? items.GetArrayLength().ToString("N0", CultureInfo.CurrentCulture) : "0";
        XpText.Text = Number(progression, "experience", out var xp) ? xp.ToString("N0", CultureInfo.CurrentCulture) : "-";
        ShipText.Text = progression.TryGetProperty("shipName", out var name) && name.ValueKind == JsonValueKind.Array &&
            name.GetArrayLength() >= 2 && name[0].ValueKind == JsonValueKind.Number && name[1].ValueKind == JsonValueKind.Number
            ? $"#{name[0].GetRawText()} + #{name[1].GetRawText()}" : "Not named yet";
        OnboardingStateText.Text = Number(progression, "onboardingState", out var onboarding) ? onboarding.ToString(CultureInfo.InvariantCulture) : "-";
        var hasFlags = Number(progression, "onboardingFlags", out var flags);
        OnboardingFlagsText.Text = hasFlags ? flags.ToString(CultureInfo.InvariantCulture) : "-";
        OnboardingFlagsHex.Text = hasFlags ? $"0x{flags:X}" : "";
    }

    static bool Number(JsonElement parent, string name, out long value)
    {
        value = 0;
        return parent.TryGetProperty(name, out var element) && element.ValueKind == JsonValueKind.Number && element.TryGetInt64(out value);
    }

    void AppendRequest(string line)
    {
        try
        {
            using var record = JsonDocument.Parse(line);
            var r = record.RootElement;
            var time = DateTimeOffset.TryParse(SessionFiles.Text(r, "timeUtc"), CultureInfo.InvariantCulture, DateTimeStyles.None, out var at)
                ? at.ToLocalTime().ToString("HH:mm:ss", CultureInfo.InvariantCulture) : "--:--:--";
            var status = int.TryParse(SessionFiles.Text(r, "status"), out var code) ? code : 0;
            var path = SessionFiles.Text(r, "path");
            if (path == "/SeasonPass") seasonPassUtc ??= DateTime.UtcNow;
            else if (path.StartsWith("/Pes/", StringComparison.Ordinal)) reachedShip = true;
            Append($"{time}  {status,3}  {SessionFiles.Text(r, "method"),-6} {path}   {SessionFiles.Text(r, "outcome")}",
                status switch { >= 200 and < 300 => Green, 501 => Red, >= 400 => Orange, _ => Light });
        }
        catch (JsonException) { Append(line, Grey); }  // A complete but malformed line: show it as is.
    }

    void Append(string text, IBrush brush)
    {
        lines.Add(new LogLine(Privacy.Scrub(text), brush));
        while (lines.Count > MaxLogLines) lines.RemoveAt(0);
        QueueScroll();
    }

    void QueueScroll()
    {
        if (scrollQueued) return;
        scrollQueued = true;
        Dispatcher.UIThread.Post(() =>
        {
            scrollQueued = false;
            if (AutoScroll.IsChecked == true && lines.Count > 0 && ConsolePage.IsVisible) ConsoleList.ScrollIntoView(lines.Count - 1);
        }, DispatcherPriority.Background);
    }

    // Source mods being compiled before the game starts (HellForge.Cli mods-stage --progress, ModBuilds): the status, a
    // bar and *progress (null while the compiler is fetched). False when no build is under way.
    bool ShowModBuilds(out double? progress)
    {
        progress = null;
        if (runDirectory == null || SessionFiles.ReadJson(Path.Combine(runDirectory, "mod-builds.json")) is not { } builds) return false;
        var stage = SessionFiles.Text(builds, "stage");
        if (stage is not ("compiling" or "tools")) return false;
        int Count(string name) => builds.TryGetProperty(name, out var v) && v.TryGetInt32(out var n) ? n : 0;
        int done = Count("done"), total = Math.Max(1, Count("total"));
        var mod = SessionFiles.Text(builds, "mod");
        SetStatus(stage == "tools" ? "Fetching the mod compiler..." : $"Compiling mods ({done + 1} of {total})...", Phase.Busy,
            stage == "tools" ? "Once per installation: HellForge's pinned Zig, to build mods that ship their sources."
                : $"Building {mod} from its sources.");
        BootProgress.IsVisible = true;
        BootProgress.IsIndeterminate = stage == "tools";
        BootProgress.Value = 1000.0 * done / total;
        if (stage != "tools") progress = (double)done / total;
        return true;
    }

    void SetStatus(string text, Phase phase, string hint = "", string? failure = null)
    {
        if (BootProgress.IsVisible && !text.StartsWith("Compiling mods", StringComparison.Ordinal) && !text.StartsWith("Fetching the mod compiler", StringComparison.Ordinal))
            BootProgress.IsVisible = false;
        // Shown text is screenshotted for help: it does not name the PC (Privacy).
        StatusText.Text = Privacy.Scrub(text);
        HintText.Text = Privacy.Scrub(hint);
        HintText.IsVisible = hint.Length > 0;
        FailureText.Text = failure != null ? Privacy.Scrub(failure) : null;
        FailureText.IsVisible = failure != null;
        var (brush, label, pulse) = phase switch
        {
            Phase.Busy => (Gold, "Working", true),
            Phase.Attention => (Orange, "Action needed", true),
            Phase.Running => (Green, "Running", true),
            Phase.Ended => (Light, "Ended", false),
            Phase.Failed => (Red, "Failed", false),
            _ => (Grey, "Idle", false)
        };
        var color = ((ISolidColorBrush)brush).Color;
        foreach (var dot in new[] { StatusDot, SidebarDot })
        {
            dot.Background = brush;
            dot.Classes.Set("pulse", pulse);
            dot.BoxShadow = phase == Phase.Idle ? default
                : new BoxShadows(new BoxShadow { Blur = 12, Spread = 1, Color = Color.FromArgb(140, color.R, color.G, color.B) });
        }
        SidebarState.Text = label;
        SidebarState.Foreground = phase == Phase.Idle ? Text : brush;
    }

    void UpdateButton()
    {
        var running = runner.IsRunning;
        PlayLabel.Text = !running ? "PLAY" : stopRequested ? "STOPPING" : "STOP";
        PlayButton.Classes.Set("stop", running);
        PlayButton.IsEnabled = SessionRunner.IsSupported && !(running && stopRequested) && !(bundledSyncing && !running);
        // The session keeps the profile it started with.
        ProfileCombo.IsEnabled = NewProfileButton.IsEnabled = !running;
        CreateProfileButton.IsEnabled = !running && TemplateCombo.SelectedItem != null;
        if (running && NewProfileForm.IsVisible) ShowNewProfileForm(false);
        SetModsLocked(running);
        SetWeaponLevelsLocked(running);
        UpdateSettingsImport();
    }

    void BuildSteps()
    {
        StepsGrid.ColumnDefinitions = new ColumnDefinitions(string.Join(",", Enumerable.Repeat("Auto", StepNames.Length)
            .Select((c, i) => i == 0 ? c : "*," + c)));
        for (var i = 0; i < StepNames.Length; i++)
        {
            var number = new TextBlock
            {
                Text = (i + 1).ToString(CultureInfo.InvariantCulture), FontWeight = FontWeight.Bold,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
            };
            var glyph = new Shape
            {
                StrokeThickness = 2.6, StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
            };
            var circle = new Border
            {
                Width = 40, Height = 40, CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(2),
                HorizontalAlignment = HorizontalAlignment.Center, Child = new Panel { Children = { number, glyph } }
            };
            var label = new TextBlock { Text = StepNames[i], FontSize = 13, FontWeight = FontWeight.SemiBold, HorizontalAlignment = HorizontalAlignment.Center };
            var cell = new StackPanel { Spacing = 10, Children = { circle, label } };
            Grid.SetColumn(cell, i * 2);
            StepsGrid.Children.Add(cell);
            steps.Add((circle, number, glyph, label));
            if (i == StepNames.Length - 1) continue;
            var connector = new ProgressBar
            {
                Classes = { "track" }, Minimum = 0, Maximum = 1000, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(12, 18, 12, 0)
            };
            Grid.SetColumn(connector, i * 2 + 1);
            StepsGrid.Children.Add(connector);
            connectors.Add(connector);
        }
    }

    // Steps before `current` are complete, `current` is active (or failed), later steps are pending.
    // current == StepNames.Length marks every step complete; -1 marks every step pending. The active step's track (to
    // the next step) fills with its progress (0 .. 1), or moves while its progress cannot be told.
    void RenderSteps(int current, bool failed, double? progress = null)
    {
        UpdateTracks(current, failed, progress);
        if (shownStep == (current, failed)) return;
        shownStep = (current, failed);
        for (var i = 0; i < steps.Count; i++)
        {
            var (circle, number, glyph, label) = steps[i];
            var done = i < current;
            var active = i == current;
            circle.Background = done ? Accent : active && failed ? Red : active ? ActiveFill : Brushes.Transparent;
            circle.BorderBrush = done ? Accent : active && failed ? Red : active ? Accent : Pending;
            number.IsVisible = !done && !(active && failed);
            number.Foreground = active ? Accent : Grey;
            glyph.IsVisible = done || (active && failed);
            glyph.Data = Geometry.Parse(done ? "M 0,6 L 5,11 L 14,1" : "M 0,0 L 11,11 M 11,0 L 0,11");
            glyph.Stroke = done ? White : Text;
            label.Foreground = active && failed ? Red : done || active ? Text : Grey;
        }
    }

    void UpdateTracks(int current, bool failed, double? progress)
    {
        if (shownStep.Index == current && shownStep.Failed == failed && stepProgress == progress) return;
        stepProgress = progress;
        for (var i = 0; i < connectors.Count; i++)
        {
            var track = connectors[i];
            var active = i == current && !failed;
            track.IsIndeterminate = active && progress == null;
            track.Value = i < current ? 1000 : active && progress != null ? 1000 * Math.Clamp(progress.Value, 0, 1) : 0;
            track.Classes.Set("busy", active);
            if (i == current && failed) track.Foreground = Red; else track.ClearValue(ForegroundProperty);
        }
    }

    void FailCurrentStep() => RenderSteps(Math.Clamp(shownStep.Index, 0, StepNames.Length - 1), true);

    void BuildPaths()
    {
        var rows = new (string Label, string Value)[]
        {
            (paths.InstallRoot != null ? "Installation" : "Project", paths.Root), ("Session runner", paths.Session), ("Data folder", paths.HostRoot),
            ("Profiles", paths.ProfilesRoot), ("Templates", paths.TemplatesFile), ("Selected profile", ""),
            ("Current run", "No session started yet")
        };
        for (var i = 0; i < rows.Length; i++)
        {
            PathsGrid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var label = new TextBlock { Text = rows[i].Label, Foreground = Grey, Margin = new Thickness(0, 4) };
            var value = new SelectableTextBlock { Text = rows[i].Value, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4), FontSize = 13 };
            Grid.SetRow(label, i);
            Grid.SetRow(value, i);
            Grid.SetColumn(value, 1);
            PathsGrid.Children.Add(label);
            PathsGrid.Children.Add(value);
            if (rows[i].Label == "Selected profile") profilePathText = value;
            runPathText = value;  // The last row is updated when a run starts.
        }
    }

    void ShowPage(int index)
    {
        Control[] pages = [PlayPage, ProfilePage, ModsPage, ConsolePage, SettingsPage];
        for (var i = 0; i < pages.Length; i++) pages[i].IsVisible = i == index;
        SetupPage.IsVisible = false;
        if (index == 2) RefreshMods();
        if (index == 3) QueueScroll();
    }

    // Zips the latest session folder to the Desktop, for sending to whoever helps test (it names Steam accounts by id).
    void SaveLogsToDesktop()
    {
        try
        {
            var run = runDirectory ?? (Directory.Exists(paths.RunsFolder)
                ? new DirectoryInfo(paths.RunsFolder).GetDirectories().OrderByDescending(d => d.LastWriteTimeUtc).FirstOrDefault()?.FullName : null);
            if (run == null) { Append("No session logs yet: play a session first.", Orange); return; }
            var target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                $"HellForge-logs-{DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}.zip");
            // the same zip as HellForge.Cli logs --export: scrubbed of the PC's names (SessionLogs.Export)
            var gameLogs = paths.InstallRoot != null ? Path.Combine(paths.InstallRoot, "userdata", "logs") : null;
            var skipped = HellForge.Core.SessionLogs.Export(run, gameLogs, target).Skipped;
            Append("Saved the latest session's logs to " + target + (skipped > 0 ? $" ({skipped} files in use were left out)" : ""), Green);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Append("Could not save the logs: " + error.Message, Red);
        }
    }

    // The HellForge Discord server: support, news and mods (the sidebar's Discord button).
    const string DiscordInvite = "https://discord.gg/3Yye92xCzh";

    // A web page in the player's browser.
    async void OpenLink(string url)
    {
        try { if (!await this.Launcher.LaunchUriAsync(new Uri(url))) Append("Could not open " + url, Red); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { Append("Could not open " + url + ": " + error.Message, Red); }
    }

    async void OpenFolder(string path)
    {
        try
        {
            Directory.CreateDirectory(path);
            if (!await this.Launcher.LaunchDirectoryInfoAsync(new DirectoryInfo(path))) Append("Could not open " + path, Red);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not open " + path + ": " + error.Message, Red); }
    }

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        if (!closeConfirmed && runner.IsRunning)
        {
            e.Cancel = true;
            _ = ConfirmCloseAsync();
        }
        else poll.Stop();
        base.OnClosing(e);
    }

    async Task ConfirmCloseAsync()
    {
        if (ConfirmOverlay.IsVisible) return;
        ConfirmTitle.Text = "Session running";
        ConfirmText.Text = "A play session is running. Stop it and close the game?";
        ConfirmButtons.IsVisible = true;
        ConfirmOverlay.IsVisible = true;
        confirm = new TaskCompletionSource<bool>();
        if (!await confirm.Task)
        {
            ConfirmOverlay.IsVisible = false;
            return;
        }
        ConfirmTitle.Text = "Stopping session";
        ConfirmText.Text = "Closing the game and the local backend...";
        ConfirmButtons.IsVisible = false;
        RequestStop();
        // Give the script time to stop the game and restore its logs; the game's job ends with the script anyway.
        await runner.WaitForExitAsync(TimeSpan.FromSeconds(60));
        closeConfirmed = true;
        Close();
    }

    // What is wrong with the typed launch options, under their box (nothing when they can be used).
    void ShowLaunchOptionsProblem()
    {
        string? problem = null;
        try { HellForge.Core.GameLaunchOptions.Parse(LaunchOptionsBox.Text); }
        catch (InvalidDataException error) { problem = error.Message; }
        LaunchOptionsProblem.Text = problem;
        LaunchOptionsProblem.IsVisible = problem != null;
    }

    static IBrush Solid(string color) => new ImmutableSolidColorBrush(Color.Parse(color));
}
