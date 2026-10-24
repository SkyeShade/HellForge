using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public sealed partial class ProfileStore
{
    const uint EarnedSuperCredits = 3422537285;
    const uint PaidSuperCredits = 3481751602;

    static PurchaseResult PremiumError(int status, string code) =>
        new(status, new JsonObject { ["errorCode"] = code }, "premium_warbond_purchase_rejected");

    public PurchaseResult BuyPremiumWarbond(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object ||
            !PurchaseNumber(body, ["mixId"], out var mix) || mix == null ||
            !PurchaseNumber(body, ["amount"], out var quantity) || quantity != 1)
            return PremiumError(400, "HELLFORGE_INVALID_PREMIUM_PURCHASE");

        // The game also buys other items here; only premium Warbond tokens are accepted, at catalogue prices.
        using var data = LocalBackend.LoadResource("progression-data.json");
        var passes = data.RootElement.GetProperty("seasonPassDetails").EnumerateObject()
            .Select(p => p.Value).Where(p => p.GetProperty("id32").GetUInt32() != FreeWarbond &&
                p.GetProperty("premiumTokenMixIds").EnumerateArray().Any(t => t.GetUInt32() == mix)).ToArray();
        if (passes.Length != 1) return PremiumError(400, "HELLFORGE_UNKNOWN_PREMIUM_WARBOND");
        using var catalogue = LocalBackend.LoadResource("item-catalog.json");
        var entries = catalogue.RootElement.GetProperty("items").EnumerateArray()
            .Where(i => i.GetProperty("mixId").GetUInt32() == mix).ToArray();
        if (entries.Length != 1 || entries[0].GetProperty("isConsumable").GetBoolean())
            return PremiumError(400, "HELLFORGE_INVALID_PREMIUM_TOKEN");
        var prices = entries[0].GetProperty("buyPrice");
        if (prices.GetArrayLength() != 1 ||
            prices[0].GetProperty("mixId").GetUInt32() is not (EarnedSuperCredits or PaidSuperCredits) ||
            !prices[0].GetProperty("amount").TryGetInt32(out var cost) || cost <= 0)
            return PremiumError(400, "HELLFORGE_UNSUPPORTED_PREMIUM_PRICE");

        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            var relevant = passes[0].GetProperty("premiumTokenMixIds").EnumerateArray().Select(t => t.GetUInt32())
                .Concat([EarnedSuperCredits, PaidSuperCredits]).ToArray();
            if (passes[0].GetProperty("premiumTokenMixIds").EnumerateArray().Any(t => CountItem(updated, t.GetUInt32()) > 0))
                return new(200, RecoverItems(updated, body, relevant, new JsonArray()), "premium_warbond_already_owned");
            var earned = CountItem(updated, EarnedSuperCredits);
            var paid = CountItem(updated, PaidSuperCredits);
            if (earned + paid < cost) return PremiumError(409, "HELLFORGE_NOT_ENOUGH_SUPER_CREDITS");

            // The client shows both credit wallets as one; spend earned credits first.
            var earnedDebit = Math.Min(earned, cost);
            var paidDebit = cost - earnedDebit;
            var changes = new JsonArray();
            if (earnedDebit > 0)
            {
                SetItem(updated, EarnedSuperCredits, earned - earnedDebit);
                changes.Add((JsonNode)new JsonObject { ["mixId"] = EarnedSuperCredits, ["amount"] = -earnedDebit });
            }
            if (paidDebit > 0)
            {
                SetItem(updated, PaidSuperCredits, paid - paidDebit);
                changes.Add((JsonNode)new JsonObject { ["mixId"] = PaidSuperCredits, ["amount"] = -paidDebit });
            }
            SetItem(updated, mix.Value, 1);
            changes.Add((JsonNode)new JsonObject { ["mixId"] = mix.Value, ["amount"] = 1L });
            changes = RecoverItems(updated, body, relevant, changes);
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            foreach (var (section, _) in Sections) profile[section] = updated[section]!.DeepClone();

            // The reply is a root array of signed deltas (parser 0x107eb20); expectedCost is not debited separately.
            return new(200, changes, "premium_warbond_purchased");
        }
    }
}
