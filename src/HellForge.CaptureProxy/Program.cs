// Sync Inventory's capture: reads the player's own HELLDIVERS 2 inventory and progression from the live API while the
// normal game logs in (HellForge.Core InventorySync starts it from the launcher). The Windows build is elevated and owns
// the hosts file and LocalMachine\Root. The native Linux build runs unprivileged: InventorySync starts it below the
// installed capture helper, which owns the marked /etc/hosts lines and the 127.0.0.1:443 relay, and puts the exported
// public CA in HELLDIVERS 2's Proton prefix through Wine.
//
// It maps the API host to 127.0.0.1, trusts a per-run CA (with a CRL served on loopback, since Schannel checks
// revocation), terminates the game's TLS and forwards every request unchanged to the real API by IP. It is passive: it
// never sends a request of its own. After the login it keeps the API's answers to the game's own GET /api/Progression and
// GET /api/Progression/inventory (CaptureCompletion) and writes them to --progression and --output, and with --catalogue
// the game catalogues it reads (CaptureCompletion.Catalogues) to that folder, unsealed. With --sign and --steam-account it
// signs the two account files for that Steam account (SnapshotSeal, SnapshotSigner: elevated or the Linux helper's service
// only) and says so in its state file (CaptureState), so the launcher takes them; the backend unlocks premium items only from a
// seal. With --close-game it then closes the game the sync opened (CaptureGameEnd), before the game gets the inventory's
// answer. It keeps forwarding until the game exits, removes the hosts line and the CA, and exits 0 (1 when nothing was
// captured before the timeout or the seal failed). Nothing else is kept. The log (proxy.log) is detailed but holds no values: for every
// connection its TLS version, cipher and SNI, timings and where it failed; for every request and response the method, the
// first two path segments, status, sizes, content types and the names of headers and query parameters; a refused
// request's reason (CaptureLog); the key-and-count shape of what the capture keeps (CaptureDiagnostics); the game's and
// Easy Anti-Cheat's processes; and a summary. Never bodies, header or query values, or the session.
//
// A run that cannot get as far as listening ends with a CaptureStart exit code (20 and up) and writes capture-failure.json
// beside proxy.log with what was in its way (StartProblems), for the launcher to tell the player.
//
//   --close-game   end the sync's game once the capture is sealed (the launcher's sync passes it)
//   --cleanup      only remove a hosts line and CA left by a run that was killed
//   --ca-cert <file>  write the public CA for the Linux supervisor to put in the game prefix
//   --no-hosts --no-ca --listen-port 8443 --real-ip <ip>   local plumbing test, no system changes

using HellForge.CaptureProxy;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using HellForge.Core;

const string CaName = "HellForge Capture CA";
// The redirect stays while the game runs (ForwardUntilGameExits), but never past the certificates' day.
const int ForwardHoursAtMost = 12;
var hostsPath = CapturePlatform.HostsPath;

var host = "api.live.prod.thehelldiversgame.com";
var listenPort = 443;
List<IPAddress>? fixedUpstream = null;
var outputPath = Path.Combine(AppContext.BaseDirectory, "real-inventory.json");
string? progressionPath = null;
string? catalogueDir = null;
var evidenceDir = Path.Combine(AppContext.BaseDirectory, "capture");
bool manageHosts = true, manageCa = true, cleanupOnly = false, closeGame = false;
var startedUtc = DateTime.UtcNow;
// Set once this run's hosts line is in, until cleanup removes it: a line already gone at cleanup was removed by something else.
var hostsAdded = false;
var timeoutMin = 10;
var sign = false;
ulong? steamAccount = null;
// The launcher's Windows account, granted the sync folder beside this one's (it differs on a standard account).
string? grantSid = null;
string? caCertificatePath = null;

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
            case "--catalogue": catalogueDir = Next(); break;
            case "--evidence": evidenceDir = Next(); break;
            case "--timeout-min": timeoutMin = Math.Max(1, int.Parse(Next(), CultureInfo.InvariantCulture)); break;
            case "--no-hosts": manageHosts = false; break;
            case "--no-ca": manageCa = false; break;
            case "--cleanup": cleanupOnly = true; break;
            case "--close-game": closeGame = true; break;
            case "--sign": sign = true; break;
            case "--shared-evidence": CapturePlatform.ShareEvidence = true; break;
            case "--steam-account": steamAccount = ulong.Parse(Next(), NumberStyles.None, CultureInfo.InvariantCulture); break;
            case "--ca-cert": caCertificatePath = Next(); break;
            case "--grant-sid":
                grantSid = Next();
                if (!CaptureAccess.IsSid(grantSid)) throw new ArgumentException("--grant-sid needs a SID (S-1-5-...).");
                break;
            case "--help":
                Console.WriteLine("HellForge.CaptureProxy [--output <file>] [--progression <file>] [--catalogue <dir>] [--evidence <dir>] [--timeout-min <n>]");
                Console.WriteLine("  [--sign --steam-account <SteamID64>] [--shared-evidence] [--grant-sid <launcher's SID>]");
                Console.WriteLine("  [--host <name>] [--listen-port <n>] [--real-ip <ip>]");
                Console.WriteLine("  [--ca-cert <file>] [--close-game] [--no-hosts] [--no-ca] | --cleanup");
                return 0;
            default: throw new ArgumentException("Unknown argument: " + args[i]);
        }
    }
    if (sign != (steamAccount != null)) throw new ArgumentException("--sign and --steam-account go together.");
    // The pins (CaptureUpstreamPins) are Let's Encrypt's roots: they prove the API only together with its name.
    if (sign && !host.Equals(CaptureUpstreamPins.ApiHost, StringComparison.OrdinalIgnoreCase))
        throw new ArgumentException("A signing capture reads only " + CaptureUpstreamPins.ApiHost + ".");
    if (manageCa && !CapturePlatform.ManagesSystemTrustStore && caCertificatePath == null && !cleanupOnly)
        throw new ArgumentException("On Linux --ca-cert is required when the capture makes its CA.");
    // The seal covers the snapshot's own file names, side by side.
    if (sign && (Path.GetFileName(outputPath) != SnapshotSeal.InventoryFile ||
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
    try { Directory.CreateDirectory(evidenceDir); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Console.Error.WriteLine("Cannot use " + evidenceDir + ": " + error.Message); }
    // A capture still running would remove or re-add its lines as it ends.
    var removed = StartProblems.StopOtherCaptures(Log) == null;
    removed = Step("hosts cleanup", RemoveHosts) & Step("CA cleanup", () => RemoveCas(null)) & removed;
    return removed ? 0 : 1;
}

try { Directory.CreateDirectory(evidenceDir); }
catch (Exception error) when (error is IOException or UnauthorizedAccessException)
{
    Console.Error.WriteLine("Cannot use " + evidenceDir + ": " + error.Message);
    return CaptureStart.ExitCode(CaptureProblem.EvidenceFolder);
}
var statePath = Path.Combine(evidenceDir, CaptureState.FileName);
// The sync folder stays private to this account and the launcher's (0700 on Linux, where both are the same user). On a
// standard Windows account the UAC prompt runs this capture as the administrator who answered it: granting only this
// account hid the folder from the launcher, which then never opened the game and found no inventory after a capture
// that worked.
if (OperatingSystem.IsWindows())
{
    string captureSid;
    using (var identity = System.Security.Principal.WindowsIdentity.GetCurrent()) captureSid = identity.User!.Value;
    var aclCode = Run("icacls", CaptureAccess.SyncFolderAclArguments(evidenceDir, captureSid, grantSid));
    Log($"sync folder private to {(grantSid == null ? "this account only (no launcher account given)" : string.Equals(grantSid, captureSid, StringComparison.OrdinalIgnoreCase) ? "this account, which is the launcher's" : "this account and the launcher's, another account")}" +
        (aclCode == 0 ? "" : $": icacls failed with code {aclCode}"));
}
else CapturePlatform.RestrictEvidenceDirectory(evidenceDir);

// What a crashed or abandoned earlier sync left goes first, so a restart of the PC is never what clears it: its capture
// (still holding port 443, and removing the hosts line when it times out), then its hosts line (below) and its CA.
if (StartProblems.StopOtherCaptures(Log) is { } stillRunning) return Fail(CaptureProblem.LeftoverCapture, stillRunning);
// Resolve the real host before the hosts line points it at loopback; clear a line an earlier killed run left first.
if (manageHosts && HostsStep("hosts cleanup", RemoveHosts) is { } cleanupFailure) return Fail(cleanupFailure.Problem, cleanupFailure.Detail);
List<IPAddress> upstreamIps;
try
{
    upstreamIps = fixedUpstream ?? [.. Dns.GetHostAddresses(host).Where(address => address.AddressFamily == AddressFamily.InterNetwork)];
}
catch (SocketException error)
{
    Log("could not resolve " + host + ": " + error.SocketErrorCode);
    return Fail(CaptureProblem.Offline, host + " does not resolve: " + error.SocketErrorCode);
}
if (upstreamIps.Count == 0) { Log("no IPv4 address for " + host); return Fail(CaptureProblem.Offline, "no IPv4 address for " + host); }
Log("upstream " + string.Join(", ", upstreamIps) + ":443");

// Loopback only: the hosts line points the API host at 127.0.0.1, and nothing else on the network may log in through us.
// Bound before the certificate and the hosts line: when another program holds the port, neither is touched.
var listener = new TcpListener(IPAddress.Loopback, listenPort);
if (StartListener() is { } portFailure) return Fail(portFailure.Problem, portFailure.Detail);

string? caThumbprint = null;
X509Certificate2 serverCert;
// A crash from here on would leave the hosts line and the CA trusted: an exception nothing catches is logged with its
// cause and they are removed before the process ends (the cleanup below runs only once the listener exists).
AppDomain.CurrentDomain.UnhandledException += (_, e) =>
{
    Log("crashed: " + (e.ExceptionObject is Exception error ? CaptureDiagnostics.Chain(error) : e.ExceptionObject));
    if (manageHosts) Step("hosts cleanup", RemoveHosts);
    if (manageCa && CapturePlatform.ManagesSystemTrustStore) Step("CA cleanup", () => RemoveCas(null));
};
// Making the certificates can fail as well as installing the CA (a broken crypto provider; Wine's, 2026-10-30): either
// stops the run as a certificate problem, with every HellForge CA removed.
try
{
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
        caThumbprint = ca.Thumbprint;
        if (CapturePlatform.ManagesSystemTrustStore)
        {
            if (!Step("CA cleanup", () => RemoveCas(null))) return Fail(CaptureProblem.Certificate, "an earlier sync's certificate could not be removed");
            try
            {
                using var store = new X509Store(StoreName.Root, StoreLocation.LocalMachine);
                store.Open(OpenFlags.ReadWrite);
                using var publicCa = X509CertificateLoader.LoadCertificate(ca.Export(X509ContentType.Cert));
                store.Add(publicCa);
            }
            catch (Exception error) when (error is CryptographicException or UnauthorizedAccessException)
            {
                // A security program guarding the trusted root store refuses the add.
                throw new CryptographicException("Windows refused to add the certificate: " + error.Message, error);
            }
            Log("CA installed");
        }
        else
        {
            // Native Linux cannot write the game's Wine prefix store without Wine; the supervisor imports this public
            // certificate as the system store's serialized Blob with reg.exe and proves the value before the game opens.
            WriteCaCertificate(ca);
            Log("CA written for the game prefix");
        }
        serverCert = MakeLeaf(ca, host, cdp);
        StartCrlServer(crlListener, new CertificateRevocationListBuilder().Build(ca, BigInteger.One, new DateTimeOffset(ca.NotAfter),
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1, DateTimeOffset.UtcNow.AddDays(-1)));
    }
    else serverCert = MakeLeaf(null, host, null);
}
catch (CryptographicException error)
{
    Log("certificate failed: " + CaptureDiagnostics.Chain(error));
    if (manageCa && CapturePlatform.ManagesSystemTrustStore) Step("CA cleanup", () => RemoveCas(null));
    listener.Stop();
    return Fail(CaptureProblem.Certificate, error.Message);
}

var cleaned = 0;
void Cleanup()
{
    if (Interlocked.Exchange(ref cleaned, 1) == 1) return;
    listener.Stop();
    if (manageHosts) Step("hosts cleanup", RemoveHosts);
    if (caThumbprint != null && CapturePlatform.ManagesSystemTrustStore) Step("CA cleanup", () => RemoveCas(caThumbprint));
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
    // A signing capture keeps only what the API itself answered (CaptureUpstreamPins).
    SslOptions = new SslClientAuthenticationOptions
    {
        RemoteCertificateValidationCallback = (_, _, chain, errors) =>
        {
            if (!sign) return errors == SslPolicyErrors.None;
            if (CaptureUpstreamPins.Refusal(chain, errors) is not { } refusal) return true;
            Log("upstream refused: " + refusal);
            return false;
        }
    },
    AllowAutoRedirect = false,
    UseCookies = false,
    AutomaticDecompression = DecompressionMethods.All,
    ConnectTimeout = TimeSpan.FromSeconds(15)
};
using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };

var captured = 0;
var sealOk = false;
ECDsa? signer = null;
string? inventorySha256 = null, progressionSha256 = null;
DateTimeOffset? inventoryDate = null;
// What of the game's own traffic is kept, and when the capture is complete (passive: the capture sends nothing itself).
var completion = new CaptureCompletion(progressionWanted: progressionPath != null, catalogueWanted: catalogueDir != null);
var connections = 0;
var tally = new CaptureTally();
var lastRequest = Stopwatch.StartNew();
// Cancelled when nothing was captured in time, and after a capture once the game exits (ForwardUntilGameExits).
using var stop = new CancellationTokenSource(TimeSpan.FromMinutes(timeoutMin));
try
{
    LogStart();
    if (sign)
    {
        try
        {
            signer = SnapshotSigner.Open(Environment.GetEnvironmentVariable("STATE_DIRECTORY"), evidenceDir, out var where);
            Log("snapshot signer: " + where);
        }
        catch (Exception error) when (error is CryptographicException or IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return Fail(CaptureProblem.Signer, error.Message);
        }
    }
    if (manageHosts && HostsStep("hosts line", AddHosts) is { } hostsFailure) return Fail(hostsFailure.Problem, hostsFailure.Detail);
    LogResolution();
    Log($"listening on port {listenPort}");
    CaptureState.Write(evidenceDir, Environment.ProcessId, sealedCapture: false, progression: false);
    CapturePlatform.RestrictEvidence(statePath);
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

// Stops before listening: says why in proxy.log and capture-failure.json, for the launcher (CaptureStart).
int Fail(CaptureProblem problem, string? detail)
{
    Log($"could not start: {problem}" + (detail != null ? ": " + detail : ""));
    // A hosts failure's reason is already in hosts-failure.txt (HostsWrite): one message per failure.
    if (problem is not (CaptureProblem.HostsBusy or CaptureProblem.HostsDenied)) Step("failure", () => CaptureStart.Write(evidenceDir, problem, detail));
    return CaptureStart.ExitCode(problem);
}

CaptureFailure? StartListener()
{
    try
    {
        listener.Start();
        return null;
    }
    catch (SocketException error)
    {
        Log($"port {listenPort} refused: {CaptureDiagnostics.Chain(error)}");
        if (error.SocketErrorCode != SocketError.AddressAlreadyInUse)
            return new CaptureFailure(CaptureProblem.PortDenied, $"{error.Message} ({(int)error.SocketErrorCode})");
        var owner = StartProblems.PortOwner(listenPort);
        if (owner is { } pid && StartProblems.Describe(pid).StartsWith(StartProblems.ProcessName + ".exe", StringComparison.OrdinalIgnoreCase))
            return new CaptureFailure(CaptureProblem.LeftoverCapture, "it still holds port " + listenPort);
        return new CaptureFailure(CaptureProblem.PortInUse, owner is { } holder ? "used by " + StartProblems.Describe(holder) : null);
    }
}

// A hosts change before listening (CaptureHosts retries a locked file and clears read-only; HostsWrite names who holds it
// and writes the reason to hosts-failure.txt). Null when it worked; otherwise that same reason, as this run's start failure.
CaptureFailure? HostsStep(string name, Action change)
{
    try
    {
        change();
        return null;
    }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
    {
        Log(name + " failed: " + error.Message);
        var failure = error as HostsFileException ?? error.InnerException as HostsFileException;
        return new CaptureFailure(failure?.Failure == HostsFailure.InUse ? CaptureProblem.HostsBusy : CaptureProblem.HostsDenied, error.Message);
    }
}

// What this run is, so a log from another PC or build can be read on its own.
void LogStart()
{
    var exe = Environment.ProcessPath;
    var built = exe != null ? File.GetLastWriteTimeUtc(exe).ToString("u", CultureInfo.InvariantCulture) : "unknown";
    Log($"capture built {built}, {Environment.OSVersion.VersionString}, .NET {Environment.Version}, elevated {(CapturePlatform.IsElevated ? "yes" : "no")}");
    Log($"options: host {host}, port {listenPort}, timeout {timeoutMin} min, hosts line {(manageHosts ? "yes" : "no")}, " +
        $"CA {(manageCa ? "yes" : "no")}, close game {(closeGame ? "yes" : "no")}, progression {(progressionPath != null ? "yes" : "no")}, sign {(sign ? "yes" : "no")}");
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
                $"game {(seen.Values.Any(IsGameName) ? "running" : "not running")}, captured {(Volatile.Read(ref captured) == 1 ? "yes" : "no")}");
        }
        try { await Task.Delay(TimeSpan.FromSeconds(2), stop.Token); }
        catch (OperationCanceledException) { return; }
    }
}

void LogProcessChanges(Dictionary<int, string> seen)
{
    var now = new Dictionary<int, string>();
    foreach (var name in CapturePlatform.ProcessNames)
        foreach (var process in Process.GetProcessesByName(name))
        {
            if (!now.ContainsKey(process.Id)) now[process.Id] = name;
            process.Dispose();
        }
    foreach (var (pid, name) in now.Where(entry => !seen.ContainsKey(entry.Key))) Log($"process {name} started (pid {pid})");
    foreach (var (pid, name) in seen.Where(entry => !now.ContainsKey(entry.Key))) Log($"process {name} exited (pid {pid})");
    seen.Clear();
    foreach (var entry in now) seen[entry.Key] = entry.Value;
}

// Once the capture is complete (CaptureCompletion): seal what was captured and tell the launcher, then (--close-game) end
// the sync's game, and keep forwarding until it exits. In this order: the files are sealed and the state says so before
// the game is ended, so the launcher always finds a finished capture; the game is ended before this returns, normally
// while its inventory read is still waiting for the answer, so it never reaches Easy Anti-Cheat's "Could not connect"
// (CaptureGameEnd).
void CompleteCapture()
{
    var progression = progressionPath != null && File.Exists(progressionPath);
    sealOk = !sign || Step("seal", Seal);
    if (sign && sealOk)
    {
        Log("capture sealed" + (progression ? "" : " without progression"));
        CapturePlatform.RestrictFile(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outputPath))!, SnapshotSeal.FileName));
    }
    Step("state", () => CaptureState.Write(evidenceDir, Environment.ProcessId, sealOk, progression));
    CapturePlatform.RestrictEvidence(statePath);
    if (closeGame) EndSyncGame();
    _ = Task.Run(ForwardUntilGameExits);
}

// Signs the hashes of what the API answered, taken as it arrived, and the API's own time of the inventory reply.
void Seal()
{
    if (signer == null || inventorySha256 == null) throw new IOException("there is no signer or no inventory to seal");
    if (inventoryDate is not { } date) throw new IOException("the API's inventory reply had no Date header");
    SnapshotSeal.Write(Path.GetDirectoryName(Path.GetFullPath(outputPath))!, signer, steamAccount!.Value, date.UtcDateTime, inventorySha256, progressionSha256);
}

void EndSyncGame() => Log("close game: " + CaptureGameEnd.Describe(ProcessGame.EndSyncGame(startedUtc, Log)));

// The redirect and the CA stay until the game exits, so its requests after the login still reach the API. Easy Anti-Cheat
// closed the game ("Could not connect to Easy Anti Cheat network") both when the redirect ended right after the login and
// when it stayed (live runs, 2026-10-26), so that error has another cause; with --close-game the game is ended before it
// shows, and WatchProcesses and the per-connection lines are there to find the cause.
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
    var running = CapturePlatform.FindProcesses(CapturePlatform.ProcessNames.Where(IsGameName));
    foreach (var process in running) process.Dispose();
    return running.Count > 0;
}

static bool IsGameName(string name) =>
    name.Equals("helldivers2", StringComparison.OrdinalIgnoreCase) ||
    name.Equals("helldivers2.exe", StringComparison.OrdinalIgnoreCase);

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
        Capture(CaptureCompletion.Classify(request.Method, path), path, response.IsSuccessStatusCode, responseBody, response.Headers.Date);
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
    path.Equals(CaptureCompletion.LoginPath, StringComparison.OrdinalIgnoreCase) || path.Equals(CaptureCompletion.ProgressionPath, StringComparison.OrdinalIgnoreCase) ||
    path.Equals(CaptureCompletion.InventoryPath, StringComparison.OrdinalIgnoreCase);

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

// The capture is passive: it keeps the API's answers to the game's own reads after the login (CaptureCompletion) and
// sends nothing itself. Runs before the game gets the answer, so --close-game can end the game before the inventory
// reaches it. A failure here is logged and never keeps the response from the game. Once complete, nothing more is kept.
void Capture(CaptureCompletion.Kind kind, string path, bool success, byte[] body, DateTimeOffset? date)
{
    if (kind == CaptureCompletion.Kind.Login)
    {
        if (!success) return;
        completion.Login();
        Log("login answered: the game's own reads that follow are kept");
        return;
    }
    if (kind == CaptureCompletion.Kind.Other) return;
    var what = kind switch
    {
        CaptureCompletion.Kind.Progression => "progression",
        CaptureCompletion.Kind.Inventory => "inventory",
        _ => "catalogue " + CaptureCompletion.CatalogueFile(path)
    };
    if (completion.Refusal(kind, success, path) is { } refusal) { Log($"{what} not kept: {refusal}"); return; }
    try
    {
        var saved = kind switch
        {
            CaptureCompletion.Kind.Progression => SaveProgression(body),
            CaptureCompletion.Kind.Inventory => SaveInventory(body, date),
            _ => SaveCatalogue(CaptureCompletion.CatalogueFile(path)!, body)
        };
        if (saved) completion.Saved(kind, DateTime.UtcNow, path);
    }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
    {
        Log("capture failed: " + error.GetType().Name + " " + error.Message);
        return;
    }
    if (kind == CaptureCompletion.Kind.Inventory && completion.HasInventory && completion.Missing() is { Count: > 0 } missing)
    {
        Log($"inventory in; waiting up to {CaptureCompletion.ResponseWait.TotalSeconds:F0} s for {string.Join(", ", missing)}");
        _ = Task.Run(async () =>
        {
            await Task.Delay(CaptureCompletion.ResponseWait);
            if (completion.Missing() is { Count: > 0 } late && !completion.Completed) Log("completing without " + string.Join(", ", late));
            TryCompleteCapture();
        });
    }
    TryCompleteCapture();
}

void TryCompleteCapture()
{
    if (!completion.TryComplete(DateTime.UtcNow)) return;
    Volatile.Write(ref captured, 1);
    CompleteCapture();
}

bool SaveProgression(byte[] body)
{
    if (progressionPath == null) return false;
    if (!IsJsonObject(body, null)) { Log("progression response is not an object"); return false; }
    WriteAtomically(progressionPath, body);
    progressionSha256 = SnapshotSeal.Sha256(body);
    Log("progression saved");
    return true;
}

// A catalogue the game read (CaptureCompletion.Catalogues): the API's answer as it came, a JSON list.
bool SaveCatalogue(string file, byte[] body)
{
    if (catalogueDir == null) return false;
    if (!IsJsonArray(body)) { Log($"catalogue {file} is not a list"); return false; }
    WriteAtomically(Path.Combine(catalogueDir, file), body);
    Log($"catalogue {file} saved");
    return true;
}

static bool IsJsonArray(byte[] body)
{
    try
    {
        using var document = JsonDocument.Parse(body);
        return document.RootElement.ValueKind == JsonValueKind.Array;
    }
    catch (JsonException) { return false; }
}

bool SaveInventory(byte[] body, DateTimeOffset? date)
{
    if (!IsJsonObject(body, "items")) { Log("inventory response has no items list"); return false; }
    WriteAtomically(outputPath, body);
    (inventorySha256, inventoryDate) = (SnapshotSeal.Sha256(body), date);
    Log("inventory saved");
    return true;
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
    CapturePlatform.RestrictFile(path + ".tmp");
    File.Move(path + ".tmp", path, overwrite: true);
    CapturePlatform.RestrictFile(path);
}

// "/api/FriendsV2/Recent" stays, "/api/Proxy/<id>/<id>" becomes "/api/Proxy/...": later segments can carry account ids.
static string Shorten(string path)
{
    var segments = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
    return segments.Length <= 3 && (path is CaptureCompletion.LoginPath or CaptureCompletion.ProgressionPath or CaptureCompletion.InventoryPath ||
        CaptureCompletion.CatalogueFile(path) != null) ? path
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
    var path = Path.Combine(evidenceDir, "proxy.log");
    try
    {
        File.AppendAllText(path, line + Environment.NewLine);
        CapturePlatform.RestrictEvidence(path);
    }
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

// The exit code, or -1 when it did not start or finish in time.
static int Run(string file, string arguments)
{
    using var process = Process.Start(new ProcessStartInfo(file, arguments) { UseShellExecute = false, CreateNoWindow = true });
    return process != null && process.WaitForExit(8000) ? process.ExitCode : -1;
}

void RemoveHosts()
{
    if (!HostsWrite(() => CaptureHosts.Remove(hostsPath, host)))
    {
        if (hostsAdded) HostsGone();
        return;
    }
    hostsAdded = false;
    CapturePlatform.FlushDns();
    Log("hosts line removed");
}

void AddHosts()
{
    LogHostsFile();
    HostsWrite(() => { CaptureHosts.Add(hostsPath, host); return true; });
    hostsAdded = true;
    CapturePlatform.FlushDns();
    Log("hosts line added");
}

// The public half only: the supervisor imports this as the prefix Root certificate's Blob with reg.exe. The private key
// stays in this process.
void WriteCaCertificate(X509Certificate2 ca)
{
    var path = Path.GetFullPath(caCertificatePath!);
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllBytes(path, ca.Export(X509ContentType.Cert));
    CapturePlatform.RestrictEvidence(path);
}

// A failed hosts write: the plain reason, named more exactly where Windows can say who, goes to the log and to the sync
// folder for the launcher's message (CaptureHosts.FailureFile). Rethrown as an IOException with that reason for Step.
bool HostsWrite(Func<bool> write)
{
    try
    {
        return write();
    }
    catch (HostsFileException error)
    {
        Log($"hosts file {error.Failure}: {CaptureDiagnostics.Chain(error.InnerException ?? error)}");
        var reason = error.Failure switch
        {
            HostsFailure.InUse when HostsLockers() is { Count: > 0 } lockers =>
                $"{string.Join(", ", lockers)} kept the hosts file ({hostsPath}) open, so the sync could not change it. Close it, then sync again.",
            HostsFailure.Blocked or HostsFailure.Reverted or HostsFailure.ReadOnly when DefenderFlaggedHosts() => DefenderReason("blocked the sync's change to"),
            _ => error.Message
        };
        Step("hosts failure note", () => CaptureHosts.WriteFailure(evidenceDir, reason));
        throw new IOException(reason, error);
    }
}

string DefenderReason(string what) =>
    $"Microsoft Defender {what} the hosts file ({hostsPath}) as SettingsModifier:Win32/HostsFileHijack. The sync needs one line " +
    "there pointing the game's API at this PC while it reads your inventory, and removes it afterwards.";

// The capture's line vanished before cleanup: something other than the capture removed it while the game was meant to use it.
void HostsGone()
{
    hostsAdded = false;
    Log("hosts line was already gone at cleanup: something else removed it");
    var reason = DefenderFlaggedHosts() ? DefenderReason("removed the sync's line from")
        : HostsFileException.Describe(HostsFailure.Reverted, hostsPath);
    Step("hosts failure note", () => CaptureHosts.WriteFailure(evidenceDir, reason));
}

// The hosts file's attributes before the capture changes it (read-only, system, hidden), for a log from another PC.
void LogHostsFile()
{
    try { Log(File.Exists(hostsPath) ? $"hosts file attributes: {File.GetAttributes(hostsPath)}" : "hosts file missing: the capture creates it and deletes it at cleanup"); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Log("hosts file attributes unreadable: " + error.Message); }
}

// Whether Microsoft Defender's threat history holds its hosts-file detection (asked only after a hosts write failed).
bool DefenderFlaggedHosts()
{
    try
    {
        using var process = Process.Start(new ProcessStartInfo("powershell.exe",
            "-NoProfile -NonInteractive -Command \"if (Get-MpThreat -ErrorAction SilentlyContinue | Where-Object ThreatName -like '*HostsFileHijack*') { exit 3 } else { exit 0 }\"")
            { UseShellExecute = false, CreateNoWindow = true });
        if (process == null || !process.WaitForExit(15000)) return false;
        var flagged = process.ExitCode == 3;
        Log("Microsoft Defender's history " + (flagged ? "holds" : "does not hold") + " HostsFileHijack");
        return flagged;
    }
    catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
    {
        Log("Microsoft Defender's history not read: " + error.Message);
        return false;
    }
}

// The programs holding the hosts file open, by name (Windows Restart Manager).
List<string> HostsLockers()
{
    var names = new List<string>();
    try
    {
        if (RestartManager.RmStartSession(out var session, 0, Guid.NewGuid().ToString("N")) != 0) return names;
        try
        {
            if (RestartManager.RmRegisterResources(session, 1, [hostsPath], 0, null, 0, null) == 0)
            {
                uint count = 0;
                // ERROR_MORE_DATA with the count needed, then the list.
                if (RestartManager.RmGetList(session, out var needed, ref count, null, out _) == 234 && needed > 0)
                {
                    var processes = new RestartManager.ProcessInfo[needed];
                    count = needed;
                    if (RestartManager.RmGetList(session, out _, ref count, processes, out _) == 0)
                        names.AddRange(processes.Take((int)count).Select(process => $"{process.AppName} (pid {process.Process.ProcessId})"));
                }
            }
        }
        finally { RestartManager.RmEndSession(session); }
    }
    catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { }
    Log("hosts file held open by: " + (names.Count > 0 ? string.Join(", ", names) : "not known"));
    return names;
}

// This run's CA by thumbprint, or with null every CA this tool installed.
void RemoveCas(string? thumbprint)
{
    if (!CapturePlatform.ManagesSystemTrustStore) return;
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

// Windows Restart Manager: which programs hold a file open, to name the one blocking a hosts write.
static class RestartManager
{
    [StructLayout(LayoutKind.Sequential)]
    public struct UniqueProcess
    {
        public int ProcessId;
        public System.Runtime.InteropServices.ComTypes.FILETIME StartTime;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct ProcessInfo
    {
        public UniqueProcess Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string AppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string ServiceShortName;
        public int ApplicationType;
        public uint AppStatus;
        public uint TSSessionId;
        [MarshalAs(UnmanagedType.Bool)] public bool Restartable;
    }

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmStartSession(out uint session, int flags, string key);

    [DllImport("rstrtmgr.dll")]
    public static extern int RmEndSession(uint session);

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmRegisterResources(uint session, uint files, string[] names, uint applications, UniqueProcess[]? processes,
        uint services, string[]? serviceNames);

    [DllImport("rstrtmgr.dll")]
    public static extern int RmGetList(uint session, out uint needed, ref uint count, [In, Out] ProcessInfo[]? processes, out uint rebootReasons);
}
