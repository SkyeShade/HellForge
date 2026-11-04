using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Layout;
using Avalonia.Markup.Xaml;
using Avalonia.Media;

namespace HellForge.Launcher;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var paths = LauncherPaths.Find();
            var installerPayload = OperatingSystem.IsLinux()
                ? Environment.GetEnvironmentVariable("HELLFORGE_INSTALLER_PAYLOAD") ??
                    (File.Exists(Path.Combine(AppContext.BaseDirectory, "HellForge-Linux.zip"))
                        ? Path.Combine(AppContext.BaseDirectory, "HellForge-Linux.zip") : null)
                : null;
            if (OperatingSystem.IsLinux() && installerPayload != null)
                desktop.MainWindow = new LinuxInstallerWindow(installerPayload);
            else if (paths != null)
                desktop.MainWindow = new MainWindow(paths);
            else
                desktop.MainWindow = new Window
            {
                Title = "HellForge Launcher",
                Width = 480,
                Height = 150,
                WindowStartupLocation = WindowStartupLocation.CenterScreen,
                Content = new TextBlock
                {
                    Text = "Run the launcher from a HellForge installation or the HellForge repository.",
                    Margin = new Thickness(24),
                    TextWrapping = TextWrapping.Wrap,
                    VerticalAlignment = VerticalAlignment.Center
                }
            };
        }
        base.OnFrameworkInitializationCompleted();
    }
}
