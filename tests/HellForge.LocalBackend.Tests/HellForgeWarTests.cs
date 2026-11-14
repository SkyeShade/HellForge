using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.LocalBackend;

// The HellForge war against a fake war server: the snapshot's checks, its bodies served unchanged on each route, the
// cache (one server's, used offline, advanced in time), the installation's id in mission reports, heartbeats, and a
// server that is down, slow or refusing never changing or delaying the game's replies.
static class HellForgeWarTests
{
    sealed class FakeServer : HttpMessageHandler
    {
        public Func<HttpRequestMessage, string, Task<(HttpStatusCode, string)>> Answer = (_, _) => Task.FromResult((HttpStatusCode.ServiceUnavailable, ""));
        public readonly List<(string Method, string Path, string Body)> Requests = [];

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content == null ? "" : await request.Content.ReadAsStringAsync(cancellationToken);
            lock (Requests) Requests.Add((request.Method.Method, request.RequestUri!.AbsolutePath, body));
            var (status, reply) = await Answer(request, body).WaitAsync(cancellationToken);
            return new HttpResponseMessage(status) { Content = new StringContent(reply, Encoding.UTF8, "application/json") };
        }

        public List<(string Method, string Path, string Body)> Seen(string path) { lock (Requests) return [.. Requests.Where(r => r.Path == path)]; }
    }

    const string Server = "http://localhost:5093";
    // Integers that a double would change (2^53 + 1), a float as the server wrote it, and keys in the server's order.
    const string Assignment = """{"id32":4242,"progress":[9007199254740993],"expiresIn":86400,"setting":{"type":4,"overrideTitle":"HOLD","overrideBrief":"Hold.","taskDescription":"","tasks":[{"type":3,"values":[1,100],"valueTypes":[1,3]}],"rewards":[{"type":1,"id32":897894480,"amount":50}],"flags":0}}""";

    static string Snapshot(JsonNode info, JsonNode status, long sequence, int schema = 1) =>
        "{\"schema\":" + schema + ",\"warId\":801,\"sequence\":" + sequence + ",\"generatedUtc\":\"2026-11-09T12:00:00Z\",\"warInfo\":" + info.ToJsonString() +
        ",\"status\":" + status.ToJsonString() + ",\"galacticWarEffects\":[{\"id\":9100,\"gameplayEffectId32\":1,\"effectType\":2.50,\"values\":[1]}]" +
        ",\"assignments\":[" + Assignment + "],\"news\":[{\"id\":7002,\"published\":2000,\"type\":0,\"tagIds\":[],\"message\":\"Newer\"}," +
        "{\"id\":7001,\"published\":1000,\"type\":0,\"tagIds\":[],\"message\":\"Older\"}]}";

    public static async Task Run(string root, Action<bool, string> check)
    {
        using var embedded = LocalBackend.LoadResource("war-data.json");
        var info = JsonNode.Parse(embedded.RootElement.GetProperty("warInfo").GetRawText())!;
        var status = JsonNode.Parse(embedded.RootElement.GetProperty("status").GetRawText())!;
        status["time"] = 95000000;
        var snapshot = Snapshot(info, status, 17);
        var server = new FakeServer { Answer = (r, _) => Task.FromResult(r.RequestUri!.AbsolutePath == "/v1/snapshot" ? (HttpStatusCode.OK, snapshot) : (HttpStatusCode.NotFound, "")) };
        var folder = Path.Combine(root, "hellforge-war");
        var cache = HellForge.Core.WarServer.CachePath(folder, Server);
        var log = new List<string>();
        void Log(string line) { lock (log) log.Add(line); }
        var installId = Guid.NewGuid();

        // Validation: each missing or wrong part keeps the war out.
        using (var good = JsonDocument.Parse(snapshot)) HellForgeWar.Validate(good.RootElement);
        foreach (var (name, broken) in new (string, Func<JsonObject, JsonObject>)[]
        {
            ("schema 2", s => { s["schema"] = 2; return s; }), ("war 802", s => { s["warId"] = 802; return s; }),
            ("no war time", s => { s["status"]!["time"] = 0; return s; }), ("no planets", s => { s["warInfo"]!["planetInfos"] = new JsonArray(); return s; }),
            ("no news", s => { s.Remove("news"); return s; }), ("no assignments", s => { s.Remove("assignments"); return s; }),
            ("no effects", s => { s.Remove("galacticWarEffects"); return s; }), ("no sequence", s => { s.Remove("sequence"); return s; }),
        })
        {
            var refused = false;
            using (var document = JsonDocument.Parse(broken(JsonNode.Parse(snapshot)!.AsObject()).ToJsonString()))
                try { HellForgeWar.Validate(document.RootElement); }
                catch (InvalidDataException) { refused = true; }
            check(refused, "A snapshot with " + name + " was accepted.");
        }

        var clock = new WarClock();
        using var profile = ProfileStore.Open(Path.Combine(root, "hellforge-war-profile"));
        await using (var war = await HellForgeWar.StartAsync(cache, Server + "/", installId, clock, Log, new(server, TimeSpan.FromHours(1), HeartbeatInterval: TimeSpan.FromMilliseconds(100))))
        {
            check(war.HasWar && war.Server == Server && war.Sequence == 17 && Math.Abs(clock.Seconds - 95000000) < 5, "The snapshot did not set the war and its clock.");
            check(File.Exists(cache) && JsonNode.Parse(File.ReadAllText(cache))!["server"]!.GetValue<string>() == Server, "The snapshot was not cached with its server.");
            check(!File.Exists(cache + ".tmp"), "The cache's temporary file was left.");
            await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "hellforge-war-backend"), LoginMode.LocalSession)
                { StartupFixtures = true, Profile = profile, WarClock = clock, HellForgeWar = war });
            await app.StartAsync();
            try
            {
                using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
                async Task<string> Get(string path)
                {
                    using var response = await client.GetAsync(path);
                    check(response.StatusCode == HttpStatusCode.OK, $"GET {path} answered {(int)response.StatusCode}.");
                    return (await response.Content.ReadAsStringAsync()).TrimEnd();
                }
                // Each route serves the snapshot's bytes, unchanged.
                using var served = JsonDocument.Parse(snapshot);
                check(await Get("/WarSeason/801/warinfo") == served.RootElement.GetProperty("warInfo").GetRawText(), "warinfo is not the snapshot's.");
                check(await Get("/WarSeason/801/Status") == served.RootElement.GetProperty("status").GetRawText(), "Status is not the snapshot's.");
                check(await Get("/WarSeason/GalacticWarEffects") == served.RootElement.GetProperty("galacticWarEffects").GetRawText(), "The effects are not the snapshot's (2.50 kept as written).");
                check(await Get("/v2/Assignment/War/801") == "[" + Assignment + "]", "The major orders are not the snapshot's, byte for byte (2^53 + 1 kept).");
                using (var news = JsonDocument.Parse(await Get("/NewsFeed/801?maxEntries=16&fromTimeStamp=0")))
                    check(news.RootElement.GetArrayLength() == 2 && news.RootElement[0].GetProperty("id").GetInt32() == 7002, "All dispatches are not served from 0.");
                using (var news = JsonDocument.Parse(await Get("/NewsFeed/801?maxEntries=16&fromTimeStamp=1000")))
                    check(news.RootElement.GetArrayLength() == 1 && news.RootElement[0].GetProperty("message").GetString() == "Newer", "fromTimeStamp is not respected.");
                check((await Get("/v2/Assignment/Player")) is "[]", "Personal orders are not the empty list.");
                using (var time = JsonDocument.Parse(await Get("/WarSeason/801/timeSinceStart")))
                    check(Math.Abs(time.RootElement.GetProperty("secondsSinceStart").GetInt64() - 95000000) < 5, "The war clock is not the snapshot's.");

                // Mission reports: start with the operation's planet, difficulty (11 included) and enemy, heartbeats, end.
                async Task<JsonElement> Post(string path, string body)
                {
                    using var response = await client.PostAsync(path, new StringContent(body, Encoding.UTF8, "application/json"));
                    check(response.StatusCode == HttpStatusCode.OK, $"POST {path} answered {(int)response.StatusCode}.");
                    using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                    return json.RootElement.Clone();
                }
                var ticket = Guid.NewGuid().ToString();
                server.Answer = (r, body) => Task.FromResult(r.RequestUri!.AbsolutePath switch
                {
                    "/v1/mission/start" => (HttpStatusCode.OK, "{\"ticket\":\"" + ticket + "\"}"),
                    "/v1/mission/heartbeat" => (HttpStatusCode.NoContent, ""),
                    "/v1/mission/end" => (HttpStatusCode.OK, "{\"impact\":1234.5,\"capped\":false}"),
                    _ => (HttpStatusCode.OK, snapshot)
                });
                await Post("/Operation/Create", "{\"operations\":[{\"id\":31,\"planetIndex\":126,\"difficulty\":11,\"race\":2,\"missions\":[{\"index\":0}]}]}");
                var missionId = (await Post("/Operation/Mission/Start", "{\"operationId\":31,\"missionIndex\":0}")).GetProperty("missionId").GetString()!;
                check(Poll.Until(() => server.Seen("/v1/mission/heartbeat").Count >= 2, 10), "No heartbeats while the mission ran.");
                var start = server.Seen("/v1/mission/start").Single();
                using (var startBody = JsonDocument.Parse(start.Body))
                    check(start.Method == "POST" && startBody.RootElement.GetProperty("installId").GetString() == installId.ToString("D") &&
                        startBody.RootElement.GetProperty("planetIndex").GetInt32() == 126 && startBody.RootElement.GetProperty("difficulty").GetInt32() == 11 &&
                        startBody.RootElement.GetProperty("race").GetInt32() == 2, "The mission start did not carry the install id, planet, difficulty and enemy: " + start.Body);
                check(server.Seen("/v1/mission/heartbeat").All(h => h.Body == "{\"ticket\":\"" + ticket + "\"}"), "A heartbeat did not carry the ticket.");
                var ended = await Post("/Operation/Mission/end", "{\"operationId\":31,\"missionIndex\":0,\"missionId\":\"" + missionId + "\",\"missionSuccess\":true,\"difficultyType\":11,\"planetIndex\":126}");
                check(ended.GetProperty("impact").GetRawText() == "0", "The Mission/end reply's impact changed.");
                check(Poll.Until(() => server.Seen("/v1/mission/end").Count == 1, 10), "The mission end was not reported.");
                check(server.Seen("/v1/mission/end").Single().Body == "{\"ticket\":\"" + ticket + "\",\"success\":true}", "The mission end did not carry the ticket and the win.");
                var beats = server.Seen("/v1/mission/heartbeat").Count;
                await Task.Delay(400);
                check(server.Seen("/v1/mission/heartbeat").Count <= beats + 1, "Heartbeats went on after the mission ended.");
                check(Poll.Until(() => { lock (log) return log.Any(l => l.Contains("impact 1234.5", StringComparison.Ordinal)); }, 5), "The impact was not logged.");

                // An abandoned operation ends its mission as lost, and its heartbeats stop. The game's body names the operations
                // (operationIds); another operation's mission runs on.
                await Post("/Operation/Create", "{\"operations\":[{\"id\":32,\"planetIndex\":5,\"difficulty\":3,\"race\":3},{\"id\":33,\"planetIndex\":6,\"difficulty\":3,\"race\":3}]}");
                await Post("/Operation/Mission/Start", "{\"operationId\":32,\"missionIndex\":0}");
                await Post("/Operation/Mission/Start", "{\"operationId\":33,\"missionIndex\":0}");
                check(Poll.Until(() => server.Seen("/v1/mission/start").Count == 3, 10), "The second and third mission starts were not reported.");
                await Post("/Operation/Abandon", "{ \"operationIds\" : [ 32]}");
                check(Poll.Until(() => server.Seen("/v1/mission/end").Count == 2, 10) && server.Seen("/v1/mission/end")[1].Body.EndsWith(",\"success\":false}", StringComparison.Ordinal),
                    "An abandoned mission was not reported as lost.");
                await Task.Delay(300);
                check(server.Seen("/v1/mission/end").Count == 2, "Another operation's mission was ended by the abandon.");
                await Post("/Operation/Abandon", "{ \"operationIds\" : [ 33]}");
                check(Poll.Until(() => server.Seen("/v1/mission/end").Count == 3, 10), "The other operation's abandon was not reported.");
                await Task.Delay(300);
                var afterAbandon = server.Seen("/v1/mission/heartbeat").Count;
                await Task.Delay(400);
                check(server.Seen("/v1/mission/heartbeat").Count <= afterAbandon + 1, "An abandoned mission kept beating.");

                // A server that hangs, then one that refuses, then one that is gone: the game's replies are the same and prompt.
                var withServer = ended;
                foreach (var (name, answer) in new (string, Func<HttpRequestMessage, string, Task<(HttpStatusCode, string)>>)[]
                {
                    ("hangs", async (_, _) => { await Task.Delay(TimeSpan.FromSeconds(30)); return (HttpStatusCode.OK, "{}"); }),
                    ("refuses", (_, _) => Task.FromResult((HttpStatusCode.InternalServerError, "oops"))),
                    ("is gone", (_, _) => throw new HttpRequestException("connection refused")),
                    ("answers rubbish", (_, _) => Task.FromResult((HttpStatusCode.OK, "{not json"))),
                })
                {
                    server.Answer = answer;
                    var timer = System.Diagnostics.Stopwatch.StartNew();
                    var created = await Post("/Operation/Create", "{\"operations\":[{\"id\":40,\"planetIndex\":7,\"difficulty\":4,\"race\":2,\"missions\":[]}]}");
                    var id = (await Post("/Operation/Mission/Start", "{\"operationId\":40,\"missionIndex\":0}")).GetProperty("missionId").GetString()!;
                    var end = await Post("/Operation/Mission/end", "{\"operationId\":40,\"missionIndex\":0,\"missionId\":\"" + id + "\",\"missionSuccess\":true,\"difficultyType\":4}");
                    check(timer.Elapsed < TimeSpan.FromSeconds(3), $"A war server that {name} delayed the game's mission requests ({timer.Elapsed}).");
                    check(created.GetProperty("operations")[0].GetProperty("id").GetInt64() == 40 && Guid.TryParse(id, out _) &&
                        end.GetProperty("experience").GetInt64() == MissionService.Rewards.Experience(4, true, 0) && end.GetProperty("impact").GetRawText() == "0",
                        $"A war server that {name} changed the game's replies.");
                }
                check(Poll.Until(() => { lock (log) return log.Any(l => l.Contains("timed out", StringComparison.Ordinal)); }, 10), "A hanging server did not time out.");
                check(log.Any(l => l.Contains("refused v1/mission/start", StringComparison.Ordinal)), "A refused report was not logged.");
            }
            finally { await app.StopAsync(); }

            // A war script on top: its major orders and dispatches after the server's, its effects and Status additions merged in.
            var script = Path.Combine(folder, "war-script.json");
            File.WriteAllText(script, "{\"majorOrders\":[{\"id\":9001,\"title\":\"LOCAL\",\"expiresIn\":600,\"tasks\":[]}]," +
                "\"dispatches\":[{\"id\":8001,\"published\":1500,\"message\":\"Local\"}],\"effects\":[{\"id\":9200}],\"status\":{\"globalEvents\":[{\"eventId\":1}]}}");
            var services = new WarServices(script, clock, null, () => 0, Log, war);
            var none = new Microsoft.AspNetCore.Http.QueryCollection();
            using var emptyBody = JsonDocument.Parse("{}");
            var orders = services.Handle("GET", "/v2/Assignment/War/801", none, emptyBody.RootElement).Reply.AsArray();
            check(orders.Count == 2 && orders[0]!.ToJsonString() == Assignment && (uint)orders[1]!["id32"]! == 9001, "The script's major order does not follow the server's: " + orders.ToJsonString());
            var dispatches = services.Handle("GET", "/NewsFeed/801", new Microsoft.AspNetCore.Http.QueryCollection(new Dictionary<string, Microsoft.Extensions.Primitives.StringValues> { ["fromTimeStamp"] = "0" }), emptyBody.RootElement).Reply.AsArray();
            check(dispatches.Count == 3 && (uint)dispatches[2]!["id"]! == 8001, "The script's dispatch does not follow the server's.");
            war.TryGet("/WarSeason/GalacticWarEffects", out var effects);
            check(services.Apply("/WarSeason/GalacticWarEffects", effects).GetArrayLength() == 2, "The script's effect was not merged into the server's.");
        }

        // Offline: the cached war, its time advanced by the time since it was fetched.
        server.Answer = (_, _) => Task.FromResult((HttpStatusCode.ServiceUnavailable, ""));
        var cached = JsonNode.Parse(File.ReadAllText(cache))!;
        cached["fetchedUtc"] = DateTimeOffset.UtcNow.AddHours(-1).ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture);
        File.WriteAllText(cache, cached.ToJsonString());
        var offline = new WarClock();
        await using (var war = await HellForgeWar.StartAsync(cache, Server, installId, offline, Log, new(server, TimeSpan.FromHours(1))))
            check(war.HasWar && war.Assignments?.GetArrayLength() == 1 && Math.Abs(offline.Seconds - (95000000 + 3600)) < 5,
                "The cached war was not served offline, or its time not advanced.");
        check(log.Any(l => l.StartsWith("Could not fetch the HellForge war", StringComparison.Ordinal)), "A failed fetch was not logged.");

        // Another server's cache is not served, even under this server's name; nothing cached: the frozen war.
        var other = HellForge.Core.WarServer.CachePath(folder, "https://war.example.com");
        Directory.CreateDirectory(Path.GetDirectoryName(other)!);
        File.Copy(cache, other, overwrite: true);
        await using (var war = await HellForgeWar.StartAsync(other, "https://war.example.com", installId, new WarClock(), Log, new(server, TimeSpan.FromHours(1))))
            check(!war.HasWar && log.Any(l => l.Contains("another server's war", StringComparison.Ordinal)), "Another server's cached war was served.");
        await using (var war = await HellForgeWar.StartAsync(Path.Combine(folder, "none.json"), Server, installId, new WarClock(), Log, new(server, TimeSpan.FromHours(1))))
        {
            check(!war.HasWar, "A war appeared from nowhere.");
            await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "hellforge-war-frozen"), LoginMode.LocalSession) { StartupFixtures = true, HellForgeWar = war });
            await app.StartAsync();
            try
            {
                using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()) };
                using var json = JsonDocument.Parse(await client.GetStringAsync("/WarSeason/801/Status"));
                check(json.RootElement.GetProperty("time").GetInt64() == embedded.RootElement.GetProperty("status").GetProperty("time").GetInt64(), "Without a war the frozen war is not served.");
                check(await client.GetStringAsync("/v2/Assignment/War/801") is var orders && orders.Trim() == "[]", "Without a war the major orders are not empty.");
            }
            finally { await app.StopAsync(); }
        }

        // A refused newer snapshot keeps the one served before.
        server.Answer = (r, _) => Task.FromResult((HttpStatusCode.OK, Snapshot(info, status, 18, schema: 2)));
        await using (var war = await HellForgeWar.StartAsync(cache, Server, installId, new WarClock(), Log, new(server, TimeSpan.FromHours(1))))
            check(war.HasWar && war.Sequence == 17, "A refused snapshot replaced the cached one.");

        // Before a session with no network: fetched once; then served from the cache without asking, and no reports.
        server.Answer = (r, _) => Task.FromResult(r.RequestUri!.AbsolutePath == "/v1/snapshot" ? (HttpStatusCode.OK, Snapshot(info, status, 19)) : (HttpStatusCode.OK, "{\"ticket\":\"x\"}"));
        var prefetched = HellForge.Core.WarServer.CachePath(Path.Combine(root, "hellforge-war-linux"), Server);
        check(await HellForgeWar.FetchOnceAsync(prefetched, Server, Log, server) && File.Exists(prefetched), "The one-time fetch did not write the cache.");
        var asked = server.Requests.Count;
        await using (var war = await HellForgeWar.StartAsync(prefetched, Server, installId, new WarClock(), Log, new(server, TimeSpan.FromMilliseconds(10), CacheOnly: true)))
        {
            war.OperationsCreated(JsonDocument.Parse("{\"operations\":[{\"id\":1,\"planetIndex\":1,\"difficulty\":1,\"race\":2}]}").RootElement);
            war.MissionStarted(JsonDocument.Parse("{\"operationId\":1,\"missionIndex\":0}").RootElement, "m");
            await Task.Delay(100);
            check(war.HasWar && war.Sequence == 19 && server.Requests.Count == asked, "A cache-only war asked the server, or did not serve the fetched copy.");
        }
        server.Answer = (_, _) => Task.FromResult((HttpStatusCode.ServiceUnavailable, ""));
        check(!await HellForgeWar.FetchOnceAsync(Path.Combine(root, "hellforge-war-linux", "unreachable.json"), Server, Log, server), "A failed one-time fetch reported success.");
        var plainHttp = false;
        try { await HellForgeWar.FetchOnceAsync(Path.Combine(root, "x.json"), "http://war.example.com", Log, server); }
        catch (ArgumentException) { plainHttp = true; }
        check(plainHttp, "A plain-http remote server was used.");
    }
}
