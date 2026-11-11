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
            var paths = LauncherPaths.Find(out var problem);
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
                desktop.MainWindow = MessageWindow(problem);
        }
        base.OnFrameworkInitializationCompleted();
    }

    // Shown instead of the launcher when it has no installation to run: why an unpacked package cannot be one where it
    // is (problem), else where the launcher belongs. Selectable, so the folder it names can be copied.
    static Window MessageWindow(string? problem) => new()
    {
        Title = "HellForge Launcher",
        Width = problem == null ? 480 : 620,
        Height = problem == null ? 150 : 300,
        WindowStartupLocation = WindowStartupLocation.CenterScreen,
        Content = new ScrollViewer
        {
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
            Content = new SelectableTextBlock
            {
                Text = problem ?? "Run the launcher from a HellForge installation or the HellForge repository.",
                Margin = new Thickness(24),
                TextWrapping = TextWrapping.Wrap,
                VerticalAlignment = VerticalAlignment.Center
            }
        }
    };
}
