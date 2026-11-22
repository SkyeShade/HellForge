using System.Diagnostics;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// Downpatch files in the launcher (Downpatch). In setup, the fourth way on the card: the player picks a file a friend made,
// and it is applied to Steam's installed game. In Settings > Installation: one made from Steam's game and this
// installation's copy, saved where the player chooses. HellForge never hosts, fetches or lists these files.
public partial class MainWindow
{
    // Steam's installed game the downpatch way applies to (null: none, so the way is not offered).
    SteamGame? downpatchGame;
    TaskCompletionSource<string>? downpatchPicked;
    CancellationTokenSource? downpatchMaking;

    const string NotHosted = "HellForge doesn't host these; you make one and share it yourself.";

    void InitializeDownpatch(InstallLayout install)
    {
        DownpatchPickButton.Click += async (_, _) => await PickDownpatchFile();
        DownpatchMakeText.Text = "Make a downpatch file to let a friend install HellForge from their own Steam copy of HELLDIVERS 2, " +
            $"even if Steam's download of build {SupportedBuild.Current.BuildId} ever stops working. It is made from your Steam game (it must " +
            $"be the same build as your friend's) and this installation's copy. {NotHosted}";
        DownpatchMakeButton.Click += async (_, _) =>
        {
            if (downpatchMaking != null) downpatchMaking.Cancel();
            else await MakeDownpatchFile(install);
        };
    }

    async Task PickDownpatchFile()
    {
        if (downpatchPicked == null) return;
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose the downpatch file",
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("HellForge downpatch file") { Patterns = ["*.zip"] }, FilePickerFileTypes.All],
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path) downpatchPicked?.TrySetResult(path);
    }

    // The setup card's downpatch way: waits for a file, checks it is for Steam's build and this HellForge's, then writes
    // the build into the installation. null: the file was refused (said on the card), ready for another.
    async Task<string?> ApplyDownpatchFile(InstallLayout install, SetupSource found, CancellationToken attempt)
    {
        var game = found.Game ?? throw new SetupStopped("A downpatch file is applied to HELLDIVERS 2 as Steam has it: install the game in Steam first.");
        var source = await Task.Run(() => DownpatchSource.Of(game), attempt);
        downpatchPicked = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var picked = downpatchPicked;
        string pack;
        using (attempt.Register(() => picked.TrySetCanceled()))
            pack = await picked.Task;
        downpatchPicked = null;
        DownpatchPickButton.IsEnabled = false;
        ChooseAnotherButton.IsEnabled = false;
        try
        {
            SignInStatus.Text = "Checking the downpatch file...";
            var manifest = await Task.Run(() =>
            {
                var read = Downpatch.ReadManifest(pack);
                Downpatch.Plan(read, found.Build);
                Downpatch.CheckSource(read, source);
                return read;
            }, attempt);
            SignInStatus.Text = $"A downpatch file from build {manifest.Source.BuildId}, made with {manifest.CreatedWith}. Writing build {found.Build.BuildId}...";
            SetRow(SetupStep.Copy, null, $"Writing build {found.Build.BuildId} from the downpatch file and your Steam install...", running: true);
            CopyCard.IsVisible = true;
            SetupPauseButton.Content = "Pause";
            var clock = Stopwatch.StartNew();
            var progress = GameFilesProgress(clock);
            var result = await Task.Run(() => Downpatch.Apply(pack, source, install.Game, found.Build, p => progress(p.Done, p.Total, p.File), attempt), attempt);
            CopyProgress.Value = 1000;
            CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
            return $"{result.Files} files ({result.Bytes / 1e9:0.0} GB) from a downpatch file and your Steam install: {result.Same} as Steam has them, " +
                $"{result.Patched} patched, {result.New} carried whole. Each verified against Steam's hashes.";
        }
        catch (OperationCanceledException)
        {
            SignInStatus.Text = "Paused. Continue setup and pick the file again: the files already written are kept.";
            throw;
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException && !attempt.IsCancellationRequested)
        {
            SignInStatus.Text = Privacy.Scrub(error.Message);
            SetRow(SetupStep.Copy, null, "The downpatch file could not be used: see the card below.");
            return null;
        }
        finally
        {
            DownpatchPickButton.IsEnabled = true;
            ChooseAnotherButton.IsEnabled = true;
        }
    }

    // Settings > Installation: a downpatch file from Steam's game to this installation's build, where the player saves it.
    async Task MakeDownpatchFile(InstallLayout install)
    {
        if (runner.IsRunning || setupRunning)
        {
            DownpatchMakeStatus.Text = "Wait until the game session or setup has ended.";
            return;
        }
        var build = SupportedBuild.Current;
        DownpatchSource source;
        try
        {
            var steamRoot = SteamLocator.SteamRoot(install);
            var game = (steamRoot == null ? null : await Task.Run(() => SteamLocator.FindGame(steamRoot)))
                ?? throw new InvalidDataException("HELLDIVERS 2 is not installed in Steam: a downpatch file is made from Steam's game.");
            source = await Task.Run(() => DownpatchSource.Of(game));
            if (source.Pin.BuildId == build.BuildId) throw new InvalidDataException($"Steam has build {build.BuildId} itself: a friend with it needs no downpatch file.");
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            DownpatchMakeStatus.Text = Privacy.Scrub(error.Message);
            return;
        }
        var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Save the downpatch file",
            SuggestedFileName = Downpatch.FileName(source.Pin.BuildId, build.BuildId),
            DefaultExtension = "zip",
            FileTypeChoices = [new FilePickerFileType("HellForge downpatch file") { Patterns = ["*.zip"] }],
        });
        if (file?.TryGetLocalPath() is not { } outFile) return;
        using var making = new CancellationTokenSource();
        downpatchMaking = making;
        DownpatchMakeButton.Content = "Stop";
        RepairButton.IsEnabled = false;
        DownpatchMakeProgress.IsVisible = true;
        DownpatchMakeProgress.Value = 0;
        var clock = Stopwatch.StartNew();
        var shown = TimeSpan.Zero;
        void Progress(DownpatchProgress p)
        {
            if (clock.Elapsed - shown < TimeSpan.FromMilliseconds(200)) return;
            shown = clock.Elapsed;
            Dispatcher.UIThread.Post(() =>
            {
                DownpatchMakeProgress.Value = p.Total > 0 ? p.Done * 1000.0 / p.Total : 0;
                DownpatchMakeStatus.Text = $"{p.Stage}: {p.Done / 1e9:0.0} of {p.Total / 1e9:0.0} GB  ·  {p.File}";
            });
        }
        try
        {
            var result = await Task.Run(() => DownpatchMaker.Make(install.Game, source, build, outFile, Progress, making.Token), making.Token);
            DownpatchMakeStatus.Text = $"Saved {Privacy.Scrub(outFile)} ({result.PackBytes / 1e9:0.00} GB, build {result.SourceBuild} to {result.TargetBuild}) " +
                $"in {clock.Elapsed.TotalMinutes:0} min. It works for a friend whose Steam has build {result.SourceBuild}. {NotHosted}";
        }
        catch (OperationCanceledException) { DownpatchMakeStatus.Text = "Stopped; nothing was saved."; }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            DownpatchMakeStatus.Text = Privacy.Scrub(error.Message);
        }
        finally
        {
            downpatchMaking = null;
            DownpatchMakeButton.Content = "Make a downpatch file...";
            RepairButton.IsEnabled = true;
            DownpatchMakeProgress.IsVisible = false;
        }
    }
}
