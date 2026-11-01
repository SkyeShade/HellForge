using System.ComponentModel;
using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// How setup gets the supported build from Steam, on the setup screen's card: one of three ways (DownloadChoices), the QR
// code first. Scan a QR code with the Steam app on a phone: HellForge signs in and downloads from Steam's servers
// (SteamDepotSession). No phone: SteamCMD, Valve's own tool, downloads it, and the password is typed into SteamCMD's
// window, never into HellForge (SteamCmd). Or the Steam app on this PC downloads it through its console (the console
// steps, SetupFlow's ShowDownload), which is also where setup goes when another way cannot work. Each way has "Choose
// another way" back to the three, and every way's progress shows on the game files card. Nothing of a sign-in is kept:
// HellForge's own session ends with the download, and SteamCMD's remembered sign-in is removed after every run.
public partial class MainWindow
{
    DownloadChoice? downloadChoice = DownloadChoices.Default; // null: the player is choosing
    TaskCompletionSource? choiceMade;
    CancellationTokenSource? signInAttempt;
    TaskCompletionSource<string>? steamCmdStarted;
    bool signInWired;

    // The player chose the Steam app: setup waits for its console download (SetupFlow).
    sealed class SteamAppChosen() : Exception("The Steam app downloads the build.");

    (Button Button, TextBlock Hint, DownloadChoice Choice)[] Choices =>
    [
        (ChoiceQrButton, ChoiceQrHint, DownloadChoice.QrCode), (ChoiceSteamCmdButton, ChoiceSteamCmdHint, DownloadChoice.SteamCmd),
        (ChoiceClientButton, ChoiceClientHint, DownloadChoice.SteamClient),
    ];

    // The ways this setup can offer: under Wine, inside HellForge's network namespace, only the Steam app can reach Steam.
    static IReadOnlyList<DownloadChoice> OfferedChoices => DownloadChoices.Offered(reachesSteam: !LinuxNetworkBlock.Requested);

    void WireSignIn()
    {
        if (signInWired) return;
        signInWired = true;
        foreach (var (button, hint, choice) in Choices)
        {
            button.Content = DownloadChoices.Title(choice);
            hint.Text = DownloadChoices.Hint(choice);
            button.Click += (_, _) => Choose(choice);
        }
        ChooseAnotherButton.Click += (_, _) => Choose(null);
        SteamCmdButton.Click += (_, _) => StartSteamCmd();
        SteamCmdAccount.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) StartSteamCmd(); };
    }

    // A way chosen, or null: back to the three. What runs ends (a QR sign-in, SteamCMD); GetBuildFromSteam starts the
    // chosen way, and setup starts again when it was waiting for the Steam app's download.
    void Choose(DownloadChoice? choice)
    {
        if (choice == downloadChoice) return;
        downloadChoice = choice;
        if (choice is DownloadChoice.QrCode or DownloadChoice.SteamCmd) downloadTimer?.Stop();
        ShowChoice();
        signInAttempt?.Cancel();
        choiceMade?.TrySetResult();
        if (choice != null && !setupRunning) _ = RunSetup();
    }

    // The copy step when Steam has another build: waits on the card for the player's way and gets the build that way.
    // Returns the copy row's words when the build is in the installation; throws SteamAppChosen for the Steam app.
    async Task<string> GetBuildFromSteam(InstallLayout install, SetupSource found, CancellationToken cancel)
    {
        WireSignIn();
        SignInIntro.Text = $"Steam has another build of HELLDIVERS 2 than the one this HellForge runs (build {found.Build.BuildId}). Steam " +
            "can send that build to your own account, under your own licence. Choose how; nothing of a sign-in is kept, and your normal " +
            "game and Steam stay as they are.";
        if (downloadChoice is { } chosen && !OfferedChoices.Contains(chosen)) downloadChoice = DownloadChoice.SteamClient;
        ShowChoice();
        SignInCard.IsVisible = true;
        try
        {
            while (true)
            {
                cancel.ThrowIfCancellationRequested();
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                signInAttempt = attempt;
                try
                {
                    var done = downloadChoice switch
                    {
                        null => await WaitForChoice(attempt.Token),
                        DownloadChoice.QrCode => await DownloadWithQr(install, found, attempt.Token),
                        DownloadChoice.SteamCmd => await DownloadWithSteamCmd(install, found, attempt.Token),
                        _ => throw new SteamAppChosen(),
                    };
                    if (done != null)
                    {
                        SignInCard.IsVisible = false;
                        return done;
                    }
                }
                catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { } // another way was chosen
            }
        }
        finally
        {
            signInAttempt = null;
            steamCmdStarted = null;
            ChooseAnotherButton.IsEnabled = true;
        }
    }

    async Task<string?> WaitForChoice(CancellationToken attempt)
    {
        choiceMade = new(TaskCreationOptions.RunContinuationsAsynchronously);
        await choiceMade.Task.WaitAsync(attempt);
        return null;
    }

    // The QR code: HellForge signs in and downloads from Steam's servers. null: try again (a failure shown on the card).
    async Task<string?> DownloadWithQr(InstallLayout install, SetupSource found, CancellationToken attempt)
    {
        SteamDepotSession session;
        SignInQrWait.IsVisible = true;
        try { session = await SteamDepotSession.SignInAsync(new CardPrompt(this), attempt); }
        catch (SteamSignInException failed) when (!attempt.IsCancellationRequested)
        {
            SignInStatus.Text = failed.Message + " A new code follows.";
            await Task.Delay(TimeSpan.FromSeconds(3), attempt);
            return null;
        }
        await using (session)
        {
            ChooseAnotherButton.IsEnabled = false; // Pause download stops it
            SignInStatus.Text = "Signed in. The download's progress shows under Game files.";
            SetRow(SetupStep.Copy, null, $"Downloading build {found.Build.BuildId} from Steam...", running: true);
            var (downloaded, copied) = await DownloadFromSteam(session, install, found, attempt);
            return $"{found.Build.Files.Count} files ({found.Build.TotalBytes / 1e9:0.0} GB) from Steam for your account: {downloaded / 1e9:0.0} GB " +
                $"downloaded, {copied / 1e9:0.0} GB copied from your Steam install. Each file verified against Steam's hashes.";
        }
    }

    // SteamCMD: started once the player has typed the account name; the password goes into SteamCMD's own window. null:
    // SteamCMD ended without the whole build (closed, a failed sign-in), said on the card, ready to start again.
    async Task<string?> DownloadWithSteamCmd(InstallLayout install, SetupSource found, CancellationToken attempt)
    {
        var account = await WaitForSteamCmdStart(attempt);
        var build = found.Build;
        var steamCmd = SteamCmd.In(install);
        int exitCode;
        try
        {
            // room for the download first; it is moved, not copied, into the installation afterwards
            var present = await Task.Run(() => SteamDepotDownload.Status(steamCmd.Folder, build).Sum(depot => depot.Done), attempt);
            DiskSpace.Require((install.Root, build.TotalBytes - present));
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) })
                await steamCmd.InstallAsync(http, attempt);
            SteamCmdButton.Content = "SteamCMD is updating itself...";
            SignInStatus.Text = "SteamCMD updates itself from Valve the first time; this takes a minute.";
            await steamCmd.UpdateAsync(line => Dispatcher.UIThread.Post(() => SignInStatus.Text = "SteamCMD: " + line), attempt);
            await Task.Run(steamCmd.ForgetSignIn, attempt);
            SteamCmdButton.Content = "SteamCMD is running...";
            SignInStatus.Text = "Type your password in the SteamCMD window; it goes to Valve's own tool, not HellForge. If Steam asks for " +
                "a Steam Guard code, type that there too. The download's progress shows under Game files.";
            SetupPauseButton.Content = "Stop SteamCMD";
            SetRow(SetupStep.Copy, null, $"SteamCMD downloads build {build.BuildId} for your account...", running: true);
            using var watching = CancellationTokenSource.CreateLinkedTokenSource(attempt);
            var watch = WatchDepots(steamCmd.Folder, watching.Token);
            try
            {
                using var process = steamCmd.Start(SteamCmd.Arguments(account, build), visible: true);
                exitCode = await steamCmd.WaitAsync(process, attempt);
            }
            finally
            {
                await watching.CancelAsync();
                await watch;
            }
        }
        catch (Exception error) when (error is HttpRequestException or TimeoutException or IOException or InvalidDataException or Win32Exception
            && error is not NotEnoughSpaceException && !attempt.IsCancellationRequested)
        {
            throw new SteamCmdException($"it could not be set up ({error.Message}).", error);
        }
        finally
        {
            await Task.Run(() => ForgetSteamCmdSignIn(steamCmd));
            SteamCmdButton.IsEnabled = true;
            SteamCmdButton.Content = "Start SteamCMD again";
            SetupPauseButton.Content = "Cancel";
        }
        var depots = await Task.Run(() => SteamDepotDownload.Status(steamCmd.Folder, build));
        ShowDepotProgress(depots, "SteamCMD");
        if (!depots.All(depot => depot.Complete))
        {
            SignInStatus.Text = SteamCmd.Unfinished(exitCode, depots);
            SetRow(SetupStep.Copy, null, "SteamCMD closed before the download was complete: see the card below.");
            return null;
        }
        ChooseAnotherButton.IsEnabled = false;
        SignInStatus.Text = "SteamCMD has downloaded the build. Checking it and moving it into this folder...";
        SetRow(SetupStep.Copy, null, "Checking SteamCMD's download and moving it into this folder...", running: true);
        await MoveSteamCmdDownload(steamCmd, install, attempt);
        return $"{build.Files.Count} files ({build.TotalBytes / 1e9:0.0} GB), downloaded by SteamCMD for your account, each verified against " +
            "Steam's hashes. SteamCMD and what it remembered of the sign-in were removed.";
    }

    // SteamCMD may still hold its files for a moment after it ends (and an antivirus scans them): waits for that.
    static void ForgetSteamCmdSignIn(SteamCmd steamCmd)
    {
        steamCmd.Stop();
        ModKit.RetryWhileHeld(steamCmd.ForgetSignIn, _ => { }).GetAwaiter().GetResult();
    }

    async Task MoveSteamCmdDownload(SteamCmd steamCmd, InstallLayout install, CancellationToken attempt)
    {
        var clock = Stopwatch.StartNew();
        var progress = GameFilesProgress(clock);
        await Task.Run(() => GameCopier.MoveFromDepots(steamCmd.Folder, install.Game, SupportedBuild.Current, null, progress, attempt), attempt);
        await Task.Run(() => ModKit.RetryWhileHeld(steamCmd.Remove, _ => { }), attempt);
        CopyProgress.Value = 1000;
        CopyText.Text = $"Done in {clock.Elapsed.TotalMinutes:0.0} min.";
    }

    // The download's progress on the game files card, from the depot folders, every 2 s until stopped.
    async Task WatchDepots(string root, CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            try
            {
                ShowDepotProgress(await Task.Run(() => SteamDepotDownload.Status(root, SupportedBuild.Current), stop), "SteamCMD");
                await Task.Delay(TimeSpan.FromSeconds(2), stop);
            }
            catch (OperationCanceledException) { break; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { CopyText.Text = "Could not check the download: " + Privacy.Scrub(error.Message); }
        }
    }

    Task<string> WaitForSteamCmdStart(CancellationToken attempt)
    {
        steamCmdStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var waiting = steamCmdStarted;
        attempt.Register(() => waiting.TrySetCanceled());
        return waiting.Task;
    }

    // Start SteamCMD: the account name only, checked; the button says at once what happens.
    void StartSteamCmd()
    {
        var account = SteamCmdAccount.Text?.Trim() ?? "";
        if (steamCmdStarted == null || !SteamCmdButton.IsEnabled) return;
        if (!SteamCmd.IsAccountName(account))
        {
            SignInStatus.Text = "Enter your Steam account name: the name you sign in with (letters, digits and underscores), not your " +
                "profile name. Your password goes into the SteamCMD window, not here.";
            return;
        }
        SteamCmdButton.IsEnabled = false;
        SteamCmdButton.Content = "Getting SteamCMD from Valve...";
        SignInStatus.Text = "Getting SteamCMD from Valve and checking its signature...";
        steamCmdStarted.TrySetResult(account);
    }

    // The card as the choice says: the three ways (choosing), or the chosen way's words and steps.
    void ShowChoice()
    {
        var offered = OfferedChoices;
        var choosing = downloadChoice == null;
        ChoicesPanel.IsVisible = choosing;
        foreach (var (button, _, choice) in Choices) ((Control)button.Parent!).IsVisible = offered.Contains(choice);
        SignInChoiceText.IsVisible = !choosing;
        SignInChoiceText.Text = downloadChoice is { } chosen ? DownloadChoices.Explanation(chosen, SupportedBuild.Current.BuildId) : "";
        SignInQrPanel.IsVisible = downloadChoice == DownloadChoice.QrCode;
        SteamCmdPanel.IsVisible = downloadChoice == DownloadChoice.SteamCmd;
        ClientPanel.IsVisible = downloadChoice == DownloadChoice.SteamClient;
        ChooseAnotherButton.IsVisible = !choosing && offered.Count > 1;
        ChooseAnotherButton.IsEnabled = true;
        SteamCmdButton.IsEnabled = true;
        SteamCmdButton.Content = "Start SteamCMD";
        SignInQr.Data = null;
        SignInQrWait.IsVisible = true;
        SignInStatus.Text = "";
    }

    void DrawQr(string challenge)
    {
        var modules = QrCode.Modules(challenge);
        var geometry = new StreamGeometry();
        using (var draw = geometry.Open())
        {
            for (var y = 0; y < modules.GetLength(0); y++)
                for (var x = 0; x < modules.GetLength(1); x++)
                {
                    if (!modules[y, x]) continue;
                    draw.BeginFigure(new Point(x, y), true);
                    draw.LineTo(new Point(x + 1, y));
                    draw.LineTo(new Point(x + 1, y + 1));
                    draw.LineTo(new Point(x, y + 1));
                    draw.EndFigure(true);
                }
        }
        SignInQr.Data = geometry;
        SignInQrWait.IsVisible = false;
    }

    // What sign-in shows, on the card (from SteamKit2's threads, so through the UI thread).
    sealed class CardPrompt(MainWindow window) : ISignInPrompt
    {
        public void ShowQr(string challenge) => Dispatcher.UIThread.Post(() => window.DrawQr(challenge));

        public void Status(string text) => Dispatcher.UIThread.Post(() => window.SignInStatus.Text = text);
    }
}
