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
                    ? "没有可导入的内容：这台电脑上还没有正常版 HELLDIVERS 2 的设置（先玩一次，或确认使用的是同一个 Windows 用户）。"
                    : "已导入到 " + Selected.Name + "：" + string.Join("、", result.Copied) + "。下次会话会使用它们。" +
                      (result.Missing.Count > 0 ? " 未找到：" + string.Join("、", result.Missing) + "。" : "");
                ShowImportStatus(text, result.Copied.Count == 0);
                Append($"Profile {Selected.Name}: game settings imported from HELLDIVERS 2 ({string.Join(", ", result.Copied)}).", LauncherLine);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
            {
                ShowImportStatus("设置无法导入：" + error.Message, true);
            }
            UpdateSettingsImport();
        };
        UndoImportButton.Click += (_, _) =>
        {
            if (runner.IsRunning || paths.InstallRoot == null) return;
            try
            {
                var count = GameSettings.UndoImport(new InstallLayout(paths.InstallRoot), Selected.Name);
                ShowImportStatus(count > 0 ? "该档案自己的设置已恢复。" : "没有可撤回的导入。", false);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                ShowImportStatus("无法撤回导入：" + error.Message, true);
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
        ToolTip.SetTip(ImportSettingsButton, running ? "请先停止会话：游戏会在关闭时写入设置。" : null);
    }
}
