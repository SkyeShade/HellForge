using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.LocalBackend;

// The live war against a fake community mirror: headers, serving, the offline cache and refusals.
static class LiveWarTests
{
    sealed class FakeMirror : HttpMessageHandler
    {
        public Func<string, (HttpStatusCode, string)> Answer = _ => (HttpStatusCode.ServiceUnavailable, "");
        public readonly List<HttpRequestMessage> Requests = [];

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            lock (Requests) Requests.Add(request);
            var (status, body) = Answer(request.RequestUri!.AbsolutePath);
            return Task.FromResult(new HttpResponseMessage(status) { Content = new StringContent(body, Encoding.UTF8, "application/json") });
        }
    }

    public static async Task Run(string root, Action<bool, string> check)
    {
        using var embedded = LocalBackend.LoadResource("war-data.json");
        var info = JsonNode.Parse(embedded.RootElement.GetProperty("warInfo").GetRawText())!;
        var status = JsonNode.Parse(embedded.RootElement.GetProperty("status").GetRawText())!;
        status["time"] = 90000000;
        var mirror = new FakeMirror
        {
            Answer = path => path switch
            {
                "/raw/api/WarSeason/801/WarInfo" => (HttpStatusCode.OK, info.ToJsonString()),
                "/raw/api/WarSeason/801/Status" => (HttpStatusCode.OK, status.ToJsonString()),
                _ => (HttpStatusCode.NotFound, "")
            }
        };
        var cache = Path.Combine(root, "live-war", "live-war.json");
        var log = new List<string>();
        var clock = new WarClock();
        await using (var war = await LiveWar.StartAsync(cache, clock, log.Add, mirror, "https://mirror.test", TimeSpan.FromHours(1)))
        {
            check(war.HasWar && Math.Abs(clock.Seconds - 90000000) < 5, "The live war's time did not set the war clock.");
            var request = mirror.Requests[0];
            check(request.Headers.GetValues("X-Super-Client").Single() == LiveWar.Client && request.Headers.GetValues("X-Super-Contact").Single() == LiveWar.Contact,
                "The mirror was not told the client and contact.");
            check(mirror.Requests.All(r => r.Method == HttpMethod.Get && r.RequestUri!.Host == "mirror.test"), "The live war made a request other than a GET to the mirror.");
            check(File.Exists(cache), "The live war was not cached.");
            // The game's war requests are answered from it.
            await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "live-war-backend"), LoginMode.LocalSession) { StartupFixtures = true, WarClock = clock, LiveWar = war });
            await app.StartAsync();
            try
            {
                using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()) };
                using var response = await client.GetAsync("/WarSeason/801/Status");
                using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                check(response.StatusCode == HttpStatusCode.OK && json.RootElement.GetProperty("time").GetInt64() == 90000000, "The game was not served the live war status.");
                using var timeResponse = await client.GetAsync("/WarSeason/801/timeSinceStart");
                using var time = JsonDocument.Parse(await timeResponse.Content.ReadAsStringAsync());
                check(Math.Abs(time.RootElement.GetProperty("secondsSinceStart").GetInt64() - 90000000) < 5, "The war clock fell back to the embedded war.");
            }
            finally { await app.StopAsync(); }
        }

        // Offline: the cached war is used, its time advanced by the time since it was fetched.
        mirror.Answer = _ => (HttpStatusCode.ServiceUnavailable, "");
        var cached = JsonNode.Parse(File.ReadAllText(cache))!;
        cached["source"]!["fetchedUtc"] = DateTimeOffset.UtcNow.AddHours(-1).ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture);
        File.WriteAllText(cache, cached.ToJsonString());
        var offlineClock = new WarClock();
        await using (var war = await LiveWar.StartAsync(cache, offlineClock, log.Add, mirror, "https://mirror.test", TimeSpan.FromHours(1)))
            check(war.HasWar && war.TryGet("/WarSeason/801/warinfo", out _) && Math.Abs(offlineClock.Seconds - (90000000 + 3600)) < 5,
                "The cached war was not used offline, or its time was not advanced.");
        check(log.Any(l => l.StartsWith("Could not fetch the live war", StringComparison.Ordinal)), "A failed fetch was not logged.");

        // A session with no network (Linux): the war fetched beforehand is served, and the mirror is never asked.
        mirror.Answer = path => path switch
        {
            "/raw/api/WarSeason/801/WarInfo" => (HttpStatusCode.OK, info.ToJsonString()),
            "/raw/api/WarSeason/801/Status" => (HttpStatusCode.OK, status.ToJsonString()),
            _ => (HttpStatusCode.NotFound, "")
        };
        var fetchedCache = Path.Combine(root, "live-war", "fetched.json");
        check(await LiveWar.FetchOnceAsync(fetchedCache, log.Add, mirror, "https://mirror.test") && File.Exists(fetchedCache), "The one-time fetch did not write the cache.");
        var asked = mirror.Requests.Count;
        var namespaceClock = new WarClock();
        await using (var war = await LiveWar.StartAsync(fetchedCache, namespaceClock, log.Add, mirror, "https://mirror.test", TimeSpan.FromMilliseconds(10), cacheOnly: true))
        {
            await Task.Delay(100);
            check(war.HasWar && Math.Abs(namespaceClock.Seconds - 90000000) < 5 && mirror.Requests.Count == asked, "A cache-only war asked the mirror, or did not serve the fetched copy.");
        }
        mirror.Answer = _ => (HttpStatusCode.ServiceUnavailable, "");
        check(!await LiveWar.FetchOnceAsync(Path.Combine(root, "live-war", "unreachable.json"), log.Add, mirror, "https://mirror.test"),
            "A failed one-time fetch reported success.");

        // A different war, or a body without planets, keeps what was there before.
        status["warId"] = 802;
        mirror.Answer = path => path.EndsWith("/WarInfo", StringComparison.Ordinal) ? (HttpStatusCode.OK, info.ToJsonString()) : (HttpStatusCode.OK, status.ToJsonString());
        var otherClock = new WarClock();
        await using (var war = await LiveWar.StartAsync(Path.Combine(root, "live-war", "none.json"), otherClock, log.Add, mirror, "https://mirror.test", TimeSpan.FromHours(1)))
            check(!war.HasWar && !war.TryGet("/WarSeason/801/Status", out _), "Another war was accepted.");
        mirror.Answer = _ => (HttpStatusCode.OK, "{\"warId\":801}");
        await using (var war = await LiveWar.StartAsync(Path.Combine(root, "live-war", "none.json"), otherClock, log.Add, mirror, "https://mirror.test", TimeSpan.FromHours(1)))
            check(!war.HasWar, "A war without planets was accepted.");
        File.WriteAllText(Path.Combine(root, "live-war", "broken.json"), "{");
        await using (var war = await LiveWar.StartAsync(Path.Combine(root, "live-war", "broken.json"), otherClock, log.Add, mirror, "https://mirror.test", TimeSpan.FromHours(1)))
            check(!war.HasWar && log.Any(l => l.StartsWith("Ignored an unreadable live war cache", StringComparison.Ordinal)), "A broken cache was used.");
    }
}
