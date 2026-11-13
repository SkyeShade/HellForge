using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The launcher's update notice (docs/roadmap.md, Launcher updates: the first step). On start the launcher asks GitHub
// for HellForge's releases and, when one is newer than the version it runs, shows it with a link to its release page.
// Nothing is downloaded or installed: the player installs the new version by hand, as today. Every HellForge release so
// far is marked as a pre-release on GitHub, so /releases/latest has nothing to give; the list is read instead. Tags
// that are not a version (the rolling "nightly" tag) and drafts are skipped. No answer, a slow one or one that can't be
// read means no notice: the launcher works the same without it.
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
            return new(int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), int.Parse(match.Groups[3].Value),
                match.Groups[4].Success ? match.Groups[4].Value : "");
        }

        // Semantic version order: the numbers first; for equal numbers a release with no label ("1.0.0") is newer than
        // a labelled one ("1.0.0-beta"), and labels compare as text.
        public int CompareTo(Number? other)
        {
            if (other == null) return 1;
            var numbers = (Major, Minor, Patch).CompareTo((other.Major, other.Minor, other.Patch));
            if (numbers != 0) return numbers;
            if (Label.Length == 0 || other.Label.Length == 0) return other.Label.Length.CompareTo(Label.Length) switch { < 0 => -1, > 0 => 1, _ => 0 };
            return string.CompareOrdinal(Label, other.Label);
        }

        public override string ToString() => Label.Length == 0 ? $"{Major}.{Minor}.{Patch}" : $"{Major}.{Minor}.{Patch}-{Label}";
    }

    [GeneratedRegex(@"^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.]+))?(?:\s*\(.*\))?$")]
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

    // One request, at most 10 s. Any failure (offline, rate limit, a changed answer) is no notice.
    public static async Task<Release?> CheckAsync(string running, CancellationToken cancel = default)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            using var request = new HttpRequestMessage(HttpMethod.Get, ReleasesApi);
            request.Headers.UserAgent.Add(new ProductInfoHeaderValue("HellForge-Launcher", Number.Parse(running)?.ToString() ?? "unknown"));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
            using var response = await http.SendAsync(request, cancel);
            if (!response.IsSuccessStatusCode) return null;
            return Newer(running, ParseReleases(await response.Content.ReadAsStringAsync(cancel)));
        }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException)
        {
            return null;
        }
    }
}
