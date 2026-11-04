using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Which items a profile's /Progression/inventory may serve (PaidContentSet says what is paid):
// - Super Credits never, but for the Forge Credits the player earned (PaidContentSet.ForgeCreditsWallet).
// - paid items (premium warbond tokens, Superstore items) only when the verified account snapshot holds them. Those it
//   holds are served even when the profile lacks them, so premium ownership always follows the latest sync, whatever
//   the player chose to import.
// - a premium warbond's items only when one of its tokens is owned.
// - a child item (a stratagem's loadout entry, a weapon's attachment) only when its parent may be served: the loadout
//   checks a stratagem's child, so a premium stratagem's child served alone unlocked the stratagem (unlock-all profiles
//   had every premium stratagem with no snapshot).
// /Progression drops the purchases on a premium warbond's pages without an owned token, so its pages show nothing bought
// (an unlock-all profile has bought every page). The profile keeps them for when the warbond is owned.
// Everything else passes through. Without a verified snapshot no paid item is served.
// A premium warbond is never sold here: one the account owns is open on every profile, and one it does not own has no
// price (FilterCatalogueResponse), so its button reads WARBOND LOCKED.
public sealed class OwnershipFilter
{
    readonly Dictionary<uint, JsonObject> owned = [];
    // Each premium warbond token's warbond's tokens: one priced in Super Credits, and most have one priced in another item.
    // An account holds the warbond with either (PremiumContent's rule too), and the client buys only the first.
    readonly Dictionary<uint, uint[]> warbondTokens = [];

    // Whether the verified snapshot holds this paid item; a premium warbond's token, whether it holds the warbond: bought on
    // live, with any of its tokens.
    public bool Owns(uint mixId) => warbondTokens.TryGetValue(mixId, out var tokens) ? tokens.Any(owned.ContainsKey) : owned.ContainsKey(mixId);

    public OwnershipFilter(PaidContentSet paid, IEnumerable<JsonObject>? snapshotItems)
    {
        Paid = paid;
        HasRealInventory = snapshotItems != null;
        foreach (var item in snapshotItems ?? [])
            if (MixId(item) is { } id && paid.IsPaid(id)) owned[id] = (JsonObject)item.DeepClone();
        foreach (var warbond in paid.Warbonds)
            foreach (var token in warbond.Tokens) warbondTokens[token] = warbond.Tokens;
    }

    public PaidContentSet Paid { get; }

    // True when the filter was built from a verified account snapshot.
    public bool HasRealInventory { get; }

    // The paid items the snapshot holds.
    public int OwnedPaidCount => owned.Count;

    public bool IsAllowed(uint mixId) => Passes(mixId) && (!Paid.TryGetParent(mixId, out var parent) || Passes(parent));

    // The item's own rules, its parent aside. A premium warbond's token and items: open when the account owns the warbond.
    bool Passes(uint mixId)
    {
        if (PaidContentSet.IsSuperCredits(mixId)) return PaidContentSet.IsForgeCredits(mixId);
        if (Paid.WarbondTokens.Contains(mixId)) return Owns(mixId);
        if (Paid.IsPaid(mixId)) return owned.ContainsKey(mixId);
        return !Paid.TryGetWarbondTokens(mixId, out var tokens) || tokens.Any(Owns);
    }

    // The inventory section as served: disallowed items removed, owned paid items the profile lacks added.
    public JsonElement FilterInventoryResponse(JsonElement inventorySection)
    {
        var node = JsonNode.Parse(inventorySection.GetRawText())!.AsObject();
        var served = new JsonArray();
        var present = new HashSet<uint>();
        foreach (var item in node["items"] as JsonArray ?? [])
        {
            if (item is not JsonObject entry) continue;
            if (MixId(entry) is { } id)
            {
                if (!IsAllowed(id)) continue;
                present.Add(id);
            }
            served.Add(entry.DeepClone());
        }
        foreach (var (id, item) in owned)
            if (!present.Contains(id)) served.Add(item.DeepClone());
        node["items"] = served;
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }

    // The item catalogue as served: a premium warbond's token the live account does not own has no price, so the client
    // offers no purchase for it (its unlock looks for the token priced in Super Credits, game.dll 0x136C270) and its button
    // reads WARBOND LOCKED (forge_credits.c); Purchases refuses one anyway. An owned warbond's token keeps its price, as on
    // the live game: the client takes the warbond as owned from the token it holds.
    public JsonElement FilterCatalogueResponse(JsonElement catalogue)
    {
        if (catalogue.ValueKind != JsonValueKind.Array) return catalogue;
        var node = JsonNode.Parse(catalogue.GetRawText())!.AsArray();
        foreach (var item in node.OfType<JsonObject>())
            if (MixId(item) is { } id && Paid.WarbondTokens.Contains(id) && !Owns(id) && item["buyPrice"] is JsonArray { Count: > 0 })
                item["buyPrice"] = new JsonArray();
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }

    // The progression section as served: purchasedSeasonPassItems without the items of a premium warbond the account does
    // not own.
    public JsonElement FilterProgressionResponse(JsonElement progressionSection)
    {
        var locked = Paid.Warbonds.Where(warbond => warbond.Tokens.Length > 0 && !warbond.Tokens.Any(Owns))
            .SelectMany(warbond => warbond.Items.Select(item => item.ItemId)).ToHashSet();
        if (locked.Count == 0 || !progressionSection.TryGetProperty("purchasedSeasonPassItems", out var bought) || bought.ValueKind != JsonValueKind.Array)
            return progressionSection;
        var node = JsonNode.Parse(progressionSection.GetRawText())!.AsObject();
        var served = new JsonArray();
        foreach (var item in bought.EnumerateArray())
            if (!(item.ValueKind == JsonValueKind.Number && item.TryGetInt64(out var id) && locked.Contains(id))) served.Add(JsonNode.Parse(item.GetRawText()));
        node["purchasedSeasonPassItems"] = served;
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }

    // Without a filter (a profile served with no ownership rules, as in tests), Super Credits still never leave (Forge Credits do).
    public static JsonElement WithoutSuperCredits(JsonElement inventorySection)
    {
        var node = JsonNode.Parse(inventorySection.GetRawText())!.AsObject();
        if (node["items"] is not JsonArray items) return inventorySection;
        foreach (var item in items.OfType<JsonObject>().Where(i => MixId(i) is { } id && PaidContentSet.IsSuperCredits(id) && !PaidContentSet.IsForgeCredits(id)).ToList())
            items.Remove(item);
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }

    // The filter for a profile's snapshot folder: ownership only from a snapshot that is sealed on this PC for steamAccount.
    // problem says why there is none.
    public static OwnershipFilter ForSnapshot(PaidContentSet paid, string? snapshotDir, ulong? steamAccount, out string? problem)
    {
        problem = null;
        if (snapshotDir == null || !HellForge.Core.InventorySync.HasSnapshot(snapshotDir)) return new OwnershipFilter(paid, null);
        using var snapshot = AccountSnapshot.Open(snapshotDir, steamAccount, out problem);
        return new OwnershipFilter(paid, snapshot?.Items());
    }

    static uint? MixId(JsonObject item) =>
        item["mixId"] is JsonValue value && value.GetValueKind() == JsonValueKind.Number && uint.TryParse(value.ToJsonString(),
            System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id) ? id : null;
}
