using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public sealed partial class ProfileStore
{
    // A client may attach its live inventory to a retried request. It only shapes the reply's deltas,
    // never prices, ownership or saved balances.
    static Dictionary<uint, long>? ClientInventory(JsonElement body)
    {
        if (!body.TryGetProperty("clientInventory", out var snapshot)) return null;
        if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty("version", out var version) ||
            version.ValueKind != JsonValueKind.Number || !version.TryGetInt32(out var v) || v != 1 || !snapshot.TryGetProperty("items", out var rows) ||
            rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() > 4096)
            throw new JsonException("Invalid inventory recovery snapshot.");
        var result = new Dictionary<uint, long>();
        foreach (var row in rows.EnumerateArray())
            if (row.ValueKind != JsonValueKind.Object || !row.TryGetProperty("mixId", out var mix) || mix.ValueKind != JsonValueKind.Number || !mix.TryGetUInt32(out var id) ||
                !row.TryGetProperty("amount", out var count) || count.ValueKind != JsonValueKind.Number || !count.TryGetUInt32(out var amount) || !result.TryAdd(id, amount))
                throw new JsonException("Invalid or duplicate recovery inventory entry.");
        return result;
    }

    // The deltas that bring the client to the saved inventory; without a snapshot, the normal reply.
    static JsonArray RecoverItems(JsonObject state, JsonElement request, IEnumerable<uint> relevant,
        JsonArray legacy, bool medalClaim = false, int medalCost = 0, bool positiveOnly = false)
    {
        var client = ClientInventory(request);
        if (client == null) return legacy;
        if (medalClaim)
        {
            if (!request.TryGetProperty("expectedCost", out var expected) || expected.ValueKind != JsonValueKind.Array ||
                expected.GetArrayLength() > 1 || (medalCost > 0 && expected.GetArrayLength() != 1))
                throw new JsonException("Medal recovery requires the native expected cost.");
            if (expected.GetArrayLength() == 1 && (expected[0].ValueKind != JsonValueKind.Object ||
                !expected[0].TryGetProperty("mixId", out var wallet) || wallet.ValueKind != JsonValueKind.Number || !wallet.TryGetUInt32(out var w) || w != MedalWallet ||
                !expected[0].TryGetProperty("amount", out var cost) || cost.ValueKind != JsonValueKind.Number || !cost.TryGetInt32(out var c) || c != medalCost))
                throw new JsonException("Medal recovery cost differs from the catalogue.");
        }
        var changes = new JsonArray();
        foreach (var mix in relevant.Distinct())
        {
            if (!client.TryGetValue(mix, out var before)) throw new JsonException("Recovery snapshot misses a required inventory entry.");
            // The medal completion debits expectedCost before applying positive grants.
            if (medalClaim && mix == MedalWallet) before = Math.Max(0, before - medalCost);
            var delta = CountItem(state, mix) - before;
            if (delta < 0 && medalClaim) throw new JsonException("Medal client state requires a full reload.");
            if (positiveOnly) delta = Math.Max(0, delta);
            if (delta is < int.MinValue or > int.MaxValue) throw new JsonException("Recovery delta exceeds the native signed field.");
            if (delta != 0) changes.Add((JsonNode)new JsonObject { ["mixId"] = mix, ["amount"] = delta });
        }
        return changes;
    }

    // Rewards are saved with a receipt for the mission id, so a retried end pays nothing again.
    public JsonObject ApplyMissionOnce(string missionId, long experience, IReadOnlyDictionary<uint, long> items,
        JsonObject reply, JsonElement request)
    {
        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            var receipts = updated["progression"]!["missionReceipts"] as JsonObject ?? new JsonObject();
            var previous = receipts[missionId] as JsonObject;
            if (previous != null) return MissionRecovery(updated, previous, reply, request, duplicate: true);
            ApplyRewardAmounts(updated, experience, items);
            var receipt = new JsonObject { ["items"] = new JsonArray(items.Keys.Select(k => (JsonNode?)JsonValue.Create(k)).ToArray()) };
            var answer = MissionRecovery(updated, receipt, reply, request, duplicate: false);
            receipts[missionId] = receipt;
            updated["progression"]!["missionReceipts"] = receipts;
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            foreach (var (section, _) in Sections) profile[section] = updated[section]!.DeepClone();
            return answer;
        }
    }

    static JsonObject MissionRecovery(JsonObject state, JsonObject receipt, JsonObject originalReply, JsonElement request, bool duplicate)
    {
        var reply = originalReply.DeepClone().AsObject();
        if (!request.TryGetProperty("clientInventory", out var snapshot))
        {
            if (duplicate) { reply["experience"] = 0; foreach (var name in new[] { "rewards", "lootedItemRewards", "lootedHeavyItemRewards" }) reply[name] = new JsonArray(); }
            return reply;
        }
        if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty("experience", out var xp) || xp.ValueKind != JsonValueKind.Number || !xp.TryGetUInt32(out var beforeXp))
            throw new JsonException("Mission recovery requires live experience.");
        var relevant = receipt["items"]!.AsArray().Select(i => i!.GetValue<uint>()).ToArray();
        var changes = RecoverItems(state, request, relevant, new JsonArray(), positiveOnly: true);
        reply["experience"] = Math.Max(0, (Whole(state["progression"]!["experience"]) ?? 0) - beforeXp);
        reply["rewards"] = new JsonArray(changes.OfType<JsonObject>().Where(i => i["mixId"]!.GetValue<uint>() == MissionService.RequisitionMixId).Select(i => i.DeepClone()).ToArray());
        reply["lootedHeavyItemRewards"] = new JsonArray();
        reply["lootedItemRewards"] = new JsonArray(changes.OfType<JsonObject>().Where(i => i["mixId"]!.GetValue<uint>() != MissionService.RequisitionMixId).Select(i => i.DeepClone()).ToArray());
        return reply;
    }
}
