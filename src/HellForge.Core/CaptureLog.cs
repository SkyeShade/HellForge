using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// What the capture's proxy.log may say about a request beyond its method, path, status and size: a refused request's
// reason, and the names (never the values) of the headers and query parameters the game sent. A reason is the error
// response's short fields; a long value without spaces (a session, token or id) is replaced by its length.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureLog
{
    const int MaxReason = 300, MaxDepth = 4, LongestPlainValue = 24;

    // "title=Bad request; errors.version[0]=The version field is required." for a JSON error body.
    public static string ErrorText(ReadOnlySpan<byte> body)
    {
        if (body.IsEmpty) return "empty body";
        try
        {
            using var document = JsonDocument.Parse(body.ToArray());
            var fields = new List<string>();
            Collect(document.RootElement, "", 0, fields);
            return fields.Count == 0 ? "no fields" : Cap(string.Join("; ", fields));
        }
        catch (JsonException)
        {
            return $"not JSON, {body.Length} bytes";
        }
    }

    // "Authorization, Content-Type, User-Agent": names only, sorted, so the log never holds a value.
    public static string Names(IEnumerable<string> names) =>
        string.Join(", ", names.Order(StringComparer.OrdinalIgnoreCase));

    // The query's parameter names ("none" without a query).
    public static string QueryNames(string target)
    {
        var query = target.IndexOf('?') is var at and >= 0 ? target[(at + 1)..] : "";
        var names = query.Split('&', StringSplitOptions.RemoveEmptyEntries).Select(pair => pair.Split('=')[0]);
        return query.Length == 0 ? "none" : Names(names.Select(Uri.UnescapeDataString));
    }

    static void Collect(JsonElement element, string path, int depth, List<string> fields)
    {
        if (depth > MaxDepth) return;
        switch (element.ValueKind)
        {
            case JsonValueKind.Object:
                foreach (var property in element.EnumerateObject())
                    Collect(property.Value, path.Length == 0 ? property.Name : path + "." + property.Name, depth + 1, fields);
                break;
            case JsonValueKind.Array:
                var index = 0;
                foreach (var item in element.EnumerateArray()) Collect(item, $"{path}[{index++}]", depth + 1, fields);
                break;
            case JsonValueKind.String:
                fields.Add(Field(path, Plain(element.GetString() ?? "")));
                break;
            default:
                fields.Add(Field(path, element.GetRawText()));
                break;
        }
    }

    static string Field(string path, string value) => path.Length == 0 ? value : path + "=" + value;

    static string Plain(string value)
    {
        if (value.Length > LongestPlainValue && !value.Contains(' ', StringComparison.Ordinal)) return $"<{value.Length} characters>";
        var text = new StringBuilder(value.Length);
        foreach (var c in value) text.Append(char.IsControl(c) ? ' ' : c);
        return text.ToString();
    }

    static string Cap(string text) => text.Length <= MaxReason ? text : text[..MaxReason] + "...";
}
