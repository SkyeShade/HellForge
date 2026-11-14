using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The last check on every reply that can give the client an item, run after everything that adds to it (RegisteredItems,
// ModWarbondService): the served inventory, the package definitions the client grants levels from, purchases, mastery
// rewards and mission loot. One rule for all of them, IsAllowed against the warbond tokens the saved profile holds, so a
// path that adds an id without asking (an edited catalogue, profile.json, registered-items.json) still serves nothing paid.
public sealed partial class OwnershipFilter
{
    // Whether a grant may give this item now (inventory: the saved profile's).
    public bool Grantable(uint mixId, JsonArray inventory) => IsAllowed(mixId, HeldIn(inventory));

    // The reserved ids the player may not hold now (profileInventory: the saved one), sorted. A code mod can add an item in
    // the game without asking this backend, so the loader takes back any of these the game holds (paid_guard.c).
    public uint[] Refused(JsonElement profileInventory)
    {
        var held = Held(profileInventory);
        return [.. Paid.Reserved().Where(id => !IsAllowed(id, held)).Order()];
    }

    // The inventory as served, every addition made: what may not be granted is dropped (profileInventory: the saved one).
    public JsonElement GateInventory(JsonElement inventorySection, JsonElement profileInventory)
    {
        var held = Held(profileInventory);
        var node = JsonNode.Parse(inventorySection.GetRawText());
        if (node is not JsonObject { } section || section["items"] is not JsonArray items) return inventorySection;
        var dropped = items.OfType<JsonObject>().Where(i => MixId(i) is { } id && !IsAllowed(id, held)).ToList();
        if (dropped.Count == 0) return inventorySection;
        foreach (var item in dropped) items.Remove(item);
        return JsonSerializer.SerializeToElement(section, BackendJson.Default.JsonNode);
    }

    // The progression packages as served: a level's rewards and unlockable items the player may not be granted are left
    // out, so neither the client nor a purchase grants them from a definition.
    public JsonElement FilterPackagesResponse(JsonElement packages, JsonElement profileInventory)
    {
        if (packages.ValueKind != JsonValueKind.Array) return packages;
        var held = Held(profileInventory);
        var node = JsonNode.Parse(packages.GetRawText())!.AsArray();
        var changed = false;
        foreach (var level in node.OfType<JsonObject>().SelectMany(p => p["levels"] as JsonArray ?? []).OfType<JsonObject>())
        {
            changed |= Drop(level["rewards"] as JsonArray, r => Id(r, "type") == 1 ? Id(r, "id32") : null, held);
            changed |= Drop(level["unlockableItems"] as JsonArray, u => Id(u, "mixId"), held);
        }
        return changed ? JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode) : packages;
    }

    bool Drop(JsonArray? list, Func<JsonObject, uint?> item, IReadOnlySet<uint> held)
    {
        var dropped = (list ?? []).OfType<JsonObject>().Where(e => item(e) is { } id && !IsAllowed(id, held)).ToList();
        foreach (var entry in dropped) list!.Remove(entry);
        return dropped.Count > 0;
    }

    static uint? Id(JsonObject entry, string name) => entry[name] is JsonValue v && uint.TryParse(v.ToJsonString(),
        System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id) ? id : null;

    static HashSet<uint> HeldIn(JsonArray inventory) =>
        [.. inventory.OfType<JsonObject>().Where(i => i["amount"] is JsonValue a && long.TryParse(a.ToJsonString(), System.Globalization.NumberStyles.AllowLeadingSign,
            System.Globalization.CultureInfo.InvariantCulture, out var n) && n > 0).Select(MixId).OfType<uint>()];
}
