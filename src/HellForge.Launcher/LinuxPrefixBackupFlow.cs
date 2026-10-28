using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using Avalonia;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// Settings > Installation > Prefix backup on Linux: back up the whole Proton prefix with the system's tar and zstd (or
// gzip when zstd is missing), and restore one. A restore is listed and unpacked beside the prefix first; because this
// launcher runs from inside the prefix it cannot swap it, so a small helper does that after the launcher exits and then
// starts the restored launcher again.
public partial class MainWindow
{
    bool linuxArchiveBusy;
    CancellationTokenSource? linuxBackupCancel;
    string? linuxRestoreArchive;

    void InitializeLinuxPrefixBackup(InstallLayout install)
    {
        LinuxBackupCard.IsVisible = true;
        LinuxBackupFolder.Text = LinuxPrefixBackup.DefaultFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        LinuxBackupText.Text = "";
        LinuxBackupFolderButton.Click += async (_, _) => await ChooseLinuxBackupFolder();
        LinuxBackupButton.Click += async (_, _) => await BackupLinuxPrefix(install);
        LinuxRestoreButton.Click += async (_, _) => await ChooseLinuxRestoreArchive(install);
        LinuxBackupCancelButton.Click += (_, _) => linuxBackupCancel?.Cancel();
        LinuxRestoreConfirmButton.Click += async (_, _) => await RestoreLinuxPrefix(install);
        LinuxRestoreCancelButton.Click += (_, _) =>
        {
            LinuxRestoreConfirm.IsVisible = false;
            linuxRestoreArchive = null;
        };
    }

    async Task ChooseLinuxBackupFolder()
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose where prefix backups are saved (default ~/hellforge-backups)",
            AllowMultiple = false
        });
        if (folders.Count > 0) LinuxBackupFolder.Text = folders[0].Path.LocalPath;
    }

    async Task BackupLinuxPrefix(InstallLayout install)
    {
        if (linuxArchiveBusy) return;
        if (runner.IsRunning || setupRunning) { LinuxBackupText.Text = "Stop the session (or setup) first."; return; }
        LinuxRestoreConfirm.IsVisible = false;
        linuxRestoreArchive = null;
        var text = LinuxBackupFolder.Text?.Trim() ?? "";
        if (text.Length == 0) { LinuxBackupText.Text = "Choose a folder for the backup."; return; }
        string prefix;
        try { prefix = WinePath.PrefixFromInstallRoot(install.Root); }
        catch (InvalidOperationException error) { LinuxBackupText.Text = error.Message; return; }
        var folder = Path.GetFullPath(text);
        var locations = UninstallLocations();
        var compression = LinuxPrefixBackup.ChooseCompression(LinuxPrefixBackup.ZstdInstalled());
        var cancellation = new CancellationTokenSource();
        linuxBackupCancel = cancellation;
        linuxArchiveBusy = true;
        SetLinuxArchiveBusy(true);
        LinuxBackupCancelButton.IsVisible = true;
        LinuxBackupProgress.IsVisible = true;
        LinuxBackupProgress.IsIndeterminate = true;
        LinuxBackupText.Text = $"Measuring {prefix}...";
        try
        {
            var prepared = await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                LinuxPrefixBackup.CheckBackupSource(prefix, locations, SteamLocator.SteamRoot(), install.Root);
                LinuxPrefixBackup.CheckBackupFolder(prefix, folder);
                var bytes = LinuxPrefixBackup.EstimateBytes(prefix);
                cancellation.Token.ThrowIfCancellationRequested();
                if (LinuxPrefixBackup.BackupSpaceProblem(LinuxPrefixBackup.FreeBytes(folder), bytes) is { } space)
                    throw new NotEnoughSpaceException(space);
                return (Archive: LinuxPrefixBackup.NextArchive(folder, DateTime.Now, compression), Bytes: bytes);
            }, cancellation.Token);
            LinuxBackupText.Text = $"Writing {Path.GetFileName(prepared.Archive)} from {DiskSpace.GB(prepared.Bytes)}...";
            var completed = false;
            IProgress<long> progress = new Progress<long>(written =>
            {
                if (!completed && linuxArchiveBusy) LinuxBackupText.Text =
                    $"Writing {Path.GetFileName(prepared.Archive)}... {DiskSpace.GB(written)} so far.";
            });
            var result = await Task.Run(() => LinuxPrefixBackup.Backup(prefix, prepared.Archive, compression,
                progress.Report, cancellation.Token), cancellation.Token);
            completed = true;
            LinuxBackupText.Text = $"Saved {result.Archive} ({DiskSpace.GB(result.Bytes)}). Restore it from this page.";
        }
        catch (OperationCanceledException)
        {
            LinuxBackupText.Text = "Backup cancelled; the partial archive was deleted.";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or
                                          InvalidOperationException or Win32Exception)
        {
            LinuxBackupText.Text = Privacy.Scrub(error.Message);
        }
        finally
        {
            linuxArchiveBusy = false;
            SetLinuxArchiveBusy(false);
            LinuxBackupCancelButton.IsVisible = false;
            LinuxBackupProgress.IsVisible = false;
            linuxBackupCancel = null;
            cancellation.Dispose();
        }
    }

    async Task ChooseLinuxRestoreArchive(InstallLayout install)
    {
        if (linuxArchiveBusy) return;
        // The backup card shows the folder the backups are written to; open the picker there when it exists.
        IStorageFolder? backupFolder = null;
        var backupText = LinuxBackupFolder.Text?.Trim() ?? "";
        if (backupText.Length > 0 && Directory.Exists(backupText))
            backupFolder = await StorageProvider.TryGetFolderFromPathAsync(Path.GetFullPath(backupText));
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose a prefix backup to restore",
            AllowMultiple = false,
            SuggestedStartLocation = backupFolder,
            FileTypeFilter = [new FilePickerFileType("Prefix backups (.tar.zst, .tar.gz)")
                { Patterns = ["*.tar.zst", "*.tar.gz"] }]
        });
        var chosen = files.Select(file => file.TryGetLocalPath()).OfType<string>().FirstOrDefault();
        if (chosen == null) return;
        string prefix;
        try { prefix = WinePath.PrefixFromInstallRoot(install.Root); }
        catch (InvalidOperationException error) { LinuxBackupText.Text = error.Message; return; }
        linuxRestoreArchive = chosen;
        LinuxRestoreWarning.Text = $"Restore {Path.GetFileName(chosen)} over the Proton prefix?\n\n" +
            $"Current prefix: {prefix}\n\n" +
            "The backup replaces the game copy, every profile and every mod in the prefix. The current prefix is kept " +
            "until the restored one is in place; if the replacement fails, it is put back. HellForge closes to replace " +
            "the prefix, then starts again. Close any session first.";
        LinuxRestoreConfirm.IsVisible = true;
        // The card can sit below the fold of the Settings scroll view. Once the confirmation has been laid out, move
        // the view to its position in the Settings content.
        Dispatcher.UIThread.Post(() =>
        {
            if (SettingsPage.Content is Visual content &&
                LinuxRestoreConfirm.TranslatePoint(new Point(0, 0), content) is { } position)
                SettingsPage.Offset = new Vector(SettingsPage.Offset.X, position.Y);
        }, DispatcherPriority.Loaded);
        LinuxBackupText.Text = "";
    }

    async Task RestoreLinuxPrefix(InstallLayout install)
    {
        if (linuxArchiveBusy || linuxRestoreArchive is not { } archive) return;
        if (runner.IsRunning || setupRunning) { LinuxBackupText.Text = "Stop the session (or setup) first."; return; }
        string prefix;
        try { prefix = WinePath.PrefixFromInstallRoot(install.Root); }
        catch (InvalidOperationException error) { LinuxBackupText.Text = error.Message; return; }
        var token = Guid.NewGuid().ToString("N");
        var locations = UninstallLocations();
        linuxArchiveBusy = true;
        SetLinuxArchiveBusy(true);
        LinuxRestoreConfirm.IsVisible = false;
        LinuxBackupProgress.IsVisible = true;
        LinuxBackupProgress.IsIndeterminate = true;
        LinuxBackupText.Text = "Checking the backup and unpacking it beside the prefix...";
        try
        {
            var plan = await Task.Run(() => LinuxPrefixBackup.PrepareRestore(archive, prefix, token, locations,
                SteamLocator.SteamRoot(), install.Root));
            LinuxBackupText.Text = $"Unpacked {DiskSpace.GB(plan.UncompressedBytes)}. HellForge closes, replaces the " +
                "prefix, and starts again...";
            StartLinuxRestoreHelper(install, plan);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or
                                          InvalidOperationException or Win32Exception)
        {
            LinuxPrefixBackup.DiscardRestore(LinuxPrefixBackup.StagingFolder(prefix, token));
            LinuxBackupText.Text = Privacy.Scrub(error.Message);
            LinuxRestoreConfirm.IsVisible = false;
            linuxArchiveBusy = false;
            SetLinuxArchiveBusy(false);
            LinuxBackupProgress.IsVisible = false;
        }
    }

    void StartLinuxRestoreHelper(InstallLayout install, LinuxPrefixRestorePlan plan)
    {
        var script = Path.Combine(Path.GetTempPath(), $"hellforge-restore-{Guid.NewGuid():N}.sh");
        var log = Path.Combine(Path.GetTempPath(), $"hellforge-restore-{Guid.NewGuid():N}.log");
        File.WriteAllText(script, LinuxPrefixBackup.RestoreHelperScript);
        var launcher = Path.Combine(install.App, "launcher-linux", "HellForge.Launcher");
        var start = new ProcessStartInfo("bash")
        {
            WorkingDirectory = Path.GetTempPath(),
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in new[] { script, plan.Prefix, plan.StagedFolder, plan.StagingRoot, plan.OldFolder, log,
                     Environment.ProcessId.ToString(CultureInfo.InvariantCulture), launcher })
            start.ArgumentList.Add(argument);
        using var helper = Process.Start(start) ?? throw new IOException("The Linux restore helper did not start.");
        Close();
    }

    void SetLinuxArchiveBusy(bool busy)
    {
        LinuxBackupButton.IsEnabled = LinuxRestoreButton.IsEnabled = !busy;
        LinuxBackupFolderButton.IsEnabled = LinuxBackupFolder.IsEnabled = !busy;
    }
}
