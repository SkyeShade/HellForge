using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// A code mod's spend of the player's currency (SDK spend_currency; native/mod-loader/currency.c). The profile is taken
// from here first and the loader takes the game's own copy after, so the shown and saved balances stay equal. Only the
// currencies a mod's warbond may price in, and only ever taken: nothing a mod sends can add to a balance.
public static class CurrencySpend
{
    public const string Path = "/hellforge/spend";
    public const long MaxAmount = ModWarbonds.MaxCost;
    const int MaxText = 128;

    // POST {"mod", "currency" (a mixId), "amount", "reason"}: {"result": "spent" or "too_little", "balance"}.
    public static (int Status, JsonNode Reply, string Outcome) Handle(ProfileStore? profile, JsonElement body)
    {
        if (profile == null) return (409, Problem("HELLFORGE_NO_PROFILE"), "currency_spend_unavailable");
        if (Request(body) is not { } request) return (400, Problem("HELLFORGE_BAD_SPEND"), "currency_spend_refused");
        var result = "too_little";
        long balance = 0;
        profile.TryChange((_, inventory) =>
        {
            balance = ProfileStore.Held(inventory, request.Currency);
            if (balance < request.Amount) return false;
            balance += ProfileStore.AddItem(inventory, request.Currency, -request.Amount);
            result = "spent";
            return true;
        });
        return (200, new JsonObject { ["result"] = result, ["balance"] = balance }, "currency_" + result);
    }

    readonly record struct Spend(uint Currency, long Amount);

    // A well-formed request for a currency mods may spend, or null.
    static Spend? Request(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object ||
            !body.TryGetProperty("currency", out var currency) || !currency.TryGetUInt32(out var mixId) ||
            !body.TryGetProperty("amount", out var amount) || !amount.TryGetInt64(out var taken) ||
            !Text(body, "mod") || !Text(body, "reason"))
            return null;
        if (taken < 1 || taken > MaxAmount || !ModWarbonds.Currencies.Values.Contains(mixId)) return null;
        return new Spend(mixId, taken);
    }

    static bool Text(JsonElement body, string name) =>
        body.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String && value.GetString()!.Length is > 0 and <= MaxText;

    static JsonObject Problem(string code) => new() { ["errorCode"] = code };
}
