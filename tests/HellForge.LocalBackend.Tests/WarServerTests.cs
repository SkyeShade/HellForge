using System.Buffers.Binary;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using HellForge.Core;
using HellForge.LocalBackend;

// A war server (--war server) against a fake one: the origin rule, the hello, what is forwarded and what stays here, the
// player's headers, failures, and the CA check.
static class WarServerTests
{
    sealed record Seen(string Method, string PathAndQuery, Dictionary<string, string> Headers, byte[] Body);

    sealed class FakeServer : HttpMessageHandler
    {
        public Func<string, (HttpStatusCode Status, string? Type, string Body)> Answer = _ => (HttpStatusCode.NotFound, null, "");
        public bool Fail;
        public HttpRequestException Failure = new("connection refused (test)");
        public readonly List<Seen> Requests = [];

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content != null ? await request.Content.ReadAsByteArrayAsync(cancellationToken) : [];
            var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var header in request.Headers.Concat(request.Content?.Headers ?? Enumerable.Empty<KeyValuePair<string, IEnumerable<string>>>()))
                headers[header.Key] = string.Join(",", header.Value);
            lock (Requests) Requests.Add(new Seen(request.Method.Method, request.RequestUri!.PathAndQuery, headers, body));
            if (Fail) throw Failure;
            var (status, type, text) = Answer(request.RequestUri!.AbsolutePath);
            var response = new HttpResponseMessage(status) { Content = new ByteArrayContent(Encoding.UTF8.GetBytes(text)) };
            if (type != null) response.Content.Headers.ContentType = MediaTypeHeaderValue.Parse(type);
            return response;
        }
    }

    const string Hello = "{\"protocol\":1,\"name\":\"Test War\"}";

    public static async Task Run(string root, Action<bool, string> check)
    {
        // The origin: https and nothing after the authority, in its canonical form.
        check(WarServerOptions.ParseOrigin("https://war.example") == "https://war.example" &&
            WarServerOptions.ParseOrigin("https://War.Example:8443/") == "https://war.example:8443" &&
            WarServerOptions.ParseOrigin("https://war.example:443") == "https://war.example", "A war server origin was not taken in its canonical form.");
        var refusedOrigins = 0;
        string[] badOrigins = ["http://war.example", "https://war.example/api", "https://war.example?x=1", "https://user@war.example", "https://war.example#top",
            "", " https://war.example", "war.example", "https://war.example//"];
        foreach (var bad in badOrigins)
            try { WarServerOptions.ParseOrigin(bad); } catch (ArgumentException) { refusedOrigins++; }
        check(refusedOrigins == badOrigins.Length, "A war server origin with more than https://host[:port] was accepted.");

        var folder = Directory.CreateDirectory(Path.Combine(root, "war-server")).FullName;
        var tokenFile = Path.Combine(folder, "token.txt");
        File.WriteAllText(tokenFile, "  test-token-42 \r\n");
        var log = new List<string>();

        // The hello: refused unless it is protocol 1 with a name; the token goes with it.
        foreach (var (answer, why) in new (Func<string, (HttpStatusCode, string?, string)>, string)[]
        {
            (_ => (HttpStatusCode.OK, "application/json", "{\"protocol\":2,\"name\":\"Later\"}"), "another protocol"),
            (_ => (HttpStatusCode.OK, "application/json", "{\"protocol\":1}"), "no name"),
            (_ => (HttpStatusCode.OK, "application/json", "{\"protocol\":1,\"name\":\"\"}"), "an empty name"),
            (_ => (HttpStatusCode.OK, "text/html", "<html>"), "not JSON"),
            (_ => (HttpStatusCode.Forbidden, "application/json", Hello), "HTTP 403"),
        })
        {
            var refused = false;
            try { using var _ = await WarServer.ConnectAsync("https://war.test", null, null, log.Add, new FakeServer { Answer = answer }); }
            catch (InvalidDataException) { refused = true; }
            check(refused, "A war server whose hello had " + why + " was accepted.");
        }
        var unreachable = false;
        try { using var _ = await WarServer.ConnectAsync("https://war.test", null, null, log.Add, new FakeServer { Fail = true }); }
        catch (InvalidDataException error) { unreachable = error.Message.Contains("could not be reached", StringComparison.Ordinal); }
        check(unreachable, "An unreachable war server was accepted, or not said so.");
        // In words, not the AOT build's resource keys.
        var untrusted = "";
        try
        {
            using var _ = await WarServer.ConnectAsync("https://war.test", null, null, log.Add,
                new FakeServer { Fail = true, Failure = new HttpRequestException(HttpRequestError.SecureConnectionError, "net_http_ssl_connection_failed") });
        }
        catch (InvalidDataException error) { untrusted = error.Message; }
        check(untrusted.Contains("certificate is not trusted", StringComparison.Ordinal) && !untrusted.Contains("net_http", StringComparison.Ordinal),
            "A refused certificate was not said in words: " + untrusted);
        foreach (var (content, why) in new[] { ("", "an empty token"), ("two words", "a token with a space"), (new string('x', 513), "a token too long") })
        {
            File.WriteAllText(Path.Combine(folder, "bad-token.txt"), content);
            var refused = false;
            try { using var _ = await WarServer.ConnectAsync("https://war.test", null, Path.Combine(folder, "bad-token.txt"), log.Add, new FakeServer { Answer = _ => (HttpStatusCode.OK, "application/json", Hello) }); }
            catch (InvalidDataException) { refused = true; }
            check(refused, "A token file with " + why + " was accepted.");
        }
        File.WriteAllText(Path.Combine(folder, "bad-ca.pem"), "not a certificate");
        var badCa = false;
        try { using var _ = await WarServer.ConnectAsync("https://war.test", Path.Combine(folder, "bad-ca.pem"), null, log.Add); }
        catch (InvalidDataException) { badCa = true; }
        check(badCa, "A CA file that is no certificate was accepted.");

        var fake = new FakeServer
        {
            Answer = path => path switch
            {
                WarServer.HelloPath => (HttpStatusCode.OK, "application/json", Hello),
                "/WarSeason/801/Status" => (HttpStatusCode.OK, "application/json; charset=utf-8", "{\"warId\":801,\"time\":12345678,\"planetStatus\":[]}"),
                "/Account/Login" => (HttpStatusCode.OK, "application/json", "{\"sessionId\":\"war-server-session\"}"),
                "/Operation/Mission/end" => (HttpStatusCode.OK, "application/json", "{\"rewards\":[]}"),
                "/Progression/ShipName" => (HttpStatusCode.NoContent, null, ""),
                _ => (HttpStatusCode.NotFound, "application/json", "{\"error\":\"not here\"}")
            }
        };
        using var server = await WarServer.ConnectAsync("https://War.Test:8443", null, tokenFile, log.Add, fake);
        check(server.Origin == "https://war.test:8443" && server.Name == "Test War", "The war server's origin or name was not kept.");
        check(fake.Requests.Single().PathAndQuery == WarServer.HelloPath && fake.Requests[0].Method == "GET" &&
            fake.Requests[0].Headers.GetValueOrDefault(WarServer.TokenHeader) == "test-token-42" &&
            fake.Requests[0].Headers.ContainsKey(WarServer.VersionHeader) && !fake.Requests[0].Headers.ContainsKey(WarServer.SteamIdHeader),
            "The hello did not carry the token and version, or carried a SteamID.");

        // A backend with it: only one war.
        var otherWar = false;
        try { await using var _ = LocalBackend.Create(0, Path.Combine(folder, "refused"), LoginMode.LocalSession, true, openWar: true, warServer: server); }
        catch (ArgumentException) { otherWar = true; }
        check(otherWar, "A war server was accepted beside the all-planets war.");

        var clock = new WarClock();
        var keys = new AccountKeys();
        var evidence = Path.Combine(folder, "backend");
        await using var app = LocalBackend.Create(0, evidence, LoginMode.LocalSession, true, warClock: clock, accountKeys: keys, warServer: server);
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false }) { BaseAddress = new Uri(app.Urls.Single()) };
            int Asked(string path) { lock (fake.Requests) return fake.Requests.Count(r => r.PathAndQuery.StartsWith(path, StringComparison.Ordinal)); }

            // What stays here: the locator, PlayFab, lobbies, player keys.
            const ulong steamId = 76561197960265728UL + 22202;
            var ticket = new byte[20];
            BinaryPrimitives.WriteUInt32LittleEndian(ticket, 20);
            BinaryPrimitives.WriteUInt64LittleEndian(ticket.AsSpan(4), 0x1122334455667788UL);
            BinaryPrimitives.WriteUInt64LittleEndian(ticket.AsSpan(12), steamId);
            using (var response = await client.PostAsync("/playfab/Client/LoginWithSteam", Json("{\"SteamTicket\":\"" + Convert.ToHexString(ticket) + "\"}")))
                check(response.StatusCode == HttpStatusCode.OK, "The local PlayFab login failed beside a war server.");
            using (var response = await client.GetAsync("/config/locator.json"))
            {
                using var locator = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                check(new Uri(locator.RootElement.GetProperty("backends")[0].GetProperty("url").GetString()!) == new Uri(app.Urls.Single()),
                    "The locator did not name this backend.");
            }
            using (var response = await client.PutAsync("/lobby", Json("{\"lobbyId\":\"l-1\",\"players\":[]}")))
                check(response.StatusCode == HttpStatusCode.OK, "The social tab's lobby upload was not answered here.");
            using (var response = await client.GetAsync("/Account/Keys"))
                check(response.StatusCode == HttpStatusCode.OK, "The player keys were not answered here.");
            check(Asked("/config") + Asked("/playfab") + Asked("/lobby") + Asked("/Account/Keys") == 0, "A local route reached the war server.");

            // The login: forwarded, and its player key kept here.
            var login = "{\"publicKey\":\"key-abc\",\"token\":\"t\"}";
            using (var response = await client.PostAsync("/Account/Login", Json(login)))
            {
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                check(response.StatusCode == HttpStatusCode.OK && json.RootElement.GetProperty("sessionId").GetString() == "war-server-session",
                    "The game was not given the war server's login answer.");
            }
            check(keys.Local == "key-abc", "The login's player key was not kept for co-op.");
            var forwardedLogin = fake.Requests.Last(r => r.PathAndQuery == "/Account/Login");
            check(forwardedLogin.Method == "POST" && Encoding.UTF8.GetString(forwardedLogin.Body) == login, "The login was not forwarded as it was.");

            // A war request: path and query as they are, the game's headers, this backend's own, and the answer as it is.
            using (var request = new HttpRequestMessage(HttpMethod.Get, "/WarSeason/801/Status?probe=1&x=%20y"))
            {
                request.Headers.TryAddWithoutValidation("Authorization", "session war-server-session");
                request.Headers.TryAddWithoutValidation("X-Game-Header", "kept");
                request.Headers.TryAddWithoutValidation(WarServer.SteamIdHeader, "1");
                request.Headers.TryAddWithoutValidation("Accept-Encoding", "gzip, deflate");
                using var response = await client.SendAsync(request);
                var bytes = await response.Content.ReadAsByteArrayAsync();
                var original = "{\"warId\":801,\"time\":12345678,\"planetStatus\":[]}";
                check(response.StatusCode == HttpStatusCode.OK && response.Content.Headers.ContentType?.MediaType == "application/json" &&
                    Encoding.UTF8.GetString(bytes).TrimEnd(' ') == original, "The war status did not come back as the war server answered it.");
            }
            var status = fake.Requests.Last(r => r.PathAndQuery.StartsWith("/WarSeason/801/Status", StringComparison.Ordinal));
            check(status.PathAndQuery == "/WarSeason/801/Status?probe=1&x=%20y", "The path or query changed on the way: " + status.PathAndQuery);
            check(status.Headers.GetValueOrDefault("Authorization") == "session war-server-session" && status.Headers.GetValueOrDefault("X-Game-Header") == "kept",
                "The game's headers were not passed on.");
            check(status.Headers.GetValueOrDefault(WarServer.SteamIdHeader) == steamId.ToString(System.Globalization.CultureInfo.InvariantCulture),
                "The war server was not told the player's SteamID64 (or the game's own header got through).");
            check(status.Headers.GetValueOrDefault(WarServer.TokenHeader) == "test-token-42" && status.Headers.ContainsKey(WarServer.VersionHeader),
                "The token or version header was missing.");
            check(!status.Headers.TryGetValue("Host", out var host) || host == "war.test:8443", "The game's Host header was passed on.");
            check(!status.Headers.ContainsKey("Accept-Encoding"), "Accept-Encoding was passed on (the answer could come back compressed).");
            check(Math.Abs(clock.Seconds - 12345678) < 5, "The war clock did not follow the war server's status.");

            // A write: its body byte for byte, its content type.
            var report = "{\"missionSuccess\":true,\"name\":\"Fünf ✓\",\"objectives\":[1,2,3]}";
            using (var response = await client.PostAsync("/Operation/Mission/end", Json(report)))
                check(response.StatusCode == HttpStatusCode.OK, "The mission report was not answered by the war server.");
            var end = fake.Requests.Last(r => r.PathAndQuery == "/Operation/Mission/end");
            check(Encoding.UTF8.GetString(end.Body) == report && end.Headers.GetValueOrDefault("Content-Type")?.StartsWith("application/json", StringComparison.Ordinal) == true,
                "The mission report's body or content type changed on the way.");

            // Nothing answered here in its place: a route the profile would answer, the server's 404 and 204 as they are.
            using (var response = await client.GetAsync("/Progression/inventory"))
            {
                var text = await response.Content.ReadAsStringAsync();
                check(response.StatusCode == HttpStatusCode.NotFound && text.Contains("not here", StringComparison.Ordinal), "A route the war server does not have was answered locally.");
            }
            using (var response = await client.PutAsync("/Progression/ShipName", Json("{\"name\":\"SES Test\"}")))
                check(response.StatusCode == HttpStatusCode.NoContent && (await response.Content.ReadAsByteArrayAsync()).Length == 0, "A 204 did not come back empty.");

            // The war server gone: a 502, never a local answer, and the log says so.
            fake.Fail = true;
            using (var response = await client.GetAsync("/WarSeason/801/warinfo"))
            {
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                check(response.StatusCode == HttpStatusCode.BadGateway && json.RootElement.GetProperty("errorCode").GetString() == "HELLFORGE_WAR_SERVER_UNREACHABLE",
                    "A request the war server did not answer was not a 502.");
            }
            check(log.Any(l => l.Contains("/WarSeason/801/warinfo was not answered", StringComparison.Ordinal)), "An unanswered request was not logged.");
            fake.Fail = false;

            // The audit names what was forwarded, without the token or the session.
            var audit = File.ReadAllText(Path.Combine(evidence, "requests.jsonl"));
            check(audit.Contains("\"outcome\":\"forwarded\"", StringComparison.Ordinal) && audit.Contains("\"forwardMilliseconds\"", StringComparison.Ordinal) &&
                audit.Contains("\"outcome\":\"war_server_unreachable\"", StringComparison.Ordinal), "The audit does not show the forwarded requests.");
            check(!audit.Contains("test-token-42", StringComparison.Ordinal) && !audit.Contains("war-server-session", StringComparison.Ordinal) &&
                !audit.Contains("probe=1", StringComparison.Ordinal), "The audit kept a token, a session or a query.");
        }
        finally { await app.StopAsync(); }

        // With a CA given: the server's certificate must chain to it (or be it) and be for the server's name.
        using var caKey = RSA.Create(2048);
        using var ca = Authority("CN=Test War CA", caKey);
        using var otherKey = RSA.Create(2048);
        using var other = Authority("CN=Someone Else", otherKey);
        using var leaf = Leaf("war.test", ca);
        using var stranger = Leaf("war.test", other);
        check(WarServer.Trusted(leaf, null, SslPolicyErrors.RemoteCertificateChainErrors, ca), "A certificate from the given CA was refused.");
        check(WarServer.Trusted(ca, null, SslPolicyErrors.RemoteCertificateChainErrors, ca), "The given certificate itself was refused.");
        check(!WarServer.Trusted(stranger, null, SslPolicyErrors.None, ca), "A certificate from another CA was trusted.");
        check(!WarServer.Trusted(leaf, null, SslPolicyErrors.RemoteCertificateNameMismatch, ca), "A certificate for another name was trusted.");
        check(!WarServer.Trusted(null, null, SslPolicyErrors.RemoteCertificateNotAvailable, ca), "A server without a certificate was trusted.");
    }

    static StringContent Json(string text) => new(text, Encoding.UTF8, "application/json");

    static X509Certificate2 Authority(string name, RSA key)
    {
        var request = new CertificateRequest(name, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
        request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign, true));
        return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1));
    }

    // Signed by the issuer, which holds its key (CreateSelfSigned).
    static X509Certificate2 Leaf(string host, X509Certificate2 issuer)
    {
        using var key = RSA.Create(2048);
        var request = new CertificateRequest("CN=" + host, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var names = new SubjectAlternativeNameBuilder();
        names.AddDnsName(host);
        request.CertificateExtensions.Add(names.Build());
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension([new Oid("1.3.6.1.5.5.7.3.1")], false));
        var serial = RandomNumberGenerator.GetBytes(8);
        return request.Create(issuer, DateTimeOffset.UtcNow.AddHours(-1), DateTimeOffset.UtcNow.AddMonths(6), serial);
    }
}
