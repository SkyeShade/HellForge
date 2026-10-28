using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.Versioning;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
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
    IReadOnlyList<ProtonVersion> protons = [];
    bool installing;

    public LinuxInstallerWindow(string payload)
    {
        this.payload = Path.GetFullPath(payload);
        Title = "HellForge Linux setup";
        Width = 620;
        Height = 560;
        MinWidth = 440;
        MinHeight = 280;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Background = new SolidColorBrush(Color.Parse("#11141B"));
        Foreground = Brushes.White;

        var defaultPrefix = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share", "hellforge-prefix");
        prefixPath = new TextBox { Text = defaultPrefix, HorizontalAlignment = HorizontalAlignment.Stretch };
        protonChoice = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
        status = new TextBlock
        {
            Text = "Looking for installed Proton versions...",
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(Color.Parse("#B8C0CE"))
        };
        installButton = new Button { Content = "Install and start setup", HorizontalAlignment = HorizontalAlignment.Right, IsEnabled = false };
        installButton.Click += async (_, _) => await InstallAndLaunch();

        var browseButton = new Button { Content = "Choose…", Margin = new Thickness(8, 0, 0, 0) };
        browseButton.Click += async (_, _) => await ChoosePrefix();
        var prefixRow = new DockPanel();
        DockPanel.SetDock(browseButton, Dock.Right);
        prefixRow.Children.Add(browseButton);
        prefixRow.Children.Add(prefixPath);

        var body = new StackPanel
        {
            Margin = new Thickness(32, 32, 32, 0),
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
                status
            }
        };
        // The body scrolls when the screen or the player makes the window short; the button row stays outside the
        // ScrollViewer and docked to the bottom, so it is visible at every window size.
        var bodyScroll = new ScrollViewer
        {
            Content = body,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled
        };
        var buttonRow = new StackPanel
        {
            Margin = new Thickness(32, 18, 32, 32),
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Children = { installButton }
        };
        var layout = new DockPanel();
        DockPanel.SetDock(buttonRow, Dock.Bottom);
        layout.Children.Add(buttonRow);
        layout.Children.Add(bodyScroll);
        Content = layout;
        // The window is shown before this runs, so a slow Steam library does not hold up the first paint.
        Opened += (sender, args) =>
        {
            FitToScreen();
            _ = DiscoverProtons();
        };
    }

    // The default size fits the whole body on a large screen. On a small or heavily scaled screen, shrink to the
    // screen's working area (in logical pixels, less a margin) so the window and its docked button stay on screen; the
    // body then scrolls.
    void FitToScreen()
    {
        var screen = Screens.ScreenFromWindow(this) ?? Screens.Primary;
        if (screen is null || screen.Scaling <= 0) return;
        const double margin = 16;
        Width = Math.Max(MinWidth, Math.Min(Width, screen.WorkingArea.Width / screen.Scaling - margin * 2));
        Height = Math.Max(MinHeight, Math.Min(Height, screen.WorkingArea.Height / screen.Scaling - margin * 2));
    }

    // Steam's libraries and Proton installations are on disk and can take a while (many libraries, a slow disk): find them
    // after the window is shown, then fill the list on the UI thread. Install stays off until the result is known.
    async Task DiscoverProtons()
    {
        (IReadOnlyList<ProtonVersion> Protons, string? Error, string? ConfiguredProton) found;
        try { found = await Task.Run(FindProtons).ConfigureAwait(false); }
        catch (Exception error)
        {
            found = (Array.Empty<ProtonVersion>(), "Could not look up Steam's Proton installations: " + error.Message, null);
        }
        Dispatcher.UIThread.Post(() => ApplyProtons(found));
    }

    static (IReadOnlyList<ProtonVersion> Protons, string? Error, string? ConfiguredProton) FindProtons()
    {
        IReadOnlyList<ProtonVersion> protons;
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
        return (Protons: protons, Error: protonError, ConfiguredProton: configuredProton);
    }

    void ApplyProtons((IReadOnlyList<ProtonVersion> Protons, string? Error, string? ConfiguredProton) found)
    {
        protons = found.Protons;
        var selectedIndex = protons.ToList().FindIndex(proton => proton.ScriptPath == found.ConfiguredProton);
        if (selectedIndex < 0) selectedIndex = protons.Count - 1;
        protonChoice.ItemsSource = protons.Select(proton => proton.Name).ToArray();
        protonChoice.SelectedIndex = selectedIndex;
        status.Text = protons.Count == 0
            ? found.Error ?? "No Proton versions were found. Install Proton in Steam, then restart this installer."
            : "Choose the Proton version to prepare the prefix with. The same version will be used for setup and game sessions.";
        installButton.IsEnabled = !installing && protons.Count > 0;
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
        if (installing) return;
        installing = true;
        installButton.IsEnabled = false;
        try
        {
            if (!File.Exists(payload)) throw new InvalidDataException("The HellForge Linux package was not found beside this installer.");
            // Reading the package's index and walking Steam's folders are disk work too; keep both off the UI thread.
            var steamRoot = await Task.Run(() =>
            {
                ValidatePackage(payload);
                return SteamLocator.SteamRoot()
                    ?? throw new InvalidDataException("Steam was not found. Install Steam, start it, sign in, and try again.");
            });
            if (protonChoice.SelectedIndex < 0 || protonChoice.SelectedIndex >= protons.Count)
                throw new InvalidDataException("Choose an installed Proton version before continuing.");
            var proton = protons[protonChoice.SelectedIndex].ScriptPath;
            var prefixText = prefixPath.Text?.Trim() ?? "";
            if (prefixText.Length == 0) throw new InvalidDataException("Choose a folder for the HellForge prefix.");
            var prefix = Path.GetFullPath(prefixText);
            if (prefix == Path.GetPathRoot(prefix))
                throw new InvalidDataException("Choose a folder for the HellForge prefix.");

            var installRoot = Path.Combine(prefix, "pfx", "drive_c", "users", "steamuser", "Documents", "HellForge");
            var install = new InstallLayout(installRoot);
            var updating = File.Exists(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"));
            if (updating)
                Status("Existing installation found; updating its app files and preserving game data...");
            else
            {
                Status("Preparing the Proton prefix...");
                await InitializePrefix(proton, prefix, steamRoot);
            }

            // The status above is painted before this yield; the package is staged and swapped in without blocking repaints.
            Status(updating ? "Updating HellForge..." : "Extracting HellForge...");
            var previousApp = await Task.Run(() => LinuxAppInstaller.Install(install, payload, () =>
            {
                EnginePackageInstaller.Install(install);
                SteamLocator.SaveProtonSelection(install, proton);
            }));
            var launcher = Path.Combine(install.App, "launcher-linux", "HellForge.Launcher");
            Status("Starting the native launcher and setup wizard...");
            try
            {
                _ = Process.Start(new ProcessStartInfo(launcher) { WorkingDirectory = installRoot, UseShellExecute = true }) ??
                    throw new InvalidDataException("The new launcher did not start.");
            }
            catch (Exception error)
            {
                if (previousApp != null)
                {
                    await Task.Run(() => LinuxAppInstaller.Rollback(install, previousApp));
                    throw new InvalidDataException("The new launcher could not start: " + error.Message + " The previous installation was restored.", error);
                }
                throw new InvalidDataException("HellForge was installed, but the new launcher could not start: " + error.Message, error);
            }
            await Task.Run(() => LinuxAppInstaller.Commit(install, previousApp));
            installing = false;
            Close();
        }
        catch (Exception error)
        {
            Status(error.Message);
        }
        finally
        {
            installing = false;
            installButton.IsEnabled = protons.Count > 0;
        }
    }

    async Task InitializePrefix(string proton, string prefix, string steamRoot)
    {
        // Record whether this installer makes the prefix before wineboot fills it. Only a new or empty folder is ours to
        // offer for deletion later; a prefix HELLDIVERS 2 or the player chose may be shared and never gets the marker.
        var existed = Directory.Exists(prefix);
        var empty = !existed || !Directory.EnumerateFileSystemEntries(prefix).Any();
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
        LinuxUninstall.WritePrefixMarker(prefix, existed, empty);
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

    void Status(string message) => status.Text = message;
}
