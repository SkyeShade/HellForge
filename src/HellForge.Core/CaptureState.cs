using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The capture's state file in its sync folder, for the launcher: written once the capture listens (the launcher then opens
// the game), again once the capture is sealed (the launcher then takes it while the game plays on), and deleted when the
// capture exits.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureState
{
    public const string FileName = "capture-state.json";

    public sealed record Info(bool Sealed, bool Progression);

    public static void Write(string syncDir, int pid, bool sealedCapture, bool progression)
    {
        var state = new JsonObject { ["pid"] = pid, ["sealed"] = sealedCapture, ["progression"] = progression };
        var path = Path.Combine(syncDir, FileName);
        File.WriteAllText(path + ".tmp", state.ToJsonString() + "\n");
        File.Move(path + ".tmp", path, overwrite: true);
    }

    // Null while the capture is not listening (or has exited).
    public static Info? Read(string syncDir)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(syncDir, FileName)));
            var root = document.RootElement;
            return new Info(Flag(root, "sealed"), Flag(root, "progression"));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    static bool Flag(JsonElement root, string name) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.True;
}
