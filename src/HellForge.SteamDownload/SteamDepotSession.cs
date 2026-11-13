using HellForge.Core;
using SteamKit2;
using SteamKit2.Authentication;
using SteamKit2.CDN;
using DepotChunk = HellForge.Core.DepotChunk;
using DepotManifest = HellForge.Core.DepotManifest;

namespace HellForge.SteamDownload;

// A sign-in to Steam for setup's download, on SteamKit2: Steam's own QR code sign-in, approved in the Steam app, then
// depot keys, manifest request codes, manifests and chunks from Steam's content servers. Written for HellForge on
// SteamKit2's API. For setup the session is not persistent and nothing about it is stored: the tokens Steam returns live
// in this object until it is disposed. The asset kit's fetches pass saved sign-ins (SteamLogins): those are tried first,
// and a QR sign-in is then persistent and kept there. It signs in as a separate session (its own login id), so the Steam
// client stays signed in.
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

    // Connects, signs in and logs on: with each saved sign-in in turn (saved: null for none), else with a QR code approved
    // in the Steam app, which is then kept in saved. Throws SteamSignInException with the reason in plain words.
    public static async Task<SteamDepotSession> SignInAsync(ISignInPrompt prompt, CancellationToken cancellation, SteamLogins? saved = null)
    {
        if (saved != null)
            foreach (var login in saved.Read())
                if (await SavedAsync(prompt, login, saved, cancellation) is { } signedIn) return signedIn;
        var session = new SteamDepotSession();
        try
        {
            prompt = new ScrubbedPrompt(prompt, session.scrubber);
            prompt.Status("Connecting to Steam...");
            await session.ConnectAsync(cancellation);
            var (account, token) = await session.AuthenticateAsync(prompt, persistent: saved != null, cancellation);
            prompt.Status("Signing in...");
            await session.LogOnAsync(account, token, cancellation);
            if (saved != null) Keep(saved, account, token, prompt);
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

    // A session signed in with a saved sign-in, or null when Steam did not take it (one it no longer accepts is
    // forgotten, SteamLogins.Rejected). HellForge's own token is renewed when Steam offers a new one.
    static async Task<SteamDepotSession?> SavedAsync(ISignInPrompt prompt, SteamLogins.Login login, SteamLogins saved, CancellationToken cancellation)
    {
        var session = new SteamDepotSession();
        session.scrubber.Add(login.RefreshToken);
        var scrubbed = new ScrubbedPrompt(prompt, session.scrubber);
        try
        {
            scrubbed.Status($"Signing in to Steam as {login.AccountName} ({login.From})...");
            await session.ConnectAsync(cancellation);
            var result = await session.TryLogOnAsync(login.AccountName, login.RefreshToken, cancellation);
            if (result.Result == EResult.OK)
            {
                if (login.Own) await session.RenewAsync(login, result.ClientSteamID, saved, scrubbed, cancellation);
                return session;
            }
            if (Rejects(result.Result)) saved.Rejected(login);
            scrubbed.Status(Rejects(result.Result) ? $"Steam no longer accepts {login.From}" + (login.Own ? "; it is forgotten." : ".")
                : $"Steam did not take {login.From}: {Describe(result.Result)}");
        }
        catch (Exception error) when (error is TimeoutException or IOException)
        {
            scrubbed.Status($"{login.From} did not work: " + session.scrubber.Clean(error.Message));
        }
        catch
        {
            await session.DisposeAsync();
            throw;
        }
        await session.DisposeAsync();
        return null;
    }

    // Steam's answers that mean the token is no longer good (others can pass: rate limits, maintenance).
    static bool Rejects(EResult result) => result is EResult.InvalidPassword or EResult.AccessDenied or EResult.Expired or EResult.Revoked
        or EResult.InvalidSignature;

    // A new refresh token when Steam offers one (close to its expiry), kept in place of the old; best effort.
    async Task RenewAsync(SteamLogins.Login login, SteamID? steamId, SteamLogins saved, ISignInPrompt prompt, CancellationToken cancellation)
    {
        if (steamId == null) return;
        try
        {
            var renewed = await client.Authentication.GenerateAccessTokenForAppAsync(steamId, login.RefreshToken, allowRenewal: true).WaitAsync(Timeout, cancellation);
            scrubber.Add(renewed.AccessToken);
            scrubber.Add(renewed.RefreshToken);
            if (renewed.RefreshToken is { Length: > 0 } token && token != login.RefreshToken) Keep(saved, login.AccountName, token, prompt);
        }
        catch (Exception error) when (error is AuthenticationException or TimeoutException or IOException or TaskCanceledException && !cancellation.IsCancellationRequested) { }
    }

    static void Keep(SteamLogins saved, string account, string token, ISignInPrompt prompt)
    {
        try { saved.Save(account, token); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            prompt.Status("Signed in, but the sign-in could not be kept for next time: " + error.Message);
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

    // persistent: the token is to be kept (SteamLogins), and Steam lists the sign-in as "HellForge" among the account's
    // authorized devices, where the player can remove it.
    async Task<(string Account, string Token)> AuthenticateAsync(ISignInPrompt prompt, bool persistent, CancellationToken cancellation)
    {
        var details = new AuthSessionDetails { DeviceFriendlyName = persistent ? "HellForge" : "HellForge setup", IsPersistentSession = persistent };
        var qr = await client.Authentication.BeginAuthSessionViaQRAsync(details).WaitAsync(Timeout, cancellation);
        prompt.ShowQr(qr.ChallengeURL);
        qr.ChallengeURLChanged = () => prompt.ShowQr(qr.ChallengeURL);
        prompt.Status("Scan the code with the Steam app on your phone, then approve the sign-in there.");
        var result = await qr.PollingWaitForResultAsync(cancellation);
        scrubber.Add(result.RefreshToken);
        scrubber.Add(result.AccessToken);
        return (result.AccountName, result.RefreshToken);
    }

    async Task LogOnAsync(string account, string token, CancellationToken cancellation)
    {
        var result = await TryLogOnAsync(account, token, cancellation);
        if (result.Result != EResult.OK) throw new SteamSignInException($"Steam sign-in did not work: {Describe(result.Result)}");
    }

    // Logs on with a refresh token; Steam's answer (signed in when OK).
    async Task<SteamUser.LoggedOnCallback> TryLogOnAsync(string account, string token, CancellationToken cancellation)
    {
        user.LogOn(new SteamUser.LogOnDetails { Username = account, AccessToken = token, ShouldRememberPassword = false, LoginID = LoginId });
        var result = await loggedOn.Task.WaitAsync(Timeout, cancellation);
        if (result.Result != EResult.OK) return result;
        cellId = result.CellID;
        cdn = new Client(client);
        return result;
    }

    // Steam's result in words a player can act on.
    static string Describe(EResult result) => result switch
    {
        EResult.AccountLoginDeniedThrottle or EResult.RateLimitExceeded => "Steam has had too many sign-in attempts; wait a while and try again.",
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
        ManifestRequestCodeAsync(app, depot, manifest, "public", cancellation);

    public Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, string branch, CancellationToken cancellation) =>
        content.GetManifestRequestCode(depot, app, manifest, branch).WaitAsync(Timeout, cancellation);

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
}
