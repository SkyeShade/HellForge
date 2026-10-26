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
    static readonly string[] StepNames = ["构建模组", "校验文件", "网络封锁", "启动", "运行"];
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
        ToolTip.SetTip(DiscordButton, "支持、新闻和模组：HellForge 的 Discord（" + DiscordInvite + ")");
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
        var war = ProfileStore.LoadWar(paths);
        WarLiveRadio.IsChecked = war == ProfileStore.WarLive;
        WarFrozenRadio.IsChecked = war == ProfileStore.WarFrozen;
        WarOpenRadio.IsChecked = war == ProfileStore.WarOpen;
        foreach (var (radio, mode) in new[] { (WarLiveRadio, ProfileStore.WarLive), (WarFrozenRadio, ProfileStore.WarFrozen), (WarOpenRadio, ProfileStore.WarOpen) })
            radio.IsCheckedChanged += (_, _) => { if (radio.IsChecked == true) ProfileStore.SaveWar(paths, mode); };
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
        // it as a picture and closes (a look at the launcher without a session).
        if (Environment.GetEnvironmentVariable("HELLFORGE_LAUNCHER_SHOT") is { Length: > 0 } shot)
            Opened += async (_, _) =>
            {
                RenderSteps(BuildStep, false, 0.4);
                SetStatus("正在编译模组（5 个中的第 3 个）…", Phase.Busy, "正在从源码构建战略配备汇编。");
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
        VersionText.Text = $"HellForge {HellForge.Core.AppVersion.Text}\nHELLDIVERS 2 版本 {HellForge.Core.SupportedBuild.Current.BuildId}";
        if (SessionRunner.IsSupported)
            SetStatus("就绪。Steam 必须正在运行并已登录。", Phase.Idle,
                "按「开始」来校验游戏文件并启动游戏。");
        else
        {
            PlatformNote.Text = SessionRunner.UnsupportedMessage;
            PlatformNote.IsVisible = true;
            SetStatus(SessionRunner.UnsupportedMessage, Phase.Idle, "在这套系统上，档案、控制台和设置仍然可用。");
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
                Dispatcher.UIThread.Post(() => Append($"已清理 {pruned.Removed} 个较早会话的日志（{pruned.Bytes / 1e6:0} MB）；最新的 {HellForge.Core.SessionLogs.Keep} 个会保留。", LauncherLine));
        }, TaskScheduler.Default);
    }

    void Start()
    {
        if (runner.IsRunning) return;
        runDirectory = null;  // Forget the previous run so its final state does not overwrite this one.
        launchedUtc = default;
        if (!SessionRunner.IsSupported)
        {
            SetStatus(SessionRunner.UnsupportedMessage, Phase.Idle);
            return;
        }
        foreach (var (part, file) in new[] { ("本地后端", paths.Backend), ("会话运行器", paths.Session) })
            if (!File.Exists(file))
            {
                SetStatus(part + " 缺失。", Phase.Failed,
                    failure: paths.InstallRoot != null ? "请重新安装 HellForge：" + file + " 缺失。" : "会话只能在用 HellForge-Setup 创建的安装中运行。");
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
        Append("正在启动游戏会话。", LauncherLine);
        Append($"档案：{profile.Name}（模板 {profile.Template}），位置 {profile.Folder}", LauncherLine);
        // both on by default (Settings > Multiplayer): only a switch turned off is worth a line
        if (LocalPlayFabBox.IsChecked != true) Append("本地 PlayFab 已关闭（设置 → 多人联机）：与好友联机需要它。", Orange);
        if (SdkShimsBox.IsChecked != true) Append("「通过 Steam 好友的会话大厅」已关闭（设置 → 多人联机）：与好友联机需要它。", Orange);
        List<string> gameOptions;
        try { gameOptions = HellForge.Core.GameLaunchOptions.Parse(LaunchOptionsBox.Text); }
        catch (InvalidDataException error)
        {
            launchedUtc = default;
            SetStatus("无法使用这些启动选项。", Phase.Failed, failure: error.Message + "（设置 → 启动选项）");
            return;
        }
        if (gameOptions.Count > 0) Append("启动选项：" + HellForge.Core.GameLaunchOptions.CommandLine(gameOptions), LauncherLine);
        try
        {
            runner.Start(paths, profile, new(LocalPlayFabBox.IsChecked == true, SdkShimsBox.IsChecked == true, ProfileStore.LoadWar(paths),
                EngineLogBox.IsChecked == true, CrashDumpBox.IsChecked == true, gameOptions));
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or PlatformNotSupportedException)
        {
            launchedUtc = default;
            SetStatus("无法启动会话运行器。", Phase.Failed, failure: error.Message);
            return;
        }
        RenderSteps(BuildStep, false);
        SetStatus("正在准备会话…", Phase.Busy, "构建自带源码的模组，然后校验复制出的游戏。");
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
                Append("无法写入停止信号：" + error.Message, Red);
                return;
            }
            SetStatus("正在停止：关闭游戏…", Phase.Busy);
        }
        else
        {
            runner.Kill();
            RenderSteps(-1, false);
            SetStatus("已在启动前取消。", Phase.Idle, "没有启动任何内容。");
        }
        UpdateButton();
    }

    void Refresh()
    {
        try { RefreshState(); }
        catch (Exception error) { Append("启动器状态错误：" + error.Message, Red); }  // Keep the window usable.
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
        }
        var runnerExited = runner.CollectExited();
        if (runnerExited) PruneSessionLogs();
        if (runnerExited && runDirectory == null)
        {
            if (stopRequested)
            {
                RenderSteps(-1, false);
                SetStatus("已在启动前取消。", Phase.Idle);
            }
            else
            {
                FailCurrentStep();
                SetStatus("会话运行器在开始前退出了。", Phase.Failed, paths.InstallRoot != null ? "请查看 " + Path.Combine(paths.InstallRoot, "logs", "session-runner.log") + " 了解原因。" : "会话只能在用 HellForge-Setup 创建的安装中运行。");
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
            if (problem.ValueKind == JsonValueKind.String) Append("本次会话未加载的模组：" + problem.GetString(), Orange);
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
                if (!stopRequested) SetStatus("正在校验游戏文件…", Phase.Busy, "按固定的哈希校验复制出的游戏。");
                break;
            case "awaiting_network_block_install":
                RenderSteps(NetworkStep, false);
                if (!stopRequested) SetStatus("请批准 Windows 的提示，以安装网络封锁。", Phase.Attention,
                    "只会询问一次：网络封锁让复制出的游戏无法联网。之后的会话不再提示。");
                break;
            case "awaiting_windows_firewall_consent":
                RenderSteps(NetworkStep, false);
                if (!stopRequested) SetStatus("请批准 Windows 的提示，为本次会话放置网络封锁。", Phase.Attention,
                    "它会在会话结束前让复制出的游戏无法联网。");
                break;
            case "patching":
                RenderSteps(LaunchStep, false);
                if (!stopRequested) SetStatus("正在启动游戏…", Phase.Busy, "用本地后端启动复制出的游戏。");
                break;
            case "observing":
                RenderSteps(RunStep, false);
                if (stopRequested) break;
                if (!reachedShip && DateTime.UtcNow - seasonPassUtc > IntroStallAfter)
                    SetStatus("游戏似乎卡在冷冻舱里。", Phase.Attention,
                        "新安装第一次启动时可能出现这种情况。先按「停止」，再按「开始」：下一次启动就能通过。");
                else SetStatus("运行中", Phase.Running, "按「停止」或关闭游戏来结束会话。");
                break;
            case "observed":
                RenderSteps(SessionFiles.Text(state, "gameLaunched") == "true" ? StepNames.Length : -1, false);
                if (stopped) SetStatus("会话已结束。", Phase.Ended, Cleanup(state) + "按「开始」再启动一次会话。");
                else SetStatus("正在结束会话…", Phase.Busy);
                break;
            case "failed":
                FailCurrentStep();
                var failure = SessionFiles.Text(state, "failure");
                SetStatus(stopped ? "会话失败。" : "会话失败，正在清理…", Phase.Failed,
                    stopped ? "打开会话日志查看详情。" : "", failure.Length > 0 ? failure : "unknown");
                break;
            case "cleanup_failed":
                FailCurrentStep();
                SetStatus("清理失败。", Phase.Failed, "", "清理失败。再次游玩前请先查看会话日志。");
                break;
        }
    }

    // The network block an installation keeps (WFP, the Linux namespace, persistent rules) stays; only a session that placed
    // rules of its own for itself (older runners: firewallCleanup) reports taking them down.
    static string Cleanup(JsonElement state) => !state.TryGetProperty("firewallCleanup", out _) ? ""
        : "本次会话的网络封锁已移除：" + (state.TryGetProperty("firewallCleanup", out var cleanup) && SessionFiles.Text(cleanup, "rulesRemoved") == "true" ? "是" : "未确认") + ". ";

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
        TemplateCaption.Text = "在档案首次创建时应用";
        ImportSettingsStatus.Text = "";
        ShowProfile();
        UpdateSettingsImport();
    }

    void ShowTemplateDescription()
    {
        var template = TemplateCombo.SelectedItem as ProfileTemplate;
        TemplateDescription.Text = template?.Summary ?? "未找到模板：" + paths.TemplatesFile + "。";
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
        var error = TemplateCombo.SelectedItem is ProfileTemplate template ? ProfileStore.Create(paths, name, template.Name) : "请选择一个模板。";
        if (error != null)
        {
            NewProfileError.Text = error;
            NewProfileError.IsVisible = true;
            return false;
        }
        Append($"已创建档案 {name}（模板 {((ProfileTemplate)TemplateCombo.SelectedItem!).Name}）。本地后端会在首次会话时填充它。", LauncherLine);
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
            ProfileNote.Text = $"尚未使用。本地后端会在首次会话时按「{profile.Template}」模板创建它。";
            XpText.Text = ShipText.Text = InventoryText.Text = OnboardingStateText.Text = OnboardingFlagsText.Text = "-";
            OnboardingFlagsHex.Text = "";
            ShowWeaponLevels(null);
            return;
        }
        ShowWeaponLevels(data);
        ProfileNote.Text = "存储在这台电脑上，由本地后端提供。";
        InventoryText.Text = data.TryGetProperty("inventory", out var inventory) && inventory.ValueKind == JsonValueKind.Object &&
            inventory.TryGetProperty("items", out var items) && items.ValueKind == JsonValueKind.Array
            ? items.GetArrayLength().ToString("N0", CultureInfo.CurrentCulture) : "0";
        XpText.Text = Number(progression, "experience", out var xp) ? xp.ToString("N0", CultureInfo.CurrentCulture) : "-";
        ShipText.Text = progression.TryGetProperty("shipName", out var name) && name.ValueKind == JsonValueKind.Array &&
            name.GetArrayLength() >= 2 && name[0].ValueKind == JsonValueKind.Number && name[1].ValueKind == JsonValueKind.Number
            ? $"#{name[0].GetRawText()} + #{name[1].GetRawText()}" : "尚未命名";
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
        SetStatus(stage == "tools" ? "正在获取模组编译器…" : $"正在编译模组（{done + 1}/{total}）…", Phase.Busy,
            stage == "tools" ? "每个安装只做一次：使用 HellForge 固定版本的 Zig 来构建自带源码的模组。"
                : $"正在从源码构建 {mod}。");
        BootProgress.IsVisible = true;
        BootProgress.IsIndeterminate = stage == "tools";
        BootProgress.Value = 1000.0 * done / total;
        if (stage != "tools") progress = (double)done / total;
        return true;
    }

    void SetStatus(string text, Phase phase, string hint = "", string? failure = null)
    {
        if (BootProgress.IsVisible && !text.StartsWith("正在编译模组", StringComparison.Ordinal) && !text.StartsWith("正在获取模组编译器", StringComparison.Ordinal))
            BootProgress.IsVisible = false;
        // Shown text is screenshotted for help: it does not name the PC (Privacy).
        StatusText.Text = Privacy.Scrub(text);
        HintText.Text = Privacy.Scrub(hint);
        HintText.IsVisible = hint.Length > 0;
        FailureText.Text = failure != null ? Privacy.Scrub(failure) : null;
        FailureText.IsVisible = failure != null;
        var (brush, label, pulse) = phase switch
        {
            Phase.Busy => (Gold, "工作中", true),
            Phase.Attention => (Orange, "需要操作", true),
            Phase.Running => (Green, "运行中", true),
            Phase.Ended => (Light, "已结束", false),
            Phase.Failed => (Red, "失败", false),
            _ => (Grey, "空闲", false)
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
        PlayLabel.Text = !running ? "开始" : stopRequested ? "正在停止" : "停止";
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
            (paths.InstallRoot != null ? "安装位置" : "项目文件夹", paths.Root), ("会话运行器", paths.Session), ("数据文件夹", paths.HostRoot),
            ("档案", paths.ProfilesRoot), ("模板", paths.TemplatesFile), ("当前档案", ""),
            ("当前运行", "尚未启动会话")
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
            if (rows[i].Label == "当前档案") profilePathText = value;
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
            if (run == null) { Append("还没有会话日志：请先运行一次会话。", Orange); return; }
            var target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                $"HellForge-logs-{DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}.zip");
            // the same zip as HellForge.Cli logs --export: scrubbed of the PC's names (SessionLogs.Export)
            var gameLogs = paths.InstallRoot != null ? Path.Combine(paths.InstallRoot, "userdata", "logs") : null;
            var skipped = HellForge.Core.SessionLogs.Export(run, gameLogs, target).Skipped;
            Append("已保存最近一次会话的日志到 " + target + (skipped > 0 ? $"（{skipped} 个正在使用的文件已跳过）" : ""), Green);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Append("无法保存日志：" + error.Message, Red);
        }
    }

    // The HellForge Discord server: support, news and mods (the sidebar's Discord button).
    const string DiscordInvite = "https://discord.gg/3Yye92xCzh";

    // A web page in the player's browser.
    async void OpenLink(string url)
    {
        try { if (!await this.Launcher.LaunchUriAsync(new Uri(url))) Append("无法打开 " + url, Red); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { Append("无法打开 " + url + ": " + error.Message, Red); }
    }

    async void OpenFolder(string path)
    {
        try
        {
            Directory.CreateDirectory(path);
            if (!await this.Launcher.LaunchDirectoryInfoAsync(new DirectoryInfo(path))) Append("无法打开 " + path, Red);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("无法打开 " + path + ": " + error.Message, Red); }
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
        ConfirmTitle.Text = "会话正在运行";
        ConfirmText.Text = "游戏会话正在运行。要停止它并关闭游戏吗？";
        ConfirmButtons.IsVisible = true;
        ConfirmOverlay.IsVisible = true;
        confirm = new TaskCompletionSource<bool>();
        if (!await confirm.Task)
        {
            ConfirmOverlay.IsVisible = false;
            return;
        }
        ConfirmTitle.Text = "正在停止会话";
        ConfirmText.Text = "正在关闭游戏和本地后端…";
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
