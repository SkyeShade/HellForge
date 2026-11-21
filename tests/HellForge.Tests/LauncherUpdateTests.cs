using HellForge.Core;

// The launcher's update notice: which GitHub release, if any, counts as newer than the running version.
static class LauncherUpdateTests
{
    const string Page = "https://github.com/SkyeShade/HellForge/releases/tag/";

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Versions are read from tags and from the launcher's own version text", () =>
        {
            check(LauncherUpdate.Number.Parse("v0.4.0-beta")?.ToString() == "0.4.0-beta", "A v tag was not read.");
            check(LauncherUpdate.Number.Parse("0.4.0-beta (d8a8b46)")?.ToString() == "0.4.0-beta", "The version text with its commit was not read.");
            check(LauncherUpdate.Number.Parse("1.0.0")?.ToString() == "1.0.0", "A plain version was not read.");
            foreach (var other in new[] { "nightly", "unknown", "", "0.4", "v0.4.0-beta-final!" })
                check(LauncherUpdate.Number.Parse(other) == null, "Not a version, but read as one: " + other);
        });

        test("Versions compare by number first, and a release is newer than its pre-release", () =>
        {
            static int Compare(string a, string b) => LauncherUpdate.Number.Parse(a)!.CompareTo(LauncherUpdate.Number.Parse(b));
            check(Compare("0.5.0-beta", "0.4.0-beta") > 0, "0.5.0-beta is not newer than 0.4.0-beta.");
            check(Compare("0.10.0-beta", "0.9.0-beta") > 0, "Numbers compared as text.");
            check(Compare("1.0.0", "1.0.0-beta") > 0, "1.0.0 is not newer than 1.0.0-beta.");
            check(Compare("1.0.0-beta", "1.0.0") < 0, "1.0.0-beta is not older than 1.0.0.");
            check(Compare("0.4.0-beta", "v0.4.0-beta") == 0, "The same version compared as different.");
        });

        test("The newest newer release is offered; nightly, drafts, older ones and other sites are not", () =>
        {
            var json = """
                [
                  { "tag_name": "nightly", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/nightly", "draft": false, "prerelease": true },
                  { "tag_name": "v0.6.0-beta", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v0.6.0-beta", "draft": true },
                  { "tag_name": "v0.9.0-beta", "html_url": "https://example.com/releases/v0.9.0-beta", "draft": false },
                  { "tag_name": "v0.5.0-beta", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v0.5.0-beta", "draft": false, "prerelease": true },
                  { "tag_name": "v0.4.1-beta", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v0.4.1-beta", "draft": false },
                  { "tag_name": "v0.3.0-beta", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v0.3.0-beta", "draft": false },
                  { "name": "no tag" }
                ]
                """;
            var releases = LauncherUpdate.ParseReleases(json);
            check(releases.Count == 6, "Releases read: " + releases.Count);
            var newer = LauncherUpdate.Newer("0.4.0-beta (d8a8b46)", releases);
            check(newer?.Tag == "v0.5.0-beta" && newer.Url == Page + "v0.5.0-beta", "Offered: " + newer?.Tag);
            check(LauncherUpdate.Newer("0.5.0-beta (abc1234)", releases) == null, "A release was offered to the newest version.");
            check(LauncherUpdate.Newer("0.6.0-beta", releases) == null, "An older release was offered.");
            check(LauncherUpdate.Newer("unknown", releases) == null, "A development build was offered a release.");
        });

        test("An answer that isn't a release list offers nothing", () =>
        {
            check(LauncherUpdate.ParseReleases("""{ "message": "API rate limit exceeded" }""").Count == 0, "A rate-limit answer was read as releases.");
            check(LauncherUpdate.ParseReleases("[]").Count == 0, "An empty list was read as releases.");
            var unreadable = false;
            try { LauncherUpdate.ParseReleases("<html>"); }
            catch (System.Text.Json.JsonException) { unreadable = true; }
            check(unreadable, "A page that isn't JSON was read; CheckAsync turns that error into no notice.");
        });

        test("A tag that is no version is skipped, never an error", () =>
        {
            // Reported in #387's review: the first and the Arabic-Indic one threw (OverflowException, FormatException).
            var bad = new[]
            {
                "v99999999999.0.0", "v0.99999999999.0", "v0.0.99999999999", "v٠.٥.٠", "v١.0.0-beta", "v1.0", "v1.0.0.0", "1.0.0-",
                "v-1.0.0", "v1.0.0-béta", " ", "v"
            };
            foreach (var tag in bad) check(LauncherUpdate.Number.Parse(tag) == null, "Read as a version: " + tag);
            var releases = new[]
            {
                new LauncherUpdate.Release("v99999999999.0.0", Page + "v99999999999.0.0", false),
                new LauncherUpdate.Release("v٠.٥.٠", Page + "bad", false),
                new LauncherUpdate.Release("v0.5.0-beta", Page + "v0.5.0-beta", false)
            };
            check(LauncherUpdate.Newer("0.4.0-beta", releases)?.Tag == "v0.5.0-beta", "A bad tag hid the real release.");
            check(LauncherUpdate.Newer("v99999999999.0.0", releases) == null, "A running version that can't be read was offered a release.");
        });

        test("Pre-release labels compare part by part: numbers as numbers, fewer parts older", () =>
        {
            static int Compare(string a, string b) => LauncherUpdate.Number.Parse(a)!.CompareTo(LauncherUpdate.Number.Parse(b));
            var order = new[]
            {
                "0.5.0-alpha", "0.5.0-alpha.1", "0.5.0-beta", "0.5.0-beta.1", "0.5.0-beta.2", "0.5.0-beta.9", "0.5.0-beta.10", "0.5.0-beta.10.1",
                "0.5.0-beta.99999999999999999999", "0.5.0-beta.x", "0.5.0-dev.4", "0.5.0-rc.1", "0.5.0"
            };
            for (var i = 0; i < order.Length; i++)
                for (var j = 0; j < order.Length; j++)
                    check(Compare(order[i], order[j]) == Math.Sign(i.CompareTo(j)), $"{order[i]} against {order[j]}: {Compare(order[i], order[j])}");
            check(Compare("0.5.0-beta.9", "0.5.0-beta.10") < 0, "beta.9 is not older than beta.10.");
            check(Compare("0.5.0-beta", "0.5.0-beta.1") < 0, "beta is not older than beta.1.");
            check(Compare("0.5.0-beta.010", "0.5.0-beta.10") == 0, "Leading zeros changed the order.");
            var releases = new[] { "v0.5.0-beta.9", "v0.5.0-beta.10", "v0.5.0-beta.2" }.Select(tag => new LauncherUpdate.Release(tag, Page + tag, false)).ToArray();
            check(LauncherUpdate.Newer("0.5.0-beta.9", releases)?.Tag == "v0.5.0-beta.10", "beta.10 was not offered to beta.9.");
            check(LauncherUpdate.Newer("0.5.0-beta.10", releases) == null, "beta.9 was offered to beta.10.");
        });

        test("The check never throws: any answer or failure is no notice and one quiet line", () =>
        {
            const string good = """[{ "tag_name": "v0.5.0-beta", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v0.5.0-beta", "draft": false }]""";
            var net = new Network(_ => Answer(200, good));
            var log = new List<string>();
            check(LauncherUpdate.CheckAsync("0.4.0-beta (d8a8b46)", true, log.Add, net).Result?.Tag == "v0.5.0-beta" && log.Count == 0, "A good answer was not offered, or was logged.");

            // What the request carries: the fixed User-Agent and the Accept line, nothing about the player or the version.
            var sent = net.Requests.Single();
            check(sent.Uri == LauncherUpdate.ReleasesApi && sent.Method == "GET", "Asked: " + sent.Method + " " + sent.Uri);
            check(sent.Headers.Count == 2 && sent.Headers["User-Agent"] == "HellForge-Launcher" && sent.Headers["Accept"] == "application/vnd.github+json",
                "Headers sent: " + string.Join("; ", sent.Headers.Select(h => h.Key + "=" + h.Value)));
            check(!sent.Uri.Contains("0.4.0") && !sent.Headers.Values.Any(v => v.Contains("0.4.0") || v.Contains("d8a8b46")), "The running version was sent.");

            var bodies = new[]
            {
                "<html>", "", "null", "{", """[1, "x", null, []]""", """{ "message": "API rate limit exceeded" }""",
                """[{ "tag_name": "v99999999999.0.0", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/v99999999999.0.0" }]""",
                "[{ \"tag_name\": \"v٠.٥.٠\", \"html_url\": \"https://github.com/SkyeShade/HellForge/releases/tag/x\" }]",
                """[{ "tag_name": 5, "html_url": false, "draft": "yes" }, { "tag_name": "nightly", "html_url": "https://github.com/SkyeShade/HellForge/releases/tag/nightly" }]"""
            };
            foreach (var body in bodies)
                check(LauncherUpdate.CheckAsync("0.4.0-beta", true, null, new Network(_ => Answer(200, body))).Result == null, "Offered from: " + body);
            foreach (var status in new[] { 301, 403, 404, 429, 500 })
            {
                log.Clear();
                check(LauncherUpdate.CheckAsync("0.4.0-beta", true, log.Add, new Network(_ => Answer(status, good))).Result == null && log.Count == 1,
                    "Offered or not logged on status " + status);
            }
            var failures = new Exception[]
            {
                new HttpRequestException("no route"), new TaskCanceledException("timed out"), new System.Net.Sockets.SocketException(10051), new IOException("reset"),
                new OverflowException("overflow"), new FormatException("format"), new NullReferenceException("null"), new NotSupportedException("no")
            };
            foreach (var failure in failures)
            {
                log.Clear();
                check(LauncherUpdate.CheckAsync("0.4.0-beta", true, log.Add, new Network(_ => throw failure)).Result == null, "Not caught: " + failure.GetType().Name);
                check(log.Count == 1 && log[0].Contains(failure.GetType().Name), "Not logged in one line: " + failure.GetType().Name);
            }
            // A log that fails itself changes nothing.
            check(LauncherUpdate.CheckAsync("0.4.0-beta", true, _ => throw new InvalidOperationException("log"), new Network(_ => throw new IOException("reset"))).Result == null,
                "The log's own error escaped.");
        });

        test("The update check's switch is on by default, is kept by other saves, and off sends nothing", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "launcher-update-switch"));
            install.Create(SupportedBuild.Current.BuildId);
            var paths = new InstallSessionPaths(install);
            check(ProfileStore.LoadUpdateCheck(paths), "The check is off without a settings file.");
            ProfileStore.SaveUpdateCheck(paths, false);
            check(!ProfileStore.LoadUpdateCheck(paths), "Off was not saved.");
            ProfileStore.SaveCrashDump(paths, true);
            ProfileStore.SaveSteamUdp(paths, false);
            check(!ProfileStore.LoadUpdateCheck(paths) && ProfileStore.LoadCrashDump(paths), "Another setting's save turned the check back on.");
            var net = new Network(_ => Answer(200, "[]"));
            check(LauncherUpdate.CheckAsync("0.4.0-beta", ProfileStore.LoadUpdateCheck(paths), null, net).Result == null && net.Requests.Count == 0,
                "Switched off, a request was still sent.");
            ProfileStore.SaveUpdateCheck(paths, true);
            check(ProfileStore.LoadUpdateCheck(paths) && ProfileStore.LoadCrashDump(paths) && !ProfileStore.LoadSteamUdp(paths), "On was not saved, or it changed another setting.");
            check(LauncherUpdate.CheckAsync("0.4.0-beta", ProfileStore.LoadUpdateCheck(paths), null, net).Result == null && net.Requests.Count == 1,
                "Switched on, no request was sent.");
        });
    }

    static HttpResponseMessage Answer(int status, string body) => new((System.Net.HttpStatusCode)status) { Content = new StringContent(body) };

    // The network for CheckAsync: records what was asked and answers from the test.
    sealed class Network(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        public sealed record Sent(string Method, string Uri, Dictionary<string, string> Headers);
        public List<Sent> Requests { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancel)
        {
            Requests.Add(new(request.Method.Method, request.RequestUri!.ToString(), request.Headers.ToDictionary(h => h.Key, h => string.Join(",", h.Value))));
            return Task.FromResult(answer(request));
        }
    }
}
