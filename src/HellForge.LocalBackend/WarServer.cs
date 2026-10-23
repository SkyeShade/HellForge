using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using System.Text.RegularExpressions;
using HellForge.Core;

namespace HellForge.LocalBackend;

// A war server (--war server): the game's Arrowhead routes go to a server the player named, a community or private war,
// instead of being answered here (docs/external-launchers.md, "War servers"). What stays on this PC is what co-op with
// friends needs: the locator, the PlayFab stand-in, the social tab's lobbies and the player keys (the login is forwarded,
// but its player key is kept here first). A forwarded request goes as it is and its answer comes back as it is; one the
// server does not answer gets a 502 (HELLFORGE_WAR_SERVER_UNREACHABLE), never a local stand-in, so the game never sees a
// mix of the server's war and the built-in one. The server is checked before the backend is ready (GET
// /hellforge/v1/hello). Only this process talks to it: the game still reaches loopback alone.
public sealed class WarServer : IDisposable
{
    public const string Mode = "server";
    public const int Protocol = 1;
    public const string HelloPath = "/hellforge/v1/hello";
    public const string SteamIdHeader = "X-HellForge-SteamId", VersionHeader = "X-HellForge-Version", TokenHeader = "X-HellForge-Token";
    static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(20);
    const int MaximumResponseBytes = 64 * 1024 * 1024;
    // Not passed on: the connection's own headers, and Accept-Encoding, so the server's answer comes back uncompressed
    // (only its Content-Type goes back to the game). HellForge's (X-HellForge-*) only this backend sets.
    static readonly HashSet<string> Dropped = new(StringComparer.OrdinalIgnoreCase)
        { "Host", "Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Content-Length", "Expect",
          "Accept-Encoding" };
    static readonly Regex StatusPath = new("^/WarSeason/[0-9]+/Status$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    readonly HttpClient http;
    readonly string? token;
    readonly Action<string> log;
    readonly string version;

    public string Origin { get; }
    // The name the server gave in its hello (1 to 64 printable characters).
    public string Name { get; private set; } = "";

    // A forwarded request's answer.
    public sealed record Reply(int Status, string? ContentType, byte[] Body, long Milliseconds);

    WarServer(string origin, string? token, HttpMessageHandler handler, Action<string> log)
    {
        Origin = origin;
        this.token = token;
        this.log = log;
        version = $"HellForge {AppVersion.Text}; engine {typeof(WarServer).Assembly.GetName().Version}";
        http = new HttpClient(handler, disposeHandler: true) { BaseAddress = new Uri(origin), Timeout = RequestTimeout, MaxResponseContentBufferSize = MaximumResponseBytes };
    }

    // What stays here: the locator, PlayFab, the social tab's lobbies and the player keys. Everything else is forwarded.
    public static bool Forwards(PathString path) =>
        !(path == "/config/locator.json" || PlayFabService.Handles(path.Value ?? "") || path == "/Account/Keys" || path.StartsWithSegments("/lobby"));

    // Checks the server (its hello) and returns it ready to forward. caFile: trust only this CA (or this certificate) for
    // the server, instead of Windows' roots. tokenFile: a token sent with every request, from a file so it never shows on a
    // command line. A failure says why, as an InvalidDataException.
    public static async Task<WarServer> ConnectAsync(string origin, string? caFile, string? tokenFile, Action<string> log,
        HttpMessageHandler? handler = null, CancellationToken cancellation = default)
    {
        origin = WarServerOptions.ParseOrigin(origin);
        var token = tokenFile != null ? ReadToken(tokenFile) : null;
        var ca = caFile != null ? LoadCa(caFile) : null;
        var server = new WarServer(origin, token, handler ?? Handler(ca), log);
        try
        {
            await server.HelloAsync(cancellation);
            log($"War server {origin} ({server.Name}) answered; the game's war routes go to it{(ca != null ? ", trusted through the given CA" : "")}.");
            return server;
        }
        catch
        {
            server.Dispose();
            throw;
        }
    }

    async Task HelloAsync(CancellationToken cancellation)
    {
        string Failure(string reason) => $"The war server {Origin} could not be reached: {reason}";
        try
        {
            using var message = new HttpRequestMessage(HttpMethod.Get, HelloPath);
            message.Headers.TryAddWithoutValidation(VersionHeader, version);
            if (token != null) message.Headers.TryAddWithoutValidation(TokenHeader, token);
            using var response = await http.SendAsync(message, cancellation);
            if (response.StatusCode != HttpStatusCode.OK) throw new InvalidDataException(Failure($"its hello answered HTTP {(int)response.StatusCode}."));
            using var hello = JsonDocument.Parse(await response.Content.ReadAsByteArrayAsync(cancellation));
            var root = hello.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("protocol", out var protocol) || !protocol.TryGetInt32(out var number))
                throw new InvalidDataException(Failure("its hello has no protocol."));
            if (number != Protocol) throw new InvalidDataException(Failure($"it speaks war server protocol {number}, this HellForge speaks {Protocol}."));
            var name = root.TryGetProperty("name", out var named) && named.ValueKind == JsonValueKind.String ? named.GetString()! : "";
            if (name.Length is 0 or > 64 || !name.All(c => c is >= ' ' and <= '~')) throw new InvalidDataException(Failure("its hello has no name (1 to 64 printable characters)."));
            Name = name;
        }
        catch (JsonException) { throw new InvalidDataException(Failure("its hello is not JSON.")); }
        catch (HttpRequestException error) { throw new InvalidDataException(Failure(Reason(error))); }
        catch (TaskCanceledException error) when (!cancellation.IsCancellationRequested) { throw new InvalidDataException(Failure(Reason(error))); }
    }

    // Why a request failed, in words: the NativeAOT build keeps the framework's resource keys
    // (net_http_ssl_connection_failed), not its messages.
    static string Reason(Exception error) => error switch
    {
        TaskCanceledException => $"no answer within {RequestTimeout.TotalSeconds} s.",
        HttpRequestException { HttpRequestError: HttpRequestError.NameResolutionError } => "its name could not be resolved.",
        HttpRequestException { HttpRequestError: HttpRequestError.ConnectionError } => "no connection could be made.",
        HttpRequestException { HttpRequestError: HttpRequestError.SecureConnectionError } =>
            "the secure connection failed: its certificate is not trusted for that name (--war-server-ca gives the CA to trust).",
        _ => error.Message
    };

    // The game's request as it is (method, path, query, body and headers), with this backend's own headers added, and the
    // server's answer. Throws HttpRequestException or TaskCanceledException when the server does not answer.
    public async Task<Reply> ForwardAsync(HttpRequest request, byte[] body, ulong? steamId, CancellationToken cancellation)
    {
        var clock = Stopwatch.StartNew();
        using var message = new HttpRequestMessage(new HttpMethod(request.Method), request.Path.ToUriComponent() + request.QueryString.ToUriComponent());
        if (body.Length > 0 || !(HttpMethods.IsGet(request.Method) || HttpMethods.IsHead(request.Method) || HttpMethods.IsDelete(request.Method)))
            message.Content = new ByteArrayContent(body);
        foreach (var (name, values) in request.Headers)
        {
            if (Dropped.Contains(name) || name.StartsWith("X-HellForge-", StringComparison.OrdinalIgnoreCase)) continue;
            if (!message.Headers.TryAddWithoutValidation(name, (IEnumerable<string?>)values)) message.Content?.Headers.TryAddWithoutValidation(name, (IEnumerable<string?>)values);
        }
        message.Headers.TryAddWithoutValidation(VersionHeader, version);
        if (steamId is { } id) message.Headers.TryAddWithoutValidation(SteamIdHeader, id.ToString(CultureInfo.InvariantCulture));
        if (token != null) message.Headers.TryAddWithoutValidation(TokenHeader, token);
        try
        {
            using var response = await http.SendAsync(message, cancellation);
            var answer = await response.Content.ReadAsByteArrayAsync(cancellation);
            return new Reply((int)response.StatusCode, response.Content.Headers.ContentType?.ToString(), answer, clock.ElapsedMilliseconds);
        }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException && !cancellation.IsCancellationRequested)
        {
            log($"{request.Method} {request.Path.Value} was not answered by the war server ({Reason(error)}); the game got HTTP 502.");
            throw;
        }
    }

    // The war time of a war status the server answered (GET /WarSeason/<id>/Status), which the war clock follows, so a
    // co-op host's war.clock still matches its war.
    public static long? StatusTime(HttpRequest request, Reply reply)
    {
        if (!HttpMethods.IsGet(request.Method) || reply.Status != StatusCodes.Status200OK || !StatusPath.IsMatch(request.Path.Value ?? "")) return null;
        try
        {
            using var status = JsonDocument.Parse(reply.Body);
            return status.RootElement.ValueKind == JsonValueKind.Object && status.RootElement.TryGetProperty("time", out var time) &&
                time.TryGetInt64(out var seconds) && seconds > 0 ? seconds : null;
        }
        catch (JsonException) { return null; }
    }

    static string ReadToken(string file)
    {
        var text = File.ReadAllText(file).Trim();
        if (text.Length is 0 or > 512 || !text.All(c => c is > ' ' and <= '~'))
            throw new InvalidDataException("The war server token file must hold one token of 1 to 512 printable characters.");
        return text;
    }

    static X509Certificate2 LoadCa(string file)
    {
        try { return X509Certificate2.CreateFromPem(File.ReadAllText(file)); }
        catch (CryptographicException error) { throw new InvalidDataException($"The war server CA is not a PEM certificate ({error.Message})."); }
    }

    static SocketsHttpHandler Handler(X509Certificate2? ca)
    {
        var handler = new SocketsHttpHandler { AllowAutoRedirect = false, AutomaticDecompression = DecompressionMethods.None, PooledConnectionLifetime = TimeSpan.FromMinutes(5) };
        if (ca != null) handler.SslOptions.RemoteCertificateValidationCallback = (_, certificate, chain, errors) => Trusted(certificate, chain, errors, ca);
        return handler;
    }

    // With a CA given: the server's certificate must be for the server's name and chain to that CA (or be it), whatever
    // Windows' roots say. No revocation check: a private CA has no revocation list.
    public static bool Trusted(X509Certificate? certificate, X509Chain? presented, SslPolicyErrors errors, X509Certificate2 ca)
    {
        if (certificate is not X509Certificate2 leaf || (errors & (SslPolicyErrors.RemoteCertificateNotAvailable | SslPolicyErrors.RemoteCertificateNameMismatch)) != 0)
            return false;
        using var chain = new X509Chain();
        chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
        chain.ChainPolicy.CustomTrustStore.Add(ca);
        chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
        if (presented != null)
            foreach (var element in presented.ChainElements)
                chain.ChainPolicy.ExtraStore.Add(element.Certificate);
        return chain.Build(leaf);
    }

    public void Dispose() => http.Dispose();
}
