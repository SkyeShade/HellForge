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
            var install = new Button { Content = modKitBusy ? "Installing..." : "Install mod tools", Classes = { "primary" }, IsEnabled = !modKitBusy, Margin = new(0, 8, 0, 0) };
            install.Click += (_, _) => InstallModTools(sdk);
            var panel = new StackPanel { Spacing = 4 };
            panel.Children.Add(new TextBlock { Text = "MOD TOOLS", Classes = { "h2" }, Margin = new(0) });
            panel.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" },
                Text = "Code mods are compiled by Zig, which HellForge fetches once into the installation's tools folder (about 90 MB from ziglang.org, pinned and checksummed; nothing is installed system-wide). CMake and Ninja come too, for IDEs." });
            if (modKitStatus.Length > 0) panel.Children.Add(new TextBlock { Text = modKitStatus, FontSize = 12, Foreground = Light });
            panel.Children.Add(install);
            ModsList.Children.Add(new Border { Classes = { "card" }, Child = panel });
        }
    }

    MenuItem BuildItem(ModManifest mod)
    {
        var sdk = Sdk;
        var linux = OperatingSystem.IsLinux();
        var linuxBuilder = paths.InstallRoot == null ? null : Path.Combine(paths.InstallRoot, "app", "linux", "hellforge-linux-install.sh");
        var build = new MenuItem
        {
            Header = "Build now",
            IsEnabled = !modsLocked && !modKitBusy && sdk != null &&
                (linux ? linuxBuilder != null && File.Exists(linuxBuilder) : ModKit.ToolsReady(sdk))
        };
        ToolTip.SetShowOnDisabled(build, true);
        ToolTip.SetTip(build, modsLocked ? "Builds wait until the session ends: the game has the mod loaded."
            : linux ? "Compiles src/ with native Linux Zig via app/linux/hellforge-linux-install.sh."
            : "Compiles src/ with HellForge's pinned Zig now, as a session would (sessions build it anyway; this shows errors early).");
        build.Click += (_, _) => BuildMod(mod);
        return build;
    }

    // The build a session would make (ModBuilds), now: its errors show on the mod's card.
    void BuildMod(ModManifest mod)
    {
        var sdk = Sdk;
        if (sdk == null || modsLocked || modKitBusy || paths.InstallRoot == null) return;
        modKitBusy = true;
        Append($"Building {mod.Id}...", LauncherLine);
        RefreshMods();
        var install = new InstallLayout(paths.InstallRoot);
        Task.Run(() =>
        {
            try
            {
                if (OperatingSystem.IsLinux())
                {
                    var helper = Path.Combine(install.App, "linux", "hellforge-linux-install.sh");
                    if (!File.Exists(helper)) throw new InvalidDataException("the Linux mod-build helper is missing; reinstall HellForge.");
                    var start = new ProcessStartInfo("bash")
                    {
                        WorkingDirectory = install.Root,
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true
                    };
                    start.ArgumentList.Add(helper);
                    start.ArgumentList.Add("build");
                    start.ArgumentList.Add(mod.Folder);
                    start.Environment["PFX"] = WinePath.PrefixFromInstallRoot(install.Root);
                    using var process = Process.Start(start) ?? throw new InvalidDataException("the Linux mod-build helper did not start.");
                    void ReportLine(object? _, DataReceivedEventArgs e)
                    {
                        if (e.Data is { Length: > 0 } line) Dispatcher.UIThread.Post(() => Append(line, LauncherLine));
                    }
                    process.OutputDataReceived += ReportLine;
                    process.ErrorDataReceived += ReportLine;
                    process.BeginOutputReadLine();
                    process.BeginErrorReadLine();
                    process.WaitForExit();
                    process.WaitForExit();
                    if (process.ExitCode != 0) throw new InvalidDataException($"the Linux mod-build helper exited with code {process.ExitCode}.");
                    Dispatcher.UIThread.Post(() => Append($"{mod.Id} built with native Linux Zig.", Green));
                }
                else
                {
                    var dll = ModBuilds.Ensure(install, sdk, mod, SupportedBuild.Current.BuildId,
                        line => Dispatcher.UIThread.Post(() => Append(line, LauncherLine)));
                    Dispatcher.UIThread.Post(() => Append($"{mod.Id} is built ({new FileInfo(dll).Length / 1024} KB); the next session loads it.", Green));
                }
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            { Dispatcher.UIThread.Post(() => Append($"{mod.Id} {failure.Message}", Red)); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }

    MenuItem ZipItem(ModManifest mod)
    {
        var needsBuild = mod.HasSource;
        var zip = new MenuItem { Header = "Zip to share", IsEnabled = !modKitBusy && !(needsBuild && (modsLocked || Sdk is not { } s || !ModKit.ToolsReady(s))) };
        ToolTip.SetShowOnDisabled(zip, true);
        ToolTip.SetTip(zip, needsBuild && modsLocked ? "Zipping builds the mod first, which waits until the session ends."
            : "Packs the mod to share, into the installation's exports folder: drop the zip into a mods folder as it is. " +
              (needsBuild ? "It is built first, so a mod that does not compile is not packed; the zip holds its sources, not a DLL." : ""));
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
        Append($"Packing {mod.Id}...", LauncherLine);
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
                    Append($"{mod.Id} packed: {zip} ({new FileInfo(zip).Length / 1024} KB). Share that file; players drop it into their mods folder.", Green);
                    if (OperatingSystem.IsWindows())
                        try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{zip}\"") { UseShellExecute = false }); }
                        catch (System.ComponentModel.Win32Exception) { }
                });
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            { Dispatcher.UIThread.Post(() => Append($"{mod.Id} was not packed: {failure.Message}", Red)); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }

    void InstallModTools(ModSdk sdk)
    {
        if (modKitBusy) return;
        modKitBusy = true;
        modKitStatus = "Starting...";
        RefreshMods();
        Task.Run(async () =>
        {
            try
            {
                await ModKit.InstallToolsAsync(sdk, ide: true, line => Dispatcher.UIThread.Post(() => { modKitStatus = line; Append(line, LauncherLine); RefreshMods(); }));
                Dispatcher.UIThread.Post(() => Append("Mod tools installed in " + sdk.Tools, Green));
            }
            catch (Exception failure) when (failure is IOException or InvalidDataException or HttpRequestException or TaskCanceledException or UnauthorizedAccessException)
            { Dispatcher.UIThread.Post(() => { modKitStatus = "Not installed: " + failure.Message; Append("Mod tools not installed: " + failure.Message, Red); }); }
            finally { Dispatcher.UIThread.Post(() => { modKitBusy = false; RefreshMods(); }); }
        });
    }
}
