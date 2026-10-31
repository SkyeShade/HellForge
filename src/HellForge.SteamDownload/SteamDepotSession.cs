using HellForge.Core;
using SteamKit2;
using SteamKit2.Authentication;
using SteamKit2.CDN;
using DepotChunk = HellForge.Core.DepotChunk;
using DepotManifest = HellForge.Core.DepotManifest;

namespace HellForge.SteamDownload;

// A sign-in to Steam for setup's download, on SteamKit2: Steam's own sign-in (QR code or password with Steam Guard), then
// depot keys, manifest request codes, manifests and chunks from Steam's content servers. Written for HellForge on
// SteamKit2's API. The session is not persistent and nothing about it is stored: the tokens Steam returns live in this
// object until it is disposed. It signs in as a separate session (its own login id), so the Steam client stays signed in.
public sealed class SteamDepotSession : IDepotService, IAsyncDisposable
{
    // Steam's login id for this session: any value other than the Steam client's own keeps the client signed in.
    const uint LoginId = 0x48466467; // "HFdg"
    static readonly TimeSpan Timeout = TimeSpan.FromSeconds(30);

    readonly SteamClient client;
    readonly CallbackManager callbacks;
    readonly SteamUser user;
    readonly SteamApps apps;
    readonly SteamContent content;
    readonly CancellationTokenSource pump = new();
    readonly SecretScrubber scrubber = new();
    readonly TaskCompletionSource connected = new(TaskCreationOptions.RunContinuationsAsynchronously);
    TaskCompletionSource<SteamUser.LoggedOnCallback> loggedOn = new(TaskCreationOptions.RunContinuationsAsynchronously);
    Task? pumping;
    Client? cdn;
    uint cellId;
    readonly ContentServers servers;

    SteamDepotSession()
    {
        client = new SteamClient(SteamConfiguration.Create(config => config.WithConnectionTimeout(Timeout)));
        callbacks = new CallbackManager(client);
        user = client.GetHandler<SteamUser>()!;
        apps = client.GetHandler<SteamApps>()!;
        content = client.GetHandler<SteamContent>()!;
        servers = new(content);
        callbacks.Subscribe<SteamClient.ConnectedCallback>(_ => connected.TrySetResult());
        callbacks.Subscribe<SteamClient.DisconnectedCallback>(_ =>
        {
            var closed = new IOException("Steam closed the connection.");
            connected.TrySetException(closed);
            loggedOn.TrySetException(closed);
        });
        callbacks.Subscribe<SteamUser.LoggedOnCallback>(result => loggedOn.TrySetResult(result));
    }

    // Connects, signs in as the request says and logs on. Throws SteamSignInException with the reason in plain words.
    public static async Task<SteamDepotSession> SignInAsync(SignInRequest request, ISignInPrompt prompt, CancellationToken cancellation)
    {
        var session = new SteamDepotSession();
        try
        {
            prompt = new ScrubbedPrompt(prompt, session.scrubber);
            prompt.Status("Connecting to Steam...");
            await session.ConnectAsync(cancellation);
            var (account, token) = await session.AuthenticateAsync(request, prompt, cancellation);
            prompt.Status("Signing in...");
            await session.LogOnAsync(account, token, cancellation);
            return session;
        }
        catch (Exception error) when (error is not OperationCanceledException and not SteamSignInException)
        {
            var reason = session.scrubber.Clean(error switch
            {
                AuthenticationException auth => Describe(auth.Result),
                TimeoutException => "Steam did not answer in time; check the internet connection and try again.",
                _ => error.Message,
            });
            await session.DisposeAsync();
            throw new SteamSignInException($"Steam sign-in did not work: {reason}");
        }
        catch
        {
            await session.DisposeAsync();
            throw;
        }
    }

    async Task ConnectAsync(CancellationToken cancellation)
    {
        pumping = Task.Run(async () =>
        {
            while (!pump.IsCancellationRequested)
            {
                try { await callbacks.RunWaitCallbackAsync(pump.Token); }
                catch (OperationCanceledException) { break; }
            }
        });
        client.Connect();
        await connected.Task.WaitAsync(Timeout, cancellation);
    }

    async Task<(string Account, string Token)> AuthenticateAsync(SignInRequest request, ISignInPrompt prompt, CancellationToken cancellation)
    {
        var details = new AuthSessionDetails { DeviceFriendlyName = "HellForge setup", IsPersistentSession = false };
        AuthSession session;
        if (request is PasswordSignIn password)
        {
            scrubber.Add(password.Password);
            details.Username = password.AccountName;
            details.Password = password.Password;
            details.Authenticator = new PromptAuthenticator(prompt, cancellation);
            session = await client.Authentication.BeginAuthSessionViaCredentialsAsync(details).WaitAsync(Timeout, cancellation);
            prompt.Status("Waiting for Steam to accept the sign-in...");
        }
        else
        {
            var qr = await client.Authentication.BeginAuthSessionViaQRAsync(details).WaitAsync(Timeout, cancellation);
            prompt.ShowQr(qr.ChallengeURL);
            qr.ChallengeURLChanged = () => prompt.ShowQr(qr.ChallengeURL);
            prompt.Status("Scan the code with the Steam app on your phone, then approve the sign-in there.");
            session = qr;
        }
        var result = await session.PollingWaitForResultAsync(cancellation);
        scrubber.Add(result.RefreshToken);
        scrubber.Add(result.AccessToken);
        return (result.AccountName, result.RefreshToken);
    }

    async Task LogOnAsync(string account, string token, CancellationToken cancellation)
    {
        user.LogOn(new SteamUser.LogOnDetails { Username = account, AccessToken = token, ShouldRememberPassword = false, LoginID = LoginId });
        var result = await loggedOn.Task.WaitAsync(Timeout, cancellation);
        if (result.Result != EResult.OK) throw new SteamSignInException($"Steam sign-in did not work: {Describe(result.Result)}");
        cellId = result.CellID;
        cdn = new Client(client);
    }

    // Steam's result in words a player can act on.
    static string Describe(EResult result) => result switch
    {
        EResult.InvalidPassword => "the account name or password is wrong.",
        EResult.AccountLoginDeniedThrottle or EResult.RateLimitExceeded => "Steam has had too many sign-in attempts; wait a while and try again.",
        EResult.TwoFactorCodeMismatch or EResult.InvalidLoginAuthCode => "the Steam Guard code was wrong.",
        EResult.Expired or EResult.FileNotFound => "the sign-in expired before it was approved; try again.",
        EResult.ServiceUnavailable or EResult.TryAnotherCM => "Steam is busy; try again in a few minutes.",
        _ => $"Steam answered {result}.",
    };

    public async Task<byte[]> DepotKeyAsync(uint app, uint depot, CancellationToken cancellation)
    {
        var key = await apps.GetDepotDecryptionKey(depot, app).ToTask().WaitAsync(Timeout, cancellation);
        if (key.Result != EResult.OK || key.DepotKey is not { Length: > 0 }) throw new DepotAccessDeniedException(depot, $"Steam answered {key.Result}");
        return key.DepotKey;
    }

    public Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, CancellationToken cancellation) =>
        content.GetManifestRequestCode(depot, app, manifest, "public").WaitAsync(Timeout, cancellation);

    public Task<DepotManifest> ManifestAsync(uint app, uint depot, ulong manifest, ulong requestCode, byte[] depotKey, CancellationToken cancellation) =>
        servers.Try(app, depot, cellId, async (server, cdnToken, token) =>
            ManifestFormat.From(await Cdn.DownloadManifestAsync(depot, manifest, requestCode, server, depotKey, null, cdnToken).WaitAsync(token)), cancellation);

    public Task<int> ChunkAsync(uint app, uint depot, DepotChunk chunk, byte[] depotKey, byte[] buffer, CancellationToken cancellation) =>
        servers.Try(app, depot, cellId, (server, cdnToken, token) =>
            Cdn.DownloadDepotChunkAsync(depot, ManifestFormat.ToSteam(chunk), server, buffer, depotKey, null, cdnToken).WaitAsync(token), cancellation);

    Client Cdn => cdn ?? throw new InvalidOperationException("Not signed in to Steam.");

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (cdn != null) user.LogOff();
            client.Disconnect();
        }
        catch (Exception error) when (error is InvalidOperationException or IOException) { }
        await pump.CancelAsync();
        if (pumping != null) await pumping.ConfigureAwait(false);
        cdn?.Dispose();
        pump.Dispose();
        scrubber.Forget();
    }

    // Steam Guard codes asked of the player. A sign-in that the Steam app could approve asks for the app's code instead,
    // so the password path always works with a code (the QR path is the one approved in the app).
    sealed class PromptAuthenticator(ISignInPrompt prompt, CancellationToken cancellation) : IAuthenticator
    {
        public Task<string> GetDeviceCodeAsync(bool previousCodeWasIncorrect) =>
            prompt.GuardCodeAsync(GuardCodeKind.App, null, previousCodeWasIncorrect, cancellation);

        public Task<string> GetEmailCodeAsync(string email, bool previousCodeWasIncorrect) =>
            prompt.GuardCodeAsync(GuardCodeKind.Email, email, previousCodeWasIncorrect, cancellation);

        public Task<bool> AcceptDeviceConfirmationAsync() => Task.FromResult(false);
    }
}
