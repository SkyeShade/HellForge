using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// How setup gets the supported build from Steam, on the setup screen's card: three equal choices (DownloadChoices). Scan a
// QR code with the Steam app (the default), or sign in with account name and password (no phone needed; Steam emails a
// code when the account has Steam Guard by email): both download from Steam's servers (SteamDepotSession). Or let the
// Steam client on this PC download it through its console, with no sign-in in HellForge (SteamDepotDownload, the console
// card). Nothing of a sign-in is kept: the password box is emptied as soon as it is read, the session ends with the
// download, and no token or Steam Guard data is saved, so the next download asks again.
public partial class MainWindow
{
    DownloadChoice downloadChoice = DownloadChoices.Default;
    CancellationTokenSource? signInAttempt;
    TaskCompletionSource<SignInRequest>? passwordEntered;
    TaskCompletionSource<string>? codeEntered;
    bool signInWired;

    // The player chose the Steam client: setup waits for its console download instead of signing in.
    sealed class SteamClientChosen() : Exception("The Steam client downloads the build.");

    (Button Button, DownloadChoice Choice)[] ChoiceButtons =>
        [(ChoiceQrButton, DownloadChoice.QrCode), (ChoicePasswordButton, DownloadChoice.Password), (ChoiceClientButton, DownloadChoice.SteamClient)];

    void WireSignIn()
    {
        if (signInWired) return;
        signInWired = true;
        foreach (var (button, choice) in ChoiceButtons)
        {
            button.Content = DownloadChoices.Title(choice);
            button.Click += (_, _) => Choose(choice);
        }
        SignInPasswordButton.Click += (_, _) => SubmitPassword();
        SignInPassword.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) SubmitPassword(); };
        SignInCodeButton.Click += (_, _) => SubmitCode();
        SignInCode.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) SubmitCode(); };
    }

    void Choose(DownloadChoice choice)
    {
        if (choice == downloadChoice) return;
        downloadChoice = choice;
        ShowSignInMode();
        signInAttempt?.Cancel(); // the attempt under way ends; SignInForDownload starts the chosen one
        if (!DownloadChoices.SignsIn(choice)) return;
        // a sign-in instead of the Steam client's download: its console card goes, and setup (waiting for it) starts again
        downloadTimer?.Stop();
        DownloadCard.IsVisible = false;
        if (!setupRunning) _ = RunSetup();
    }

    // Waits on the card until the player has signed in (or cancel ends it), and returns the signed-in session. Throws
    // SteamClientChosen when the player picks (or had picked) the Steam client's download.
    async Task<SteamDepotSession> SignInForDownload(SupportedBuild build, CancellationToken cancel)
    {
        WireSignIn();
        SignInIntro.Text = $"Steam has another build of HELLDIVERS 2 than the one this HellForge runs (build {build.BuildId}). Steam can send " +
            "that build to your own account, under your own licence; only what your Steam install does not already have is downloaded. " +
            "Choose how. Nothing of a sign-in is kept, and your normal game and Steam stay as they are.";
        ShowSignInMode();
        SignInCard.IsVisible = true;
        try
        {
            while (true)
            {
                cancel.ThrowIfCancellationRequested();
                if (!DownloadChoices.SignsIn(downloadChoice)) throw new SteamClientChosen();
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                signInAttempt = attempt;
                var session = await TrySignIn(attempt.Token, cancel);
                if (session != null) return session;
            }
        }
        finally
        {
            signInAttempt = null;
            passwordEntered = null;
            SignInPassword.Text = "";
            SignInCodePanel.IsVisible = false;
            // the Steam client's choice keeps the card, so the player can still switch to a sign-in
            SignInCard.IsVisible = !DownloadChoices.SignsIn(downloadChoice);
        }
    }

    // One sign-in, of the chosen kind: the session, or null to try again (another choice, or a failure shown on the card).
    async Task<SteamDepotSession?> TrySignIn(CancellationToken attempt, CancellationToken cancel)
    {
        var byQr = downloadChoice == DownloadChoice.QrCode;
        try
        {
            var request = byQr ? DownloadChoices.Request(DownloadChoice.QrCode, null, null)! : await WaitForPassword(attempt);
            if (byQr) SignInQrWait.IsVisible = true;
            return await SteamDepotSession.SignInAsync(request, new CardPrompt(this), attempt);
        }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { return null; }
        catch (SteamSignInException failed) when (!cancel.IsCancellationRequested)
        {
            SignInStatus.Text = failed.Message + (byQr ? " A new code follows." : " Check the details and sign in again.");
            SignInPasswordButton.IsEnabled = true;
            SignInPasswordButton.Content = "Sign in";
            SignInCodePanel.IsVisible = false;
            if (byQr) await Task.Delay(TimeSpan.FromSeconds(3), cancel);
            return null;
        }
    }

    void ShowSignInMode()
    {
        foreach (var (button, choice) in ChoiceButtons)
        {
            button.Classes.Set("primary", choice == downloadChoice);
            button.Classes.Set("ghost", choice != downloadChoice);
        }
        SignInChoiceText.Text = DownloadChoices.Explanation(downloadChoice, SupportedBuild.Current.BuildId);
        SignInQrPanel.IsVisible = downloadChoice == DownloadChoice.QrCode;
        SignInPasswordPanel.IsVisible = downloadChoice == DownloadChoice.Password;
        SignInCodePanel.IsVisible = false;
        SignInPasswordButton.IsEnabled = true;
        SignInPasswordButton.Content = "Sign in";
        SignInQr.Data = null;
        SignInStatus.Text = "";
    }

    Task<SignInRequest> WaitForPassword(CancellationToken cancel)
    {
        passwordEntered = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var waiting = passwordEntered;
        cancel.Register(() => waiting.TrySetCanceled());
        return waiting.Task;
    }

    void SubmitPassword()
    {
        var request = DownloadChoices.Request(DownloadChoice.Password, SignInAccount.Text, SignInPassword.Text);
        if (passwordEntered == null || request == null)
        {
            SignInStatus.Text = "Enter your Steam account name and password.";
            return;
        }
        SignInPassword.Text = "";
        SignInPasswordButton.IsEnabled = false;
        SignInPasswordButton.Content = "Signing in...";
        SignInStatus.Text = "Signing in to Steam...";
        passwordEntered.TrySetResult(request);
    }

    void SubmitCode()
    {
        var code = SignInCode.Text?.Trim() ?? "";
        if (codeEntered == null || code.Length == 0) return;
        SignInCode.Text = "";
        SignInCodeButton.IsEnabled = false;
        SignInCodeButton.Content = "Checking the code...";
        codeEntered.TrySetResult(code);
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

    // What sign-in shows and asks, on the card (from SteamKit2's threads, so through the UI thread).
    sealed class CardPrompt(MainWindow window) : ISignInPrompt
    {
        public void ShowQr(string challenge) => Dispatcher.UIThread.Post(() => window.DrawQr(challenge));

        public void Status(string text) => Dispatcher.UIThread.Post(() => window.SignInStatus.Text = text);

        public Task<string> GuardCodeAsync(GuardCodeKind kind, string? emailHint, bool previousWasWrong, CancellationToken cancellation)
        {
            var entered = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
            cancellation.Register(() => entered.TrySetCanceled());
            Dispatcher.UIThread.Post(() =>
            {
                window.codeEntered = entered;
                window.SignInCodeText.Text = (previousWasWrong ? "That code was not right. " : "") + (kind == GuardCodeKind.Email
                    ? $"Steam sent a Steam Guard code to your email ({emailHint}). Enter it here:"
                    : "Enter the Steam Guard code the Steam app on your phone shows:");
                window.SignInCodeButton.IsEnabled = true;
                window.SignInCodeButton.Content = "Continue";
                window.SignInCodePanel.IsVisible = true;
                window.SignInStatus.Text = "";
                window.SignInCode.Focus();
            });
            return entered.Task;
        }
    }
}
