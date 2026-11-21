using System.Globalization;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The launcher's update notice (docs/roadmap.md, Launcher updates: the first step). On start the launcher asks GitHub
// for HellForge's releases and, when one is newer than the version it runs, shows it with a link to its release page.
// Nothing is downloaded or installed: the player installs the new version by hand, as today. Every HellForge release so
// far is marked as a pre-release on GitHub, so /releases/latest has nothing to give; the list is read instead. Tags
// that are not a version (the rolling "nightly" tag) and drafts are skipped. No answer, a slow one or one that can't be
// read means no notice: the launcher works the same without it. The request carries nothing about the player or the
// PC: no cookie, no token, no version, only the fixed User-Agent GitHub's API asks for. The launcher's "Check for a
// newer HellForge" switch (ProfileStore.LoadUpdateCheck) turns the check off: then no request is made.
public static partial class LauncherUpdate
{
    public const string ReleasesApi = "https://api.github.com/repos/SkyeShade/HellForge/releases?per_page=30";

    public sealed record Release(string Tag, string Url, bool Draft);

    // "0.4.0-beta", "v0.4.0-beta" or AppVersion.Text's "0.4.0-beta (d8a8b46)": the parts compared, or null when it is
    // not a version (a "nightly" tag, "unknown").
    public sealed record Number(int Major, int Minor, int Patch, string Label) : IComparable<Number>
    {
        public static Number? Parse(string text)
        {
            var match = VersionPattern().Match(text.Trim());
            if (!match.Success) return null;
            // A number too large for an int ("v99999999999.0.0") is not a version: null, never an exception.
            if (!Part(match.Groups[1].Value, out var major) || !Part(match.Groups[2].Value, out var minor) || !Part(match.Groups[3].Value, out var patch))
                return null;
            return new(major, minor, patch, match.Groups[4].Success ? match.Groups[4].Value : "");
        }

        static bool Part(string digits, out int value) => int.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out value);

        // Semantic version order: the numbers first; for equal numbers a release with no label ("1.0.0") is newer than
        // a labelled one ("1.0.0-beta"). Labels compare part by part between their dots: numbers as numbers
        // (beta.9 < beta.10), a number before text, and with every shared part equal the label with fewer parts is
        // older (beta < beta.1).
        public int CompareTo(Number? other)
        {
            if (other == null) return 1;
            var numbers = (Major, Minor, Patch).CompareTo((other.Major, other.Minor, other.Patch));
            if (numbers != 0) return numbers;
            if (Label.Length == 0 || other.Label.Length == 0) return other.Label.Length.CompareTo(Label.Length) switch { < 0 => -1, > 0 => 1, _ => 0 };
            return CompareLabels(Label, other.Label);
        }

        static int CompareLabels(string left, string right)
        {
            var a = left.Split('.');
            var b = right.Split('.');
            for (var i = 0; i < Math.Min(a.Length, b.Length); i++)
            {
                var part = ComparePart(a[i], b[i]);
                if (part != 0) return part;
            }
            return Math.Sign(a.Length.CompareTo(b.Length));
        }

        // Digits compare by length, then as text, so a number of any length compares right without being parsed.
        static int ComparePart(string a, string b)
        {
            var (aNumber, bNumber) = (IsNumber(a), IsNumber(b));
            if (aNumber != bNumber) return aNumber ? -1 : 1;
            if (!aNumber) return Math.Sign(string.CompareOrdinal(a, b));
            (a, b) = (a.TrimStart('0'), b.TrimStart('0'));
            return a.Length != b.Length ? Math.Sign(a.Length.CompareTo(b.Length)) : Math.Sign(string.CompareOrdinal(a, b));
        }

        static bool IsNumber(string part) => part.Length > 0 && part.All(c => c is >= '0' and <= '9');

        public override string ToString() => Label.Length == 0 ? $"{Major}.{Minor}.{Patch}" : $"{Major}.{Minor}.{Patch}-{Label}";
    }

    // [0-9], not \d: \d also matches other scripts' digits, which are not a version.
    [GeneratedRegex(@"^v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z.]+))?(?:\s*\(.*\))?$")]
    private static partial Regex VersionPattern();

    // GitHub's release list (the JSON array /releases returns): tag, page and draft flag of each.
    public static IReadOnlyList<Release> ParseReleases(string json)
    {
        using var document = JsonDocument.Parse(json);
        if (document.RootElement.ValueKind != JsonValueKind.Array) return [];
        var releases = new List<Release>();
        foreach (var item in document.RootElement.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object) continue;
            var tag = item.TryGetProperty("tag_name", out var t) && t.ValueKind == JsonValueKind.String ? t.GetString() : null;
            var url = item.TryGetProperty("html_url", out var u) && u.ValueKind == JsonValueKind.String ? u.GetString() : null;
            var draft = item.TryGetProperty("draft", out var d) && d.ValueKind == JsonValueKind.True;
            if (tag != null && url != null) releases.Add(new(tag, url, draft));
        }
        return releases;
    }

    // The newest release that is newer than the running version, or null. Only release pages on HellForge's own
    // repository are offered, so a changed answer can't send the player somewhere else.
    public static Release? Newer(string running, IEnumerable<Release> releases)
    {
        var current = Number.Parse(running);
        if (current == null) return null; /* a development build ("unknown"): nothing to compare */
        Release? best = null;
        Number? bestNumber = null;
        foreach (var release in releases)
        {
            if (release.Draft || !release.Url.StartsWith("https://github.com/SkyeShade/HellForge/releases/", StringComparison.Ordinal)) continue;
            var number = Number.Parse(release.Tag);
            if (number == null || number.CompareTo(current) <= 0) continue;
            if (bestNumber == null || number.CompareTo(bestNumber) > 0) (best, bestNumber) = (release, number);
        }
        return best;
    }

    // The request's User-Agent: GitHub's API refuses a request without one. The same for every player and version.
    public const string UserAgent = "HellForge-Launcher";

    // One request, at most 10 s. Any failure at all (offline, rate limit, a changed answer, a tag that can't be read)
    // is no notice, with one line for log: this runs when the launcher opens and must never stop it. enabled false
    // (the player's switch) returns before anything is sent. handler: the tests' stand-in for the network.
    public static async Task<Release?> CheckAsync(string running, bool enabled = true, Action<string>? log = null, HttpMessageHandler? handler = null,
        CancellationToken cancel = default)
    {
        if (!enabled) return null;
        try
        {
            using var http = handler == null ? new HttpClient() : new HttpClient(handler, disposeHandler: false);
            http.Timeout = TimeSpan.FromSeconds(10);
            using var request = new HttpRequestMessage(HttpMethod.Get, ReleasesApi);
            request.Headers.UserAgent.Add(new ProductInfoHeaderValue(UserAgent, null));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
            using var response = await http.SendAsync(request, cancel);
            if (!response.IsSuccessStatusCode)
            {
                Note(log, "GitHub answered " + ((int)response.StatusCode).ToString(CultureInfo.InvariantCulture));
                return null;
            }
            return Newer(running, ParseReleases(await response.Content.ReadAsStringAsync(cancel)));
        }
        catch (Exception error) /* every error: best effort, and the caller is the window's Opened handler */
        {
            Note(log, error.GetType().Name + ": " + error.Message);
            return null;
        }
    }

    static void Note(Action<string>? log, string why)
    {
        try { log?.Invoke("The check for a newer HellForge got no answer it could read (" + why + "). Nothing else is affected."); }
        catch (Exception) { /* the log is a courtesy too */ }
    }
}
