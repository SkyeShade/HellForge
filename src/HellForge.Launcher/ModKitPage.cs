using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The Mods page's mod kit (docs/modding/first-mod.md): a mod starts as a copy of the SDK's template (app/sdk/templates/c-mod);
// BUILD compiles a mod's sources with the SDK's pinned Zig, ZIP packs it to share, and the mod tools are fetched
// on request. Builds wait while a session runs: the game has the mods' DLLs loaded.
public partial class MainWindow
{
    bool modKitBusy;
    string modKitStatus = "";

    ModSdk? Sdk => paths.InstallRoot != null ? ModSdk.Find(paths.InstallRoot) : null;

    // Mod folders with sources (src/ and a manifest): the ones BUILD can make.
    IEnumerable<string> SourceMods() => paths.ModsFolder == null || !Directory.Exists(paths.ModsFolder) ? [] :
        Directory.EnumerateDirectories(paths.ModsFolder).Where(f => !Path.GetFileName(f).StartsWith('.') &&
            File.Exists(Path.Combine(f, ModCatalog.ManifestName)) && Directory.Exists(Path.Combine(f, "src"))).Order(StringComparer.OrdinalIgnoreCase);

    // A card above the mod list: the tools prompt (source mods are built when a session starts, which
    // fetches the tools too; installing them here lets BUILD show a mod's errors early).
    void AddModKitCards(IReadOnlyCollection<string> loadedFolders)
    {
        var sdk = Sdk;
        if (sdk == null) return;
        if (SourceMods().Any() && !ModKit.ToolsReady(sdk))
        {
            var install = new Button { Content = modKitBusy ? "正在安装…" : "安装模组工具", Classes = { "primary" }, IsEnabled = !modKitBusy, Margin = new(0, 8, 0, 0) };
            install.Click += (_, _) => InstallModTools(sdk);
            var panel = new StackPanel { Spacing = 4 };
            panel.Children.Add(new TextBlock { Text = "模组工具", Classes = { "h2" }, Margin = new(0) });
            panel.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" },
                Text = "代码模组由 Zig 编译。HellForge 会把它一次下载到安装目录的 tools 文件夹（约 90 MB，来自 ziglang.org，固定版本并校验和；不会在系统范围内安装任何东西）。还会一并下载 CMake 和 Ninja，供 IDE 使用。" });
            if (modKitStatus.Length > 0) panel.Children.Add(new TextBlock { Text = modKitStatus, FontSize = 12, Foreground = Light });
            panel.Children.Add(install);
            ModsList.Children.Add(new Border { Classes = { "card" }, Child = panel });
        }
    }

    MenuItem BuildItem(ModManifest mod)
    {
        var build = new MenuItem { Header = "立即构建", IsEnabled = !modsLocked && !modKitBusy && Sdk is { } s && ModKit.ToolsReady(s) };
        ToolTip.SetShowOnDisabled(build, true);
        ToolTip.SetTip(build, modsLocked ? "构建会等到会话结束后进行：游戏已加载该模组。"
            : "像会话那样，立即用 HellForge 固定版本的 Zig 编译 src/（会话本来也会构建；这样可以提前看到错误）。");
        build.Click += (_, _) => BuildMod(mod);
        return build;
    }

    // The build a session would make (ModBuilds), now: its errors show on the mod's card.
    void BuildMod(ModManifest mod)
    {
        var sdk = Sdk;
        if (sdk == null || modsLocked || modKitBusy || paths.InstallRoot == null) return;
        modKitBusy = true;
        Append($"正在构建 {mod.Id}…", LauncherLine);
        RefreshMods();
        var install = new InstallLayout(paths.InstallRoot);
        Task.Run(() =>
        {
            try
            {
                var dll = ModBuilds.Ensure(install, sdk, mod, SupportedBuild.Current.BuildId, line => Dispatcher.UIThread.Post(() => Append(line, LauncherLine)));
                Dispatcher.UIThread.Post(() => Append($"{mod.Id} 已构建（{new FileInfo(dll).Length / 1024} KB）；下次会话会加载它。", Green));
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            { Dispatcher.UIThread.Post(() => Append($"{mod.Id} {failure.Message}", Red)); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }

    MenuItem ZipItem(ModManifest mod)
    {
        var needsBuild = mod.HasSource;
        var zip = new MenuItem { Header = "打包分享", IsEnabled = !modKitBusy && !(needsBuild && (modsLocked || Sdk is not { } s || !ModKit.ToolsReady(s))) };
        ToolTip.SetShowOnDisabled(zip, true);
        ToolTip.SetTip(zip, needsBuild && modsLocked ? "打包前会先构建模组，因此会等到会话结束。"
            : "把模组打包到安装目录的 exports 文件夹以便分享：直接把 zip 放进 mods 文件夹即可。" +
              (needsBuild ? "会先构建，所以编译失败的模组不会被打包；zip 里放的是源码，不是 DLL。" : ""));
        zip.Click += (_, _) => ZipMod(mod);
        return zip;
    }

    // ModPackage, after the build a session would make (a source mod that does not compile is not packed); then the zip
    // is shown in Explorer.
    void ZipMod(ModManifest mod)
    {
        if (modKitBusy || paths.InstallRoot == null) return;
        var sdk = Sdk;
        if (mod.HasSource && (sdk == null || modsLocked)) return;
        modKitBusy = true;
        Append($"正在打包 {mod.Id}…", LauncherLine);
        RefreshMods();
        var install = new InstallLayout(paths.InstallRoot);
        Task.Run(() =>
        {
            try
            {
                if (mod.HasSource) ModBuilds.Ensure(install, sdk!, mod, SupportedBuild.Current.BuildId, line => Dispatcher.UIThread.Post(() => Append(line, LauncherLine)));
                var zip = ModPackage.Create(mod, install.Exports);
                Dispatcher.UIThread.Post(() =>
                {
                    Append($"{mod.Id} 已打包：{zip}（{new FileInfo(zip).Length / 1024} KB）。分享这个文件；其他玩家直接把它放进 mods 文件夹。", Green);
                    if (OperatingSystem.IsWindows())
                        try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{zip}\"") { UseShellExecute = false }); }
                        catch (System.ComponentModel.Win32Exception) { }
                });
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            { Dispatcher.UIThread.Post(() => Append($"{mod.Id} 未能打包：{failure.Message}", Red)); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }

    void InstallModTools(ModSdk sdk)
    {
        if (modKitBusy) return;
        modKitBusy = true;
        modKitStatus = "正在启动…";
        RefreshMods();
        Task.Run(async () =>
        {
            try
            {
                await ModKit.InstallToolsAsync(sdk, ide: true, line => Dispatcher.UIThread.Post(() => { modKitStatus = line; Append(line, LauncherLine); RefreshMods(); }));
                Dispatcher.UIThread.Post(() => Append("模组工具已安装到 " + sdk.Tools, Green));
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or HttpRequestException or TaskCanceledException or UnauthorizedAccessException)
            { Dispatcher.UIThread.Post(() => { modKitStatus = "未安装：" + failure.Message; Append("模组工具未安装：" + failure.Message, Red); }); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }
}
