using System.Text.Json;

namespace HellForge.Core;

public sealed partial record StatsSchema
{
    // What enemies fire from their behaviour code as they die (mappings/death-explosions.json; native/mod-loader/
    // death_explosions.c), on their pages: a row of HellForge's per enemy and explosion, its type to fire instead (0 none,
    // a mod's by name). The explosion's own values stay on its explosion page: other things fire the same type.
    static void DeathExplosions(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.death-explosions.json")!;
        using var json = JsonDocument.Parse(stream);
        using var refs = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        using var references = JsonDocument.Parse(refs);
        var names = references.RootElement.GetProperty("explosions").EnumerateArray()
            .Concat(references.RootElement.TryGetProperty("explosionNames", out var named) ? named.EnumerateArray() : [])
            .ToDictionary(x => x.GetProperty("type").GetInt64(), x => x.GetProperty("label").GetString()!);
        var rows = json.RootElement.GetProperty("enemies").EnumerateObject()
            .SelectMany(e => e.Value.GetProperty("rows").EnumerateArray().Select(r => (Enemy: e.Name, Row: r))).ToList();
        foreach (var (enemy, row) in rows)
        {
            if (!schema.Targets["enemies"].TryGetValue(enemy, out var target)) continue;
            var type = row.GetProperty("explosion").GetInt64();
            var others = rows.Count(r => r.Enemy != enemy && r.Row.GetProperty("explosion").GetInt64() == type);
            var label = names.GetValueOrDefault(type, $"explosion {type}");
            var note = $"The game's: {label} (type {type}), {row.GetProperty("trigger").GetString()}. This enemy's own: other enemies keep theirs. " +
                $"Its radii and damage are the explosion's, shared by {(others > 0 ? $"{others} other enemies and " : "")}what else fires it: " +
                "edit them on its explosion page, or pick a mod's explosion for values of its own. None: it fires nothing. The host's enemies fire it, so the host's choice is what everyone sees.";
            var order = target.Fields.Values.Select(f => f.Order).DefaultIfEmpty(0).Max();
            AddField(target, $"death.{row.GetProperty("role").GetString()}.explosion", new StatsField(row.GetProperty("label").GetString()!, "EnemyDeathExplosion", "",
                "0x0", row.GetProperty("id").GetInt64(), 0, "u32", null, null, null, false, true)
            { Order = order + 1, Section = "Death explosion", Note = note });
        }
    }
}
