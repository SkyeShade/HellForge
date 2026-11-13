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
//                              The parser leaves a missing packageProgress unset on the caller's stack, and 0x103e1e0
//                              walks that garbage (crash at 0x103e2b9, in game 2026-11-04), so the empty list is sent.
//   POST /Progression/buy/v2   0xb04e1beb  builder 0x12c1f65  {mixId, amount, warId, expectedCost:[{mixId, amount}]}
//                              reply [{mixId, amount}] (an array, parser 0x107eb20): the client adds the positive amounts
//                              and takes the negative ones (0x12c9170), so the price comes back negative.
//   POST /Progression/ProgressionPackage/AcquireLevel  0x35c47a6d  {id32, level, expectedCost} (ship modules)
//   POST /Progression/ProgressionPackage/AcquireUnlockableItem  0x1b13fab5  {packageId32, targetLevel, itemMixId,
//                              expectedCost} (weapon attachments). Neither reply is parsed (0xb64d30, 0xb666a0).
// Super Credits are never spent or granted, and nothing paid (OwnershipFilter's rules) is sold: a premium warbond's items
// only to a player who owns one of its tokens. Forge Credits (PaidContentSet.ForgeCreditsWallet) buy a premium warbond's
// token at its Super Credits price, only one the player owns on the live game (the verified snapshot): the client's
// unlock (0x136c3a0) is /Progression/buy/v2 {mixId: token, amount: 1, expectedCost: [{mixId: 0xCF875032, amount}]}, and
// the reply takes the price from the wallet the client shows (a negative amount on 0xCF875032 would change nothing).
public sealed class Purchases(ProfileStore profile, Func<uint, JsonElement?> catalogue, ModWarbondService? modWarbonds = null)
{
    public static bool Handles(string path) => path is "/SeasonPass/item/buy" or "/Progression/buy/v2" or "/StoreFront/buy"
        or "/Progression/ProgressionPackage/AcquireLevel" or "/Progression/ProgressionPackage/AcquireUnlockableItem";

    static readonly Lazy<JsonDocument> Progression = new(() => LocalBackend.LoadResource("progression-data.json"));
    static readonly Lazy<PaidContentSet> DefaultPaid = new(PaidContentSet.Load);

    PaidContentSet Paid => profile.Ownership?.Paid ?? DefaultPaid.Value;

    // This session's mod weapons, set by LocalBackend.
    public RegisteredItems? Registered { get; init; }

    // Status, reply and the audit's outcome; a refusal is a 400 the client shows as a failed purchase.
    public (int Status, JsonNode Reply, string Outcome) Handle(string path, JsonElement body)
    {
        var reply = path switch
        {
            "/SeasonPass/item/buy" => WarbondItem(body),
            "/Progression/buy/v2" => StoreItem(body),
            "/StoreFront/buy" => StoreFrontItem(body),
            "/Progression/ProgressionPackage/AcquireLevel" => PackageLevel(body),
            _ => UnlockableItem(body)
        };
        return reply != null ? (200, reply, "purchase_saved") : (400, new JsonObject { ["errorCode"] = "HELLFORGE_PURCHASE_REFUSED" }, "purchase_refused");
    }

    // A warbond page item: the page must be open (the warbond's medalsSpent at least the page's requirement), the warbond
    // owned, the item not bought yet (unless repeatable), and the medals there. Only a price in the warbond's own currency
    // counts as spent on it, and never a mod's repeatable item's (ModWarbonds.Item.CountsAsSpent).
    JsonNode? WarbondItem(JsonElement body)
    {
        if (!Whole(body, "seasonPassItemId", out var itemId) || FindWarbondItem(itemId) is not { } found) return null;
        var (warbond, page, item) = found;
        var cost = item.GetProperty("medalCost").GetInt64();
        var own = modWarbonds?.ItemCurrency(itemId); /* a mod's item: its own currency */
        var medals = own ?? warbond.GetProperty("medalsConsumableMixId").GetUInt32();
        var spent = warbond.GetProperty("medalsSpentMixId").GetUInt32();
        var mixId = item.GetProperty("itemMixId").GetUInt32();
        var repeatable = item.TryGetProperty("isRepeatable", out var r) && r.ValueKind == JsonValueKind.True;
        var counts = medals == warbond.GetProperty("medalsConsumableMixId").GetUInt32() && !(repeatable && own != null);
        /* unlock-all opens a mod warbond's pages in what the game is served (FillCounters), never in the saved counter */
        var open = own != null && modWarbonds!.UnlockAll;
        // A mod item the player has gained since its page was served (on another warbond): its price would buy nothing.
        var consumable = catalogue(mixId) is { } entry && entry.TryGetProperty("isConsumable", out var k) && k.ValueKind == JsonValueKind.True;
        bool Gained(JsonArray bought, JsonArray inventory) => own != null && !repeatable && modWarbonds!.HasAlready(itemId, mixId, consumable,
            bought.OfType<JsonValue>().Select(v => v.TryGetValue<long>(out var id) ? id : -1).ToHashSet(), m => ProfileStore.Held(inventory, m) > 0);
        if (cost < 0 || !OwnsWarbond(warbond)) return null;
        /* the client takes back what it sent (0x13751D0): a mod item sent in another currency (the loader's per-item
           currency not installed) is refused, so the shown and saved balances never part */
        if (own != null && body.TryGetProperty("expectedCost", out var expected) && expected.ValueKind == JsonValueKind.Array &&
            expected.EnumerateArray().Any(e => e.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out var sent) && sent != medals)) return null;
        var granted = new JsonArray();
        var saved = profile.TryChange((progression, inventory) =>
        {
            var bought = progression["purchasedSeasonPassItems"] as JsonArray ?? [];
            if (!repeatable && bought.Any(b => b is JsonValue v && v.TryGetValue<long>(out var id) && id == itemId)) return false;
            if (Gained(bought, inventory)) return false;
            if ((!open && ProfileStore.Held(inventory, spent) < page.GetProperty("totalMedalsRequired").GetInt64()) || ProfileStore.Held(inventory, medals) < cost) return false;
            ProfileStore.AddItem(inventory, medals, -cost);
            if (counts) ProfileStore.AddItem(inventory, spent, cost);
            foreach (var (content, amount) in Contents(mixId, 1))
            {
                /* a mod's weapon or stratagem: RegisteredItems serves it from now on (the id is bought), nothing is saved */
                if (modWarbonds?.IsRegistered(content) == true) { granted.Add((JsonNode)new JsonObject { ["mixId"] = content, ["amount"] = amount }); continue; }
                /* a page's Super Credits (73 items, all into the wallet 0xCBFFC645, which is never stored) pay Forge Credits */
                var paid = PaidContentSet.IsSuperCredits(content) ? PaidContentSet.ForgeCreditsWallet : content;
                granted.Add((JsonNode)new JsonObject { ["mixId"] = paid, ["amount"] = ProfileStore.AddItem(inventory, paid, amount) });
            }
            if (!repeatable) { bought.Add((JsonNode)itemId); progression["purchasedSeasonPassItems"] = bought; }
            return true;
        });
        if (!saved) return null;
        if (counts) granted.Add((JsonNode)new JsonObject { ["mixId"] = spent, ["amount"] = cost });
        return new JsonObject { ["items"] = granted, ["packageProgress"] = new JsonArray() };
    }

    // An item the game sells for requisition or samples (stratagems, the requisition and samples stores): its catalogue
    // buyPrice, never one that names Super Credits, never a paid item.
    JsonNode? StoreItem(JsonElement body)
    {
        if (!Whole(body, "mixId", out var id) || id is <= 0 or > uint.MaxValue) return null;
        return StoreItem((uint)id, Whole(body, "amount", out var n) ? n : 1);
    }

    // A store front's item (the requisition terminal's stratagems: /StoreFront/buy 0xdf12cd01,
    // {expectedCost:[{mixId, amount}], salesId, warId}): the item its store front lists under the salesId (or a mixId),
    // sold as StoreItem sells it. The reply goes through the warbond item's parser (0x105a470, the case at 0x12ca52b):
    // {items:[{mixId, amount}]}, which the client adds; unlike a warbond page it does not take the price itself, so the
    // price is in the list too, negative (in game 2026-11-03: without it the requisition shown never went down).
    JsonNode? StoreFrontItem(JsonElement body)
    {
        var count = Whole(body, "amount", out var n) ? n : 1;
        var mixId = Whole(body, "mixId", out var id) && id is > 0 and <= uint.MaxValue ? (uint)id : ListedItem(body);
        return mixId != 0 && StoreItem(mixId, count) is JsonArray sold ? StoreFrontReply(sold) : null;
    }

    // StoreItem's reply ([{mixId, amount}], the price negative) as /StoreFront/buy's: {items:[...]}.
    public static JsonObject StoreFrontReply(JsonArray sold) => new() { ["items"] = sold.DeepClone(), ["packageProgress"] = new JsonArray() };

    static uint ListedItem(JsonElement body)
    {
        if (!body.TryGetProperty("salesId", out var sales) || sales.ValueKind != JsonValueKind.String) return 0;
        var listed = Progression.Value.RootElement.GetProperty("storeFront").EnumerateArray()
            .SelectMany(front => front.GetProperty("sections").EnumerateArray()).SelectMany(section => section.GetProperty("items").EnumerateArray())
            .FirstOrDefault(item => item.GetProperty("salesId").GetString() == sales.GetString());
        return listed.ValueKind == JsonValueKind.Object ? listed.GetProperty("mixId").GetUInt32() : 0;
    }

    JsonNode? StoreItem(uint mixId, long count)
    {
        if (Paid.WarbondTokens.Contains(mixId)) return WarbondToken(mixId);
        if (modWarbonds?.UnlockPrice(mixId) is { } unlock) return unlock > 0 ? TokenForCredits(mixId, unlock) : null; /* 0: nothing to sell */
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

    // A premium warbond's token for Forge Credits: owned on the live game, not held yet, its Super Credits price in the wallet.
    JsonNode? WarbondToken(uint token)
    {
        if (profile.Ownership is not { } ownership || !ownership.Owns(token) || catalogue(token) is not { } item ||
            !item.TryGetProperty("buyPrice", out var price) || price.ValueKind != JsonValueKind.Array || Costs(price, 1) is not { } cost ||
            !cost.TryGetValue(PaidContentSet.SuperCreditPrice, out var credits) || cost.Count != 1 || credits <= 0) return null;
        return TokenForCredits(token, credits);
    }

    // A warbond's token (the game's, or a mod's at its unlock price) for Forge Credits, once.
    JsonNode? TokenForCredits(uint token, long credits)
    {
        var reply = new JsonArray();
        var saved = profile.TryChange((_, inventory) =>
        {
            if (ProfileStore.Held(inventory, token) > 0 || ProfileStore.Held(inventory, PaidContentSet.ForgeCreditsWallet) < credits) return false;
            reply.Add((JsonNode)new JsonObject { ["mixId"] = token, ["amount"] = ProfileStore.AddItem(inventory, token, 1) });
            reply.Add((JsonNode)new JsonObject { ["mixId"] = PaidContentSet.ForgeCreditsWallet, ["amount"] = ProfileStore.AddItem(inventory, PaidContentSet.ForgeCreditsWallet, -credits) });
            return true;
        });
        return saved ? reply : null;
    }

    // The next level of a progression package (ship modules): its cost taken, its item rewards given.
    JsonNode? PackageLevel(JsonElement body)
    {
        if (!Whole(body, "id32", out var id) || !Whole(body, "level", out var level) || FindPackage(id) is not { } track) return null;
        if (FindLevel(track.Definition, level) is not { } step || Costs(step.GetProperty("cost"), 1) is not { } cost) return null;
        return profile.TryChange((progression, inventory) =>
        {
            var entry = track.Entry(progression);
            if (Whole(entry, "level") + 1 != level || cost.Any(c => ProfileStore.Held(inventory, c.Key) < c.Value)) return false;
            foreach (var (currency, amount) in cost) ProfileStore.AddItem(inventory, currency, -amount);
            entry["level"] = level;
            entry["progress"] = 0;
            foreach (var reward in step.GetProperty("rewards").EnumerateArray())
                if (reward.GetProperty("type").GetInt32() == 1)
                    foreach (var (content, amount) in Contents(reward.GetProperty("id32").GetUInt32(), reward.GetProperty("amount").GetInt64()))
                        track.Give(inventory, entry, content, amount);
            return true;
        }) ? new JsonObject() : null;
    }

    // An item a package level offers for a price (weapon attachments), once the package has reached that level.
    JsonNode? UnlockableItem(JsonElement body)
    {
        if (!Whole(body, "packageId32", out var id) || !Whole(body, "targetLevel", out var level) || !Whole(body, "itemMixId", out var mix) ||
            FindPackage(id) is not { } track || FindLevel(track.Definition, level) is not { } step) return null;
        if (!step.TryGetProperty("unlockableItems", out var offers)) return null;
        // The offer as its base's package lists it; the client names a copy's by the copy's own child id.
        var offer = offers.EnumerateArray().FirstOrDefault(o => track.Own(o.GetProperty("mixId").GetUInt32()) == mix);
        if (offer.ValueKind != JsonValueKind.Object || Costs(offer.GetProperty("cost"), 1) is not { } cost) return null;
        var item = offer.GetProperty("mixId").GetUInt32();
        if (Paid.IsPaid(item)) return null;
        return profile.TryChange((progression, inventory) =>
        {
            var entry = track.Entry(progression);
            if (Whole(entry, "level") < level || track.Holds(inventory, entry, item) || cost.Any(c => ProfileStore.Held(inventory, c.Key) < c.Value)) return false;
            foreach (var (currency, amount) in cost) ProfileStore.AddItem(inventory, currency, -amount);
            track.Give(inventory, entry, item, offer.TryGetProperty("amount", out var n) ? n.GetInt64() : 1);
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
        if (modWarbonds?.IsModWarbond(warbond.GetProperty("id32").GetUInt32()) == true) /* a mod's: open, or its token bought */
            return modWarbonds.Owns(warbond.GetProperty("id32").GetUInt32(), HeldSaved);
        if (!profile.TryGetResponse("/Progression/inventory", out var served)) return false;
        var held = served.GetProperty("items").EnumerateArray().Where(i => i.TryGetProperty("amount", out var a) && a.GetInt64() > 0)
            .Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet();
        // its tokens as the compiled list has them (PaidContentSet), not as the installation's catalogue says
        var id = warbond.GetProperty("id32").GetUInt32();
        return Paid.Warbonds.FirstOrDefault(w => w.Id == id) is { } known && known.Tokens.Any(held.Contains);
    }

    // Whether the saved inventory holds an item (TryChange reads it under the profile's lock; false changes nothing).
    bool HeldSaved(uint mixId)
    {
        var held = false;
        profile.TryChange((_, inventory) => { held = ProfileStore.Held(inventory, mixId) > 0; return false; });
        return held;
    }

    // The game's warbonds' item, else a mod's (ModWarbondService), sold the same way in its warbond's currency.
    (JsonElement Warbond, JsonElement Page, JsonElement Item)? FindWarbondItem(long itemId) => FindGameWarbondItem(itemId) ?? modWarbonds?.Find(itemId);

    static (JsonElement Warbond, JsonElement Page, JsonElement Item)? FindGameWarbondItem(long itemId)
    {
        foreach (var warbond in Progression.Value.RootElement.GetProperty("seasonPassDetails").EnumerateObject().Select(w => w.Value))
            foreach (var page in warbond.GetProperty("seasonPassPages").EnumerateArray())
                foreach (var item in page.GetProperty("seasonPassItems").EnumerateArray())
                    if (item.GetProperty("seasonPassItemId").GetInt64() == itemId) return (warbond, page, item);
        return null;
    }

    // A package of the game's, or a mod weapon's the player may have: it sells at its base's prices, against its own level.
    MasteryTrack? FindPackage(long id32)
    {
        if (id32 is < 0 or > uint.MaxValue) return null;
        if (WeaponMastery.Vanilla((uint)id32) is { } vanilla) return vanilla;
        return Registered?.Track((uint)id32) is { } copy && !Registered.IsLeftOut(copy.Item!) ? copy : null;
    }

    static JsonElement? FindLevel(JsonElement package, long level)
    {
        foreach (var step in package.GetProperty("levels").EnumerateArray())
            if (step.GetProperty("level").GetInt64() == level) return step;
        return null;
    }

    static bool Whole(JsonElement node, string key, out long value)
    {
        value = 0;
        return node.ValueKind == JsonValueKind.Object && node.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value);
    }

    static long Whole(JsonObject node, string key) =>
        node[key] is JsonValue v && long.TryParse(v.ToJsonString(), System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out var n) ? n : 0;
}
