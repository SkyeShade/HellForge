namespace HellForge.SteamDownload;

// HellForge's own sign-in to Steam for setup's download: a QR code shown on screen and approved in the Steam mobile app, so
// HellForge never sees a password. (Without a phone, SteamCMD signs in instead: the password goes into Valve's own
// program, never into HellForge; SteamCmd.) Nothing is kept: no token, no Steam Guard data. The sign-in lasts until the
// download ends and is never written anywhere. (The asset kit's fetches keep theirs: SteamLogins.)
public interface ISignInPrompt
{
    // The QR code's text to show (again when Steam renews it, every 30 seconds or so).
    void ShowQr(string challenge);
    // Where sign-in is, in plain words.
    void Status(string text);
}

// Sign-in did not succeed: the reason in plain words, never a token.
public sealed class SteamSignInException(string message, Exception? inner = null) : Exception(message, inner);

// Removes the secrets of a sign-in from text before it reaches the screen or a log: everything sign-in passes on goes
// through it, so a message that quotes a token by mistake shows it as "[hidden]".
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
    public void Status(string text) => inner.Status(scrubber.Clean(text));
}
