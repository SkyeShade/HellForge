using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Local operation and mission lifecycle. Request and reply shapes were decoded from the client:
//   POST /Operation/Create          0xaa641804  body {operations:[{id, seed, warId, planetIndex, campaignId, type,
//                                               difficulty, race, ...missions}]}, reply {operations:[{id, rewards, backupRewards}]} (parser 0x105d9d0)
//   POST /Operation/Mission/Start   0xc50b20e7  reply {missionId: string} (parser 0x107d980)
//   POST /Operation/Mission/end     0x1e555ea6  body from 0x11e9680 (missionSuccess, difficultyType, objectives,
//                                               lootedItems, heavyLootedItems, ...), reply parsed by 0x105e3e0/0x105e6b0
//   POST /Operation/Mission/Activity 0x385dc715 reply {rewards: []} (parser 0x105e010)
//   POST /Operation/Mission/SetMaxRewards, POST /Operation/Abandon: no reply fields are read.
// Rewards are local rules, not Arrowhead's: XP by difficulty, success and objectives; requisition; looted samples
// are granted as reported by the client.
public sealed class MissionService
{
    public const uint RequisitionMixId = 0xD7150EEC;
    static readonly int[] DifficultyExperience = [100, 150, 200, 300, 400, 500, 650, 800, 1000, 1200, 1500, 1800];

    readonly ProfileStore profile;
    readonly ConcurrentDictionary<long, int> operationDifficulty = new();

    public MissionService(ProfileStore profile) => this.profile = profile;

    public static bool Handles(string path) => path is "/Operation/Create" or "/Operation/Abandon" or "/Operation/Mission/Start"
        or "/Operation/Mission/end" or "/Operation/Mission/Activity" or "/Operation/Mission/SetMaxRewards";

    public (JsonNode Reply, string Outcome) Handle(string path, JsonElement body) => path switch
    {
        "/Operation/Create" => (Create(body), "operation_created"),
        "/Operation/Mission/Start" => (new JsonObject { ["missionId"] = Guid.NewGuid().ToString() }, "mission_started"),
        "/Operation/Mission/end" => (End(body), "mission_ended"),
        "/Operation/Mission/Activity" => (new JsonObject { ["rewards"] = new JsonArray() }, "mission_activity"),
        _ => (new JsonObject(), "acknowledged")
    };

    JsonObject Create(JsonElement body)
    {
        var operations = new JsonArray();
        if (body.ValueKind == JsonValueKind.Object && body.TryGetProperty("operations", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var operation in list.EnumerateArray())
            {
                if (!TryInt(operation, "id", out var id)) continue;
                operationDifficulty[id] = TryInt(operation, "difficulty", out var difficulty) ? (int)difficulty : 1;
                operations.Add((JsonNode)new JsonObject { ["id"] = id, ["rewards"] = new JsonArray(), ["backupRewards"] = new JsonArray() });
            }
        return new JsonObject { ["operations"] = operations };
    }

    JsonObject End(JsonElement body)
    {
        var success = FindBool(body, "missionSuccess") ?? FindBool(body, "succeeded") ?? false;
        var difficulty = (int)(FindInt(body, "difficultyType") ?? (FindInt(body, "operationId") is { } op &&
            operationDifficulty.TryGetValue(op, out var known) ? known : 1));
        var objectives = CountSucceededObjectives(body);
        var experience = Rewards.Experience(difficulty, success, objectives);
        var looted = SumItems(body, "lootedItems");
        var heavy = SumItems(body, "heavyLootedItems");
        var requisition = success ? Rewards.Requisition(difficulty) : 0;
        var grants = new Dictionary<uint, long>();
        foreach (var source in new[] { looted, heavy })
            foreach (var (mix, amount) in source) grants[mix] = grants.GetValueOrDefault(mix) + amount;
        if (requisition > 0) grants[RequisitionMixId] = grants.GetValueOrDefault(RequisitionMixId) + requisition;
        profile.ApplyMissionRewards(experience, grants);
        static JsonArray Items(Dictionary<uint, long> items) =>
            new(items.Select(item => (JsonNode?)new JsonObject { ["mixId"] = item.Key, ["amount"] = item.Value }).ToArray());
        return new JsonObject
        {
            ["operationId"] = FindInt(body, "operationId") ?? 0, ["missionIndex"] = FindInt(body, "missionIndex") ?? 0,
            ["missionId"] = FindString(body, "missionId") ?? "", ["experience"] = experience, ["multipliers"] = new JsonArray(),
            ["badges"] = new JsonArray(), ["campaignWon"] = false, ["impact"] = 0, ["othersImpactPlanet"] = 0,
            ["othersImpactCommunityTarget"] = 0, ["othersImpactPlanetEvent"] = 0, ["progressionPackageUpdates"] = new JsonArray(),
            ["lootedItemRewards"] = Items(looted), ["lootedHeavyItemRewards"] = Items(heavy),
            ["rewards"] = requisition > 0 ? new JsonArray((JsonNode)new JsonObject { ["mixId"] = RequisitionMixId, ["amount"] = requisition }) : new JsonArray(),
            ["repeatingPlayer"] = false
        };
    }

    public static class Rewards
    {
        public static long Experience(int difficulty, bool success, int objectives)
        {
            var base_ = DifficultyExperience[Math.Clamp(difficulty, 0, DifficultyExperience.Length - 1)];
            return (success ? base_ : base_ / 2) + 50L * Math.Clamp(objectives, 0, 20);
        }

        // Level 11 is a mod's (native/mod-loader/difficulty.c, HfDifficulty).
        public static long Requisition(int difficulty) => 200L * Math.Clamp(difficulty, 1, 11);
    }

    static int CountSucceededObjectives(JsonElement body)
    {
        var count = 0;
        foreach (var objectives in FindAll(body, "objectives"))
            if (objectives.ValueKind == JsonValueKind.Array)
                foreach (var objective in objectives.EnumerateArray())
                    if (objective.ValueKind == JsonValueKind.Object && objective.TryGetProperty("succeeded", out var s) && s.ValueKind == JsonValueKind.True) count++;
        return count;
    }

    static Dictionary<uint, long> SumItems(JsonElement body, string key)
    {
        var items = new Dictionary<uint, long>();
        foreach (var list in FindAll(body, key))
            if (list.ValueKind == JsonValueKind.Array)
                foreach (var item in list.EnumerateArray())
                    if (TryInt(item, "mixId", out var mix) && TryInt(item, "amount", out var amount) && mix > 0 && amount > 0 && mix <= uint.MaxValue)
                        items[(uint)mix] = items.GetValueOrDefault((uint)mix) + Math.Min(amount, 10_000);
        return items;
    }

    // The client's report nests per-player data; search the whole document for a key.
    static IEnumerable<JsonElement> FindAll(JsonElement node, string key)
    {
        if (node.ValueKind == JsonValueKind.Object)
            foreach (var property in node.EnumerateObject())
                foreach (var found in FindIn(property, key)) yield return found;
        else if (node.ValueKind == JsonValueKind.Array)
            foreach (var element in node.EnumerateArray())
                foreach (var nested in FindAll(element, key)) yield return nested;
    }

    // A property's value when it is the key, then the key's values inside it.
    static IEnumerable<JsonElement> FindIn(JsonProperty property, string key)
    {
        if (property.NameEquals(key)) yield return property.Value;
        foreach (var nested in FindAll(property.Value, key)) yield return nested;
    }

    static long? FindInt(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number)) return number;
        return null;
    }

    static bool? FindBool(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind is JsonValueKind.True or JsonValueKind.False) return value.ValueKind == JsonValueKind.True;
        return null;
    }

    static string? FindString(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind == JsonValueKind.String) return value.GetString();
        return null;
    }

    static bool TryInt(JsonElement node, string key, out long value)
    {
        value = 0;
        return node.ValueKind == JsonValueKind.Object && node.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value);
    }
}
