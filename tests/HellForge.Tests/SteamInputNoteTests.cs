using System.Text.Json;
using System.Text.RegularExpressions;
using HellForge.Core;

// Steam Input and a sandboxed game (SteamInputNote, docs/steam-bridge.md "Controllers and Steam Input"): the broker's
// controllers line (native/steam-cloud/broker_input.c, whose test input_test.c writes the lines read here), the note
// the player is told, and result.json's "steamInput" as the launcher and the CLI read it.
static class SteamInputNoteTests
{
    const string Prefix = SteamInputNote.BrokerPrefix;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Steam Input: the session reads the broker's controllers line, and only that line", () =>
        {
            check(SteamInputNote.Parse(Prefix + "none") is { Count: 0 }, "\"none\" is an empty list.");
            check(SteamInputNote.Parse(Prefix + "Steam Controller [1] as XInput pad 0") is [1], "One Steam Controller.");
            check(SteamInputNote.Parse(Prefix + "Steam Controller [1] as XInput pad 0, PS5 controller [13], Steam Deck controls [14] as XInput pad 1")
                is [1, 13, 14], "Three controllers, in order.");
            check(SteamInputNote.Parse(Prefix + "controller [99]") is [99], "An unknown type keeps its number.");
            foreach (var other in new[] { null, "", "broker: Steam's overlay is not in the broker", "broker: game connected",
                "broker: XInput pad 0 in the broker changed 15 times in the last minute: Steam Input's emulated pad reaches the broker, not the game",
                " " + Prefix + "none", "broker: Steam Input controllers:none" })
                check(SteamInputNote.Parse(other) == null, $"Not the controllers line: \"{other}\".");
        });

        test("Steam Input: the broker's names and lines are the ones the session knows", () =>
        {
            var source = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "native", "steam-cloud", "broker_input.c"));
            var table = Regex.Match(source, @"g_typeNames\[\] = \{(?<names>.*?)\};", RegexOptions.Singleline);
            var names = Regex.Matches(table.Groups["names"].Value, "\"([^\"]*)\"").Select(m => m.Groups[1].Value).ToList();
            check(table.Success && names.SequenceEqual(SteamInputNote.TypeNames), "broker_input.c's type names are SteamInputNote's, in order.");
            var lines = Regex.Matches(File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "native", "steam-cloud", "test", "input_test.c")),
                "\"(" + Regex.Escape(Prefix) + "[^\"]*)\"").Select(m => m.Groups[1].Value).ToList();
            check(lines.Count >= 3, "input_test.c checks controllers lines.");
            foreach (var line in lines)
                check(SteamInputNote.Parse(line) is { } types && types.Count == Regex.Matches(line, @"\[\d+\]").Count &&
                    types.All(t => line.Contains(SteamInputNote.Name(t) + " [" + t + "]", StringComparison.Ordinal)), $"Read as written: {line}");
        });

        test("Steam Input: the player is told which controllers won't reach a sandboxed game", () =>
        {
            check(SteamInputNote.Message([]) == null, "No controller: no note.");
            var steam = SteamInputNote.Message([1])!;
            check(steam.Contains("your Steam Controller won't work as a controller in this session", StringComparison.Ordinal) &&
                steam.Contains("A presses Enter, which opens the chat", StringComparison.Ordinal) && steam.Contains("Xbox controllers work.", StringComparison.Ordinal) &&
                steam.Contains("Settings > Sandbox off", StringComparison.Ordinal), "A Steam Controller won't work, and why A opens the chat.");
            check(SteamInputNote.Message([1, 14, 1])!.Contains("your Steam Controller and Steam Deck controls won't", StringComparison.Ordinal), "Two kinds, each named once.");
            check(SteamInputNote.Message([10, 4, 99])!.Contains("your Switch Pro controller, generic gamepad and controller won't", StringComparison.Ordinal), "Three, joined.");
            var mixed = SteamInputNote.Message([13, 1])!;
            check(mixed.Contains("your Steam Controller won't", StringComparison.Ordinal) && !mixed.Contains("PS5", StringComparison.Ordinal),
                "With a PS5 controller too, the one that cannot work is named.");
            foreach (var own in new[] { 2, 3, 5, 12, 13 })
            {
                var note = SteamInputNote.Message([own])!;
                check(note.StartsWith("Steam Input is handling your " + SteamInputNote.Name(own) + " and can't reach a sandboxed game yet.", StringComparison.Ordinal) &&
                    note.Contains("The game may still read the controller itself", StringComparison.Ordinal) && !note.Contains("won't work", StringComparison.Ordinal),
                    $"An Xbox or PlayStation controller ({own}) may still work.");
            }
            check(SteamInputNote.Name(-1) == "controller" && SteamInputNote.Name(15) == "controller" && SteamInputNote.Name(0) == "unknown controller", "Names out of range.");
        });

        test("Steam Input: result.json's note, as the launcher and the CLI read it", () =>
        {
            JsonElement State(string json) => JsonDocument.Parse(json).RootElement.Clone();
            var record = SteamInputNote.Record([1, 13]);
            check(record.ToJsonString() is var text && text.Contains("\"controllers\":[\"Steam Controller\",\"PS5 controller\"]", StringComparison.Ordinal) &&
                text.Contains("\"types\":[1,13]", StringComparison.Ordinal), "The controllers' names and types.");
            var state = State("{\"status\":\"observing\",\"steamInput\":" + record.ToJsonString() + "}");
            check(SteamInputNote.Of(state) == SteamInputNote.Message([1, 13]), "The note comes back.");
            check(SteamInputNote.Of(State("{\"steamInput\":" + SteamInputNote.Record([]).ToJsonString() + "}")) == null, "None: no note (null in the file).");
            foreach (var json in new[] { "{}", "{\"steamInput\":null}", "{\"steamInput\":{\"note\":\"\"}}", "{\"steamInput\":{\"note\":5}}", "[]" })
                check(SteamInputNote.Of(State(json)) == null, $"No note in {json}.");
        });
    }
}
