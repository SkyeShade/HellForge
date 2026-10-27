using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Which items a profile's /Progression/inventory may serve (PaidContentSet says what is paid):
// - Super Credits never.
// - paid items (premium warbond tokens, Superstore items) only when the verified account snapshot holds them. Those it
//   holds are served even when the profile lacks them, so premium ownership always follows the latest sync, whatever
//   the player chose to import.
// - a premium warbond's items only when one of its tokens is owned.
// Everything else passes through. Without a verified snapshot no paid item is served.
public sealed class OwnershipFilter
{
    readonly Dictionary<uint, JsonObject> owned = [];

    public OwnershipFilter(PaidContentSet paid, IEnumerable<JsonObject>? snapshotItems)
    {
        Paid = paid;
        HasRealInventory = snapshotItems != null;
        foreach (var item in snapshotItems ?? [])
            if (MixId(item) is { } id && paid.IsPaid(id)) owned[id] = (JsonObject)item.DeepClone();
    }

    public PaidContentSet Paid { get; }

    // True when the filter was built from a verified account snapshot.
    public bool HasRealInventory { get; }

    // The paid items the snapshot holds.
    public int OwnedPaidCount => owned.Count;

    public bool IsAllowed(uint mixId)
    {
        if (PaidContentSet.IsSuperCredits(mixId)) return false;
        if (Paid.IsPaid(mixId)) return owned.ContainsKey(mixId);
        return !Paid.TryGetWarbondTokens(mixId, out var tokens) || tokens.Any(owned.ContainsKey);
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

    // Without a filter (a profile served with no ownership rules, as in tests), Super Credits still never leave.
    public static JsonElement WithoutSuperCredits(JsonElement inventorySection)
    {
        var node = JsonNode.Parse(inventorySection.GetRawText())!.AsObject();
        if (node["items"] is not JsonArray items) return inventorySection;
        foreach (var item in items.OfType<JsonObject>().Where(i => MixId(i) is { } id && PaidContentSet.IsSuperCredits(id)).ToList())
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
