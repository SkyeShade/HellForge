using System.Globalization;

namespace HellForge.Core;

// The client headers the game sends on every request, repeated on the capture's own reads. The API answers
// GET /api/Progression with 400 errorCode=13018011 ("Accept-Language HTTP header missing") when they are absent (live run
// 2026-10-26), so a capture that sends only its session token never gets the progression. The game sent Accept,
// Accept-Language, Content-Type, ct, User-Agent and Authorization on every GET and POST of the 2026-10-24 capture;
// Authorization is the read's own session, and X-Signature and Key_ID sign a POST's body, so those three are never
// repeated here.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureHeaders
{
    // What the retail client sends in this locale; the API needs a language even when the login carried none.
    public const string DefaultLanguage = "en-US";

    // The headers of one read: the login's language and user agent when it carried them, a JSON accept, and the client time.
    public static Dictionary<string, string> ForRead(IReadOnlyDictionary<string, string>? login, DateTimeOffset nowUtc)
    {
        var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["Accept"] = "application/json",
            ["Accept-Language"] = Value(login, "Accept-Language") ?? DefaultLanguage,
            ["Content-Type"] = "application/json",
            ["ct"] = Microseconds(nowUtc)
        };
        if (Value(login, "User-Agent") is { } agent) headers["User-Agent"] = agent;
        return headers;
    }

    // The game's client time (ct: 1792899951000000 in the 2026-10-24 capture): microseconds since the epoch, so the
    // API's clock checks see a request made now. Ticks are 100 ns, ten to a microsecond.
    public static string Microseconds(DateTimeOffset nowUtc) =>
        ((nowUtc.UtcTicks - DateTimeOffset.UnixEpoch.UtcTicks) / 10).ToString(CultureInfo.InvariantCulture);

    // A header the login carried, when it carried a usable one: names are matched case-insensitively by the request's own
    // dictionary, and blank values count as absent.
    static string? Value(IReadOnlyDictionary<string, string>? headers, string name) =>
        headers != null && headers.TryGetValue(name, out var value) && !string.IsNullOrWhiteSpace(value) ? value : null;
}
