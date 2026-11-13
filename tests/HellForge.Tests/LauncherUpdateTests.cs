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
    }
}
