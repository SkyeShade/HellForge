using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The Battle for Super Earth (docs/roadmap.md, "Super Earth missions"; HD2Mods research/cyberstan): missions in Super
// Earth's mega cities against an enemy. Its map has no drop points (game.dll 0x12D88FB), so its operations come from its 7
// regions, cities (kind 1): the region builder (0x11E44D0) makes one per difficulty for an available Status region row on
// a planet with a campaign, against the attacker of a city Super Earth holds (the planet's owner, else an event's race,
// else its campaign's: 0x174C2A0), as city operations of type 8 (0x174AB50), which every enemy has. So the planet stays
// Super Earth's, with one campaign of type 3 ("BATTLE FOR SUPER EARTH") of the enemy's race, nothing else running there,
// and each city a row: Super Earth's, at full health, available.
public static class SuperEarthBattle
{
    // The campaign type the game titles "BATTLE FOR SUPER EARTH" (0x174B700).
    public const int CampaignType = 3;
    const int SuperEarth = PlanetModifiers.SuperEarth;

    // The planet attacked by the enemy (race): its other campaigns, events, attacks and joint operations go, it gets the
    // battle's campaign (campaignId) and its cities' rows. warInfo gives its regions (the frozen war's without it).
    public static void Serve(JsonObject status, int planet, int race, int campaignId, JsonElement? warInfo = null)
    {
        ClearPlanet(status, planet, campaigns: true);
        var campaigns = status["campaigns"] as JsonArray ?? (JsonArray)(status["campaigns"] = new JsonArray());
        campaigns.Add((JsonNode)new JsonObject { ["id"] = campaignId, ["planetIndex"] = planet, ["type"] = CampaignType, ["count"] = 1, ["race"] = race });
        var rows = status["planetRegions"] as JsonArray ?? (JsonArray)(status["planetRegions"] = new JsonArray());
        foreach (var row in rows.OfType<JsonObject>().Where(r => Int(r["planetIndex"]) == planet).ToList()) rows.Remove(row);
        foreach (var (region, health) in Cities(warInfo ?? OpenWar.Frozen().WarInfo, planet))
            rows.Add((JsonNode)new JsonObject
            {
                ["planetIndex"] = planet, ["regionIndex"] = region, ["owner"] = SuperEarth, ["health"] = health,
                ["regerPerSecond"] = (float)(health / 360000.0), ["availabilityFactor"] = 1, ["isAvailable"] = true, ["players"] = 0,
            });
    }

    // The planet's events with their joint operations, the attacks from or on it, and (campaigns) its campaigns go.
    public static void ClearPlanet(JsonObject status, int planet, bool campaigns)
    {
        var operations = RemoveEvents(status, planet);
        if (status["jointOperations"] is JsonArray joint) Remove(joint, j => Int(j["planetIndex"]) == planet || Int(j["id"]) is { } id && operations.Contains(id));
        if (status["planetAttacks"] is JsonArray attacks) Remove(attacks, a => Int(a["target"]) == planet || Int(a["source"]) == planet);
        if (campaigns && status["campaigns"] is JsonArray list) Remove(list, c => Int(c["planetIndex"]) == planet);
    }

    // The planet's events go; returns their joint operations' ids.
    static HashSet<int> RemoveEvents(JsonObject status, int planet)
    {
        var operations = new HashSet<int>();
        if (status["planetEvents"] is not JsonArray events) return operations;
        foreach (var e in events.OfType<JsonObject>().Where(e => Int(e["planetIndex"]) == planet).ToList())
        {
            operations.UnionWith((e["jointOperationIds"] as JsonArray ?? []).Select(Int).OfType<int>());
            events.Remove(e);
        }
        return operations;
    }

    // The planet's regions in warInfo, with their full health.
    static IEnumerable<(int Region, long Health)> Cities(JsonElement warInfo, int planet)
    {
        if (warInfo.ValueKind != JsonValueKind.Object || !warInfo.TryGetProperty("planetRegions", out var regions) || regions.ValueKind != JsonValueKind.Array) yield break;
        foreach (var r in regions.EnumerateArray())
            if (r.TryGetProperty("planetIndex", out var at) && at.TryGetInt32(out var index) && index == planet &&
                r.TryGetProperty("regionIndex", out var ri) && ri.TryGetInt32(out var region) && r.TryGetProperty("maxHealth", out var h) && h.TryGetInt64(out var health))
                yield return (region, Math.Max(health, 1));
    }

    static void Remove(JsonArray list, Func<JsonObject, bool> match)
    {
        foreach (var item in list.OfType<JsonObject>().Where(match).ToList()) list.Remove(item);
    }

    static int? Int(JsonNode? value) => value is JsonValue v && v.GetValueKind() == JsonValueKind.Number && v.TryGetValue<int>(out var i) ? i : null;
}
