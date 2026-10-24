using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public sealed partial class ProfileStore
{
    const uint MedalWallet = 1276729503;
    const uint FreeWarbond = 1929468580;

    public sealed record PurchaseResult(int Status, JsonNode Body, string Outcome);
    static PurchaseResult PurchaseError(int status, string code) => new(status, new JsonObject { ["errorCode"] = code }, "warbond_purchase_rejected");

    static bool PurchaseNumber(JsonElement body, string[] fields, out uint? result)
    {
        result = null;
        foreach (var name in fields)
        {
            if (!body.TryGetProperty(name, out var value)) continue;
            uint number;
            if (value.ValueKind == JsonValueKind.Number && value.TryGetUInt32(out number)) { }
            else if (value.ValueKind == JsonValueKind.String && uint.TryParse(value.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out number)) { }
            else return false;
            if (result is { } previous && previous != number) return false;
            result = number;
        }
        return true;
    }

    static long CountItem(JsonObject state, uint mix) => ((JsonArray)state["inventory"]!["items"]!).OfType<JsonObject>()
        .Where(i => Whole(i["mixId"]) == mix).Sum(i => Whole(i["amount"]) ?? 0);

    static void SetItem(JsonObject state, uint mix, long amount)
    {
        var list = (JsonArray)state["inventory"]!["items"]!;
        var entries = list.OfType<JsonObject>().Where(i => Whole(i["mixId"]) == mix).ToArray();
        if (entries.Length > 1) throw new InvalidDataException("Duplicate inventory currency entries.");
        var row = entries.FirstOrDefault();
        if (row == null) list.Add((JsonNode)(row = new JsonObject { ["mixId"] = mix, ["isDiscovered"] = true }));
        row["amount"] = Math.Clamp(amount, 0, uint.MaxValue);
    }

    // Prices, ownership and rewards come from this build's embedded catalogue.
    // A request can choose an item; it cannot choose its cost or grant contents.
    public PurchaseResult BuyWarbondItem(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object ||
            !PurchaseNumber(body, ["seasonPassItemId", "seasonPassItemId32", "itemId", "itemId32", "id"], out var itemId) || itemId == null ||
            !PurchaseNumber(body, ["seasonPassId", "seasonPassId32", "seasonPass", "warbondId"], out var passId))
            return PurchaseError(400, "HELLFORGE_INVALID_WARBOND_PURCHASE");

        using var data = LocalBackend.LoadResource("progression-data.json");
        var candidates = new List<(uint Pass, JsonElement Definition, JsonElement Page, JsonElement Item)>();
        foreach (var detail in data.RootElement.GetProperty("seasonPassDetails").EnumerateObject())
        {
            var definition = detail.Value;
            var id = definition.GetProperty("id32").GetUInt32();
            if (passId != null && passId != id) continue;
            foreach (var page in definition.GetProperty("seasonPassPages").EnumerateArray())
                foreach (var item in page.GetProperty("seasonPassItems").EnumerateArray())
                    if (item.GetProperty("seasonPassItemId").GetUInt32() == itemId)
                        candidates.Add((id, definition, page, item));
        }
        if (candidates.Count != 1) return PurchaseError(400, "HELLFORGE_UNKNOWN_WARBOND_ITEM");
        var chosen = candidates[0];
        var mix = chosen.Item.GetProperty("itemMixId").GetUInt32();
        var cost = chosen.Item.GetProperty("medalCost").GetInt32();
        if (cost < 0 || chosen.Item.GetProperty("isRepeatable").GetBoolean())
            return PurchaseError(400, "HELLFORGE_UNSUPPORTED_REPEATABLE_PURCHASE");
        var spentMix = chosen.Definition.GetProperty("medalsSpentMixId").GetUInt32();
        using var catalogData = LocalBackend.LoadResource("item-catalog.json");
        var catalog = catalogData.RootElement.GetProperty("items").EnumerateArray()
            .GroupBy(i => i.GetProperty("mixId").GetUInt32()).ToDictionary(g => g.Key, g => g.First());
        if (!catalog.ContainsKey(mix)) return PurchaseError(400, "HELLFORGE_UNKNOWN_WARBOND_REWARD");
        var grants = new List<(uint Mix, long Amount)>();
        var package = data.RootElement.GetProperty("itemPackages").EnumerateArray()
            .FirstOrDefault(p => p.GetProperty("itemMixId32").GetUInt32() == mix);
        if (package.ValueKind == JsonValueKind.Object)
            foreach (var grant in package.GetProperty("items").EnumerateArray())
                grants.Add((grant.GetProperty("mixId").GetUInt32(), grant.GetProperty("amount").GetInt64()));
        else grants.Add((mix, 1));
        if (grants.Any(g => !catalog.ContainsKey(g.Mix) || g.Amount <= 0))
            return PurchaseError(400, "HELLFORGE_INVALID_REWARD_PACKAGE");

        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            var relevant = grants.Select(g => g.Mix).Concat([MedalWallet, spentMix]).ToArray();
            var progression = updated["progression"]!.AsObject();
            var claimed = progression["purchasedSeasonPassItems"] as JsonArray ?? new JsonArray();
            progression["purchasedSeasonPassItems"] = claimed;
            if (claimed.Any(i => Whole(i) == itemId))
                return new PurchaseResult(200, PurchaseReply(updated, chosen.Pass, itemId.Value, mix,
                    RecoverItems(updated, body, relevant, [], medalClaim: true, medalCost: cost), true), "warbond_already_purchased");
            bool owned = chosen.Pass == FreeWarbond || chosen.Definition.GetProperty("premiumTokenMixIds").EnumerateArray()
                .Any(t => CountItem(updated, t.GetUInt32()) > 0);
            if (!owned) return PurchaseError(403, "HELLFORGE_WARBOND_NOT_OWNED");
            if (CountItem(updated, spentMix) < chosen.Page.GetProperty("totalMedalsRequired").GetInt64())
                return PurchaseError(403, "HELLFORGE_WARBOND_PAGE_LOCKED");
            if (CountItem(updated, MedalWallet) < cost)
                return PurchaseError(409, "HELLFORGE_NOT_ENOUGH_MEDALS");

            SetItem(updated, MedalWallet, CountItem(updated, MedalWallet) - cost);
            SetItem(updated, spentMix, checked(CountItem(updated, spentMix) + cost));
            // The client applies root items as positive grants and debits expectedCost itself (completion 0x13751d0).
            // The spent-medals counter goes in too, or later pages stay locked this session.
            var awarded = new JsonArray();
            if (cost > 0) awarded.Add((JsonNode)new JsonObject { ["mixId"] = spentMix, ["amount"] = (long)cost });
            foreach (var grant in grants)
            {
                var entry = catalog[grant.Mix];
                var before = CountItem(updated, grant.Mix);
                var total = entry.GetProperty("isConsumable").GetBoolean() ? checked(before + grant.Amount) : Math.Max(before, grant.Amount);
                if (entry.TryGetProperty("maxCap", out var cap)) total = Math.Min(total, cap.GetInt64());
                SetItem(updated, grant.Mix, total);
                awarded.Add((JsonNode)new JsonObject { ["mixId"] = grant.Mix, ["amount"] = Math.Max(0, total - before) });
            }
            claimed.Add((JsonNode)JsonValue.Create(itemId.Value));
            progression["activeSeasonPass"] = chosen.Pass;
            awarded = RecoverItems(updated, body, relevant, awarded, medalClaim: true, medalCost: cost);
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            foreach (var (section, _) in Sections) profile[section] = updated[section]!.DeepClone();
            return new PurchaseResult(200, PurchaseReply(updated, chosen.Pass, itemId.Value, mix, awarded, false), "warbond_item_purchased");
        }
    }

    static JsonObject PurchaseReply(JsonObject state, uint pass, uint item, uint mix, JsonArray grants, bool duplicate) => new()
    {
        ["seasonPassId"] = pass, ["seasonPassItemId"] = item, ["itemMixId"] = mix,
        ["success"] = true, ["alreadyPurchased"] = duplicate,
        ["rewards"] = grants.DeepClone(), ["items"] = grants.DeepClone(),
        ["purchasedSeasonPassItems"] = state["progression"]!["purchasedSeasonPassItems"]!.DeepClone(),
        ["inventory"] = state["inventory"]!.DeepClone(), ["progression"] = state["progression"]!.DeepClone()
    };
}
