using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.LocalBackend;

// The checks block threads (waits on queues and files) beside backends whose timers run on the thread pool; on a two-core
// CI runner the pool grows too slowly for them and a stats share's 1 s tick missed an 8 s wait (CI, 2026-10-24).
ThreadPool.GetMinThreads(out var workers, out var ports);
ThreadPool.SetMinThreads(Math.Max(workers, 32), ports);
var root = Path.GetFullPath(Path.Combine("artifacts", "local-backend-tests", Guid.NewGuid().ToString("N")));
Directory.CreateDirectory(root);
var results = new List<object>();
var checks = 0;
void Check(bool condition, string message)
{
    if (!condition) throw new InvalidDataException(message);
    checks++;
}
var bodies = new[] { "{}", "{\"gmiStr\":null}", "{\"gmiStr\":\"\"}",
    "{\"gmiStr\":\"synthetic-gmi-secret\",\"ticket\":\"synthetic-ticket-secret\"}" };
var failed = false;
try
{
    // A web page in the player's browser is refused: another Host (DNS rebinding), or a browser's marks (Origin, Referer,
    // Sec-Fetch-*). A program on this PC is served, by 127.0.0.1 or localhost.
    {
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "web-pages"), LoginMode.LocalSession));
        await app.StartAsync();
        try
        {
            var address = new Uri(app.Urls.Single());
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false }) { BaseAddress = address, Timeout = TimeSpan.FromSeconds(10) };
            foreach (var (header, value) in new[] { ("Host", "attacker.example:80"), ("Origin", "https://attacker.example"),
                         ("Referer", "https://attacker.example/page"), ("Sec-Fetch-Site", "cross-site") })
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, "/config/locator.json");
                if (header == "Host") request.Headers.Host = value;
                else request.Headers.TryAddWithoutValidation(header, value);
                using var response = await client.SendAsync(request);
                Check(response.StatusCode == HttpStatusCode.Forbidden && (await response.Content.ReadAsStringAsync()).Contains("HELLFORGE_NOT_FROM_THIS_PC"),
                    $"A request with {header}: {value} was served.");
            }
            foreach (var host in new[] { address.Authority, "localhost:" + address.Port, "[::1]:" + address.Port })
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, "/config/locator.json") { Headers = { Host = host } };
                using var response = await client.SendAsync(request);
                Check(response.StatusCode == HttpStatusCode.OK, $"A request to {host} was refused.");
            }
            // A browser's mark with this PC's own Host (a page served from loopback itself) is refused too.
            foreach (var (header, value) in new[] { ("Referer", "http://127.0.0.1/"), ("Sec-Fetch-Mode", "no-cors") })
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, "/config/locator.json") { Headers = { Host = address.Authority } };
                request.Headers.TryAddWithoutValidation(header, value);
                using var response = await client.SendAsync(request);
                Check(response.StatusCode == HttpStatusCode.Forbidden, $"A request to {address.Authority} with {header} was served.");
            }
            Console.WriteLine("PASS web pages: another Host, Origin, Referer and Sec-Fetch-* refused; 127.0.0.1, localhost and [::1] served");
        }
        finally { await app.StopAsync(); }
    }
    foreach (var mode in new[] { LoginMode.RejectGmi, LoginMode.LocalSession })
    {
        var evidence = Path.Combine(root, mode.ToString());
        await using var app = LocalBackend.Create(new BackendSettings(0, evidence, mode));
        await app.StartAsync();
        try
        {
            var address = new Uri(app.Urls.Single());
            Check(address.Host == "127.0.0.1" && address.Port > 0, "Listener is not IPv4 loopback.");
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false })
                { BaseAddress = address, Timeout = TimeSpan.FromSeconds(10) };
            var backendAddress = address;
            using (var locatorRequest = new HttpRequestMessage(HttpMethod.Get, "/config/locator.json"))
            {
                locatorRequest.Headers.Host = "localhost:1"; /* a loopback name on another port: the URL still comes from the socket */
                using var response = await client.SendAsync(locatorRequest);
                Check(response.StatusCode == HttpStatusCode.OK, "Local locator failed.");
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                var backends = json.RootElement.GetProperty("backends");
                Check(backends.GetArrayLength() == 1, "Locator advertised extra backends.");
                Check(new Uri(backends[0].GetProperty("url").GetString()!) == address,
                    "Locator URL escaped the local listener or trusted a supplied Host header.");
                Check(backends[0].GetProperty("validBranches")[0].GetString() == "*", "Missing wildcard branch.");
                // Follow the advertised local backend as a client would.
                backendAddress = new Uri(backends[0].GetProperty("url").GetString()!);
            }
            var sessions = new HashSet<string>(StringComparer.Ordinal);
            foreach (var body in bodies)
            {
                using var response = await client.PostAsync(new Uri(backendAddress, "/Account/Login?secret=synthetic-query-secret"),
                    new StringContent(body, Encoding.UTF8, "application/json"));
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                if (mode == LoginMode.RejectGmi)
                {
                    Check(response.StatusCode == HttpStatusCode.Forbidden, "Rejection control should return 403.");
                    Check(json.RootElement.GetProperty("errorCode").GetInt32() == 10002039, "Wrong error code.");
                    Check(json.RootElement.GetProperty("message").GetString() == "Gmi str verification failed.", "Wrong rejection message.");
                    Check(!json.RootElement.TryGetProperty("sessionId", out _), "Rejected login created a session.");
                }
                else
                {
                    Check(response.StatusCode == HttpStatusCode.OK, "Local login rejected missing/empty/synthetic GMI.");
                    var session = json.RootElement.GetProperty("sessionId").GetString()!;
                    Check(session.StartsWith("hellforge-local-", StringComparison.Ordinal) && Encoding.UTF8.GetByteCount("session " + session) < 64,
                        "Session does not fit the observed client buffer.");
                    Check(sessions.Add(session), "Session identifier was reused.");
                }
                Check(response.Headers.CacheControl?.NoStore == true, "Session response is cacheable.");
            }
            foreach (var invalid in new[] { "{", "[]", "null" })
            {
                using var response = await client.PostAsync("/Account/Login", new StringContent(invalid, Encoding.UTF8, "application/json"));
                Check(response.StatusCode == HttpStatusCode.BadRequest, "Malformed/non-object JSON accepted.");
            }
            using (var response = await client.GetAsync("/Account/Login"))
                Check(response.StatusCode == HttpStatusCode.MethodNotAllowed, "Unexpected login method accepted.");
            using (var response = await client.PostAsync("/Account/Login", new StringContent("{\"padding\":\"" + new string('x', 1_100_000) + "\"}")))
                Check(response.StatusCode == HttpStatusCode.RequestEntityTooLarge, "Oversized request accepted.");
            // Deliberately synthetic path: it does not claim to be an observed game endpoint.
            using (var response = await client.GetAsync("/HellForgeTest/UnknownDependency"))
            {
                Check(response.StatusCode == HttpStatusCode.NotImplemented, "Unknown endpoint returned success.");
                Check(response.Headers.Location == null, "Unknown endpoint redirected elsewhere.");
            }
            var audit = await File.ReadAllTextAsync(Path.Combine(evidence, "requests.jsonl"));
            Check(!audit.Contains("synthetic-gmi-secret") && !audit.Contains("synthetic-ticket-secret") &&
                !audit.Contains("synthetic-query-secret"), "Audit exposed request secrets.");
            Check(sessions.All(session => !audit.Contains(session, StringComparison.Ordinal)), "Audit exposed a session identifier.");
            var records = File.ReadAllLines(Path.Combine(evidence, "requests.jsonl")).Select(line => JsonDocument.Parse(line)).ToArray();
            try
            {
                Check(records.Length == 11, "Missing request audit records.");
                Check(records.Any(r => r.RootElement.GetProperty("path").GetString() == "/HellForgeTest/UnknownDependency" &&
                    r.RootElement.GetProperty("status").GetInt32() == 501), "Unknown dependency not recorded.");
                Check(records.Skip(1).Take(4).Select(r => r.RootElement.GetProperty("gmiState").GetString())
                    .SequenceEqual(new[] { "missing", "null", "empty", "present" }), "GMI presence metadata incorrect.");
            }
            finally { foreach (var record in records) record.Dispose(); }
            results.Add(new { mode = mode.ToString(), outcome = "passed", requests = 11 });
            Console.WriteLine($"PASS {mode}: locator, login variants, invalid requests, unsupported dependency, private audit");
        }
        finally { await app.StopAsync(); }
    }

    // Profile-backed startup endpoints: defaults, persistence, exclusivity, validation and session audit.
    var fixtures = LocalBackend.LoadStartupFixtures();
    // The warbond list goes out newest first, as the live game lists it: Helldivers Mobilize last.
    var warbondList = fixtures["/SeasonPass"].EnumerateArray().Select(w => w.GetProperty("id32").GetUInt32()).ToList();
    Check(warbondList.Count > 1 && warbondList[^1] == HellForge.Core.PremiumContent.FreeWarbond && warbondList[0] != HellForge.Core.PremiumContent.FreeWarbond,
        "The warbond list is not served newest first.");
    var profilePaths = new[] { "/Progression", "/Progression/inventory", "/Progression/Items/Customize/v2" };
    var profileDirectory = Path.Combine(root, "profile");
    var profileFile = Path.Combine(profileDirectory, ProfileStore.FileName);
    using (var created = ProfileStore.Open(profileDirectory))
    {
        Check(created.Created && File.Exists(profileFile), "New profile was not saved.");
        foreach (var path in profilePaths)
        {
            Check(created.TryGetResponse(path, out var body), "Profile does not serve " + path);
            Check(JsonElement.DeepEquals(body, fixtures[path]), "Default profile differs from parser-tested fixture: " + path);
        }
        Check(!created.TryGetResponse("/WarSeason/801/Status", out _), "Profile served a world endpoint.");
        var inUse = false;
        try { using var second = ProfileStore.Open(profileDirectory); }
        catch (IOException) { inUse = true; }
        Check(inUse, "Two servers could open one profile.");
    }
    // Hand edits persist across server restarts; extra progression fields pass through.
    var edited = JsonNode.Parse(File.ReadAllText(profileFile))!.AsObject();
    edited["progression"]!["experience"] = 5000;
    edited["progression"]!["hellforgeNote"] = "synthetic-ship";
    edited["inventory"]!["items"] = new JsonArray(new JsonObject { ["mixId"] = 1001, ["amount"] = 3, ["isDiscovered"] = true });
    File.WriteAllText(profileFile, edited.ToJsonString());
    using (var reopened = ProfileStore.Open(profileDirectory))
    {
        Check(!reopened.Created, "Existing profile was recreated.");
        var evidence = Path.Combine(root, "profile-server");
        await using var app = LocalBackend.Create(new BackendSettings(0, evidence, LoginMode.LocalSession) { StartupFixtures = true, Profile = reopened });
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false })
                { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
            string session;
            using (var login = await client.PostAsync("/Account/Login", new StringContent("{}", Encoding.UTF8, "application/json")))
            using (var json = JsonDocument.Parse(await login.Content.ReadAsStringAsync()))
                session = json.RootElement.GetProperty("sessionId").GetString()!;
            foreach (var authorization in new[] { "session " + session, "session hellforge-local-unknown", null, "Bearer synthetic" })
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, "/Progression");
                if (authorization != null) request.Headers.TryAddWithoutValidation("Authorization", authorization);
                using var response = await client.SendAsync(request);
                Check(response.StatusCode == HttpStatusCode.OK, "Profile progression failed.");
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                Check(json.RootElement.GetProperty("experience").GetInt32() == 5000, "Edited experience was not served.");
                Check(json.RootElement.GetProperty("hellforgeNote").GetString() == "synthetic-ship", "Optional field was dropped.");
            }
            using (var response = await client.GetAsync("/Progression/inventory"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                Check(json.RootElement.GetProperty("items")[0].GetProperty("amount").GetInt32() == 3, "Edited inventory was not served.");
            using (var response = await client.PostAsync("/Progression", new StringContent("{}")))
                Check(response.StatusCode == HttpStatusCode.MethodNotAllowed, "Profile write accepted before its schema is known.");
            using (var response = await client.GetAsync("/WarSeason/801/Status"))
                Check(response.StatusCode == HttpStatusCode.OK, "World fixture missing beside profile.");
            // The client sends the paginated form; the query string must not affect fixture selection.
            using (var response = await client.GetAsync("/FriendsV2/Block?offset=0&limit=100"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
            {
                Check(response.StatusCode == HttpStatusCode.OK, "Blocked-players fixture missing.");
                Check(json.RootElement.GetProperty("totalEntries").GetInt32() == 0 &&
                    json.RootElement.GetProperty("entries").GetArrayLength() == 0, "Fresh profile has blocked players.");
            }
            // The client's decoder rejects compact short bodies; responses carry trailing-space headroom.
            using (var response = await client.GetAsync("/WarSeason/current/WarId"))
            {
                var text = await response.Content.ReadAsStringAsync();
                Check(text.StartsWith("{\"id\":801}", StringComparison.Ordinal) && text.Length >= 2 * text.TrimEnd().Length,
                    "Response lacks client decoder headroom.");
            }
            var audit = await File.ReadAllTextAsync(Path.Combine(evidence, "requests.jsonl"));
            Check(new[] { session, "hellforge-local-unknown", "Bearer", "synthetic-ship" }.All(value => !audit.Contains(value, StringComparison.Ordinal)),
                "Audit exposed a session, header or profile value.");
            var records = File.ReadAllLines(Path.Combine(evidence, "requests.jsonl")).Select(line => JsonDocument.Parse(line)).ToArray();
            try
            {
                Check(records.Length == 10, "Missing profile audit records.");
                Check(records.Skip(1).Take(4).Select(r => r.RootElement.GetProperty("authorization").GetString())
                    .SequenceEqual(new[] { "session_known", "session_unknown", "missing", "other_scheme" }), "Authorization audit incorrect.");
                Check(records.Skip(1).Take(6).All(r => r.RootElement.GetProperty("outcome").GetString() is "local_profile" or "method_rejected"),
                    "Profile requests not attributed to the profile.");
                Check(records[^3..].All(r => r.RootElement.GetProperty("outcome").GetString() == "synthetic_startup_fixture"),
                    "World, friends or war request misattributed.");
            }
            finally { foreach (var record in records) record.Dispose(); }
        }
        finally { await app.StopAsync(); }
    }
    // Inventory templates: the item catalogue is served whole; new profiles take a template's inventory.
    Check(fixtures["/Progression/items"].GetArrayLength() > 1000, "Item catalogue missing from fixtures.");
    Check(ProfileStore.TemplateNames.Contains("starter") && ProfileStore.TemplateNames.Contains("unlock-all"), "Profile templates missing.");
    var catalogue = fixtures["/Progression/items"].EnumerateArray().Select(item => item.GetProperty("mixId").GetUInt32()).ToHashSet();
    foreach (var template in ProfileStore.TemplateNames)
    {
        using var templated = ProfileStore.Open(null, template);
        Check(templated.TryGetResponse("/Progression/inventory", out var inventory), "Template inventory missing.");
        var granted = inventory.GetProperty("items").EnumerateArray().Select(item => item.GetProperty("mixId").GetUInt32()).ToArray();
        Check(granted.Length > 0 && granted.All(catalogue.Contains), "Template grants an item outside the catalogue: " + template);
        // Three weapons, the onboarding chain's required item and its nine unlock rewards, the two stratagems every
        // account owns (Orbital Precision Strike, MG-43 Machine Gun; GitHub issue 9), level 1's unlock item and the six ship
        // module tracks' required items.
        Check(template != "starter" || granted.Length == 3 + 10 + 9, "Starter kit is not three weapons, the onboarding unlocks, the two standard stratagems, level 1 and the ship module tracks.");
        Check(granted.Contains(1497469599u) && granted.Contains(3787370254u), "A new profile lacks Orbital Precision Strike or the MG-43: " + template);
        Check(granted.Contains(0xF96CE664u), "A new profile has level 1 locked (no 0xF96CE664): " + template);
        Check(granted.Contains(0xCD183602u) && granted.Contains(0xE0623D4Cu), "A new profile has no ship module tracks: " + template);
        Check(templated.TryGetResponse("/Progression", out var templatedProgression) &&
            templatedProgression.GetProperty("packageProgress").EnumerateArray().Any(entry => entry.GetProperty("level").GetInt32() == 9),
            "Template lacks the completed tutorial chain: " + template);
        Check(templatedProgression.GetProperty("shipName") is { ValueKind: JsonValueKind.Array } ship && ship.GetArrayLength() == 2 &&
            ship[0].GetInt32() is >= 0 and < 90 && ship[1].GetInt32() is >= 0 and < 60,
            "A template that skips the intro has no valid ship name: " + template);
    }
    // A saved profile past the intro but without a ship name (made before templates named ships) gets one, kept.
    var unnamedDirectory = Path.Combine(root, "profile-unnamed");
    using (var unnamed = ProfileStore.Open(unnamedDirectory, "starter")) { }
    var unnamedFile = Path.Combine(unnamedDirectory, ProfileStore.FileName);
    var unnamedProfile = JsonNode.Parse(File.ReadAllText(unnamedFile))!.AsObject();
    unnamedProfile["progression"]!.AsObject().Remove("shipName");
    File.WriteAllText(unnamedFile, unnamedProfile.ToJsonString());
    using (var repaired = ProfileStore.Open(unnamedDirectory)) { }
    var repairedShip = JsonNode.Parse(File.ReadAllText(unnamedFile))!["progression"]!["shipName"]!.ToJsonString();
    using (var again = ProfileStore.Open(unnamedDirectory)) { }
    Check(JsonNode.Parse(File.ReadAllText(unnamedFile))!["progression"]!["shipName"]!.ToJsonString() == repairedShip,
        "An unnamed profile was not given a lasting ship name.");
    // A profile saved before every profile got the two standard stratagems and level 1 gets them when opened, once.
    var savedProfile = JsonNode.Parse(File.ReadAllText(unnamedFile))!.AsObject();
    var savedItems = savedProfile["inventory"]!["items"]!.AsArray();
    foreach (var standard in savedItems.Where(item => (uint)item!["mixId"]! is 1497469599u or 3787370254u or 0xF96CE664u).ToList()) savedItems.Remove(standard);
    File.WriteAllText(unnamedFile, savedProfile.ToJsonString());
    using (var reopened = ProfileStore.Open(unnamedDirectory)) { }
    var reopenedItems = JsonNode.Parse(File.ReadAllText(unnamedFile))!["inventory"]!["items"]!.AsArray().Select(item => (uint)item!["mixId"]!).ToList();
    Check(reopenedItems.Count(id => id == 1497469599u) == 1 && reopenedItems.Count(id => id == 3787370254u) == 1 &&
        reopenedItems.Count(id => id == 0xF96CE664u) == 1, "A saved profile was not given the two standard stratagems and level 1 exactly once.");
    // A package progress entry saved without sortOrder and claimedLevels (a ship module bought before 2026-11-03), which the
    // client's /Progression parser refuses, is completed when the profile is opened; what it has stays.
    var partial = JsonNode.Parse(File.ReadAllText(unnamedFile))!.AsObject();
    partial["progression"]!["packageProgress"]!.AsArray().Add((JsonNode)new JsonObject { ["id32"] = 131213678, ["level"] = 2, ["progress"] = 0 });
    File.WriteAllText(unnamedFile, partial.ToJsonString());
    using (var completed = ProfileStore.Open(unnamedDirectory))
    {
        Check(completed.TryGetResponse("/Progression", out var served) && served.GetProperty("packageProgress").EnumerateArray().All(entry =>
            entry.TryGetProperty("sortOrder", out _) && entry.TryGetProperty("claimedLevels", out var claimed) && claimed.ValueKind == JsonValueKind.Array &&
            entry.TryGetProperty("progress", out _)) && served.GetProperty("packageProgress").EnumerateArray()
            .Any(entry => entry.GetProperty("id32").GetUInt32() == 131213678 && entry.GetProperty("level").GetInt32() == 2),
            "A package progress entry without sortOrder or claimedLevels was served as it was.");
    }
    var unknownTemplate = false;
    try { using var _ = ProfileStore.Open(null, "hellforge-test-unknown"); }
    catch (ArgumentException) { unknownTemplate = true; }
    Check(unknownTemplate, "Unknown template accepted.");
    // Local PlayFab: identities come from the Steam ID inside the session ticket (synthetic here) and are the same on
    // every backend, so a friend's IDs need no exchange.
    {
        const ulong steamId = 0x0110000100000001;
        var ticket = new byte[52];
        BitConverter.GetBytes(20u).CopyTo(ticket, 0);
        BitConverter.GetBytes(steamId).CopyTo(ticket, 12);
        Check(PlayFabService.SteamIdFromTicket(Convert.ToHexString(ticket)) == steamId, "Steam ID not read from the ticket.");
        Check(PlayFabService.SteamIdFromTicket("00") == null && PlayFabService.SteamIdFromTicket(new string('0', 104)) == null, "Malformed ticket accepted.");
        // several installations on one PC: HELLFORGE_IDENTITY only with HELLFORGE_MULTI_INSTANCE, and only a SteamID64
        static Func<string, string?> Env(string? multi, string? identity) => name => name == "HELLFORGE_MULTI_INSTANCE" ? multi : name == "HELLFORGE_IDENTITY" ? identity : null;
        Check(PlayFabService.IdentityOverride(Env("1", "76561199000000002")) == 76561199000000002UL &&
            PlayFabService.IdentityOverride(Env(null, "76561199000000002")) == null && PlayFabService.IdentityOverride(Env("1", "12345")) == null &&
            PlayFabService.IdentityOverride(Env("1", "-1")) == null && PlayFabService.IdentityOverride(Env("1", null)) == null,
            "HELLFORGE_IDENTITY applies only beside HELLFORGE_MULTI_INSTANCE, and only as a SteamID64.");
        var playFab = new PlayFabService();
        using var login = JsonDocument.Parse($"{{\"TitleId\":\"974A9\",\"SteamTicket\":\"{Convert.ToHexString(ticket)}\"}}");
        var (loginStatus, loginReply, _) = playFab.Handle("/playfab/Client/LoginWithSteam", login.RootElement);
        var entity = loginReply["data"]!["EntityToken"]!["Entity"]!;
        Check(loginStatus == 200 && entity["Id"]!.GetValue<string>() == "110000100000001" && entity["Type"]!.GetValue<string>() == "title_player_account",
            "Local PlayFab login returned the wrong entity.");
        var master = loginReply["data"]!["PlayFabId"]!.GetValue<string>();
        using var lookup = JsonDocument.Parse($"{{\"SteamStringIDs\":[\"{steamId}\"]}}");
        Check(new PlayFabService().Handle("/playfab/Client/GetPlayFabIDsFromSteamIDs", lookup.RootElement).Reply["data"]!["Data"]![0]!["PlayFabId"]!
            .GetValue<string>() == master, "Another backend maps the same Steam ID differently.");
        using var titles = JsonDocument.Parse($"{{\"MasterPlayerAccountIds\":[\"{master}\"]}}");
        Check(playFab.Handle("/playfab/Profile/GetTitlePlayersFromMasterPlayerAccountIds", titles.RootElement).Reply["data"]!["TitlePlayerAccounts"]![master]!["Id"]!
            .GetValue<string>() == "110000100000001", "Master to title player mapping failed.");
        using var friends = JsonDocument.Parse("{ \"ExternalPlatformFriends\": \"None\", }", new JsonDocumentOptions { AllowTrailingCommas = true });
        Check(playFab.Handle("/playfab/Client/GetFriendsList", friends.RootElement).Status == 200, "Friends list failed.");
        using var setObjects = JsonDocument.Parse("{\"Entity\":{\"Id\":\"110000100000001\",\"Type\":\"title_player_account\"},\"Objects\":[" +
            "{\"DataObject\":3,\"ObjectName\":\"testament_backing_platform\"},{\"DataObject\":\"Diver\",\"ObjectName\":\"testament_platform_user_name\"}]}");
        Check(playFab.Handle("/playfab/Object/SetObjects", setObjects.RootElement).Status == 200, "SetObjects failed.");
        using var profiles = JsonDocument.Parse("{\"Entities\":[{\"Id\":\"110000100000001\",\"Type\":\"title_player_account\"},{\"Id\":\"110000100000002\",\"Type\":\"title_player_account\"}]}");
        var listed = playFab.Handle("/playfab/Profile/GetProfiles", profiles.RootElement).Reply["data"]!["Profiles"]!.AsArray();
        Check(listed.Count == 2 && listed[0]!["Objects"]!["testament_backing_platform"]!["EscapedDataObject"]!.GetValue<string>() == "3" &&
            listed[0]!["Objects"]!["testament_platform_user_name"]!["EscapedDataObject"]!.GetValue<string>() == "\"Diver\"" &&
            listed[1]!["Objects"]!.AsObject().Count == 0, "Profile objects lack the escaped form the client reads.");
        Check(listed.All(profile => profile!["Lineage"]!["MasterPlayerAccountId"]!.GetValue<string>().Length <= 20 &&
            profile["Lineage"]!["TitlePlayerAccountId"]!.GetValue<string>().Length <= 20), "Lineage ids exceed the client's 21-byte buffers.");
        // Bounded: what the game sets is shared with friends (FriendPresence.SetLocalObjects).
        static string Objects(string id, IEnumerable<string> items) =>
            "{\"Entity\":{\"Id\":\"" + id + "\",\"Type\":\"title_player_account\"},\"Objects\":[" + string.Join(",", items) + "]}";
        using var many = JsonDocument.Parse(Objects("110000100000001", Enumerable.Range(0, 40).Select(i => $"{{\"DataObject\":{i},\"ObjectName\":\"o{i}\"}}")));
        var manyResults = playFab.Handle("/playfab/Object/SetObjects", many.RootElement).Reply["data"]!["SetResults"]!.AsArray();
        using var big = JsonDocument.Parse(Objects("110000100000001", [$"{{\"DataObject\":\"{new string('x', PlayFabService.MaxObjectBytes)}\",\"ObjectName\":\"testament_platform_user_name\"}}"]));
        var bigResult = playFab.Handle("/playfab/Object/SetObjects", big.RootElement).Reply["data"]!["SetResults"]![0]!;
        Check(manyResults.Count(r => r!["SetResult"]!.GetValue<string>() == "Updated") == PlayFabService.MaxObjectsPerEntity - 2 &&
            manyResults.Count(r => r!["SetResult"]!.GetValue<string>() == "None") == 40 - (PlayFabService.MaxObjectsPerEntity - 2) &&
            bigResult["SetResult"]!.GetValue<string>() == "None", "Profile objects are not bounded per entity and in size.");
        var entities = Enumerable.Range(0, PlayFabService.MaxEntities + 1).Select(i =>
        {
            using var one = JsonDocument.Parse(Objects((0x110000100000100 + i).ToString("X"), ["{\"DataObject\":1,\"ObjectName\":\"a\"}"]));
            return playFab.Handle("/playfab/Object/SetObjects", one.RootElement).Status;
        }).ToList();
        Check(entities.Count(s => s == 200) == PlayFabService.MaxEntities - 1 && entities.Last() == 400, "Profile objects are not bounded in entities: " + string.Join(",", entities));
        using var unknown = JsonDocument.Parse("{}");
        Check(playFab.Handle("/playfab/Client/Unknown", unknown.RootElement).Status == 400, "Unknown PlayFab call succeeded.");
    }
    // The base boosters (Vitality Enhancement and the rest of Helldivers Mobilize) are consumable items held once.
    using (var unlocked = ProfileStore.Open(null, "unlock-all"))
    {
        Check(unlocked.TryGetResponse("/Progression/inventory", out var unlockedInventory) && unlockedInventory.GetProperty("items").EnumerateArray()
            .Any(item => item.GetProperty("mixId").GetUInt32() == 0x4233BB28 && item.GetProperty("amount").GetInt32() == 1), "unlock-all lacks the base boosters.");
    }
    // Adding a template's missing items keeps what the profile already holds.
    var mergeDirectory = Path.Combine(root, "template-merge");
    using (var merged = ProfileStore.Open(mergeDirectory, "starter"))
    {
        merged.ApplyMissionRewards(0, new Dictionary<uint, long> { [MissionService.RequisitionMixId] = 1234 });
        var added = merged.AddMissingTemplateItems("unlock-all");
        Check(added > 2000 && merged.AddMissingTemplateItems("unlock-all") == 0, "Template merge added the wrong items.");
        Check(merged.TryGetResponse("/Progression/inventory", out var mergedInventory) && mergedInventory.GetProperty("items").EnumerateArray()
            .Any(item => item.GetProperty("mixId").GetUInt32() == MissionService.RequisitionMixId && item.GetProperty("amount").GetInt64() == 1234),
            "Template merge changed an earned amount.");
    }
    using (var reloaded = ProfileStore.Open(mergeDirectory))
        Check(reloaded.TryGetResponse("/Progression/inventory", out var reloadedInventory) &&
            reloadedInventory.GetProperty("items").GetArrayLength() > 2000, "Template merge was not saved.");

    // unlock-all holds every booster, Integrated Extinguishers (FireExtinguish, 3545987157) among them: a 0.1.8 report
    // found it missing (the account the catalogue was rebuilt from lacked it). An unlock-all profile made before gets it
    // when it opens; a profile still earning its boosters does not.
    const long Extinguishers = 3545987157;
    var boosterDirectory = Path.Combine(root, "booster-profile");
    var boosterFile = Path.Combine(boosterDirectory, ProfileStore.FileName);
    HashSet<long> HeldIn(string file) => JsonNode.Parse(File.ReadAllText(file))!["inventory"]!["items"]!.AsArray()
        .Select(i => i!["mixId"]!.GetValue<long>()).ToHashSet();
    using (ProfileStore.Open(boosterDirectory, "unlock-all")) { }
    Check(HeldIn(boosterFile).Contains(Extinguishers), "A new unlock-all profile lacks Integrated Extinguishers.");
    var oldProfile = JsonNode.Parse(File.ReadAllText(boosterFile))!.AsObject();
    var oldItems = oldProfile["inventory"]!["items"]!.AsArray();
    foreach (var item in oldItems.Where(i => i!["mixId"]!.GetValue<long>() == Extinguishers).ToList()) oldItems.Remove(item);
    File.WriteAllText(boosterFile, oldProfile.ToJsonString());
    using (ProfileStore.Open(boosterDirectory)) { }
    Check(HeldIn(boosterFile).Contains(Extinguishers) && HeldIn(boosterFile).Count == oldItems.Count + 1,
        "An unlock-all profile made before was not given exactly Integrated Extinguishers.");
    var earningDirectory = Path.Combine(root, "earning-profile");
    using (ProfileStore.Open(earningDirectory, "starter")) { }
    var earningCount = HeldIn(Path.Combine(earningDirectory, ProfileStore.FileName)).Count;
    using (ProfileStore.Open(earningDirectory)) { }
    Check(HeldIn(Path.Combine(earningDirectory, ProfileStore.FileName)) is var earning && !earning.Contains(Extinguishers) && earning.Count == earningCount,
        "A profile still earning its boosters was given one.");

    // Starter profiles hold the onboarding chain's unlock items (war table missions and difficulty need them), and a
    // profile saved without them gets them back when it opens.
    var starterDirectory = Path.Combine(root, "starter-profile");
    uint[] onboardingItems = [4218113336, 362649168, 2783301146, 2228887020, 1786940579, 2181413545, 801921334, 1100449051, 3574625014, 2695783738];
    HashSet<long> Held() => JsonNode.Parse(File.ReadAllText(Path.Combine(starterDirectory, ProfileStore.FileName)))!["inventory"]!["items"]!.AsArray()
        .Select(i => i!["mixId"]!.GetValue<long>()).ToHashSet();
    using (ProfileStore.Open(starterDirectory, "starter")) { }
    Check(onboardingItems.All(m => Held().Contains(m)), "A new starter profile lacks the onboarding unlock items.");
    var starterFile = Path.Combine(starterDirectory, ProfileStore.FileName);
    var stripped = JsonNode.Parse(File.ReadAllText(starterFile))!.AsObject();
    var strippedItems = stripped["inventory"]!["items"]!.AsArray();
    foreach (var item in strippedItems.Where(i => onboardingItems.Contains((uint)i!["mixId"]!.GetValue<long>())).ToList()) strippedItems.Remove(item);
    File.WriteAllText(starterFile, stripped.ToJsonString());
    Check(!Held().Contains(362649168), "The test did not strip the onboarding items.");
    using (ProfileStore.Open(starterDirectory)) { }
    Check(onboardingItems.All(m => Held().Contains(m)) && Held().Count == strippedItems.Count + onboardingItems.Length, "An existing profile was not given back exactly the onboarding unlock items.");

    // Decoded onboarding writes persist in the profile and are served back by GET /Progression.
    using (var writable = ProfileStore.Open(profileDirectory))
    {
        var accountKeys = new AccountKeys { LocalId = 76561190000000002 };
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "profile-writes"), LoginMode.LocalSession) { StartupFixtures = true, Profile = writable, AccountKeys = accountKeys });
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false })
                { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
            async Task<HttpStatusCode> Put(string path, string body)
            {
                using var response = await client.PutAsync(path, new StringContent(body, Encoding.UTF8, "application/json"));
                return response.StatusCode;
            }
            Check(await Put("/Progression/ShipName", "{\"shipName\":[89,59]}") == HttpStatusCode.OK, "Ship name rejected.");
            Check(await Put("/Progression/OnboardingState", "{\"onboardingState\":1,\"onboardingFlags\":3}") == HttpStatusCode.OK,
                "Onboarding state rejected.");
            foreach (var (path, invalid) in new[] {
                ("/Progression/ShipName", "{\"shipName\":[90,0]}"), ("/Progression/ShipName", "{\"shipName\":[1]}"),
                ("/Progression/ShipName", "{\"shipName\":\"SES\"}"), ("/Progression/OnboardingState", "{\"onboardingState\":7,\"onboardingFlags\":0}"),
                ("/Progression/OnboardingState", "{\"onboardingState\":1}") })
                Check(await Put(path, invalid) == HttpStatusCode.BadRequest, "Invalid profile write accepted: " + invalid);
            // Weapon customization: a template update, then an equip without slots; GET serves the merged state.
            Check(await Put("/Progression/Items/Customize/v2", "{\"mixId\":2852344585,\"activeIndex\":1,\"modes\":[1,0,0,0]," +
                "\"customizations\":[{\"index\":1,\"slots\":[2309820280,1182286144,0,0,0,0,0,0]}]}") == HttpStatusCode.OK, "Customization rejected.");
            Check(await Put("/Progression/Items/Customize/v2", "{\"mixId\":2852344585,\"activeIndex\":-1,\"modes\":[2,0,0,0]}") == HttpStatusCode.OK,
                "Customization equip rejected.");
            using (var response = await client.GetAsync("/Progression/Items/Customize/v2"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
            {
                var item = json.RootElement.GetProperty("items").EnumerateArray().Single(i => i.GetProperty("mixId32").GetUInt32() == 2852344585);
                Check(item.GetProperty("activeIndex").GetInt32() == -1 && item.GetProperty("modes")[0].GetInt32() == 2 &&
                    item.GetProperty("customizations")[0].GetProperty("slots")[1].GetUInt32() == 1182286144, "Customization not saved and served back.");
            }
            foreach (var invalid in new[] { "{\"mixId\":0,\"activeIndex\":0}", "{\"mixId\":5,\"activeIndex\":3}",
                "{\"mixId\":5,\"activeIndex\":0,\"customizations\":[{\"index\":4,\"slots\":[]}]}" })
                Check(await Put("/Progression/Items/Customize/v2", invalid) == HttpStatusCode.BadRequest, "Invalid customization accepted: " + invalid);
            // Mission lifecycle: create, start and end grant XP, requisition and looted samples to the profile.
            async Task<JsonDocument> Post(string path, string body)
            {
                using var response = await client.PostAsync(path, new StringContent(body, Encoding.UTF8, "application/json"));
                Check(response.StatusCode == HttpStatusCode.OK, "Mission endpoint failed: " + path);
                return JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            }
            long experienceBefore;
            using (var response = await client.GetAsync("/Progression"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                experienceBefore = json.RootElement.GetProperty("experience").GetInt64();
            using (var created = await Post("/Operation/Create", "{\"operations\":[{\"id\":7,\"difficulty\":4,\"missions\":[]}]}"))
                Check(created.RootElement.GetProperty("operations")[0].GetProperty("id").GetInt64() == 7, "Operation id not echoed.");
            string missionId;
            using (var started = await Post("/Operation/Mission/Start", "{\"operationId\":7,\"missionIndex\":0}"))
            {
                missionId = started.RootElement.GetProperty("missionId").GetString()!;
                Check(Guid.TryParse(missionId, out _), "Mission id is not a GUID.");
            }
            const uint commonSamples = 0xEDF6EAF5;
            var firstEnd = "{\"operationId\":7,\"missionIndex\":0,\"missionId\":\"" + missionId + "\",\"missionSuccess\":true,\"difficultyType\":4," +
                "\"objectives\":[{\"succeeded\":true}],\"players\":[{\"lootedItems\":[{\"mixId\":" + commonSamples + ",\"amount\":5}]}]}";
            using (var ended = await Post("/Operation/Mission/end", firstEnd))
                Check(ended.RootElement.GetProperty("experience").GetInt64() == MissionService.Rewards.Experience(4, true, 1), "Mission XP differs from the rules.");
            // A retried end (its reply lost) pays nothing again; the checks below see one payout.
            using (var replayed = await Post("/Operation/Mission/end", firstEnd))
                Check(replayed.RootElement.GetProperty("experience").GetInt64() == 0 && replayed.RootElement.GetProperty("rewards").GetArrayLength() == 0 &&
                    replayed.RootElement.GetProperty("lootedItemRewards").GetArrayLength() == 0, "A retried mission end paid again.");
            using (var response = await client.GetAsync("/Progression"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                Check(json.RootElement.GetProperty("experience").GetInt64() == experienceBefore + MissionService.Rewards.Experience(4, true, 1), "Mission XP not saved.");
            using (var response = await client.GetAsync("/Progression/inventory"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
            {
                var held = json.RootElement.GetProperty("items").EnumerateArray().ToDictionary(i => i.GetProperty("mixId").GetUInt32(), i => i.GetProperty("amount").GetInt64());
                Check(held.GetValueOrDefault(commonSamples) == 5 && held.GetValueOrDefault(MissionService.RequisitionMixId) == MissionService.Rewards.Requisition(4),
                    "Looted samples or requisition not saved.");
                Check(held.GetValueOrDefault(MissionService.MedalsMixId) == MissionService.Rewards.Medals(4), "A won mission's medals not saved.");
            }
            // Level 11 pays half as much XP and medals again as level 10; the operation's card shows its medals; heavy loot
            // is granted but its reply list stays empty (the client refuses {mixId, amount} there).
            Check(MissionService.Rewards.Experience(11, true, 0) * 2 == MissionService.Rewards.Experience(10, true, 0) * 3 &&
                MissionService.Rewards.Medals(11) * 2 == MissionService.Rewards.Medals(10) * 3, "Level 11 does not pay 50% more XP and medals than level 10.");
            using (var created = await Post("/Operation/Create", "{\"operations\":[{\"id\":8,\"difficulty\":11,\"missions\":[{\"index\":0},{\"index\":1},{\"index\":2}]}]}"))
            {
                var card = created.RootElement.GetProperty("operations")[0].GetProperty("rewards")[0];
                Check(card.GetProperty("id32").GetUInt32() == MissionService.MedalRewardMixId && card.GetProperty("amount").GetInt64() == 3 * MissionService.Rewards.Medals(11) &&
                    card.GetProperty("isFullReward").GetBoolean(), "The operation's medal card is wrong.");
            }
            for (var mission = 0; mission < 2; mission++)
                using (var ended = await Post("/Operation/Mission/end", "{\"operationId\":8,\"missionIndex\":" + mission + ",\"missionSuccess\":true,\"difficultyType\":11," +
                    "\"objectives\":[],\"heavyLootedItems\":[{\"mixId\":" + commonSamples + ",\"amount\":2}]}"))
                {
                    var reply = ended.RootElement;
                    Check(reply.GetProperty("lootedHeavyItemRewards").GetArrayLength() == 0 &&
                        reply.GetProperty("rewards").EnumerateArray().Any(r => r.GetProperty("mixId").GetUInt32() == MissionService.MedalsMixId &&
                            r.GetProperty("amount").GetInt64() == MissionService.Rewards.Medals(11)), "A level 11 mission's reply is wrong.");
                }
            Dictionary<uint, long> Inventory()
            {
                using var response = client.GetAsync("/Progression/inventory").Result;
                using var json = JsonDocument.Parse(response.Content.ReadAsStringAsync().Result);
                return json.RootElement.GetProperty("items").EnumerateArray().ToDictionary(i => i.GetProperty("mixId").GetUInt32(), i => i.GetProperty("amount").GetInt64());
            }
            var afterMissions = Inventory();
            Check(afterMissions[MissionService.MedalsMixId] == MissionService.Rewards.Medals(4) + 2 * MissionService.Rewards.Medals(11) &&
                afterMissions[commonSamples] == 5 + 4, "Rewards of a second and third mission were not saved (each mission pays, not only the first).");
            // Purchases: a warbond item for medals (Helldivers Mobilize item 1, page 0, 1 medal), once only; a stratagem for
            // requisition; a ship module level for samples; refused when the balance is short or the item is paid.
            async Task<(HttpStatusCode Status, string Body)> Buy(string path, string body)
            {
                using var response = await client.PostAsync(path, new StringContent(body, Encoding.UTF8, "application/json"));
                return (response.StatusCode, await response.Content.ReadAsStringAsync());
            }
            var warbondBuy = await Buy("/SeasonPass/item/buy", "{\"expectedCost\":[{\"mixId\":1276729503,\"amount\":1}],\"seasonPassItemId\":1,\"warId\":801}");
            Check(warbondBuy.Status == HttpStatusCode.OK && warbondBuy.Body.Contains("1861407750") && warbondBuy.Body.Contains("659494344"), "Warbond item purchase failed: " + warbondBuy.Body);
            var afterWarbond = Inventory();
            Check(afterWarbond[MissionService.MedalsMixId] == afterMissions[MissionService.MedalsMixId] - 1 && afterWarbond.GetValueOrDefault(659494344u) == afterMissions.GetValueOrDefault(659494344u) + 1 &&
                afterWarbond.ContainsKey(1861407750), "Warbond purchase not saved (medals, medals spent, item).");
            Check((await Buy("/SeasonPass/item/buy", "{\"expectedCost\":[{\"mixId\":1276729503,\"amount\":1}],\"seasonPassItemId\":1,\"warId\":801}")).Status == HttpStatusCode.BadRequest,
                "A warbond item was sold twice.");
            const uint stratagem = 2273387135;
            var requisition = afterWarbond[MissionService.RequisitionMixId];
            var storeBuy = await Buy("/Progression/buy/v2", "{\"mixId\":" + stratagem + ",\"amount\":1,\"warId\":801,\"expectedCost\":[{\"mixId\":3608481516,\"amount\":4000}]}");
            Check(storeBuy.Status == (requisition >= 4000 ? HttpStatusCode.OK : HttpStatusCode.BadRequest), "Store purchase answered wrongly: " + storeBuy.Body);
            if (storeBuy.Status == HttpStatusCode.OK)
            {
                using var reply = JsonDocument.Parse(storeBuy.Body);
                Check(reply.RootElement.ValueKind == JsonValueKind.Array && reply.RootElement[0].GetProperty("mixId").GetUInt32() == stratagem &&
                    reply.RootElement[1].GetProperty("amount").GetInt64() == -4000 && Inventory()[MissionService.RequisitionMixId] == requisition - 4000,
                    "Store purchase reply or balance wrong.");
            }
            Check((await Buy("/Progression/buy/v2", "{\"mixId\":" + stratagem + ",\"amount\":1,\"warId\":801,\"expectedCost\":[]}")).Status == HttpStatusCode.BadRequest ||
                Inventory()[MissionService.RequisitionMixId] >= 4000, "A store purchase went through without the requisition.");
            // The requisition terminal buys through /StoreFront/buy: an item named by its store front's salesId (store front
            // 124284010's 3499930023, 5000 requisition), sold as /Progression/buy/v2 sells it; an unknown salesId is refused.
            var beforeFront = Inventory().GetValueOrDefault(MissionService.RequisitionMixId);
            var frontBuy = await Buy("/StoreFront/buy", "{\"salesId\":\"0af99da7-00bc-407c-b624-ca68865611c9\",\"amount\":1}");
            Check(frontBuy.Status == (beforeFront >= 5000 ? HttpStatusCode.OK : HttpStatusCode.BadRequest) &&
                (beforeFront < 5000 || Inventory()[MissionService.RequisitionMixId] == beforeFront - 5000 && Inventory().ContainsKey(3499930023)),
                "A store front purchase by salesId answered wrongly: " + frontBuy.Body);
            // Its reply is the warbond item's shape, {items:[...]}, the price in it (negative): the client adds them all.
            var frontReply = Purchases.StoreFrontReply(new JsonArray(new JsonObject { ["mixId"] = 3499930023u, ["amount"] = 1L },
                new JsonObject { ["mixId"] = MissionService.RequisitionMixId, ["amount"] = -5000L }));
            Check(frontReply["items"] is JsonArray frontItems && frontItems.Count == 2 && (uint)frontItems[0]!["mixId"]! == 3499930023u &&
                (long)frontItems[0]!["amount"]! == 1 && (uint)frontItems[1]!["mixId"]! == MissionService.RequisitionMixId && (long)frontItems[1]!["amount"]! == -5000,
                "A store front purchase's reply is not {items:[the item, the price]}: " + frontReply.ToJsonString());
            Check((await Buy("/StoreFront/buy", "{\"salesId\":\"00000000-0000-0000-0000-000000000000\",\"amount\":1}")).Status == HttpStatusCode.BadRequest,
                "A store front purchase of an unknown salesId went through.");
            var shipModule = await Buy("/Progression/ProgressionPackage/AcquireLevel", "{\"id32\":131213678,\"level\":1,\"expectedCost\":[{\"mixId\":3992382197,\"amount\":60}]}");
            Check(shipModule.Status == HttpStatusCode.BadRequest, "A ship module was sold without its 60 samples.");
            Check((await Buy("/Progression/ProgressionPackage/AcquireLevel", "{\"id32\":131213678,\"level\":2,\"expectedCost\":[]}")).Status == HttpStatusCode.BadRequest,
                "A ship module level was skipped.");
            using (var response = await client.GetAsync("/WarSeason/801/timeSinceStart"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                Check(response.StatusCode == HttpStatusCode.OK && json.RootElement.GetProperty("secondsSinceStart").GetInt64() >= 0,
                    "War clock missing.");
            using (var response = await client.GetAsync("/Progression/ShipName"))
                Check(response.StatusCode == HttpStatusCode.MethodNotAllowed, "Ship name read accepted.");
            // The ship's position (POST /Account/ReportPosition) is kept and served back as /Progression "location".
            // Away from a region the client sends regionIndex -1: accepted, the index not kept.
            using (var reported = await client.PostAsync("/Account/ReportPosition", new StringContent(
                "{\"warId\":801,\"planetIndex\":12,\"regionIndex\":-1}", Encoding.UTF8, "application/json")))
                Check(reported.StatusCode == HttpStatusCode.OK, "Ship position away from a region rejected.");
            using (var reported = await client.PostAsync("/Account/ReportPosition", new StringContent(
                "{\"warId\":801,\"planetIndex\":64,\"regionIndex\":2}", Encoding.UTF8, "application/json")))
                Check(reported.StatusCode == HttpStatusCode.OK, "Ship position rejected.");
            foreach (var invalid in new[] { "{\"planetIndex\":5}", "{\"warId\":801,\"planetIndex\":-1}", "{\"warId\":801,\"planetIndex\":\"x\"}" })
                using (var reported = await client.PostAsync("/Account/ReportPosition", new StringContent(invalid, Encoding.UTF8, "application/json")))
                    Check(reported.StatusCode == HttpStatusCode.BadRequest, "Invalid ship position accepted: " + invalid);
            // A host asks for a joining player's public key: this machine's comes from its game's login.
            var publicKey = Convert.ToBase64String(System.Security.Cryptography.RandomNumberGenerator.GetBytes(32));
            using (var login = await client.PostAsync("/Account/Login", new StringContent("{\"publicKey\":\"" + publicKey + "\",\"gmiStr\":\"\"}", Encoding.UTF8, "application/json")))
                Check(login.StatusCode == HttpStatusCode.OK, "Login with a public key failed.");
            using (var response = await client.GetAsync("/Account/Keys?id=76561190000000001"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                Check(response.StatusCode == HttpStatusCode.OK && json.RootElement.GetProperty("accountKeys").GetArrayLength() == 0,
                    "An unknown player's key lookup did not return an empty list.");
            using (var response = await client.GetAsync("/Account/Keys?id=76561190000000002"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
            {
                var entries = json.RootElement.GetProperty("accountKeys");
                Check(entries.GetArrayLength() == 1 && entries[0].GetProperty("platformAccountId").GetString() == "76561190000000002" &&
                    entries[0].GetProperty("key").GetString() == publicKey, "The local player's key was not served.");
            }
            using (var response = await client.GetAsync("/Progression"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
            {
                var progression = json.RootElement;
                Check(progression.GetProperty("shipName")[0].GetInt32() == 89 && progression.GetProperty("shipName")[1].GetInt32() == 59 &&
                    progression.GetProperty("onboardingState").GetInt32() == 1 && progression.GetProperty("onboardingFlags").GetInt32() == 3,
                    "Profile writes were not served back.");
                var location = progression.GetProperty("location");
                Check(location.GetProperty("warId").GetInt32() == 801 && location.GetProperty("planetIndex").GetInt32() == 64 &&
                    location.GetProperty("regionIndex").GetInt32() == 2 && !location.TryGetProperty("territoryIndex", out _),
                    "The ship's position was not served back.");
            }
        }
        finally { await app.StopAsync(); }
    }
    var persisted = JsonNode.Parse(File.ReadAllText(profileFile))!["progression"]!;
    Check(persisted["shipName"]![1]!.GetValue<int>() == 59 && persisted["onboardingState"]!.GetValue<int>() == 1 &&
        persisted["location"]!["planetIndex"]!.GetValue<int>() == 64, "Profile writes were not saved to disk.");

    // Invalid hand edits are reported and never overwritten.
    var valid = File.ReadAllText(profileFile);
    var invalidEdits = new (string Name, Action<JsonObject> Edit)[]
    {
        ("onboarding", p => p["progression"]!["onboardingState"] = 7),
        ("missing-experience", p => p["progression"]!.AsObject().Remove("experience")),
        ("negative-experience", p => p["progression"]!["experience"] = -1),
        ("fractional-flags", p => p["progression"]!["onboardingFlags"] = 1.5),
        ("unknown-section", p => p["progresion"] = new JsonObject()),
        ("schema-version", p => p["schemaVersion"] = 2),
        ("item-shape", p => p["inventory"]!["items"] = new JsonArray(123)),
        ("item-discovered", p => p["inventory"]!["items"]![0]!.AsObject().Remove("isDiscovered")),
        ("customization-items", p => p["customization"]!["items"] = 5),
        ("location-planet", p => p["progression"]!["location"] = new JsonObject { ["warId"] = 801 })
    };
    foreach (var (name, edit) in invalidEdits.Append(("malformed-json", null!)))
    {
        string text;
        if (edit == null) text = "{";
        else
        {
            var profile = JsonNode.Parse(valid)!.AsObject();
            edit(profile);
            text = profile.ToJsonString();
        }
        File.WriteAllText(profileFile, text);
        var rejected = false;
        try { using var store = ProfileStore.Open(profileDirectory); }
        catch (InvalidDataException) { rejected = true; }
        Check(rejected, "Invalid profile accepted: " + name);
        Check(File.ReadAllText(profileFile) == text, "Invalid profile was overwritten: " + name);
    }
    var unpaired = false;
    try { using var store = ProfileStore.Open(null); await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "unpaired"), LoginMode.LocalSession) { Profile = store }); }
    catch (ArgumentException) { unpaired = true; }
    Check(unpaired, "Profile accepted without startup fixtures.");
    AccountSyncTests.Run(root, Check);
    Console.WriteLine("PASS account sync: premium only from a sealed snapshot of the signed-in account, free warbond open, Super Credits never served, granted or imported, imports by category with backups and restore");
    results.Add(new { mode = "profile", outcome = "passed", invalidEdits = invalidEdits.Length + 1 });
    Console.WriteLine("PASS profile: fixture defaults, persistence, single server, validation, session audit");
    await LiveWarTests.Run(root, Check);
    results.Add(new { mode = "live-war", outcome = "passed" });
    Console.WriteLine("PASS live war: mirror headers, served status and clock, offline cache, fetched before a session with no network, refusals");
    await OpenWarTests.Run(root, Check);
    results.Add(new { mode = "open-war", outcome = "passed" });
    Console.WriteLine("PASS all-planets war: a liberation on every planet, nearest fronts, served in place of the frozen war");
    await MultiplayerTests.Run(Check);
    results.Add(new { mode = "multiplayer", outcome = "passed" });
    Console.WriteLine("PASS multiplayer: frames, lobby create/join/update/leave across backends, modlist gate, forged events, named pipe, loopback UDP");
    // The command line: every option the session runner passes (PlaySession.StartBackend and the WarArguments and
    // MultiplayerArguments it delegates to, up to Observe; the multiplayer path the longest) is accepted; an unknown or
    // repeated one is refused.
    var runner = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "../../../../../src/HellForge.Session/PlaySession.cs"));
    var startAt = runner.IndexOf("void StartBackend(", StringComparison.Ordinal);
    var startBackend = startAt < 0 ? "" : runner[startAt..runner.IndexOf("void Observe(", startAt, StringComparison.Ordinal)];
    var passed = System.Text.RegularExpressions.Regex.Matches(startBackend, "\"(--[a-z][a-z-]+)\"").Select(m => m.Groups[1].Value).Distinct().ToList();
    Check(passed.Contains("--mods") && passed.Contains("--pipe") && passed.Contains("--port") && passed.Contains("--mod-digest"),
        "The session runner's backend options were not found.");
    var line = passed.SelectMany(name => new[] { name, "x" }).ToList();
    Check(BackendOptions.Parse(line).Count == passed.Count, "The backend refuses an option the session runner passes.");
    var refused = 0;
    foreach (var bad in new[] { new[] { "--nope", "x" }, new[] { "--port" }, new[] { "--port", "1", "--port", "2" } })
        try { BackendOptions.Parse(bad); } catch (ArgumentException) { refused++; }
    Check(refused == 3, "The backend accepted an unknown, incomplete or repeated option.");
    Console.WriteLine($"PASS command line: the session runner's {passed.Count} backend options");
    await StatsShareTests.Run(root, Check);
    results.Add(new { mode = "stats-share", outcome = "passed" });
    Console.WriteLine("PASS stats share: a joiner plays with the host's Local Stats when it shares them (joined, changed, not shared, shared again, host gone), its live-edit pipe");
    await AssetShareTests.Run(root, Check);
    results.Add(new { mode = "asset-share", outcome = "passed" });
    Console.WriteLine("PASS asset share: every game loads the lobby's packages less its own, a joiner's new package reaches the others, malformed lists dropped, each player's projectile swaps reach every game");
    RecordGuardTests.Run(root, Check);
    await DirectoryTests.Run(root, Check);
    results.Add(new { mode = "directory", outcome = "passed" });
    Console.WriteLine("PASS lobby directory: the map's filter, listing values, publishing rules, search and records, edited stats, blocked and unjoinable mods, hostile listings, the listing switch, strangers");
    await PrivacyTests.Run(Check);
    results.Add(new { mode = "lobby-privacy", outcome = "passed" });
    Console.WriteLine("PASS lobby privacy: Public, Friends only and Invite only against friends, strangers and invited players; kicks and no rejoin; invitations; frame limits; the stranger gate's limits and memory");
    await ModSyncTests.Run(root, Check);
    results.Add(new { mode = "mod-sync", outcome = "passed" });
    Console.WriteLine("PASS mod sync: a refusal's offer, download, checks and restart; blocked, unshared and unverified mods; the host's limits; a lying host; trusting a friend's mods and GET IT, a stranger's only when sandboxed; the live pipe");
    await RejoinTests.Run(root, Check);
    results.Add(new { mode = "rejoin", outcome = "passed" });
    Console.WriteLine("PASS rejoin after a mod sync: the rejoin file, the host expecting a machine refused for its mods back (invited, privacy changed, kicked, expired), its answers, the game on the ship invited back and joining, and each way it gives up once");
    RegisteredItemsTests.Run(root, Check);
    results.Add(new { mode = "registered-items", outcome = "passed" });
    Console.WriteLine("PASS registered items: child hash, catalogue children, mastery package, progress, inventory");
}
catch (Exception error)
{
    failed = true;
    results.Add(new { outcome = "failed", error = error.Message });
    Console.Error.WriteLine(error);
}
await File.WriteAllTextAsync(Path.Combine(root, "result.json"), JsonSerializer.Serialize(new
{
    passed = !failed, checks, results, gameLaunched = false, nativeClientParserExecuted = false,
    scope = "Synthetic HTTP contract tests over loopback; not proof of independent game startup."
}, new JsonSerializerOptions { WriteIndented = true }));
Console.WriteLine($"{checks} checks; evidence: {root}");
return failed ? 1 : 0;
