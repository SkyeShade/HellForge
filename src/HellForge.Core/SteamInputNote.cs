using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// Steam Input and a sandboxed game (docs/steam-bridge.md, "Controllers and Steam Input"). Steam Input hands a game the
// controllers it handles through Steam's overlay, which a sandboxed session has in the Steam broker, not in the game, so
// those controllers never reach it. The broker asks Steam which controllers Steam Input handles and logs them
// (native/steam-cloud/broker_input.c); the session reads that line from the broker's log (Parse), keeps it in
// result.json (Record), and the launcher and the CLI tell the player (Of) while the game runs.
public static partial class SteamInputNote
{
    // The broker's line, as broker_input.c LogList writes it: "none", or "<name> [<type>]" for each controller (with " as
    // XInput pad <n>" when Steam emulates one), comma separated.
    public const string BrokerPrefix = "broker: Steam Input controllers: ";

    // ESteamInputType (isteaminput.h, SDK 1.55), named as broker_input.c names them (a test compares the two).
    static readonly string[] Names =
    [
        "unknown controller", "Steam Controller", "Xbox 360 controller", "Xbox One controller", "generic gamepad",
        "PS4 controller", "Apple MFi controller", "Android controller", "Joy-Con pair", "Joy-Con", "Switch Pro controller",
        "Steam Link touch controller", "PS3 controller", "PS5 controller", "Steam Deck controls",
    ];

    const string Trade = " With Settings > Sandbox off, Steam Input works, but the game runs without the sandbox's protection.";
    const string DesktopKeys = "its desktop keys to the game (A presses Enter, which opens the chat)";

    public static IReadOnlyList<string> TypeNames => Names;

    public static string Name(int type) => type >= 0 && type < Names.Length ? Names[type] : "controller";

    // The types a broker line lists, in order; empty for "none"; null for any other line.
    public static IReadOnlyList<int>? Parse(string? line)
    {
        if (line == null || !line.StartsWith(BrokerPrefix, StringComparison.Ordinal)) return null;
        var list = line[BrokerPrefix.Length..];
        if (list == "none") return [];
        return TypeOf().Matches(list).Select(m => int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture)).ToList();
    }

    // What the player is told, or null when Steam Input handles no controller. Xbox and PlayStation controllers the game
    // can read itself (XInput, and its own PlayStation support), so with Steam Input on them they may still work; the
    // others reach a game only through Steam Input. Either way Steam may send the game its desktop keys, as a Steam
    // Controller pressed A to open the chat in #204.
    public static string? Message(IReadOnlyList<int> types)
    {
        if (types.Count == 0) return null;
        var only = types.Where(t => !GameReadsItself(t)).Select(Name).Distinct().ToList();
        if (only.Count > 0)
            return $"Steam Input can't reach a sandboxed game yet, so your {Join(only)} won't work as a controller in this session; " +
                $"instead, Steam may send {DesktopKeys}. Xbox controllers work." + Trade;
        return $"Steam Input is handling your {Join(types.Select(Name).Distinct().ToList())} and can't reach a sandboxed game yet. " +
            $"The game may still read the controller itself, but Steam may also send {DesktopKeys}." + Trade;
    }

    // result.json's "steamInput": the controllers' names and types, and the note (null when there is none).
    public static JsonObject Record(IReadOnlyList<int> types) => new()
    {
        ["controllers"] = new JsonArray(types.Select(t => (JsonNode?)JsonValue.Create(Name(t))).ToArray()),
        ["types"] = new JsonArray(types.Select(t => (JsonNode?)JsonValue.Create(t)).ToArray()),
        ["note"] = Message(types),
    };

    // The note in a session's result.json, or null.
    public static string? Of(JsonElement state) =>
        state.ValueKind == JsonValueKind.Object && state.TryGetProperty("steamInput", out var input) && input.ValueKind == JsonValueKind.Object &&
        input.TryGetProperty("note", out var note) && note.ValueKind == JsonValueKind.String && note.GetString() is { Length: > 0 } text ? text : null;

    static bool GameReadsItself(int type) => type is 2 or 3 or 5 or 12 or 13;

    static string Join(IReadOnlyList<string> names) =>
        names.Count == 1 ? names[0] : string.Join(", ", names.Take(names.Count - 1)) + " and " + names[^1];

    [GeneratedRegex(@"\[(\d{1,9})\]")]
    private static partial Regex TypeOf();
}
