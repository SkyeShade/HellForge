using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.Versioning;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using HellForge.Core;

namespace HellForge.Launcher;

[SupportedOSPlatform("linux")]
sealed class LinuxInstallerWindow : Window
{
    readonly string payload;
    readonly TextBox prefixPath;
    readonly ComboBox protonChoice;
    readonly TextBlock status;
    readonly Button installButton;
    readonly IReadOnlyList<ProtonVersion> protons;

    public LinuxInstallerWindow(string payload)
    {
        this.payload = Path.GetFullPath(payload);
        Title = "HellForge Linux setup";
        Width = 620;
        Height = 390;
        MinWidth = 520;
        MinHeight = 350;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Background = new SolidColorBrush(Color.Parse("#11141B"));
        Foreground = Brushes.White;

        var defaultPrefix = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share", "hellforge-prefix");
        prefixPath = new TextBox { Text = defaultPrefix, HorizontalAlignment = HorizontalAlignment.Stretch };
        string? protonError = null;
        try { protons = SteamLocator.ProtonVersions(); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            protons = [];
            protonError = "Could not read Steam's Proton installations: " + error.Message;
        }
        var configuredPath = Environment.GetEnvironmentVariable("PROTON");
        var configuredProton = !string.IsNullOrEmpty(configuredPath) && File.Exists(configuredPath)
            ? LocalInstall.FullPath(configuredPath) : null;
        var selectedIndex = protons.ToList().FindIndex(proton => proton.ScriptPath == configuredProton);
        if (selectedIndex < 0) selectedIndex = protons.Count - 1;
        protonChoice = new ComboBox
        {
            ItemsSource = protons.Select(proton => proton.Name).ToArray(),
            SelectedIndex = selectedIndex,
            HorizontalAlignment = HorizontalAlignment.Stretch
        };
        status = new TextBlock
        {
            Text = protonError ?? (protons.Count == 0
                ? "No Proton versions were found. Install Proton in Steam, then restart this installer."
                : "Choose the Proton version to prepare the prefix with. The same version will be used for setup and game sessions."),
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(Color.Parse("#B8C0CE"))
        };
        installButton = new Button { Content = "Install and start setup", HorizontalAlignment = HorizontalAlignment.Right, IsEnabled = protons.Count > 0 };
        installButton.Click += async (_, _) => await InstallAndLaunch();

        var browseButton = new Button { Content = "Choose…", Margin = new Thickness(8, 0, 0, 0) };
        browseButton.Click += async (_, _) => await ChoosePrefix();
        var prefixRow = new DockPanel();
        DockPanel.SetDock(browseButton, Dock.Right);
        prefixRow.Children.Add(browseButton);
        prefixRow.Children.Add(prefixPath);

        Content = new StackPanel
        {
            Margin = new Thickness(32),
            Spacing = 18,
            Children =
            {
                new TextBlock { Text = "HELLFORGE", FontSize = 13, FontWeight = FontWeight.Bold, Foreground = new SolidColorBrush(Color.Parse("#E44B4B")) },
                new TextBlock { Text = "Set up on Linux", FontSize = 30, FontWeight = FontWeight.Bold },
                new TextBlock
                {
                    Text = "HellForge will prepare its own Proton prefix and install the launcher there. Steam and Proton must already be installed, and Steam should be running and signed in.",
                    TextWrapping = TextWrapping.Wrap,
                    Foreground = new SolidColorBrush(Color.Parse("#B8C0CE"))
                },
                new TextBlock { Text = "Proton version", FontWeight = FontWeight.SemiBold },
                protonChoice,
                new TextBlock { Text = "Proton prefix folder", FontWeight = FontWeight.SemiBold },
                prefixRow,
                status,
                installButton
            }
        };
    }

    async Task ChoosePrefix()
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose a folder for HellForge's Proton prefix",
            AllowMultiple = false
        });
        if (folders.Count > 0) prefixPath.Text = folders[0].Path.LocalPath;
    }

    async Task InstallAndLaunch()
    {
        installButton.IsEnabled = false;
        try
        {
            if (!File.Exists(payload)) throw new InvalidDataException("The HellForge Linux package was not found beside this installer.");
            ValidatePackage(payload);
            var steamRoot = SteamLocator.SteamRoot()
                ?? throw new InvalidDataException("Steam was not found. Install Steam, start it, sign in, and try again.");
            if (protonChoice.SelectedIndex < 0 || protonChoice.SelectedIndex >= protons.Count)
                throw new InvalidDataException("Choose an installed Proton version before continuing.");
            var proton = protons[protonChoice.SelectedIndex].ScriptPath;
            var prefixText = prefixPath.Text?.Trim() ?? "";
            if (prefixText.Length == 0) throw new InvalidDataException("Choose a folder for the HellForge prefix.");
            var prefix = Path.GetFullPath(prefixText);
            if (prefix == Path.GetPathRoot(prefix))
                throw new InvalidDataException("Choose a folder for the HellForge prefix.");

            var installRoot = Path.Combine(prefix, "pfx", "drive_c", "users", "steamuser", "Documents", "HellForge");
            var launcher = Path.Combine(installRoot, "app", "launcher-linux", "HellForge.Launcher");
            if (File.Exists(launcher))
                Status("Existing installation found; updating its app files and preserving game data...");
            else
            {
                Status("Preparing the Proton prefix...");
                await InitializePrefix(proton, prefix, steamRoot);
            }

            Status("Extracting HellForge...");
            Directory.CreateDirectory(installRoot);
            ZipFile.ExtractToDirectory(payload, installRoot, overwriteFiles: true);
            MakeLinuxFilesExecutable(installRoot);
            SteamLocator.SaveProtonSelection(new InstallLayout(installRoot), proton);
            if (!File.Exists(launcher))
                throw new InvalidDataException("The package did not contain app/launcher-linux/HellForge.Launcher.");

            Status("Starting the native launcher and setup wizard...");
            Process.Start(new ProcessStartInfo(launcher) { WorkingDirectory = installRoot, UseShellExecute = true });
            Close();
        }
        catch (Exception error)
        {
            Status(error.Message);
            installButton.IsEnabled = protons.Count > 0;
        }
    }

    async Task InitializePrefix(string proton, string prefix, string steamRoot)
    {
        Directory.CreateDirectory(prefix);
        var start = new ProcessStartInfo(proton) { WorkingDirectory = Path.GetDirectoryName(proton)!, UseShellExecute = false };
        foreach (var argument in new[] { "run", "wineboot", "-u" }) start.ArgumentList.Add(argument);
        start.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = steamRoot;
        start.Environment["STEAM_COMPAT_DATA_PATH"] = prefix;
        start.Environment["STEAM_COMPAT_APP_ID"] = SteamOwnershipCheck.AppId.ToString(System.Globalization.CultureInfo.InvariantCulture);
        using var process = Process.Start(start) ?? throw new InvalidDataException("Proton did not start.");
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch (OperationCanceledException)
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            throw new InvalidDataException("Proton took too long to prepare the prefix.");
        }
        if (process.ExitCode != 0) throw new InvalidDataException($"Proton could not prepare the prefix (exit code {process.ExitCode}).");
    }

    static void ValidatePackage(string archivePath)
    {
        using var archive = ZipFile.OpenRead(archivePath);
        var entries = archive.Entries.Select(entry => entry.FullName.Replace('\\', '/')).ToHashSet(StringComparer.Ordinal);
        foreach (var required in new[]
        {
            "app/app-build.json",
            "app/engine-package.zip",
            "app/launcher-linux/HellForge.Launcher",
            "app/linux/hellforge-linux.sh"
        })
            if (!entries.Contains(required)) throw new InvalidDataException("The Linux package is incomplete; missing " + required + ".");
    }

    static void MakeLinuxFilesExecutable(string installRoot)
    {
        const UnixFileMode executable = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
        var launcher = Path.Combine(installRoot, "app", "launcher-linux", "HellForge.Launcher");
        File.SetUnixFileMode(launcher, File.GetUnixFileMode(launcher) | executable);
        foreach (var script in Directory.EnumerateFiles(Path.Combine(installRoot, "app", "linux"), "*.sh"))
            File.SetUnixFileMode(script, File.GetUnixFileMode(script) | executable);
    }

    void Status(string message) => status.Text = message;
}
