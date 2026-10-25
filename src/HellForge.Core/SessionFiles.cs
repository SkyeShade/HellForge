using System.Text.Json;

namespace HellForge.Core;

// Reads files that another process (the session runner, PowerShell) may still be writing. They can start with a UTF-8 BOM
// and be half written; a failed read returns nothing and is retried on the next poll. The read shares the file fully, so
// the writer is never refused while a poll holds it (a refused result.json write once failed the session).
public static class SessionFiles
{
    public static JsonElement? ReadJson(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            using var document = JsonDocument.Parse(reader.ReadToEnd());
            return document.RootElement.Clone();
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }

    public static string Text(JsonElement parent, string name) =>
        parent.ValueKind == JsonValueKind.Object && parent.TryGetProperty(name, out var value)
            ? value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : value.GetRawText()
            : "";
}
