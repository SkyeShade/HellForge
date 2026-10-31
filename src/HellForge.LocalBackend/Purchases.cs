using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// What a player buys with what they earned: warbond items with medals, the requisition and samples store, ship module
// levels and weapon attachments. Each one is checked against the game's own prices (progression-data.json, the
// catalogue the game was served) and the profile's balance, then saved. Request shapes from the client's builders,
// reply shapes from its reply cases (game.dll; HD2Mods research/galactic-war/progression):
//   POST /SeasonPass/item/buy  0x15c6d819  builder 0x12bcc94  {expectedCost:[{mixId, amount}], seasonPassItemId, warId}
//                              reply {items:[{mixId, amount}]} (parser 0x105a470), which the client adds; it takes the
//                              medals itself. The warbond's medalsSpent item is in the reply too: it is what opens pages.
//   POST /Progression/buy/v2   0xb04e1beb  builder 0x12c1f65  {mixId, amount, warId, expectedCost:[{mixId, amount}]}
//                              reply [{mixId, amount}] (an array, parser 0x107eb20): the client adds the positive amounts
//                              and takes the negative ones (0x12c9170), so the price comes back negative.
//   POST /Progression/ProgressionPackage/AcquireLevel  0x35c47a6d  {id32, level, expectedCost} (ship modules)
//   POST /Progression/ProgressionPackage/AcquireUnlockableItem  0x1b13fab5  {packageId32, targetLevel, itemMixId,
//                              expectedCost} (weapon attachments). Neither reply is parsed (0xb64d30, 0xb666a0).
// Super Credits are never spent or granted, and nothing paid (OwnershipFilter's rules) is sold: a premium warbond's items
// only to a player who owns one of its tokens.
public sealed class Purchases(ProfileStore profile, Func<uint, JsonElement?> catalogue)
{
    public static bool Handles(string path) => path is "/SeasonPass/item/buy" or "/Progression/buy/v2"
        or "/Progression/ProgressionPackage/AcquireLevel" or "/Progression/ProgressionPackage/AcquireUnlockableItem";

    static readonly Lazy<JsonDocument> Progression = new(() => LocalBackend.LoadResource("progression-data.json"));
    static readonly Lazy<PaidContentSet> DefaultPaid = new(PaidContentSet.Load);

    PaidContentSet Paid => profile.Ownership?.Paid ?? DefaultPaid.Value;

    // Status, reply and the audit's outcome; a refusal is a 400 the client shows as a failed purchase.
    public (int Status, JsonNode Reply, string Outcome) Handle(string path, JsonElement body)
    {
        var reply = path switch
        {
            "/SeasonPass/item/buy" => WarbondItem(body),
            "/Progression/buy/v2" => StoreItem(body),
            "/Progression/ProgressionPackage/AcquireLevel" => PackageLevel(body),
            _ => UnlockableItem(body)
        };
        return reply != null ? (200, reply, "purchase_saved") : (400, new JsonObject { ["errorCode"] = "HELLFORGE_PURCHASE_REFUSED" }, "purchase_refused");
    }

    // A warbond page item: the page must be open (the warbond's medalsSpent at least the page's requirement), the warbond
    // owned, the item not bought yet (unless repeatable), and the medals there.
    JsonNode? WarbondItem(JsonElement body)
    {
        if (!Whole(body, "seasonPassItemId", out var itemId) || FindWarbondItem(itemId) is not { } found) return null;
        var (warbond, page, item) = found;
        var cost = item.GetProperty("medalCost").GetInt64();
        var medals = warbond.GetProperty("medalsConsumableMixId").GetUInt32();
        var spent = warbond.GetProperty("medalsSpentMixId").GetUInt32();
        var mixId = item.GetProperty("itemMixId").GetUInt32();
        var repeatable = item.TryGetProperty("isRepeatable", out var r) && r.ValueKind == JsonValueKind.True;
        if (cost < 0 || !OwnsWarbond(warbond)) return null;
        var granted = new JsonArray();
        var saved = profile.TryChange((progression, inventory) =>
        {
            var bought = progression["purchasedSeasonPassItems"] as JsonArray ?? [];
            if (!repeatable && bought.Any(b => b is JsonValue v && v.TryGetValue<long>(out var id) && id == itemId)) return false;
            if (ProfileStore.Held(inventory, spent) < page.GetProperty("totalMedalsRequired").GetInt64() || ProfileStore.Held(inventory, medals) < cost) return false;
            ProfileStore.AddItem(inventory, medals, -cost);
            ProfileStore.AddItem(inventory, spent, cost);
            foreach (var (content, amount) in Contents(mixId, 1)) granted.Add((JsonNode)new JsonObject { ["mixId"] = content, ["amount"] = ProfileStore.AddItem(inventory, content, amount) });
            if (!repeatable) { bought.Add((JsonNode)itemId); progression["purchasedSeasonPassItems"] = bought; }
            return true;
        });
        if (!saved) return null;
        granted.Add((JsonNode)new JsonObject { ["mixId"] = spent, ["amount"] = cost });
        return new JsonObject { ["items"] = granted };
    }

    // An item the game sells for requisition or samples (stratagems, the requisition and samples stores): its catalogue
    // buyPrice, never one that names Super Credits, never a paid item.
    JsonNode? StoreItem(JsonElement body)
    {
        if (!Whole(body, "mixId", out var id) || id is <= 0 or > uint.MaxValue) return null;
        var count = Whole(body, "amount", out var n) ? n : 1;
        var mixId = (uint)id;
        if (count is < 1 or > 100 || Paid.IsPaid(mixId) || catalogue(mixId) is not { } item) return null;
        if (!item.TryGetProperty("buyPrice", out var price) || price.ValueKind != JsonValueKind.Array || price.GetArrayLength() == 0) return null;
        var cost = Costs(price, count);
        if (cost == null || cost.Keys.Any(PaidContentSet.IsSuperCredits)) return null;
        var reply = new JsonArray();
        var saved = profile.TryChange((_, inventory) =>
        {
            if (cost.Any(c => ProfileStore.Held(inventory, c.Key) < c.Value)) return false;
            foreach (var (currency, amount) in cost) reply.Add((JsonNode)new JsonObject { ["mixId"] = currency, ["amount"] = ProfileStore.AddItem(inventory, currency, -amount) });
            reply.Insert(0, (JsonNode)new JsonObject { ["mixId"] = mixId, ["amount"] = ProfileStore.AddItem(inventory, mixId, count) });
            return true;
        });
        return saved ? reply : null;
    }

    // The next level of a progression package (ship modules): its cost taken, its item rewards given.
    JsonNode? PackageLevel(JsonElement body)
    {
        if (!Whole(body, "id32", out var id) || !Whole(body, "level", out var level) || FindPackage(id) is not { } package) return null;
        if (FindLevel(package, level) is not { } step || Costs(step.GetProperty("cost"), 1) is not { } cost) return null;
        return profile.TryChange((progression, inventory) =>
        {
            var entry = PackageEntry(progression, id);
            if (Whole(entry, "level") + 1 != level || cost.Any(c => ProfileStore.Held(inventory, c.Key) < c.Value)) return false;
            foreach (var (currency, amount) in cost) ProfileStore.AddItem(inventory, currency, -amount);
            entry["level"] = level;
            entry["progress"] = 0;
            foreach (var reward in step.GetProperty("rewards").EnumerateArray())
                if (reward.GetProperty("type").GetInt32() == 1)
                    foreach (var (content, amount) in Contents(reward.GetProperty("id32").GetUInt32(), reward.GetProperty("amount").GetInt64()))
                        ProfileStore.AddItem(inventory, content, amount);
            return true;
        }) ? new JsonObject() : null;
    }

    // An item a package level offers for a price (weapon attachments), once the package has reached that level.
    JsonNode? UnlockableItem(JsonElement body)
    {
        if (!Whole(body, "packageId32", out var id) || !Whole(body, "targetLevel", out var level) || !Whole(body, "itemMixId", out var mix) ||
            FindPackage(id) is not { } package || FindLevel(package, level) is not { } step) return null;
        if (!step.TryGetProperty("unlockableItems", out var offers)) return null;
        var offer = offers.EnumerateArray().FirstOrDefault(o => o.GetProperty("mixId").GetInt64() == mix);
        if (offer.ValueKind != JsonValueKind.Object || Costs(offer.GetProperty("cost"), 1) is not { } cost || Paid.IsPaid((uint)mix)) return null;
        return profile.TryChange((progression, inventory) =>
        {
            if (Whole(PackageEntry(progression, id), "level") < level || ProfileStore.Held(inventory, (uint)mix) > 0 ||
                cost.Any(c => ProfileStore.Held(inventory, c.Key) < c.Value)) return false;
            foreach (var (currency, amount) in cost) ProfileStore.AddItem(inventory, currency, -amount);
            ProfileStore.AddItem(inventory, (uint)mix, offer.TryGetProperty("amount", out var n) ? n.GetInt64() : 1);
            return true;
        }) ? new JsonObject() : null;
    }

    // A price list ({mixId, amount}...) times count, by currency; null when it is not one.
    static Dictionary<uint, long>? Costs(JsonElement price, long count)
    {
        var cost = new Dictionary<uint, long>();
        foreach (var entry in price.EnumerateArray())
        {
            if (!Whole(entry, "mixId", out var currency) || currency is <= 0 or > uint.MaxValue || !Whole(entry, "amount", out var amount) || amount < 0) return null;
            cost[(uint)currency] = cost.GetValueOrDefault((uint)currency) + amount * count;
        }
        return cost;
    }

    // What an item gives when it is granted: an item package's contents (itemPackages), else the item itself.
    static IEnumerable<(uint MixId, long Amount)> Contents(uint mixId, long amount)
    {
        var package = Progression.Value.RootElement.GetProperty("itemPackages").EnumerateArray()
            .FirstOrDefault(p => p.GetProperty("itemMixId32").GetUInt32() == mixId);
        if (package.ValueKind != JsonValueKind.Object) return [(mixId, amount)];
        return package.GetProperty("items").EnumerateArray().Select(i => (i.GetProperty("mixId").GetUInt32(), i.GetProperty("amount").GetInt64() * amount)).ToList();
    }

    // The free warbond, or one whose token the served inventory holds (a paid token only from the sealed snapshot).
    bool OwnsWarbond(JsonElement warbond)
    {
        if (warbond.GetProperty("id32").GetUInt32() == HellForge.Core.PremiumContent.FreeWarbond) return true;
        if (!profile.TryGetResponse("/Progression/inventory", out var served)) return false;
        var held = served.GetProperty("items").EnumerateArray().Where(i => i.TryGetProperty("amount", out var a) && a.GetInt64() > 0)
            .Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet();
        return warbond.GetProperty("premiumTokenMixIds").EnumerateArray().Any(t => held.Contains(t.GetUInt32()));
    }

    static (JsonElement Warbond, JsonElement Page, JsonElement Item)? FindWarbondItem(long itemId)
    {
        foreach (var warbond in Progression.Value.RootElement.GetProperty("seasonPassDetails").EnumerateObject().Select(w => w.Value))
            foreach (var page in warbond.GetProperty("seasonPassPages").EnumerateArray())
                foreach (var item in page.GetProperty("seasonPassItems").EnumerateArray())
                    if (item.GetProperty("seasonPassItemId").GetInt64() == itemId) return (warbond, page, item);
        return null;
    }

    static JsonElement? FindPackage(long id32)
    {
        foreach (var package in Progression.Value.RootElement.GetProperty("progressionPackages").EnumerateArray())
            if (package.GetProperty("id32").GetInt64() == id32) return package;
        return null;
    }

    static JsonElement? FindLevel(JsonElement package, long level)
    {
        foreach (var step in package.GetProperty("levels").EnumerateArray())
            if (step.GetProperty("level").GetInt64() == level) return step;
        return null;
    }

    // The profile's progress entry of a package ({id32, level, progress} in progression.packageProgress), made at level 0.
    static JsonObject PackageEntry(JsonObject progression, long id32)
    {
        if (progression["packageProgress"] is not JsonArray list) progression["packageProgress"] = list = new JsonArray();
        var entry = list.OfType<JsonObject>().FirstOrDefault(e => Whole(e, "id32") == id32);
        if (entry == null) list.Add((JsonNode)(entry = new JsonObject { ["id32"] = id32, ["level"] = 0, ["progress"] = 0 }));
        return entry;
    }

    static bool Whole(JsonElement node, string key, out long value)
    {
        value = 0;
        return node.ValueKind == JsonValueKind.Object && node.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value);
    }

    static long Whole(JsonObject node, string key) =>
        node[key] is JsonValue v && long.TryParse(v.ToJsonString(), System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out var n) ? n : 0;
}
