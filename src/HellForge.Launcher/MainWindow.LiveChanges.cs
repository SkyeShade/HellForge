using HellForge.Core;

namespace HellForge.Launcher;

// MainWindow's Settings > EXPERIMENTAL "Change mods without restarting" (ModSettings.LiveChanges): read when a session
// starts, so it applies from the next one.
public partial class MainWindow
{
    void InitializeLiveChanges()
    {
        LiveChangesBox.IsEnabled = paths.ModsFolder != null;
        LiveChangesBox.IsChecked = paths.ModsFolder != null && ModSettings.LiveChanges(paths.ModsFolder);
        LiveChangesBox.IsCheckedChanged += (_, _) =>
        {
            if (paths.ModsFolder == null) return;
            try { ModSettings.SetLiveChanges(paths.ModsFolder, LiveChangesBox.IsChecked == true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save Change mods without restarting: " + error.Message, Red); }
        };
    }
}
