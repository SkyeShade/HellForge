using Avalonia.Platform.Storage;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// Settings > Installation's Copy the installer (KeptInstaller): the installer this installation keeps of itself, copied
// where the player chooses, checked against its SHA-256. With a downpatch file (DownpatchFlow) it is the offline kit for
// a friend or another PC.
public partial class MainWindow
{
    CancellationTokenSource? installerCopying;

    void InitializeKeptInstaller(InstallLayout install)
    {
        InstallerCopyButton.Click += async (_, _) =>
        {
            if (installerCopying != null) installerCopying.Cancel();
            else await CopyInstaller(install);
        };
        ShowKeptInstaller(KeptInstaller.Find(install));
    }

    static string SetupName => OperatingSystem.IsLinux() ? "the HellForge Linux installer" : "HellForge-Setup";

    void ShowKeptInstaller(KeptInstallerState state)
    {
        var running = KeptInstaller.VersionForName(AppVersion.Text);
        var kit = "Your friend still needs their own HELLDIVERS 2 on Steam, with Steam running and signed in for the ownership check, " +
            $"and the game build HellForge runs: Steam's download of build {SupportedBuild.Current.BuildId}, or a downpatch file (below).";
        InstallerCopyButton.IsEnabled = state.Kept != null;
        if (state.Kept is not { } kept)
        {
            InstallerCopyText.Text = "To hand HellForge to a friend or install it on another PC, copy this installation's own installer to a USB " +
                "stick or a shared folder. " + kit;
            InstallerCopyStatus.Text = state.NotKept is { } why
                ? $"No installer is kept here: {SetupName} could not keep a copy of itself the last time it ran ({why}). It tries again at the next update."
                : $"No installer is kept here yet: installations made before this HellForge version don't have one. It appears after the next install or update with {SetupName}.";
            return;
        }
        InstallerCopyText.Text = $"To hand HellForge to a friend or install it on another PC, copy this installation's own installer, HellForge {kept.Version} " +
            $"({kept.Bytes / 1e6:N0} MB), to a USB stick or a shared folder. Each copy is checked against its SHA-256. " + kit;
        InstallerCopyStatus.Text = kept.Version != running
            ? $"The kept installer is HellForge {kept.Version}, not this installation's {running}" +
              (state.NotKept is { } reason ? $": the last update could not keep its own copy ({reason})." : ".")
            : "";
    }

    async Task CopyInstaller(InstallLayout install)
    {
        var state = await Task.Run(() => KeptInstaller.Find(install));
        ShowKeptInstaller(state);
        if (state.Kept is not { } kept) return;
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Choose where to copy the installer", AllowMultiple = false });
        if (folders.Count == 0 || folders[0].TryGetLocalPath() is not { } folder) return;
        using var copying = new CancellationTokenSource();
        installerCopying = copying;
        InstallerCopyButton.Content = "Stop";
        InstallerCopyProgress.IsVisible = true;
        InstallerCopyProgress.Value = 0;
        var shown = DateTime.MinValue;
        void Progress(KeptInstallerProgress p)
        {
            if (DateTime.UtcNow - shown < TimeSpan.FromMilliseconds(200)) return;
            shown = DateTime.UtcNow;
            Dispatcher.UIThread.Post(() =>
            {
                InstallerCopyProgress.Value = p.Total > 0 ? p.Done * 1000.0 / p.Total : 0;
                InstallerCopyStatus.Text = $"{p.Stage}: {p.Done / 1e6:N0} of {p.Total / 1e6:N0} MB";
            });
        }
        try
        {
            var copy = await Task.Run(() => KeptInstaller.CopyTo(kept, folder, Progress, copying.Token), copying.Token);
            InstallerCopyStatus.Text = $"Copied to {Privacy.Scrub(copy.Path)} and read back whole: SHA-256 {copy.Sha256}, as when it was kept. " +
                $"{Path.GetFileName(copy.File)}{KeptInstaller.HashSuffix} beside it holds the hash, to check it again on the other PC.";
        }
        catch (OperationCanceledException) { InstallerCopyStatus.Text = "Stopped; the unfinished copy was deleted."; }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            InstallerCopyStatus.Text = Privacy.Scrub(error.Message);
        }
        finally
        {
            installerCopying = null;
            InstallerCopyButton.Content = "Copy the installer...";
            InstallerCopyProgress.IsVisible = false;
        }
    }
}
