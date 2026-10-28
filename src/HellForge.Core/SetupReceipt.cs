using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Written once setup has finished. It separates "setup never finished" from "the copy was damaged after setup": the
// launcher can then offer Check and repair with the first bad file instead of showing a blank first-setup page.
public sealed record SetupReceipt(int SchemaVersion, string BuildId, DateTime FinishedUtc, int Files)
{
    public const string FileName = "setup-state.json";

    public static string PathFor(InstallLayout install) => Path.Combine(install.Root, FileName);

    public static SetupReceipt? Read(InstallLayout install)
    {
        var path = PathFor(install);
        if (!File.Exists(path)) return null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("schemaVersion", out var schema) || !schema.TryGetInt32(out var version) ||
                !root.TryGetProperty("buildId", out var build) || build.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty("finishedUtc", out var finished) || finished.ValueKind != JsonValueKind.String ||
                !DateTime.TryParse(finished.GetString(), System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.RoundtripKind, out var finishedUtc) ||
                !root.TryGetProperty("files", out var files) || !files.TryGetInt32(out var count))
                return null;
            return new(version, build.GetString() ?? "", finishedUtc, count);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // Whole-file write and rename: a setup cut short never leaves a half-written receipt that looks like a finished one.
    public static void Write(InstallLayout install)
    {
        var path = PathFor(install);
        var receipt = new JsonObject
        {
            ["schemaVersion"] = 1,
            ["buildId"] = SupportedBuild.Current.BuildId,
            ["finishedUtc"] = DateTime.UtcNow.ToString("o"),
            ["files"] = SupportedBuild.Current.Files.Count
        };
        File.WriteAllText(path + ".partial", receipt.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        File.Move(path + ".partial", path, overwrite: true);
    }
}
