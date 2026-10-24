using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public sealed partial class ProfileStore
{
    public sealed record StorePurchaseResult(int Status, JsonNode Body, string Outcome);
    static StorePurchaseResult StoreError(int status, string code) =>
        new(status, new JsonObject { ["errorCode"] = code }, "storefront_purchase_rejected");
    static JsonObject StoreReply(JsonArray changes) => new() { ["items"] = changes, ["packageProgress"] = new JsonArray() };

    // One-time unlocks exposed by the local storefront, priced by the same catalogue
    // served to the client. The request cannot set the cost, reward or prerequisites.
    public StorePurchaseResult BuyStoreItem(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object ||
            !body.TryGetProperty("salesId", out var sale) || sale.ValueKind != JsonValueKind.String ||
            !Guid.TryParseExact(sale.GetString(), "D", out var saleId) ||
            !PurchaseNumber(body, ["mixId"], out var requestedMix) ||
            !PurchaseNumber(body, ["amount"], out var quantity) || quantity is not (null or 1))
            return StoreError(400, "HELLFORGE_INVALID_STORE_PURCHASE");

        using var data = LocalBackend.LoadResource("progression-data.json");
        var offers = data.RootElement.GetProperty("storeFront").EnumerateArray()
            .SelectMany(s => s.GetProperty("sections").EnumerateArray())
            .SelectMany(s => s.GetProperty("items").EnumerateArray())
            .Where(i => Guid.TryParse(i.GetProperty("salesId").GetString(), out var id) && id == saleId).ToArray();
        if (offers.Length != 1) return StoreError(400, "HELLFORGE_UNKNOWN_STORE_OFFER");
        var mix = offers[0].GetProperty("mixId").GetUInt32();
        if (requestedMix != null && requestedMix != mix) return StoreError(400, "HELLFORGE_STORE_ITEM_MISMATCH");
        using var catalogue = LocalBackend.LoadResource("item-catalog.json");
        var entries = catalogue.RootElement.GetProperty("items").EnumerateArray()
            .Where(i => i.GetProperty("mixId").GetUInt32() == mix).ToArray();
        if (entries.Length != 1 || entries[0].GetProperty("isConsumable").GetBoolean())
            return StoreError(400, "HELLFORGE_UNSUPPORTED_STORE_ITEM");
        var item = entries[0];
        var relevant = item.GetProperty("buyPrice").EnumerateArray().Select(p => p.GetProperty("mixId").GetUInt32()).Append(mix).ToList();
        if (relevant.Any(i => i is EarnedSuperCredits or PaidSuperCredits)) relevant.AddRange([EarnedSuperCredits, PaidSuperCredits]);
        var costs = new Dictionary<uint, long>();
        foreach (var price in item.GetProperty("buyPrice").EnumerateArray())
        {
            uint currency = price.GetProperty("mixId").GetUInt32();
            if (!price.GetProperty("amount").TryGetInt64(out var amount) || amount <= 0 || amount > uint.MaxValue ||
                currency is not (3608481516 or EarnedSuperCredits or PaidSuperCredits or 3992382197 or 2985106497 or 3670075867) ||
                !costs.TryAdd(currency, amount))
                return StoreError(400, "HELLFORGE_UNSUPPORTED_STORE_PRICE");
        }
        if (costs.Count == 0) return StoreError(400, "HELLFORGE_STORE_ITEM_NOT_FOR_SALE");

        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            if (CountItem(updated, mix) > 0) return new(200, StoreReply(RecoverItems(updated, body, relevant, new JsonArray())), "storefront_already_owned");
            var xp = Whole(updated["progression"]!["experience"]) ?? 0;
            var level = data.RootElement.GetProperty("levelSpec").EnumerateArray()
                .Where(l => l.GetProperty("requiredExperience").GetInt64() <= xp)
                .Select(l => l.GetProperty("level").GetInt32()).DefaultIfEmpty(1).Max();
            if (level < item.GetProperty("requiredLevel").GetInt32())
                return StoreError(403, "HELLFORGE_STORE_LEVEL_REQUIRED");
            if (item.GetProperty("requiredItems").EnumerateArray().Any(i => CountItem(updated, i.GetUInt32()) < 1))
                return StoreError(403, "HELLFORGE_STORE_PREREQUISITE_REQUIRED");

            // Paid-credit prices use the combined wallet displayed by the native UI.
            var creditCost = costs.GetValueOrDefault(EarnedSuperCredits) + costs.GetValueOrDefault(PaidSuperCredits);
            costs.Remove(EarnedSuperCredits); costs.Remove(PaidSuperCredits);
            var earned = CountItem(updated, EarnedSuperCredits);
            if (creditCost > earned + CountItem(updated, PaidSuperCredits) ||
                costs.Any(p => CountItem(updated, p.Key) < p.Value))
                return StoreError(409, "HELLFORGE_NOT_ENOUGH_STORE_CURRENCY");
            var earnedDebit = Math.Min(earned, creditCost);
            if (earnedDebit > 0) costs.Add(EarnedSuperCredits, earnedDebit);
            if (creditCost > earnedDebit) costs.Add(PaidSuperCredits, creditCost - earnedDebit);

            var changes = new JsonArray();
            foreach (var (currency, cost) in costs)
            {
                SetItem(updated, currency, CountItem(updated, currency) - cost);
                changes.Add((JsonNode)new JsonObject { ["mixId"] = currency, ["amount"] = -cost });
            }
            SetItem(updated, mix, 1);
            changes.Add((JsonNode)new JsonObject { ["mixId"] = mix, ["amount"] = 1L });
            changes = RecoverItems(updated, body, relevant, changes);
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            foreach (var (section, _) in Sections) profile[section] = updated[section]!.DeepClone();
            // Build 25480438: 0x12ca561 parses the root object with 0x105a470.
            // 0x1374f60 applies signed returned items; expectedCost is NOT debited again.
            // This wrapper differs from premium checkout's root array and medal claims'
            // positive-only grants despite sharing the medal parser's object layout.
            return new(200, StoreReply(changes), "storefront_item_purchased");
        }
    }
}
