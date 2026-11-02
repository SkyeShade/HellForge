using System.Text.Json.Nodes;

namespace HellForge.Session;

// The network gate's census of a session (the stand-in's net_gate.c writes network-census.jsonl): which hooks were in
// place, and every connection, name lookup and program the copy attempted, by where it went; sandboxed, also what Windows
// refused the low-integrity game (scope "denied": named objects, pipes and files, SandboxRefusals). Repeats of one call, target
// and caller are written at 1, 2, 3, 10, 100... times, so the counts here are lower bounds.
static class NetworkCensus
{
    public const int ListedTargets = 40;

    public static JsonObject Summarize(string path)
    {
        var summary = new JsonObject { ["log"] = Path.GetFileName(path) };
        if (!File.Exists(path)) { summary["written"] = false; return summary; }
        var counts = new Dictionary<(string Api, string To, string Scope, string Caller), long>();
        var modules = new JsonArray();
        foreach (var line in File.ReadLines(path))
        {
            JsonNode? parsed;
            try { parsed = Tools.ParseJson(line); }
            catch (System.Text.Json.JsonException) { continue; } /* the line a crash cut short */
            if (parsed is not JsonObject row) continue;
            if ((string?)row["gate"] is { } gate)
            {
                if (gate == "census")
                {
                    summary["groups"] = row["groups"]?.DeepClone();
                    summary["hooked"] = row["hooked"]?.DeepClone();
                    summary["of"] = row["of"]?.DeepClone();
                    summary["missing"] = row["missing"]?.DeepClone();
                    summary["watchingLoads"] = row["watchingLoads"]?.DeepClone();
                }
                else if (gate == "module loaded") modules.Add((JsonNode?)JsonValue.Create((string?)row["module"]));
                continue;
            }
            var key = ((string?)row["api"] ?? "?", (string?)row["to"] ?? "?", (string?)row["scope"] ?? "?", (string?)row["caller"] ?? "?");
            var count = (long?)row["count"] ?? 1;
            counts[key] = Math.Max(counts.GetValueOrDefault(key), count);
        }
        summary["written"] = true;
        if (modules.Count > 0) summary["modulesHookedLater"] = modules;
        long Sum(Func<string, bool> scope) => counts.Where(c => scope(c.Key.Scope)).Sum(c => c.Value);
        bool Outside(string scope) => scope is "external" or "name";
        summary["loopback"] = Sum(s => s == "loopback");
        summary["outside"] = Sum(Outside);
        summary["programs"] = Sum(s => s == "program");
        summary["unspecified"] = Sum(s => s == "unspecified");
        summary["denied"] = Sum(s => s == "denied");
        JsonArray List(Func<string, bool> scope) => new(counts.Where(c => scope(c.Key.Scope)).OrderByDescending(c => c.Value).Take(ListedTargets)
            .Select(c => (JsonNode?)new JsonObject { ["api"] = c.Key.Api, ["to"] = c.Key.To, ["scope"] = c.Key.Scope, ["caller"] = c.Key.Caller, ["count"] = c.Value })
            .ToArray());
        summary["outsideTargets"] = List(Outside);
        summary["programsStarted"] = List(s => s == "program");
        summary["deniedTargets"] = List(s => s == "denied");
        return summary;
    }
}
