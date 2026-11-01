using Avalonia.Controls;
using HellForge.Core;

namespace HellForge.Launcher;

// The Game settings card on the profile page: the normal HELLDIVERS 2's options (and key bindings) copied into the
// profile again (GameSettings.ImportFromGame), and the last import taken back. Not while a session runs: the game writes
// these files when it closes.
public partial class MainWindow
{
    void InitializeSettingsImport()
    {
        ImportSettingsButton.Click += (_, _) =>
        {
            if (runner.IsRunning || paths.InstallRoot == null) return;
            var install = new InstallLayout(paths.InstallRoot);
            var withControls = ImportControlsBox.IsChecked == true;
            try
            {
                var result = GameSettings.ImportFromGame(install, Selected.Name, GameSettings.VanillaUserData(), GameSettings.SteamControls(), withControls);
                var text = result.Copied.Count == 0
                    ? "Nothing to import: the normal HELLDIVERS 2 has no settings on this PC yet (play it once, or check that it is the same " +
                      (LinuxNetworkBlock.UnderWine ? "Steam installation HellForge runs under)." : "Windows user).")
                    : "Imported into " + Selected.Name + ": " + string.Join(", ", result.Copied) + ". The next session uses them." +
                      (result.Missing.Count > 0 ? " Not found: " + string.Join(", ", result.Missing) + "." : "");
                ShowImportStatus(text, result.Copied.Count == 0);
                Append($"Profile {Selected.Name}: game settings imported from HELLDIVERS 2 ({string.Join(", ", result.Copied)}).", LauncherLine);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
            {
                ShowImportStatus("The settings could not be imported: " + error.Message, true);
            }
            UpdateSettingsImport();
        };
        UndoImportButton.Click += (_, _) =>
        {
            if (runner.IsRunning || paths.InstallRoot == null) return;
            try
            {
                var count = GameSettings.UndoImport(new InstallLayout(paths.InstallRoot), Selected.Name);
                ShowImportStatus(count > 0 ? "The profile's own settings are back." : "There was no import to take back.", false);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                ShowImportStatus("The import could not be taken back: " + error.Message, true);
            }
            UpdateSettingsImport();
        };
    }

    void ShowImportStatus(string text, bool problem)
    {
        ImportSettingsStatus.Text = text;
        ImportSettingsStatus.Foreground = problem ? Red : Light;
    }

    // The card follows the selected profile and the session: the buttons wait while the game runs.
    void UpdateSettingsImport()
    {
        var install = paths.InstallRoot != null ? new InstallLayout(paths.InstallRoot) : null;
        var running = runner.IsRunning;
        ImportSettingsButton.IsEnabled = install != null && !running;
        UndoImportButton.IsEnabled = install != null && !running && GameSettings.CanUndoImport(install, Selected.Name);
        ImportControlsBox.IsEnabled = !running;
        ToolTip.SetTip(ImportSettingsButton, running ? "Stop the session first: the game writes its settings when it closes." : null);
    }
}
