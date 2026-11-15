using System.Text.Json;

// The in-game test specs (tests/in-game/*.json, tests/in-game/README.md) parse as JSON and their regexes compile: a
// spec the harness refuses to read only shows up after a publish and a game start, so it is caught here instead. A
// regex is JSON text, so its backslashes are doubled ("\\d"); a single one is an invalid escape.
static class InGameSpecTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Every in-game test spec is JSON the harness can read, and every regex in it compiles", () =>
        {
            var folder = Path.Combine(RepositoryRoot(), "tests", "in-game");
            var specs = Directory.GetFiles(folder, "*.json");
            check(specs.Length > 0, "No in-game test specs found in " + folder + ".");
            foreach (var spec in specs)
            {
                JsonDocument document;
                try { document = JsonDocument.Parse(File.ReadAllText(spec)); }
                catch (JsonException error) { throw new Exception(Path.GetFileName(spec) + " is not JSON: " + error.Message); }
                using (document)
                    foreach (var pattern in Regexes(document.RootElement))
                        try { _ = new System.Text.RegularExpressions.Regex(pattern); }
                        catch (ArgumentException error) { throw new Exception(Path.GetFileName(spec) + ": the regex " + pattern + " does not compile: " + error.Message); }
            }
        });
    }

    // Every "regex" and "absent_regex" string value anywhere in the spec.
    static IEnumerable<string> Regexes(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
            foreach (var property in element.EnumerateObject())
            {
                if (property.Value.ValueKind == JsonValueKind.String && property.Name is "regex" or "absent_regex")
                    yield return property.Value.GetString()!;
                foreach (var inner in Regexes(property.Value)) yield return inner;
            }
        else if (element.ValueKind == JsonValueKind.Array)
            foreach (var item in element.EnumerateArray())
                foreach (var inner in Regexes(item)) yield return inner;
    }

    static string RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        return Environment.CurrentDirectory;
    }
}
