using System.Diagnostics;
using System.Net.Security;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// The capture's detailed diagnostics, in the same value-free terms as CaptureLog: the shape of a JSON body (its keys and
// how many entries each holds, never a value), a TLS session's protocol and cipher, an exception with its inner causes,
// and a run's per-request tally. With them proxy.log shows what each request and response looked like, how long it took
// and where it failed, without keeping the account's data.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureDiagnostics
{
    const int MaxShape = 400, MaxKeys = 24;

    // "object{items[724], limitSettings{3}, loadouts[2]}": the top level's keys with their array lengths or key counts.
    public static string JsonShape(ReadOnlySpan<byte> body)
    {
        if (body.IsEmpty) return "empty";
        try
        {
            using var document = JsonDocument.Parse(body.ToArray());
            var root = document.RootElement;
            return root.ValueKind switch
            {
                JsonValueKind.Object => Cap("object{" + string.Join(", ", root.EnumerateObject().Take(MaxKeys).Select(Member)) +
                    (root.EnumerateObject().Count() > MaxKeys ? ", ..." : "") + "}"),
                JsonValueKind.Array => $"array[{root.GetArrayLength()}]",
                var kind => kind.ToString().ToLowerInvariant()
            };
        }
        catch (JsonException)
        {
            return $"not JSON, {body.Length} bytes";
        }
    }

    static string Member(JsonProperty property) => property.Name + property.Value.ValueKind switch
    {
        JsonValueKind.Array => $"[{property.Value.GetArrayLength()}]",
        JsonValueKind.Object => "{" + property.Value.EnumerateObject().Count() + "}",
        var kind => ":" + kind.ToString().ToLowerInvariant()
    };

    // "Tls13 TLS_AES_256_GCM_SHA384 http/1.1, sni api.live.prod.thehelldiversgame.com"
    public static string Tls(SslStream ssl)
    {
        var alpn = ssl.NegotiatedApplicationProtocol.Protocol.IsEmpty ? "no alpn" : ssl.NegotiatedApplicationProtocol.ToString();
        var sni = string.IsNullOrEmpty(ssl.TargetHostName) ? "no sni" : "sni " + ssl.TargetHostName;
        return $"{ssl.SslProtocol} {ssl.NegotiatedCipherSuite} {alpn}, {sni}";
    }

    // "HttpRequestException: ... <- SocketException (10054): An existing connection was forcibly closed"
    public static string Chain(Exception error)
    {
        var text = new StringBuilder();
        for (Exception? cause = error; cause != null; cause = cause.InnerException)
        {
            if (text.Length > 0) text.Append(" <- ");
            text.Append(cause.GetType().Name);
            if (cause is System.Net.Sockets.SocketException socket) text.Append(" (").Append((int)socket.SocketErrorCode).Append(')');
            text.Append(": ").Append(cause.Message.ReplaceLineEndings(" "));
        }
        return text.ToString();
    }

    public static string Milliseconds(Stopwatch watch) => watch.ElapsedMilliseconds + " ms";

    static string Cap(string text) => text.Length <= MaxShape ? text : text[..MaxShape] + "...}";
}

// Requests by shortened path and status, for the summary a run ends with: "POST /api/Proxy/... 200 x3".
public sealed class CaptureTally
{
    readonly Dictionary<string, int> counts = new(StringComparer.Ordinal);
    readonly Lock gate = new();

    public void Add(string method, string path, int status)
    {
        lock (gate) counts[$"{method} {path} {status}"] = counts.GetValueOrDefault($"{method} {path} {status}") + 1;
    }

    public int Total
    {
        get { lock (gate) return counts.Values.Sum(); }
    }

    public IReadOnlyList<string> Lines()
    {
        lock (gate) return [.. counts.OrderBy(entry => entry.Key, StringComparer.Ordinal).Select(entry => $"{entry.Key} x{entry.Value}")];
    }
}
