// Sync Inventory's capture: reads the player's own HELLDIVERS 2 inventory and progression from the live API while the
// normal game logs in (HellForge.Core InventorySync starts it elevated from the launcher).
//
// It maps the API host to 127.0.0.1 in the hosts file, trusts a per-run CA in LocalMachine\Root (with a CRL served on
// loopback, since Schannel checks revocation), terminates the game's TLS and forwards every request unchanged to the real
// API by IP. With the session the login returns it reads GET /api/Progression and GET /api/Progression/inventory,
// repeating the client headers the login carried (CaptureHeaders: the API refuses a read without Accept-Language), and
// writes them to --progression and --output. With --seal-key and --steam-account it seals the two files for that Steam
// account (HellForge.Core SnapshotSeal) as soon as the inventory is in, and says so in its state file (CaptureState), so
// the launcher takes them while the game plays on; the backend unlocks premium items only from a seal. It then keeps
// forwarding until the game exits, removes the hosts line and the CA, and exits 0 (1 when nothing was captured before
// the timeout or the seal failed). Nothing else is kept. The log (proxy.log) is detailed but holds no values: for every
// connection its TLS version, cipher and SNI, timings and where it failed; for every request and response the method, the
// first two path segments, status, sizes, content types and the names of headers and query parameters; a refused
// request's reason (CaptureLog); the key-and-count shape of what the capture keeps (CaptureDiagnostics); the game's and
// Easy Anti-Cheat's processes; and a summary. Never bodies, header or query values, or the session.
//
//   --cleanup      only remove a hosts line and CA left by a run that was killed
//   --no-hosts --no-ca --listen-port 8443 --real-ip <ip>   local plumbing test, no system changes

using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Numerics;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using HellForge.Core;

const string CaName = "HellForge Capture CA";
const string LoginPath = "/api/Account/Login", ProgressionPath = "/api/Progression", InventoryPath = "/api/Progression/inventory";
const string GameProcess = "helldivers2";
// The redirect stays while the game runs (ForwardUntilGameExits), but never past the certificates' day.
const int ForwardHoursAtMost = 12;
var hostsPath = Path.Combine(Environment.SystemDirectory, "drivers", "etc", "hosts");

var host = "api.live.prod.thehelldiversgame.com";
var listenPort = 443;
List<IPAddress>? fixedUpstream = null;
var outputPath = Path.Combine(AppContext.BaseDirectory, "real-inventory.json");
string? progressionPath = null;
var evidenceDir = Path.Combine(AppContext.BaseDirectory, "capture");
bool manageHosts = true, manageCa = true, cleanupOnly = false;
var timeoutMin = 10;
string? sealKey = null;
ulong? steamAccount = null;

try
{
    for (var i = 0; i < args.Length; i++)
    {
        string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException(args[i] + " needs a value.");
        switch (args[i])
        {
            case "--host": host = Next(); break;
            case "--listen-port": listenPort = int.Parse(Next(), CultureInfo.InvariantCulture); break;
            case "--real-ip": fixedUpstream = [IPAddress.Parse(Next())]; break;
            case "--output": outputPath = Next(); break;
            case "--progression": progressionPath = Next(); break;
            case "--evidence": evidenceDir = Next(); break;
            case "--timeout-min": timeoutMin = Math.Max(1, int.Parse(Next(), CultureInfo.InvariantCulture)); break;
            case "--no-hosts": manageHosts = false; break;
            case "--no-ca": manageCa = false; break;
            case "--cleanup": cleanupOnly = true; break;
            case "--seal-key": sealKey = Next(); break;
            case "--steam-account": steamAccount = ulong.Parse(Next(), NumberStyles.None, CultureInfo.InvariantCulture); break;
            case "--help":
                Console.WriteLine("HellForge.CaptureProxy [--output <file>] [--progression <file>] [--evidence <dir>] [--timeout-min <n>]");
                Console.WriteLine("  [--seal-key <file> --steam-account <SteamID64>] [--host <name>] [--listen-port <n>] [--real-ip <ip>]");
                Console.WriteLine("  [--no-hosts] [--no-ca] | --cleanup");
                return 0;
            default: throw new ArgumentException("Unknown argument: " + args[i]);
        }
    }
    if ((sealKey == null) != (steamAccount == null)) throw new ArgumentException("--seal-key and --steam-account go together.");
    // The seal covers the snapshot's own file names, side by side.
    if (sealKey != null && (Path.GetFileName(outputPath) != SnapshotSeal.InventoryFile ||
        progressionPath != null && Path.GetFullPath(progressionPath) != Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outputPath))!, SnapshotSeal.ProgressionFile)))
        throw new ArgumentException("A sealed capture writes " + SnapshotSeal.InventoryFile + " and " + SnapshotSeal.ProgressionFile + " in one folder.");
}
catch (Exception error) when (error is ArgumentException or FormatException or OverflowException)
{
    Console.Error.WriteLine(error.Message);
    return 2;
}

if (cleanupOnly)
{
    var removed = Step("hosts cleanup", RemoveHosts) & Step("CA cleanup", () => RemoveCas(null));
    return removed ? 0 : 1;
}

try { Directory.CreateDirectory(evidenceDir); }
catch (Exception error) when (error is IOException or UnauthorizedAccessException)
{
    Console.Error.WriteLine("Cannot use " + evidenceDir + ": " + error.Message);
    return 1;
}
var statePath = Path.Combine(evidenceDir, CaptureState.FileName);
// The sync folder stays private to this user.
Run("icacls", $"\"{evidenceDir}\" /inheritance:r /grant:r \"{System.Security.Principal.WindowsIdentity.GetCurrent().Name}:(OI)(CI)F\"");

// Resolve the real host before the hosts line points it at loopback; clear a line an earlier killed run left first.
if (manageHosts && !Step("hosts cleanup", RemoveHosts)) return 1;
List<IPAddress> upstreamIps;
try
{
    upstreamIps = fixedUpstream ?? [.. Dns.GetHostAddresses(host).Where(address => address.AddressFamily == AddressFamily.InterNetwork)];
}
catch (SocketException error)
{
    Log("could not resolve " + host + ": " + error.SocketErrorCode);
    return 1;
}
if (upstreamIps.Count == 0) { Log("no IPv4 address for " + host); return 1; }
Log("upstream " + string.Join(", ", upstreamIps) + ":443");

string? caThumbprint = null;
X509Certificate2 serverCert;
// A crash from here on would leave the hosts line and the CA trusted: an exception nothing catches is logged with its
// cause and they are removed before the process ends (the cleanup below runs only once the listener exists).
AppDomain.CurrentDomain.UnhandledException += (_, e) =>
{
    Log("crashed: " + (e.ExceptionObject is Exception error ? CaptureDiagnostics.Chain(error) : e.ExceptionObject));
    if (manageHosts) Step("hosts cleanup", RemoveHosts);
    if (manageCa) Step("CA cleanup", () => RemoveCas(null));
};
if (manageCa)
{
    var crlListener = new TcpListener(IPAddress.Loopback, 0);
    crlListener.Start();
    var cdp = EncodeCdp($"http://127.0.0.1:{((IPEndPoint)crlListener.LocalEndpoint).Port}/ca-{Guid.NewGuid():N}.crl");
    using var caKey = RSA.Create(2048);
    var caRequest = new CertificateRequest("CN=" + CaName + ", O=HellForge", caKey, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    caRequest.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
    caRequest.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign, true));
    caRequest.CertificateExtensions.Add(new X509Extension("2.5.29.31", cdp, false));
    using var ca = caRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
    if (!Step("CA cleanup", () => RemoveCas(null))) return 1;
    using (var store = new X509Store(StoreName.Root, StoreLocation.LocalMachine))
    {
        store.Open(OpenFlags.ReadWrite);
        using var publicCa = X509CertificateLoader.LoadCertificate(ca.Export(X509ContentType.Cert));
        store.Add(publicCa);
    }
    caThumbprint = ca.Thumbprint;
    Log("CA installed");
    serverCert = MakeLeaf(ca, host, cdp);
    StartCrlServer(crlListener, new CertificateRevocationListBuilder().Build(ca, BigInteger.One, new DateTimeOffset(ca.NotAfter),
        HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1, DateTimeOffset.UtcNow.AddDays(-1)));
}
else serverCert = MakeLeaf(null, host, null);

// Loopback only: the hosts line points the API host at 127.0.0.1, and nothing else on the network may log in through us.
var listener = new TcpListener(IPAddress.Loopback, listenPort);
var cleaned = 0;
void Cleanup()
{
    if (Interlocked.Exchange(ref cleaned, 1) == 1) return;
    listener.Stop();
    if (manageHosts) Step("hosts cleanup", RemoveHosts);
    if (caThumbprint != null) Step("CA cleanup", () => RemoveCas(caThumbprint));
    Step("state cleanup", () => File.Delete(statePath));
}
Console.CancelKeyPress += (_, e) => { e.Cancel = true; Cleanup(); Environment.Exit(1); };
AppDomain.CurrentDomain.ProcessExit += (_, _) => Cleanup();

using var handler = new SocketsHttpHandler
{
    // A plain TCP stream to the real API by IP (the hosts line points the name at us); the handler does TLS against the
    // real certificate. Every address is tried before giving up.
    ConnectCallback = async (_, token) =>
    {
        Exception? last = null;
        foreach (var ip in upstreamIps)
        {
            var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            var connecting = Stopwatch.StartNew();
            try
            {
                using var connectTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                connectTimeout.CancelAfter(TimeSpan.FromSeconds(6));
                await socket.ConnectAsync(new IPEndPoint(ip, 443), connectTimeout.Token);
                Log($"upstream connected to {ip}:443 in {CaptureDiagnostics.Milliseconds(connecting)}");
                return new NetworkStream(socket, ownsSocket: true);
            }
            catch (Exception error) when (error is SocketException or OperationCanceledException)
            {
                Log($"upstream {ip}:443 failed after {CaptureDiagnostics.Milliseconds(connecting)}: {CaptureDiagnostics.Chain(error)}");
                last = error;
                socket.Dispose();
            }
        }
        throw new IOException("no upstream address reachable: " + last?.Message);
    },
    AllowAutoRedirect = false,
    UseCookies = false,
    AutomaticDecompression = DecompressionMethods.All,
    ConnectTimeout = TimeSpan.FromSeconds(15)
};
using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };

var captured = 0;
var sealOk = false;
// The login's client headers, repeated on the capture's own reads (CaptureHeaders): the API refuses a read without
// Accept-Language, which is how a live sync sealed an inventory and no progression (2026-10-26).
Dictionary<string, string>? loginHeaders = null;
var connections = 0;
var tally = new CaptureTally();
var lastRequest = Stopwatch.StartNew();
// Cancelled when nothing was captured in time, and after a capture once the game exits (ForwardUntilGameExits).
using var stop = new CancellationTokenSource(TimeSpan.FromMinutes(timeoutMin));
try
{
    LogStart();
    if (manageHosts) AddHosts();
    LogResolution();
    listener.Start();
    Log($"listening on port {listenPort}");
    CaptureState.Write(evidenceDir, Environment.ProcessId, sealedCapture: false, progression: false);
    _ = Task.Run(WatchProcesses);
    while (!stop.IsCancellationRequested)
    {
        TcpClient client;
        try { client = await listener.AcceptTcpClientAsync(stop.Token); }
        catch (OperationCanceledException) { break; }
        lastRequest.Restart();
        _ = Task.Run(() => Handle(client));
    }
}
finally
{
    Cleanup();
}
Log(captured == 1 ? "capture complete" : "nothing captured");
LogSummary();
return captured == 1 && sealOk ? 0 : 1;

// What this run is, so a log from another PC or build can be read on its own.
void LogStart()
{
    var exe = Environment.ProcessPath;
    var built = exe != null ? File.GetLastWriteTimeUtc(exe).ToString("u", CultureInfo.InvariantCulture) : "unknown";
    var elevated = new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent())
        .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
    Log($"capture built {built}, {Environment.OSVersion.VersionString}, .NET {Environment.Version}, elevated {(elevated ? "yes" : "no")}");
    Log($"options: host {host}, port {listenPort}, timeout {timeoutMin} min, hosts line {(manageHosts ? "yes" : "no")}, " +
        $"CA {(manageCa ? "yes" : "no")}, progression {(progressionPath != null ? "yes" : "no")}, seal {(sealKey != null ? "yes" : "no")}");
}

// Where the system now sends the API's name: 127.0.0.1 when the hosts line holds.
void LogResolution()
{
    try { Log($"system DNS: {host} -> {string.Join(", ", Dns.GetHostAddresses(host).Select(address => address.ToString()))}"); }
    catch (SocketException error) { Log($"system DNS: {host} does not resolve: {error.SocketErrorCode}"); }
}

void LogSummary()
{
    Log($"summary: {connections} connections, {tally.Total} requests answered by the API");
    foreach (var line in tally.Lines()) Log("  " + line);
}

// The game's and Easy Anti-Cheat's processes coming and going, and a heartbeat every 30 seconds, for the whole run.
async Task WatchProcesses()
{
    var seen = new Dictionary<int, string>();
    var beat = Stopwatch.StartNew();
    while (!stop.IsCancellationRequested)
    {
        LogProcessChanges(seen);
        if (beat.Elapsed >= TimeSpan.FromSeconds(30))
        {
            beat.Restart();
            Log($"status: {connections} connections, {tally.Total} requests, last connection {lastRequest.Elapsed.TotalSeconds:F0} s ago, " +
                $"game {(seen.ContainsValue(GameProcess) ? "running" : "not running")}, captured {(Volatile.Read(ref captured) == 1 ? "yes" : "no")}");
        }
        try { await Task.Delay(TimeSpan.FromSeconds(2), stop.Token); }
        catch (OperationCanceledException) { return; }
    }
}

void LogProcessChanges(Dictionary<int, string> seen)
{
    var now = new Dictionary<int, string>();
    foreach (var name in new[] { GameProcess, "EasyAntiCheat_EOS", "EasyAntiCheat", "start_protected_game" })
        foreach (var process in Process.GetProcessesByName(name))
        {
            now[process.Id] = name;
            process.Dispose();
        }
    foreach (var (pid, name) in now.Where(entry => !seen.ContainsKey(entry.Key))) Log($"process {name} started (pid {pid})");
    foreach (var (pid, name) in seen.Where(entry => !now.ContainsKey(entry.Key))) Log($"process {name} exited (pid {pid})");
    seen.Clear();
    foreach (var entry in now) seen[entry.Key] = entry.Value;
}

// Once the inventory is in: seal what was captured and tell the launcher, then keep the game's connection until it exits.
void CompleteCapture()
{
    var progression = progressionPath != null && File.Exists(progressionPath);
    sealOk = sealKey == null || Step("seal", () => SnapshotSeal.Write(Path.GetDirectoryName(Path.GetFullPath(outputPath))!, sealKey, steamAccount!.Value, DateTime.UtcNow));
    if (sealKey != null && sealOk) Log("capture sealed" + (progression ? "" : " without progression"));
    Step("state", () => CaptureState.Write(evidenceDir, Environment.ProcessId, sealOk, progression));
    _ = Task.Run(ForwardUntilGameExits);
}

// The redirect and the CA stay until the game exits, so its requests after the login still reach the API. Easy Anti-Cheat
// closed the game ("Could not connect to Easy Anti Cheat network") both when the redirect ended right after the login and
// when it stayed (live runs, 2026-10-26), so that error has another cause; WatchProcesses and the per-connection lines
// are there to find it.
async Task ForwardUntilGameExits()
{
    stop.CancelAfter(TimeSpan.FromHours(ForwardHoursAtMost));
    Log("forwarding until the game exits");
    try
    {
        while (IsGameRunning()) await Task.Delay(TimeSpan.FromSeconds(2), stop.Token);
        Log("game exited");
    }
    catch (OperationCanceledException)
    {
        Log($"still forwarding after {ForwardHoursAtMost} hours: stopping");
    }
    stop.Cancel();
}

static bool IsGameRunning()
{
    var running = Process.GetProcessesByName(GameProcess);
    foreach (var process in running) process.Dispose();
    return running.Length > 0;
}

// One connection, one request (the response says Connection: close). Each line carries the connection's number, so
// interleaved connections stay apart in the log.
async Task Handle(TcpClient client)
{
    var id = "#" + Interlocked.Increment(ref connections);
    using var connection = client;
    using var ssl = new SslStream(client.GetStream(), leaveInnerStreamOpen: false);
    var watch = Stopwatch.StartNew();
    Log($"{id} connection from {client.Client.RemoteEndPoint}");
    if (!await Handshake(ssl, id, watch)) return;
    var stage = "reading the request";
    try
    {
        if (await ReadRequest(ssl) is not { } request) { Log($"{id} closed before a whole request ({CaptureDiagnostics.Milliseconds(watch)})"); return; }
        var path = request.Target.Split('?')[0];
        if (path.Equals(LoginPath, StringComparison.OrdinalIgnoreCase)) loginHeaders = request.Headers;
        LogRequest(id, request);
        stage = "asking the API";
        using var upstream = Forward(request);
        var asked = Stopwatch.StartNew();
        using var response = await http.SendAsync(upstream);
        var responseBody = await response.Content.ReadAsByteArrayAsync();
        tally.Add(request.Method, Shorten(path), (int)response.StatusCode);
        Log($"{request.Method} {Shorten(path)} {(int)response.StatusCode} {responseBody.Length} bytes");
        LogResponse(id, path, response, responseBody, asked);
        stage = "keeping the capture";
        if (response.IsSuccessStatusCode) await Capture(path, responseBody);
        stage = "answering the game";
        await Respond(ssl, response, responseBody);
        await CheckReuse(ssl, id);
        Log($"{id} done in {CaptureDiagnostics.Milliseconds(watch)}");
    }
    catch (Exception error) when (error is IOException or HttpRequestException or TaskCanceledException or InvalidOperationException or FormatException)
    {
        Log($"{id} failed while {stage} after {CaptureDiagnostics.Milliseconds(watch)}: {CaptureDiagnostics.Chain(error)}");
    }
}

async Task<bool> Handshake(SslStream ssl, string id, Stopwatch watch)
{
    try
    {
        await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
        {
            ServerCertificate = serverCert,
            EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
            ApplicationProtocols = [SslApplicationProtocol.Http11]
        });
        Log($"{id} TLS {CaptureDiagnostics.Tls(ssl)} in {CaptureDiagnostics.Milliseconds(watch)}");
        return true;
    }
    catch (Exception error) when (error is AuthenticationException or IOException)
    {
        // A client that pins the API's certificate, or does not trust the CA, fails here.
        Log($"{id} TLS handshake failed after {CaptureDiagnostics.Milliseconds(watch)}: 0x" +
            (error.InnerException ?? error).HResult.ToString("X8", CultureInfo.InvariantCulture) + " " + CaptureDiagnostics.Chain(error));
        return false;
    }
}

static bool IsCapturePath(string path) =>
    path.Equals(LoginPath, StringComparison.OrdinalIgnoreCase) || path.Equals(ProgressionPath, StringComparison.OrdinalIgnoreCase) ||
    path.Equals(InventoryPath, StringComparison.OrdinalIgnoreCase);

// Every request, in names and sizes only.
void LogRequest(string id, (string Method, string Target, Dictionary<string, string> Headers, byte[] Body) request)
{
    var type = request.Headers.TryGetValue("Content-Type", out var contentType) ? contentType.Split(';')[0] : "none";
    Log($"{id} {request.Method} {Shorten(request.Target.Split('?')[0])} {request.Headers.GetValueOrDefault("Host") ?? "no host"}, " +
        $"body {request.Body.Length} bytes ({type}); headers: {CaptureLog.Names(request.Headers.Keys)}; query: {CaptureLog.QueryNames(request.Target)}");
}

// Every response: status, time, type and header names; a refused one's reason; the shape of what the capture keeps.
void LogResponse(string id, string path, HttpResponseMessage response, byte[] body, Stopwatch asked)
{
    var type = response.Content.Headers.ContentType?.MediaType ?? "none";
    Log($"{id} -> {(int)response.StatusCode} {response.ReasonPhrase} HTTP/{response.Version}, {body.Length} bytes ({type}) in " +
        $"{CaptureDiagnostics.Milliseconds(asked)}; headers: {CaptureLog.Names(response.Headers.Concat(response.Content.Headers).Select(header => header.Key))}");
    if (!response.IsSuccessStatusCode) Log("  refused: " + CaptureLog.ErrorText(body));
    else if (IsCapturePath(path)) Log($"{id} body shape: {CaptureDiagnostics.JsonShape(body)}");
}

// The response said Connection: close. A client that still sends on this connection expected to reuse it (keep-alive),
// which the capture does not do: its next request is lost and it must reconnect.
async Task CheckReuse(SslStream ssl, string id)
{
    try
    {
        using var linger = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
        var probe = new byte[1];
        var n = await ssl.ReadAsync(probe, linger.Token);
        Log(n == 0 ? $"{id} client closed after the response" : $"{id} client sent more on this connection after Connection: close (keep-alive is not supported)");
    }
    catch (OperationCanceledException) { Log($"{id} client kept the connection open after the response"); }
    catch (IOException error) { Log($"{id} connection reset after the response: {CaptureDiagnostics.Chain(error)}"); }
}

// The game's request as it was, to the real API (hop-by-hop headers left out).
HttpRequestMessage Forward((string Method, string Target, Dictionary<string, string> Headers, byte[] Body) request)
{
    var upstream = new HttpRequestMessage(new HttpMethod(request.Method), $"https://{host}{request.Target}");
    foreach (var (name, value) in request.Headers)
        if (name is not ("Host" or "Connection" or "Content-Length" or "Content-Type" or "Transfer-Encoding" or "Accept-Encoding" or "Expect" or "Proxy-Connection" or "Upgrade" or "TE"))
            upstream.Headers.TryAddWithoutValidation(name, value);
    if (request.Body.Length == 0 && request.Method is not ("POST" or "PUT")) return upstream;
    upstream.Content = new ByteArrayContent(request.Body);
    if (request.Headers.TryGetValue("Content-Type", out var contentType)) upstream.Content.Headers.TryAddWithoutValidation("Content-Type", contentType);
    return upstream;
}

static async Task Respond(SslStream ssl, HttpResponseMessage response, byte[] body)
{
    var head = new StringBuilder($"HTTP/1.1 {(int)response.StatusCode} {response.ReasonPhrase}\r\n");
    foreach (var (name, values) in response.Headers.Concat(response.Content.Headers))
        if (name is not ("Transfer-Encoding" or "Connection" or "Keep-Alive" or "Content-Length" or "Content-Encoding"))
            foreach (var value in values) head.Append(name).Append(": ").Append(value).Append("\r\n");
    head.Append("Content-Length: ").Append(body.Length).Append("\r\nConnection: close\r\n\r\n");
    await ssl.WriteAsync(Encoding.ASCII.GetBytes(head.ToString()));
    await ssl.WriteAsync(body);
    await ssl.FlushAsync();
}

// A failure here is logged and never keeps the response from the game. Once sealed, nothing more is kept.
async Task Capture(string path, byte[] body)
{
    if (Volatile.Read(ref captured) == 1) return;
    try
    {
        if (path.Equals(LoginPath, StringComparison.OrdinalIgnoreCase)) await ReadAccount(body);
        else if (path.Equals(ProgressionPath, StringComparison.OrdinalIgnoreCase)) SaveProgression(body);
        else if (path.Equals(InventoryPath, StringComparison.OrdinalIgnoreCase)) SaveInventory(body);
    }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException or HttpRequestException or TaskCanceledException)
    {
        Log("capture failed: " + error.GetType().Name + " " + error.Message);
    }
}

// The login's session reads what the game reads next: progression first, since the capture ends with the inventory.
async Task ReadAccount(byte[] loginResponse)
{
    string? session;
    try
    {
        using var login = JsonDocument.Parse(loginResponse);
        session = login.RootElement.TryGetProperty("sessionId", out var id) ? id.GetString()
            : login.RootElement.TryGetProperty("sessionToken", out var token) ? token.GetString() : null;
    }
    catch (JsonException)
    {
        Log("login response is not JSON");
        return;
    }
    if (string.IsNullOrEmpty(session)) { Log("login response has no session"); return; }
    if (progressionPath != null && await Get(ProgressionPath, session) is { } progression) SaveProgression(progression);
    if (await Get(InventoryPath, session) is { } inventory) SaveInventory(inventory);
}

async Task<byte[]?> Get(string path, string session)
{
    using var request = new HttpRequestMessage(HttpMethod.Get, $"https://{host}{path}");
    request.Headers.TryAddWithoutValidation("Authorization", "session " + session);
    foreach (var (name, value) in CaptureHeaders.ForRead(loginHeaders, DateTimeOffset.UtcNow))
        request.Headers.TryAddWithoutValidation(name, value);
    var asked = Stopwatch.StartNew();
    using var response = await http.SendAsync(request);
    var body = await response.Content.ReadAsByteArrayAsync();
    Log($"GET {path} {(int)response.StatusCode} {body.Length} bytes (login session)");
    Log($"capture GET {path}: sent headers {CaptureLog.Names(request.Headers.Select(header => header.Key))}");
    LogResponse("capture", path, response, body, asked);
    return response.IsSuccessStatusCode ? body : null;
}

void SaveProgression(byte[] body)
{
    if (progressionPath == null) return;
    if (!IsJsonObject(body, null)) { Log("progression response is not an object"); return; }
    WriteAtomically(progressionPath, body);
    Log("progression saved");
}

void SaveInventory(byte[] body)
{
    if (!IsJsonObject(body, "items")) { Log("inventory response has no items list"); return; }
    if (Interlocked.CompareExchange(ref captured, 1, 0) == 1) return;
    try { WriteAtomically(outputPath, body); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
    {
        Volatile.Write(ref captured, 0);  // a later inventory may still be saved
        throw;
    }
    Log("inventory saved");
    CompleteCapture();
}

static bool IsJsonObject(byte[] body, string? requiredArray)
{
    try
    {
        using var document = JsonDocument.Parse(body);
        return document.RootElement.ValueKind == JsonValueKind.Object && (requiredArray == null ||
            document.RootElement.TryGetProperty(requiredArray, out var array) && array.ValueKind == JsonValueKind.Array);
    }
    catch (JsonException) { return false; }
}

static void WriteAtomically(string path, byte[] body)
{
    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
    File.WriteAllBytes(path + ".tmp", body);
    File.Move(path + ".tmp", path, overwrite: true);
}

// "/api/FriendsV2/Recent" stays, "/api/Proxy/<id>/<id>" becomes "/api/Proxy/...": later segments can carry account ids.
static string Shorten(string path)
{
    var segments = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
    return segments.Length <= 3 && path is LoginPath or ProgressionPath or InventoryPath ? path
        : "/" + string.Join('/', segments.Take(2)) + (segments.Length > 2 ? "/..." : "");
}

// One HTTP/1.1 request (no chunked bodies, which the game does not send; bodies up to 1 MiB).
static async Task<(string Method, string Target, Dictionary<string, string> Headers, byte[] Body)?> ReadRequest(SslStream ssl)
{
    var buffer = new MemoryStream();
    var headerEnd = await ReadHead(ssl, buffer);
    if (headerEnd < 0) return null;
    var lines = Encoding.ASCII.GetString(buffer.GetBuffer(), 0, headerEnd).Split("\r\n");
    var first = lines[0].Split(' ');
    var headers = ParseHeaders(lines.Skip(1));
    if (first.Length < 3 || headers.ContainsKey("Transfer-Encoding")) return null;
    if (headers.TryGetValue("Expect", out var expect) && expect.Contains("100-continue", StringComparison.OrdinalIgnoreCase))
        await ssl.WriteAsync("HTTP/1.1 100 Continue\r\n\r\n"u8.ToArray());
    var length = headers.TryGetValue("Content-Length", out var text) && int.TryParse(text, out var parsed) && parsed is >= 0 and <= 1 << 20 ? parsed : 0;
    return (first[0], first[1], headers, await ReadBody(ssl, buffer, headerEnd + 4, length));
}

// Reads until the blank line after the headers; its offset, or -1 when the client stopped or sent 64 KiB without one.
static async Task<int> ReadHead(SslStream ssl, MemoryStream buffer)
{
    var chunk = new byte[8192];
    while (buffer.Length < 65536)
    {
        var n = await ssl.ReadAsync(chunk);
        if (n == 0) return -1;
        buffer.Write(chunk, 0, n);
        var end = buffer.GetBuffer().AsSpan(0, (int)buffer.Length).IndexOf("\r\n\r\n"u8);
        if (end >= 0) return end;
    }
    return -1;
}

static Dictionary<string, string> ParseHeaders(IEnumerable<string> lines)
{
    var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    foreach (var line in lines)
    {
        var colon = line.IndexOf(':');
        if (colon > 0) headers[line[..colon].Trim()] = line[(colon + 1)..].Trim();
    }
    return headers;
}

// The body: what arrived with the headers, then the rest from the stream.
static async Task<byte[]> ReadBody(SslStream ssl, MemoryStream buffer, int start, int length)
{
    var body = new byte[length];
    var read = Math.Min((int)buffer.Length - start, length);
    Array.Copy(buffer.GetBuffer(), start, body, 0, read);
    while (read < length)
    {
        var n = await ssl.ReadAsync(body.AsMemory(read, length - read));
        if (n == 0) break;
        read += n;
    }
    return body;
}

void Log(string message)
{
    var line = DateTime.Now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + " " + message;
    Console.WriteLine(line);
    try { File.AppendAllText(Path.Combine(evidenceDir, "proxy.log"), line + Environment.NewLine); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Console.Error.WriteLine("log write failed: " + error.Message); }
}

// Each cleanup step runs on its own, so a failed one never skips the next.
bool Step(string name, Action action)
{
    try
    {
        action();
        return true;
    }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException)
    {
        Log(name + " failed: " + error.Message);
        return false;
    }
}

static void Run(string file, string arguments)
{
    using var process = Process.Start(new ProcessStartInfo(file, arguments) { UseShellExecute = false, CreateNoWindow = true });
    process?.WaitForExit(8000);
}

void RemoveHosts()
{
    if (!CaptureHosts.Remove(hostsPath, host)) return;
    Run("ipconfig", "/flushdns");
    Log("hosts line removed");
}

void AddHosts()
{
    CaptureHosts.Add(hostsPath, host);
    Run("ipconfig", "/flushdns");
}

// This run's CA by thumbprint, or with null every CA this tool installed.
void RemoveCas(string? thumbprint)
{
    foreach (var location in new[] { StoreLocation.LocalMachine, StoreLocation.CurrentUser })
    {
        using var store = new X509Store(StoreName.Root, location);
        store.Open(OpenFlags.ReadWrite);
        foreach (var certificate in store.Certificates)
        {
            if (thumbprint != null ? certificate.Thumbprint == thumbprint : certificate.Subject.Contains("CN=" + CaName, StringComparison.Ordinal))
            {
                store.Remove(certificate);
                Log("CA removed from " + location);
            }
            certificate.Dispose();
        }
    }
}

static void StartCrlServer(TcpListener listener, byte[] crl) => _ = Task.Run(async () =>
{
    var head = Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: application/pkix-crl\r\nContent-Length: {crl.Length}\r\nConnection: close\r\n\r\n");
    while (true)
    {
        TcpClient client;
        try { client = await listener.AcceptTcpClientAsync(); }
        catch (Exception error) when (error is SocketException or ObjectDisposedException) { return; }
        _ = Task.Run(async () =>
        {
            using var connection = client;
            try
            {
                var stream = client.GetStream();
                if (await stream.ReadAsync(new byte[2048]) == 0) return;
                await stream.WriteAsync(head);
                await stream.WriteAsync(crl);
            }
            catch (IOException) { } // the client hung up; it asks again on its next handshake
        });
    }
});

static byte[] Tlv(byte tag, byte[] content)
{
    var length = content.Length;
    byte[] prefix = length < 0x80 ? [tag, (byte)length] : length <= 0xFF ? [tag, 0x81, (byte)length] : [tag, 0x82, (byte)(length >> 8), (byte)length];
    return [.. prefix, .. content];
}

// CRLDistributionPoints: SEQUENCE { DistributionPoint { [0] { [0] { [6] uri } } } }
static byte[] EncodeCdp(string url) => Tlv(0x30, Tlv(0x30, Tlv(0xA0, Tlv(0xA0, Tlv(0x86, Encoding.ASCII.GetBytes(url))))));

static X509Certificate2 MakeLeaf(X509Certificate2? issuer, string name, byte[]? cdp)
{
    using var key = RSA.Create(2048);
    var request = new CertificateRequest("CN=" + name, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    var san = new SubjectAlternativeNameBuilder();
    san.AddDnsName(name);
    request.CertificateExtensions.Add(san.Build());
    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
    request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));
    if (cdp != null) request.CertificateExtensions.Add(new X509Extension("2.5.29.31", cdp, false));
    var notBefore = DateTimeOffset.UtcNow.AddDays(-1);
    var notAfter = DateTimeOffset.UtcNow.AddDays(1);
    // Never past the CA's own end: made a moment after the CA (installing it can take over a second), "a day from now"
    // was later than the CA's, and the request threw (2026-10-26: the capture crashed with the CA left trusted).
    if (issuer != null && notAfter > issuer.NotAfter) notAfter = issuer.NotAfter;
    using var certificate = issuer == null ? request.CreateSelfSigned(notBefore, notAfter)
        : request.Create(issuer, notBefore, notAfter, RandomNumberGenerator.GetBytes(8)).CopyWithPrivateKey(key);
    // SslStream needs the key in a form Schannel can use; a PKCS#12 round trip gives it one.
    return X509CertificateLoader.LoadPkcs12(certificate.Export(X509ContentType.Pfx), null, X509KeyStorageFlags.Exportable);
}
