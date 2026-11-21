using Avalonia;

namespace HellForge.Launcher;

static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        // Started from a Steam shortcut: what HellForge starts must not sign in to Steam as the shortcut (#434).
        HellForge.Core.SteamShortcutEnvironment.Forget();
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    // Also used by the Avalonia designer.
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}
