using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// DSS effects on every planet (the launcher's "DSS effects on every planet"; backend --dss-everywhere <keys>). The war
// status lists the Democracy Space Station with the effects it gives the planet it orbits ("spaceStations": [{"id32",
// "planetIndex", "activeEffectIds"}]) and each planet's own effects ("planetActiveEffects": [{"index",
// "galacticEffectId"}]). Each picked effect (HellForge.Core.DssEffects: the ones it can have, and with "current" the ones
// it has now) is added to every planet's own. The station stays where it is. It applies to whichever war is served
// (frozen, live or all-planets). The game reads what an effect id does from GET /WarSeason/GalacticWarEffects at login,
// which the startup fixtures leave empty, so the DSS effects' definitions (dss-effects.json, build 25480438) join it too.
public static class DssEverywhere
{
    public const string DefinitionsPath = "/WarSeason/GalacticWarEffects";

    // The served effect definitions with every DSS effect's definition added; one the list has already stays as it is.
    public static JsonElement Definitions(JsonElement definitions)
    {
        var list = definitions.ValueKind == JsonValueKind.Array ? JsonNode.Parse(definitions.GetRawText())!.AsArray() : [];
        var known = list.Select(d => d?["id"] is JsonValue id && id.TryGetValue<int>(out var i) ? i : (int?)null).ToHashSet();
        using var data = LocalBackend.LoadResource("dss-effects.json");
        foreach (var definition in data.RootElement.GetProperty("effects").EnumerateArray())
            if (known.Add(definition.GetProperty("id").GetInt32())) list.Add(JsonNode.Parse(definition.GetRawText()));
        using var document = JsonDocument.Parse(list.ToJsonString());
        return document.RootElement.Clone();
    }

    // The war status with the picked DSS effects on every planet it lists (planetStatus); a planet that has an effect
    // already keeps its one entry.
    public static JsonElement Status(JsonElement status, IReadOnlyCollection<string> picked)
    {
        if (status.ValueKind != JsonValueKind.Object || !status.TryGetProperty("planetStatus", out var planetStatus) ||
            planetStatus.ValueKind != JsonValueKind.Array)
            return status;
        var effects = DssEffects.Ids(picked);
        if (picked.Contains(DssEffects.Current) && status.TryGetProperty("spaceStations", out var stations) && stations.ValueKind == JsonValueKind.Array)
            foreach (var station in stations.EnumerateArray())
                if (station.ValueKind == JsonValueKind.Object && station.TryGetProperty("activeEffectIds", out var ids) && ids.ValueKind == JsonValueKind.Array)
                    foreach (var id in ids.EnumerateArray())
                        if (id.TryGetInt32(out var effect) && !effects.Contains(effect)) effects.Add(effect);
        if (effects.Count == 0) return status;
        var node = JsonNode.Parse(status.GetRawText())!.AsObject();
        if (node["planetActiveEffects"] is not JsonArray active) node["planetActiveEffects"] = active = [];
        var present = new HashSet<(int, int)>();
        foreach (var entry in active)
            if (entry?["index"] is JsonValue index && entry["galacticEffectId"] is JsonValue effect &&
                index.TryGetValue<int>(out var i) && effect.TryGetValue<int>(out var e))
                present.Add((i, e));
        foreach (var planet in planetStatus.EnumerateArray())
        {
            if (!planet.TryGetProperty("index", out var index) || !index.TryGetInt32(out var i)) continue;
            foreach (var e in effects)
                if (present.Add((i, e))) active.Add((JsonNode)new JsonObject { ["index"] = i, ["galacticEffectId"] = e });
        }
        using var document = JsonDocument.Parse(node.ToJsonString());
        return document.RootElement.Clone();
    }
}
