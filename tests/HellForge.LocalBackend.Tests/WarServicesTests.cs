using System.Net;
using System.Text;
using System.Text.Json;
using HellForge.LocalBackend;

// The war's polled services (WarServices): without a war script, the empty bodies their parsers accept (major orders,
// dispatches, episodes, stats, mail, feature messages); with one, its major order, dispatch, reward mail (claimed once,
// saved before it is echoed), personal order reward, effects and Status additions, read again when the file changes.
// The served bodies are written to <root>/war-bodies for the client parser probe (HD2Mods research/galactic-war/progression).
static class WarServicesTests
{
    const uint Medals = 1276729503, Requisition = 0xD7150EEC, SuperCredits = 0xCF875032;

    public static async Task Run(string root, Action<bool, string> check)
    {
        var bodies = Directory.CreateDirectory(Path.Combine(root, "war-bodies")).FullName;
        var script = Path.Combine(root, "war-script.json");
        using var profile = ProfileStore.Open(Path.Combine(root, "war-profile"));
        var clock = new WarClock();
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "war-services"), LoginMode.LocalSession)
            { StartupFixtures = true, Profile = profile, WarClock = clock, WarScript = script });
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
            async Task<JsonElement> Send(string name, HttpMethod method, string path, string? body = null)
            {
                using var request = new HttpRequestMessage(method, path) { Content = body == null ? null : new StringContent(body, Encoding.UTF8, "application/json") };
                using var response = await client.SendAsync(request);
                var text = await response.Content.ReadAsStringAsync();
                check(response.StatusCode == HttpStatusCode.OK, $"{method} {path} answered {(int)response.StatusCode}: {text}");
                await File.WriteAllTextAsync(Path.Combine(bodies, name + ".json"), text.TrimEnd());
                using var json = JsonDocument.Parse(text);
                return json.RootElement.Clone();
            }
            const string inbox = "{\"offset\":0,\"limit\":20,\"unclaimedOnly\":true,\"orderDateAsc\":false,\"sources\":[]}";

            // No script: the empty bodies.
            check((await Send("assignments-empty", HttpMethod.Get, "/v2/Assignment/War/801")).GetArrayLength() == 0, "Major orders without a script are not [].");
            check((await Send("personal-empty", HttpMethod.Get, "/v2/Assignment/Player")).GetArrayLength() == 0, "Personal orders without a script are not [].");
            check((await Send("news-empty", HttpMethod.Get, "/NewsFeed/801?maxEntries=16&fromTimeStamp=0")).GetArrayLength() == 0, "Dispatches without a script are not [].");
            check((await Send("episode-status", HttpMethod.Get, "/Episode/801/status?maxEntries=1")).GetProperty("episodes").GetArrayLength() == 0, "Episode status is not empty.");
            await Send("episodes", HttpMethod.Get, "/Episode/801");
            await Send("stats-war", HttpMethod.Get, "/Stats/war/801/summary");
            check(!(await Send("pes-configuration", HttpMethod.Post, "/Pes/GetConfiguration", "{}")).GetProperty("isValid").GetBoolean(), "Pes configuration is not {isValid:false}.");
            await Send("mission-session", HttpMethod.Post, "/Mission/Session", "{\"anything\":1}");
            await Send("assignment-progress", HttpMethod.Post, "/v2/Assignment/War", "{\"id32\":1,\"progress\":[1],\"warId\":801}");
            var empty = await Send("inbox-empty", HttpMethod.Post, "/Mail/inbox", inbox);
            check(empty.GetProperty("totalMailCount").GetInt32() == 0 && empty.GetProperty("unclaimedOnly").ValueKind == JsonValueKind.True &&
                empty.GetProperty("entries").GetArrayLength() == 0, "The empty mailbox is wrong.");

            // The war's time (the embedded war's when the backend starts); the script's times are war seconds.
            var now = clock.Seconds;
            // A script: a major order (one ended already), a dispatch, two mails (one with Super Credits, never granted),
            // a personal order, an effect and a planet effect.
            await File.WriteAllTextAsync(script, """
                {
                  // comments are allowed
                  "majorOrders": [
                    { "id": 9001, "title": "HOLD THE LINE", "brief": "Kill 100 Automatons.", "endsAt": NOW+90000,
                      "tasks": [ { "type": 3, "values": [3, 0, 100], "valueTypes": [1, 2, 3] } ], "progress": [42],
                      "rewards": [ { "mixId": 897894480, "amount": 25 } ] },
                    { "id": 9002, "title": "OVER", "brief": "", "endsAt": NOW-10, "tasks": [], "rewards": [] }
                  ],
                  "personalOrders": [ { "id": 7001, "title": "Personal", "brief": "b", "expiresIn": 3600,
                      "tasks": [ { "type": 3, "values": [0, 10], "valueTypes": [1, 3] } ], "rewards": [ { "mixId": 897894480, "amount": 3 } ] } ],
                  "dispatches": [ { "id": 1, "published": NOW-100, "message": "<i=3>DISPATCH</i>\nThe war goes on." },
                                  { "id": 2, "published": NOW+100000, "message": "not yet" } ],
                  "mail": [ { "id": 501, "message": "For every Helldiver.", "items": [ { "mixId": 1276729503, "amount": 5 }, { "mixId": 3481751602, "amount": 1 } ] },
                            { "id": 502, "message": "Later.", "sentAt": NOW+100000, "items": [ { "mixId": 1276729503, "amount": 1 } ] } ],
                  "effects": [ { "id": 990001, "gameplayEffectId32": 0, "effectType": 40, "flags": 1, "nameHash": 0, "descriptionFluffHash": 0,
                                 "descriptionGamePlayLongHash": 0, "descriptionGamePlayShortHash": 0, "valueTypes": [16], "values": [4225335746] } ],
                  "status": { "planetActiveEffects": [ { "index": 171, "galacticEffectId": 990001 } ] }
                }
                """.Replace("3481751602", SuperCredits.ToString(System.Globalization.CultureInfo.InvariantCulture))
                   .Replace("NOW+90000", (now + 90000).ToString(System.Globalization.CultureInfo.InvariantCulture))
                   .Replace("NOW-100", (now - 100).ToString(System.Globalization.CultureInfo.InvariantCulture))
                   .Replace("NOW-10", (now - 10).ToString(System.Globalization.CultureInfo.InvariantCulture))
                   .Replace("NOW+100000", (now + 100000).ToString(System.Globalization.CultureInfo.InvariantCulture)));
            File.SetLastWriteTimeUtc(script, DateTime.UtcNow.AddSeconds(5));
            var orders = await Send("assignments", HttpMethod.Get, "/v2/Assignment/War/801");
            check(orders.GetArrayLength() == 1, "The ended major order was served, or the live one was not.");
            var order = orders[0];
            var setting = order.GetProperty("setting");
            check(order.GetProperty("id32").GetUInt32() == 9001 && order.GetProperty("progress")[0].GetInt64() == 42 &&
                order.GetProperty("expiresIn").GetInt64() is > 89000 and <= 90000 && setting.GetProperty("type").GetInt32() == 4 &&
                setting.GetProperty("overrideTitle").GetString() == "HOLD THE LINE" && setting.GetProperty("tasks")[0].GetProperty("values")[2].GetInt64() == 100 &&
                setting.GetProperty("rewards")[0].GetProperty("id32").GetUInt32() == 897894480 && setting.GetProperty("rewards")[0].GetProperty("type").GetInt32() == 1,
                "The major order is not served as written.");
            var news = await Send("news", HttpMethod.Get, "/NewsFeed/801?maxEntries=16&fromTimeStamp=0");
            check(news.GetArrayLength() == 1 && news[0].GetProperty("message").GetString()!.StartsWith("<i=3>DISPATCH</i>", StringComparison.Ordinal),
                "The published dispatch (and only it) is not served.");
            check((await Send("news-later", HttpMethod.Get, "/NewsFeed/801?maxEntries=16&fromTimeStamp=" + (now - 100))).GetArrayLength() == 0, "A seen dispatch was served again.");
            using (var response = await client.GetAsync("/WarSeason/GalacticWarEffects"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                check(json.RootElement.EnumerateArray().Any(e => e.GetProperty("id").GetInt32() == 990001) &&
                    json.RootElement.EnumerateArray().Any(e => e.GetProperty("id").GetInt32() == 1243), "The script's effect did not join the embedded ones.");
            using (var response = await client.GetAsync("/WarSeason/801/Status"))
            using (var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync()))
                check(json.RootElement.GetProperty("planetActiveEffects").EnumerateArray().Any(e => e.GetProperty("index").GetInt32() == 171 &&
                    e.GetProperty("galacticEffectId").GetInt32() == 990001), "The script's planet effect is not in Status.");

            // Mail: the sent one is listed, claimed once (medals saved, Super Credits never), then listed as claimed.
            long Held(uint mixId) => profile.TryGetResponse("/Progression/inventory", out var served) && served.TryGetProperty("items", out var items)
                ? items.EnumerateArray().Where(i => i.GetProperty("mixId").GetUInt32() == mixId).Sum(i => i.GetProperty("amount").GetInt64()) : 0;
            var mailbox = await Send("inbox", HttpMethod.Post, "/Mail/inbox", inbox);
            check(mailbox.GetProperty("totalMailCount").GetInt32() == 1 && mailbox.GetProperty("entries")[0].GetProperty("id").GetUInt32() == 501 &&
                mailbox.GetProperty("entries")[0].GetProperty("itemAttachments")[0].GetProperty("items").GetArrayLength() == 1,
                "The mailbox does not list the sent mail alone, without its Super Credits.");
            var medals = Held(Medals);
            var claim = await Send("claim", HttpMethod.Post, "/Mail/claim/mail", "{\"mailIds\":[501,502]}");
            check(claim.GetProperty("mail").GetArrayLength() == 1 && Held(Medals) == medals + 5 && Held(SuperCredits) == 0, "The mail's reward was not saved, or the unsent one was claimed.");
            check((await Send("claim-again", HttpMethod.Post, "/Mail/claim/mail/501", "{}")).GetProperty("mailId").GetUInt32() == 501 && Held(Medals) == medals + 5,
                "A mail was claimed twice.");
            check((await Send("inbox-claimed", HttpMethod.Post, "/Mail/inbox", inbox)).GetProperty("totalMailCount").GetInt32() == 0, "A claimed mail is still unclaimed.");
            // A personal order's reward, once.
            var complete = await Send("complete", HttpMethod.Post, "/v2/Assignment/Player/Complete", "{\"id32\":7001}");
            check(complete.GetProperty("completionRewards")[0].GetProperty("completionReward").GetProperty("items")[0].GetProperty("amount").GetInt64() == 3 &&
                Held(Medals) == medals + 8, "The personal order's reward was not saved and echoed.");
            check(!(await Send("complete-again", HttpMethod.Post, "/v2/Assignment/Player/Complete", "{\"id32\":7001}")).TryGetProperty("completionRewards", out _) &&
                Held(Medals) == medals + 8, "A personal order paid twice.");

            // A broken edit keeps the last good script.
            await File.WriteAllTextAsync(script, "{ not json");
            File.SetLastWriteTimeUtc(script, DateTime.UtcNow.AddSeconds(10));
            check((await Send("assignments-kept", HttpMethod.Get, "/v2/Assignment/War/801")).GetArrayLength() == 1, "A broken war script dropped the last good one.");
        }
        finally { await app.StopAsync(); }
    }
}
