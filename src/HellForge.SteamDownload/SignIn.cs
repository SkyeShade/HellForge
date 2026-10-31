namespace HellForge.SteamDownload;

// How the player signs in to Steam for setup's download. Nothing is kept: no password, no token, no Steam Guard data. The
// sign-in lasts until the download ends and is never written anywhere.
public abstract class SignInRequest
{
    private protected SignInRequest() { }
}

// A QR code shown on screen, approved in the Steam mobile app: HellForge never sees the password.
public sealed class QrSignIn : SignInRequest;

// Account name and password, with a Steam Guard code when the account asks for one. Not a record, so nothing prints the
// password (ToString names the account only).
public sealed class PasswordSignIn(string accountName, string password) : SignInRequest
{
    public string AccountName { get; } = accountName;
    internal string Password { get; } = password;
    public override string ToString() => $"password sign-in for {AccountName}";
}

public enum GuardCodeKind { Email, App }

// What sign-in shows the player and asks of them (the launcher's setup screen, the CLI's terminal).
public interface ISignInPrompt
{
    // The QR code's text to show (again when Steam renews it, every 30 seconds or so).
    void ShowQr(string challenge);
    // A Steam Guard code from the email Steam sent (emailHint: the address as Steam shows it) or from the Steam app.
    Task<string> GuardCodeAsync(GuardCodeKind kind, string? emailHint, bool previousWasWrong, CancellationToken cancellation);
    // Where sign-in is, in plain words.
    void Status(string text);
}

// Sign-in did not succeed: the reason in plain words, never the password or a token.
public sealed class SteamSignInException(string message, Exception? inner = null) : Exception(message, inner);

// Removes the secrets of a sign-in from text before it reaches the screen or a log: everything sign-in passes on goes
// through it, so a message that quotes a password or a token by mistake shows it as "[hidden]".
public sealed class SecretScrubber
{
    readonly List<string> secrets = [];

    public void Add(string? secret)
    {
        if (!string.IsNullOrEmpty(secret) && secret.Length >= 4) lock (secrets) secrets.Add(secret);
    }

    public string Clean(string? text)
    {
        if (string.IsNullOrEmpty(text)) return "";
        lock (secrets)
            foreach (var secret in secrets) text = text.Replace(secret, "[hidden]", StringComparison.Ordinal);
        return text;
    }

    public void Forget()
    {
        lock (secrets) secrets.Clear();
    }
}

// A prompt whose status lines pass through the scrubber.
sealed class ScrubbedPrompt(ISignInPrompt inner, SecretScrubber scrubber) : ISignInPrompt
{
    public void ShowQr(string challenge) => inner.ShowQr(challenge);
    public Task<string> GuardCodeAsync(GuardCodeKind kind, string? emailHint, bool previousWasWrong, CancellationToken cancellation) =>
        inner.GuardCodeAsync(kind, emailHint, previousWasWrong, cancellation);
    public void Status(string text) => inner.Status(scrubber.Clean(text));
}
