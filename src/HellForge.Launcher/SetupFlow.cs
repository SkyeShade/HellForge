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
        ["在 Steam 中查找 HELLDIVERS 2", "确认你拥有这款游戏", "把游戏复制到这个文件夹", "阻断这份副本的网络",
            "导入旧版游戏数据", "创建快捷方式"];

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
        InstallText.Text = $"{Privacy.Scrub(install.Root)}  ·  HELLDIVERS 2 版本 {SupportedBuild.Current.BuildId}";
        SetupStartButton.Click += async (_, _) => await RunSetup();
        SetupPauseButton.Click += (_, _) => setupCancel?.Cancel();
        SetupBackButton.Click += (_, _) => LeaveSetup();
        OpenConsoleButton.Click += (_, _) => OpenSteamConsole();
        DeleteDownloadBox.Content = $"复制完成后删除 Steam 的下载（可释放约 {SupportedBuild.Current.TotalBytes / 1e9:0} GB）";
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
        SetupBackButton.Content = "返回";
        SetBackButtonPrimary(false);
        SetupStartButton.IsVisible = true;
        SetupTitle.Text = repair ? "检查并修复" : "安装 HellForge";
        SetupStartButton.Content = repair ? "检查并修复" : "开始安装";
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
            SetRow(step, null, "正在 Steam 库中查找…", running: true);
            // Linux (Wine/Proton) outside the namespace, such as the launcher HellForge-Setup.exe opens: the network block
            // step could only fail there (Wine has no Windows Filtering Platform), after the whole copy.
            if (LinuxNetworkBlock.UnderWine && !LinuxNetworkBlock.Requested)
                throw new SetupStopped("在 Linux 上，安装程序需运行在 HellForge 的网络命名空间里。请关闭 HellForge，改用 " +
                    "app/linux/hellforge-linux.sh 启动（见 docs/linux.md），然后在其中运行安装程序。");
            if (!LinuxNetworkBlock.Requested) await CheckFilteringEngine(install);
            var build = SupportedBuild.Current;
            var steamRoot = SteamLocator.SteamRoot() ?? throw new SetupStopped("这台电脑上没有安装 Steam。请安装 Steam 和 HELLDIVERS 2，然后重新开始安装。");
            // The build patches published since this release (a game update gets one there, not a new HellForge).
            using (var http = new HttpClient()) await Downpatcher.RefreshAsync(http, Path.Combine(install.Temp, "downpatch-index.json"));
            SteamGame? game;
            string? unusable = null;
            try { game = await Task.Run(() => SteamLocator.FindGame(steamRoot)); }
            catch (Exception error) when (error is InvalidDataException or IOException) { game = null; unusable = error.Message; }
            var fromDepots = false;
            DownpatchPack? downpatchPack = null;
            string? downpatchSource = null;
            if (game is { SupportedBuild: true }) SetRow(step, true, $"Steam 中的版本 {game.Report.Pin.BuildId}（位置 {game.Report.Source}）");
            else if (await Task.Run(() => SteamDepotDownload.IsComplete(steamRoot, build)))
            {
                fromDepots = true;
                SetRow(step, true, $"版本 {build.BuildId}，由 Steam 下载到 {Path.GetDirectoryName(SteamDepotDownload.DepotFolder(steamRoot, build, build.Depots[0].Id))}");
            }
            else if (game != null && Downpatcher.FindPack(game.Report.Pin) is { } downpatch && !failedPatches.Contains(downpatch.Sha256))
            {
                downpatchPack = downpatch;
                downpatchSource = game.Report.Source;
                SetRow(step, true, $"Steam 中的版本为 {game.Report.Pin.BuildId}（位置 {game.Report.Source}）。一个 {downpatch.Size / 1e9:0.0} GB 的补丁会把它转换为版本 {build.BuildId}。");
            }
            // the game is there but busy (Steam updating it, the game running): that passes, so no download is offered.
            // Under Wine it does not pass: Steam's Linux libraries are on the Z: drive, which counts as a linked path
            // ("Linked/reparse paths are not supported: Z:\", GitHub issue 1), so the download is the way there.
            else if (game == null && unusable != null && !LinuxNetworkBlock.UnderWine)
                throw new SetupStopped($"Steam 里有 HELLDIVERS 2，但安装程序现在无法使用它：{unusable}");
            else
            {
                var have = game != null ? $"HELLDIVERS 2 版本 {game.Report.Pin.BuildId}"
                    : unusable != null ? "Wine 环境中的安装程序无法读取 HELLDIVERS 2" : "没有 HELLDIVERS 2";
                ShowDownload(steamRoot, have);
                var noPatch = game == null ? ""
                    : Downpatcher.FindPack(game.Report.Pin) != null ? $" 版本 {game.Report.Pin.BuildId} 的构建补丁此前未能生效。"
                    : $" HellForge 尚无版本 {game.Report.Pin.BuildId} 的补丁；补丁发布后，点「重新检查」就会使用它。";
                throw new SetupWaiting($"Steam 中为 {have}，但这个版本的 HellForge 需要版本 {build.BuildId}。{noPatch} Steam 可以为你的账号下载这个精确版本：" +
                    "请按下面的步骤操作。下载完成后，安装会自动继续。");
            }
            // When downpatching, copy first so the supported build's steam_api64.dll is available for the ownership check
            // (the newer build's DLL may not work with the probe).
            if (downpatchPack != null)
            {
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
                step = SetupStep.Copy;
                SetRow(step, null, "正在下载并应用构建补丁…", running: true);
                try { await DownpatchGame(downpatchSource!, install, downpatchPack); }
                // the patch failed (its download, Steam's copy, the pack itself): Steam's own download of the build still
                // works, and the files already patched are kept (each one has the build's hash), so it copies only the rest
                catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or HttpRequestException
                    or InvalidOperationException or Win32Exception)
                {
                    failedPatches.Add(downpatchPack.Sha256);
                    ShowDownload(steamRoot, $"HELLDIVERS 2 版本 {game!.Report.Pin.BuildId}");
                    throw new SetupWaiting($"构建补丁未能生效：{Describe(error)} 你可以改为让 Steam 为你的账号下载版本 {build.BuildId}：" +
                        "请按下面的步骤操作。下载完成后，安装会自动继续。");
                }
                SetRow(step, true, $"{build.Files.Count} 个文件（{build.TotalBytes / 1e9:0.0} GB），由版本 {downpatchPack.SourceBuild} 打补丁得到。");
            }

            var steamApi = downpatchPack != null
                ? Path.Combine(install.Game, "bin", "steam_api64.dll")
                : fromDepots
                    ? SteamDepotDownload.Locate(steamRoot, build, build.Files.Single(f => f.Path == "bin/steam_api64.dll"))
                    : Path.Combine(game!.Report.Source, "bin", "steam_api64.dll");

            step = SetupStep.Ownership;
            SetRow(step, null, "正在询问 Steam…", running: true);
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
            if (ownershipExit != 0)
                throw new SetupStopped("拥有权检查未通过：" + (ownershipError.Length > 0 ? ownershipError : $"退出码 {ownershipExit}。") +
                    " 请确认 Steam 正在运行且已登录，然后重新开始安装。");
            SetRow(step, true, "Steam 已确认此账号拥有该游戏。");

            if (downpatchPack == null)
            {
                step = SetupStep.Copy;
                SetRow(step, null, "正在复制并校验游戏文件…", running: true);
                await CopyGame(fromDepots ? null : game!.Report.Source, steamRoot, install);
                var copied = $"{build.Files.Count} 个文件（{build.TotalBytes / 1e9:0.0} GB），每个都按 Steam 的哈希校验过。";
                if (fromDepots && DeleteDownloadBox.IsChecked == true)
                {
                    SetRow(step, null, copied + " 正在删除 Steam 的下载…", running: true);
                    await Task.Run(() => SteamDepotDownload.Delete(steamRoot, build));
                    copied += " Steam 的下载已删除。";
                }
                DownloadCard.IsVisible = false;
                SetRow(step, true, copied);
            }

            step = SetupStep.NetworkBlock;
            SetRow(step, null, "正在检查网络封锁…", running: true);
            if (await NetworkBlockInPlace(install)) SetRow(step, true, "已经就位。");
            else
            {
                if (LinuxNetworkBlock.Requested)
                {
                    SetRow(step, null, "正在检查当前会话是否无法访问互联网…", running: true);
                    await LinuxNetworkBlock.Install(install);
                }
                else
                {
                    SetRow(step, null, "Windows 会请求一次权限来放置网络封锁…", running: true);
                    await InstallNetworkBlock(install);
                }
                SetRow(step, true, "复制出的游戏只能访问这台电脑（它的本地后端）。你原本的游戏不受影响。");
            }

            step = SetupStep.Import;
            if (ImportBox.IsVisible && ImportBox.IsChecked == true)
            {
                var imported = await Task.Run(() => Migration.Run(install, Path.Combine(install.Temp, "no-repository"), EarlierGameData));
                // The folder keeps the project's old name; the message doesn't show it.
                SetRow(step, true, imported.Copied.Count > 0 ? $"已从较早的 HellForge 安装导入 {imported.Copied.Count} 项游戏数据；原始文件仍保留在原处。"
                    : "没有可导入的新内容。");
            }
            else SetRow(step, true, "已跳过。");

            step = SetupStep.Shortcuts;
            var made = CreateShortcuts(install);
            SetRow(step, true, made.Count > 0 ? string.Join(", ", made) : "已跳过。");
            await SyncBundledMods(install); // with the game copied: the editor's icons and fonts, Hell Forge's icon

            // The highlighted button on the right always moves forward: here that is Continue. Checking again stays
            // available under Settings > Installation.
            SetupTitle.Text = "可以开始游戏了";
            SetupStartButton.IsVisible = false;
            SetupBackButton.Content = "继续";
            SetBackButtonPrimary(true);
            SetupBackButton.IsVisible = true;
            Nav.IsVisible = true;
        }
        catch (OperationCanceledException)
        {
            SetRow(step, null, "已暂停。再次开始安装即可从停止处继续。");
            SetupStartButton.Content = "继续安装";
        }
        catch (SetupWaiting waiting)
        {
            SetRow(step, null, waiting.Message);
            SetupStartButton.Content = "重新检查";
            SetupBackButton.IsVisible = Nav.IsVisible;
        }
        catch (Exception error) when (error is SetupStopped or IOException or InvalidDataException or UnauthorizedAccessException or Win32Exception
            or JsonException or InvalidOperationException or HttpRequestException)
        {
            SetRow(step, false, error is Win32Exception { NativeErrorCode: 1223 }
                ? "Windows 权限被拒绝。网络封锁是必需的；请重新开始安装并允许它。"
                : Describe(error));
            SetupStartButton.Content = "重试";
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
        CopyText.Text = $"用时 {clock.Elapsed.TotalMinutes:0.0} 分钟。";
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
            await Downpatcher.ApplyAsync(source, install.Game, install.Temp, pack, http,
                downloadProgress: (done, total) =>
                {
                    if (downloadStart == TimeSpan.Zero) downloadStart = clock.Elapsed;
                    var elapsed = clock.Elapsed - downloadStart;
                    var rate = elapsed.TotalSeconds > 1 ? done / elapsed.TotalSeconds / 1e6 : 0;
                    Update($"正在下载构建补丁：{done / 1e9:0.00} / {total / 1e9:0.0} GB  ·  {rate:0} MB/s",
                        total > 0 ? (double)done / total * 0.5 : 0);
                },
                applyProgress: (done, total, file) =>
                    Update($"正在应用：{done / 1e9:0.0} / {total / 1e9:0.0} GB  ·  {file}",
                        0.5 + (total > 0 ? (double)done / total * 0.5 : 0)),
                cancellation: token);
        }, token);

        CopyProgress.Value = 1000;
        CopyText.Text = $"用时 {clock.Elapsed.TotalMinutes:0.0} 分钟。";
    }

    // The download card: the console commands for the pinned depot manifests, and the download's progress, checked every
    // 2 s. When every depot is complete, setup runs again and copies from the download.
    void ShowDownload(string steamRoot, string have)
    {
        var build = SupportedBuild.Current;
        downloadSteamRoot = steamRoot;
        DownloadIntro.Text = $"Steam 中为 {have}；这个版本的 HellForge 运行版本 {build.BuildId}。Steam 仍可为你的账号下载这个精确版本。" +
            "打开 Steam 控制台，把下面的每条命令依次粘贴并回车。然后 Steam 会把约 " +
            $"{build.TotalBytes / 1e9:0} GB 下载到 {SteamDepotDownload.ContentFolder(steamRoot, build)}（不会动你原本的游戏）。" +
            "Steam 库中不会显示这项下载的进度；下面的进度条会显示。";
        // Linux: Steam for Linux downloads into its own folder, which setup cannot read under Wine (Z: counts as a linked
        // path); linux/hellforge-linux.sh moves the download into this prefix's Steam folder.
        if (LinuxNetworkBlock.UnderWine)
            DownloadIntro.Text += " 在 Linux 上，Steam 会把下载放进它自己的文件夹，进度条会一直为空：当控制台对每条命令都显示 " +
                "\"Depot download complete\" 后，关闭 HellForge，并用 app/linux/hellforge-linux.sh 重新启动，" +
                "它会把下载移动到当前 wine prefix。然后点「重新检查」。";
        DownloadCommands.Children.Clear();
        foreach (var command in SteamDepotDownload.Commands(build))
        {
            var copy = new Button { Content = "复制", Classes = { "ghost" }, Margin = new(8, 0, 0, 0) };
            var text = new TextBox { Text = command, IsReadOnly = true, FontFamily = new FontFamily("Consolas, monospace") };
            copy.Click += async (_, _) =>
            {
                if (TopLevel.GetTopLevel(this)?.Clipboard is { } clipboard) await clipboard.SetTextAsync(command);
                copy.Content = "已复制";
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
            DownloadText.Text = string.Join("   ", states.Select(s => $"depot {s.Depot.Id}：{(s.Complete ? "已完成" : $"{s.Done / 1e9:0.0} / {s.Total / 1e9:0.0} GB")}"));
            // Near the end (a few files left, which the rounded GB cannot show), the files it still waits for, by name: a file
            // Steam did not write, or wrote at another size (another build's), would otherwise hold the setup with no word.
            var left = states.Where(s => !s.Complete).SelectMany(s => s.Waiting).ToList();
            if (left.Count is > 0 and <= 5)
                DownloadText.Text += "\n正在等待 " + string.Join("、", left.Select(f => f.Path + (f.Have < 0 ? "（尚未出现）"
                    : f.Have > f.Size ? $"（{f.Have:N0} 字节，大于本版本应有的 {f.Size:N0}：来自其他版本的文件；请删除它并重新运行 download_depot 命令）"
                    : $"（{f.Have:N0} / {f.Size:N0} 字节）")));
            if (states.All(s => s.Complete))
            {
                downloadTimer?.Stop();
                DownloadText.Text = "下载完成。正在复制到本安装中…";
                if (!setupRunning) await RunSetup();
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { DownloadText.Text = "无法检查下载：" + Privacy.Scrub(error.Message); }
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
            throw new SetupStopped("Windows 的基筛选引擎（BFE）服务已关闭。HellForge 需要它来让复制出的游戏保持离线。" +
                "请重新开启它（服务 → Base Filtering Engine，或以管理员身份运行 net start BFE），然后重新开始安装。");
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
        UninstallWarning.Text = $"这会删除 {install.Root} 及其中所有内容：游戏副本（约 24 GB）、你的 HellForge " +
            "档案与进度、模组文件夹里的所有模组（包括你自己的）、存档和日志。请先把想保留的内容复制出去。" +
            "完成后 HellForge 会关闭。";
        UninstallConfirm.IsVisible = true;
        UninstallText.Text = "";
    }

    async Task Uninstall(InstallLayout install)
    {
        UninstallConfirm.IsVisible = false;
        if (runner.IsRunning || setupRunning) { UninstallText.Text = "请先停止会话（或安装）。"; return; }
        // Linux (Wine/Proton): the removal runs Windows PowerShell, which a Wine prefix has only as a stub that does nothing.
        if (LinuxNetworkBlock.UnderWine) { UninstallText.Text = "在 Linux 上，请手动移除 HellForge：见 docs/linux.md 中的「移除 HellForge」。"; return; }
        var running = Process.GetProcesses().Where(p => p.Id != Environment.ProcessId && IsFrom(p, install.Root)).Select(p => p.ProcessName).Distinct().ToList();
        if (running.Count > 0) { UninstallText.Text = "请先关闭这些进程：" + string.Join("、", running) + "。"; return; }
        try { UninstallSafety(install.Root); }
        catch (InvalidDataException refused) { UninstallText.Text = Privacy.Scrub(refused.Message); return; }
        UninstallButton.IsEnabled = false;
        UninstallText.Text = "正在移除网络封锁：Windows 会请求权限…";
        try { await InstallNetworkBlock(install, uninstall: true); }
        catch (Exception error) when (error is SetupStopped or IOException or Win32Exception or InvalidOperationException or JsonException)
        {
            UninstallText.Text = (error is Win32Exception ? "Windows 未授予权限" : Privacy.Scrub(error.Message)) + "：没有移除任何内容。";
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
        if (!new InstallLayout(root).Exists) throw new InvalidDataException("这个文件夹不是 HellForge 安装；没有移除任何内容。");
        LocalInstall.NoLinks(root);
        var keep = new[] { Path.GetPathRoot(root), Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.Windows) };
        if (keep.Any(folder => !string.IsNullOrEmpty(folder) && string.Equals(LocalInstall.FullPath(folder), root, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException($"HellForge 直接安装在 {root}；请手动删除它的文件。");
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
            $"-InstallRoot {Arg(install.Root)} -ReportPath {Arg(report)}" + (uninstall ? " -Uninstall" : ""))
        { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = install.Root };
        await Task.Run(() =>
        {
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows PowerShell 未能启动。");
            process.WaitForExit();
        });
        var state = File.Exists(report) ? JsonDocument.Parse(File.ReadAllText(report)).RootElement : default;
        if (state.ValueKind != JsonValueKind.Object || SessionFiles.Text(state, "status") != (uninstall ? "uninstalled" : "installed"))
            throw new SetupStopped((uninstall ? "网络封锁未能移除：" : "网络封锁未能安装：") +
                (state.ValueKind == JsonValueKind.Object ? SessionFiles.Text(state, "failure") : "没有写出报告。"));
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
            (DesktopShortcutBox, Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "桌面"),
            (StartMenuShortcutBox, Environment.GetFolderPath(Environment.SpecialFolder.Programs), "开始菜单") })
        {
            if (box.IsChecked != true || string.IsNullOrEmpty(folder)) continue;
            var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("Windows 脚本宿主不可用。");
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
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows PowerShell 未能启动。");
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
        ? $"无法下载构建补丁：{download.InnerException?.GetBaseException().Message ?? download.Message} " +
          "这台电脑上可能有程序在拦截或检查 HTTPS（杀毒软件网页防护、VPN 或代理）；请放行后重试。"
        : error.Message;
}
