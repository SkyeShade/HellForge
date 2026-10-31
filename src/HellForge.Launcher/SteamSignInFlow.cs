using Avalonia;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// Setup's sign-in to Steam for downloading the supported build (SteamDepotSession), on the setup screen's sign-in card: a
// QR code to approve in the Steam app first, or account name and password with a Steam Guard code. Nothing of it is kept:
// the password box is emptied as soon as it is read, the session ends with the download, and no token or Steam Guard
// data is saved, so the next download asks again.
public partial class MainWindow
{
    bool signInByAccount;
    CancellationTokenSource? signInAttempt;
    TaskCompletionSource<SignInRequest>? passwordEntered;
    TaskCompletionSource<string>? codeEntered;
    bool signInWired;

    void WireSignIn()
    {
        if (signInWired) return;
        signInWired = true;
        SignInSwitchButton.Click += (_, _) =>
        {
            signInByAccount = !signInByAccount;
            ShowSignInMode();
            signInAttempt?.Cancel(); // the attempt under way ends; SignInForDownload starts the other kind
        };
        SignInPasswordButton.Click += (_, _) => SubmitPassword();
        SignInPassword.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) SubmitPassword(); };
        SignInCodeButton.Click += (_, _) => SubmitCode();
        SignInCode.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) SubmitCode(); };
    }

    // Waits on the card until the player has signed in (or cancel ends it), and returns the signed-in session.
    async Task<SteamDepotSession> SignInForDownload(SupportedBuild build, CancellationToken cancel)
    {
        WireSignIn();
        SignInIntro.Text = $"Steam has another build of HELLDIVERS 2 than the one this HellForge runs (build {build.BuildId}). Steam can send " +
            "that build to your own account from its servers: sign in to Steam below to start the download. It runs under your own " +
            "licence, and only what your Steam install does not already have is downloaded. With the QR code, HellForge never sees your " +
            "password. Nothing of the sign-in is kept: the next download asks again. Your normal game and Steam stay as they are.";
        SignInStatus.Text = "";
        ShowSignInMode();
        SignInCard.IsVisible = true;
        try
        {
            while (true)
            {
                cancel.ThrowIfCancellationRequested();
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
            SignInCard.IsVisible = false;
        }
    }

    // One sign-in, of the kind on screen: the session, or null to try again (switched kind, or a failure shown on the card).
    async Task<SteamDepotSession?> TrySignIn(CancellationToken attempt, CancellationToken cancel)
    {
        try
        {
            SignInRequest request = signInByAccount ? await WaitForPassword(attempt) : new QrSignIn();
            if (!signInByAccount) SignInQrWait.IsVisible = true;
            return await SteamDepotSession.SignInAsync(request, new CardPrompt(this), attempt);
        }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { return null; }
        catch (SteamSignInException failed) when (!cancel.IsCancellationRequested)
        {
            SignInStatus.Text = failed.Message + (signInByAccount ? " Check the details and sign in again." : " A new code follows.");
            SignInPasswordButton.IsEnabled = true;
            SignInPasswordButton.Content = "Sign in";
            SignInCodePanel.IsVisible = false;
            if (!signInByAccount) await Task.Delay(TimeSpan.FromSeconds(3), cancel);
            return null;
        }
    }

    void ShowSignInMode()
    {
        SignInQrPanel.IsVisible = !signInByAccount;
        SignInPasswordPanel.IsVisible = signInByAccount;
        SignInCodePanel.IsVisible = false;
        SignInPasswordButton.IsEnabled = true;
        SignInPasswordButton.Content = "Sign in";
        SignInQr.Data = null;
        SignInSwitchButton.Content = signInByAccount ? "Sign in with a QR code instead" : "Sign in with account name and password instead";
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
        var account = SignInAccount.Text?.Trim() ?? "";
        var typed = SignInPassword.Text ?? "";
        if (passwordEntered == null || account.Length == 0 || typed.Length == 0)
        {
            SignInStatus.Text = "Enter your Steam account name and password.";
            return;
        }
        SignInPassword.Text = "";
        SignInPasswordButton.IsEnabled = false;
        SignInPasswordButton.Content = "Signing in...";
        SignInStatus.Text = "Signing in to Steam...";
        passwordEntered.TrySetResult(new PasswordSignIn(account, typed));
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
