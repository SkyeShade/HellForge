using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The warbonds mods add (HellForge.Core ModWarbonds, --mod-warbonds), served beside the game's: list entries after the
// game's while its list has room (ListCap), each warbond's pages, its spent counter in the catalogue (what
// opens its pages), and on unlock-all every item bought and every counter full (never saved). Its currency is its
// medalsConsumableMixId, which the page view checks (0x196D2E4) and charges (0x196BB0F). An item is left out when the game
// would not have it: a catalogue item missing or not the player's to hold (premium, Super Credits), a price above the
// currency's cap, or a mod's weapon or stratagem not registered this session. Only purchases are saved.
public sealed class ModWarbondService(IReadOnlyList<ModWarbonds.Warbond> warbonds)
{
    // The warbond list the mod loader makes room for (native warbond_list.c WB_LIST_CAP; the game's own keeps 32, game.dll
    // 0x1080633, and without the loader's room keeps the first 32 served).
    public const int ListCap = 128;
    // The mods' rows in the game's warbond table (native warbonds.c WB_MAX: 127 entries, signed 8-bit bounds, less the
    // game's 28): name, look, art and Helldiver. The loader gives them to the session's warbonds in order (the plan lists
    // them all, served or not), so only the first TableRoom are served: none shows without its own.
    public const int TableRoom = 127 - 28;
    // The bought items a profile holds (game.dll: 5,280 ids, profile+0xD8A5C, no bound on any writer) less the game's own
    // warbonds' (ids 1 to 989): what mods' warbonds may sell in all.
    public const int ItemRoom = 5280 - 1000;
    // Every game warbond counts spent medals into the same total (2398121714, the medals' child).
    const uint MedalsTotal = 2398121714;
    // Ids the game's own identity table (0x32E0C20) knows besides the served warbonds: a test entry and an unreleased one.
    static readonly HashSet<uint> TableIds = [0xA06FA15D, 0x82244C47];

    public IReadOnlyList<ModWarbonds.Warbond> Warbonds => warbonds;
    // The backend's startup bodies, which the entries are checked against.
    public Func<string, JsonElement?>? Source { get; set; }
    // Mods' registered weapons and stratagems.
    public RegisteredItems? Registered { get; set; }
    // Whether the player may hold a catalogue item (OwnershipFilter's rules); null: every item but Super Credits.
    public Func<uint, bool>? IsAllowed { get; set; }
    // Paid content (PaidContentSet: premium warbonds' items and tokens, the Superstore, DLC): never sold on a mod's warbond,
    // owned or not, so nothing premium is had without its own token (OwnershipFilter.EarnWarbonds).
    public Func<uint, bool>? IsPaid { get; set; }
    // An unlock-all profile has bought everything.
    public bool UnlockAll { get; set; }
    // The profile's purchasedSeasonPassItems.
    public Func<IReadOnlySet<long>>? Purchased { get; set; }

    // The mixIds the profile holds now (its saved inventory, amount above 0).
    public Func<IReadOnlySet<uint>>? Holds { get; set; }

    // What the player had when the session's first warbond was served: the pages show what they had then as claimed, so
    // they stay as the game loaded them (a later purchase is refused at the buy, HasAlready).
    (IReadOnlySet<long> Bought, IReadOnlySet<uint> Held)? owned;
    public Action<string> Log { get; set; } = line => Console.Error.WriteLine("[mod warbonds] " + line);
    readonly HashSet<string> reported = [];

    // A warbond as served this session: its spec, the items it sells (by seasonPassItemId), the items shown claimed because
    // the player has them already (never sold, never counted as spent) and its game-shaped bodies.
    public sealed record Served(ModWarbonds.Warbond Spec, JsonObject ListEntry, JsonObject Detail, IReadOnlyDictionary<long, uint> Sells, long TotalCost)
    {
        public IReadOnlySet<long> Claimed { get; init; } = new HashSet<long>();
    }

    // What a warbond's items are checked against: the catalogue's ids and currency caps, and the registered mod items.
    sealed record Lookup(HashSet<uint>? Catalogue, HashSet<uint>? Consumables, Dictionary<uint, long> Caps, List<RegisteredItems.Item> Registered)
    {
        // What the player has already: their mod items bought, and the mixIds they hold or bought on a mod's warbond.
        public IReadOnlySet<long> Bought { get; init; } = new HashSet<long>();
        public HashSet<uint> Owned { get; init; } = [];
    }

    // The warbonds the game gets now, their items resolved.
    public IReadOnlyList<Served> Resolve()
    {
        if (warbonds.Count == 0) return [];
        var list = Source?.Invoke("/SeasonPass") is { ValueKind: JsonValueKind.Array } served
            ? served.EnumerateArray().Select(w => w.TryGetProperty("id32", out var id) && id.TryGetUInt32(out var value) ? value : 0).ToHashSet() : [];
        var lookup = Look();
        List<Served> result = [];
        // every bought id of a mod's item stays in the list the game is served and takes room, served or not (a mod taken
        // out, a key renamed, a warbond waiting for room); a served warbond adds the ids it may add (sold or shown claimed)
        var bought = Purchased?.Invoke() ?? new HashSet<long>();
        var items = bought.Count(id => id >= ModWarbonds.FirstItemId);
        foreach (var (warbond, row) in warbonds.Select((w, i) => (w, i)))
        {
            if (row >= TableRoom)
                Report(warbond, "", $"the game's warbond table names {TableRoom} mods' warbonds, and this is number {row + 1}");
            else if (list.Contains(warbond.Id32) || TableIds.Contains(warbond.Id32) || lookup.Catalogue?.Contains(warbond.Spent) == true ||
                (warbond.HasToken && lookup.Catalogue?.Contains(warbond.Token) == true))
                Report(warbond, "", "its id is one the game uses; rename its key");
            else if (result.Count >= ListCap - list.Count)
                Report(warbond, "", $"the game lists {ListCap} warbonds, and {list.Count + result.Count} are listed already");
            else if (Serve(warbond, lookup) is not { } ready) { }
            else if (items + Adds(ready, bought) > ItemRoom)
                Report(warbond, "", $"the game holds {ItemRoom} items of mods' warbonds in all, and {items} are served already");
            else
            {
                items += Adds(ready, bought);
                result.Add(ready);
            }
        }
        return result;
    }

    // The ids a served warbond may add to the purchased list beyond what is bought already: every item it sells (bought
    // later, or all on unlock-all) and every one it shows claimed.
    static int Adds(Served warbond, IReadOnlySet<long> bought) => warbond.Sells.Keys.Concat(warbond.Claimed).Count(id => !bought.Contains(id));

    Lookup Look()
    {
        var catalogue = Source?.Invoke("/Progression/items") is { ValueKind: JsonValueKind.Array } items
            ? items.EnumerateArray().Where(i => i.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out _)).ToList() : null;
        // What a player may hold of each currency (its maxCap): a price above it could never be paid.
        var caps = new Dictionary<uint, long>();
        foreach (var item in catalogue ?? [])
            if (item.TryGetProperty("maxCap", out var cap) && cap.TryGetInt64(out var most)) caps.TryAdd(item.GetProperty("mixId").GetUInt32(), most);
        var registered = Registered is { } r ? r.Served().Where(i => !r.IsLocked(i) && !i.IsDifficulty).ToList() : [];
        var consumables = catalogue?.Where(i => i.TryGetProperty("isConsumable", out var c) && c.ValueKind == JsonValueKind.True)
            .Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet();
        lock (reported) owned ??= (Purchased?.Invoke() ?? new HashSet<long>(), Holds?.Invoke() ?? new HashSet<uint>());
        var lookup = new Lookup(catalogue?.Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet(), consumables, caps, registered)
            { Bought = owned.Value.Bought, Owned = OwnedMixIds(owned.Value.Bought, owned.Value.Held, catalogue?.Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet(), registered) };
        return lookup;
    }

    // The mixIds the player has: what they hold, and what their bought mod items sell.
    HashSet<uint> OwnedMixIds(IReadOnlySet<long> bought, IReadOnlySet<uint> held, HashSet<uint>? catalogue, List<RegisteredItems.Item> registered)
    {
        HashSet<uint> result = [.. held];
        foreach (var item in warbonds.SelectMany(w => w.Pages.SelectMany(p => p.Items)).Where(i => bought.Contains(i.Id)))
            if (ItemOf(item.Sells, catalogue, registered, out _) is { } mixId) result.Add(mixId);
        return result;
    }

    // Whether a mod item (not repeatable) would buy what the player has already, now: held (not a consumable), or sold by
    // another mod item they bought. Never on unlock-all, which has everything bought.
    public bool HasAlready(long itemId, uint mixId, bool consumable, IReadOnlySet<long> bought, Func<uint, bool> holds)
    {
        if (UnlockAll || consumable || bought.Contains(itemId)) return false;
        if (holds(mixId)) return true;
        var registered = Registered is { } r ? r.Served().ToList() : [];
        return warbonds.SelectMany(w => w.Pages.SelectMany(p => p.Items))
            .Any(i => i.Id != itemId && bought.Contains(i.Id) && ItemOf(i.Sells, null, registered, out _) == mixId);
    }

    // One warbond in the game's shapes, or null when it has no item to sell or show.
    Served? Serve(ModWarbonds.Warbond warbond, Lookup lookup)
    {
        var sells = new Dictionary<long, uint>();
        var claimed = new HashSet<long>();
        var pages = new JsonArray([.. warbond.Pages.Select(page => (JsonNode)new JsonObject
        {
            ["totalMedalsRequired"] = page.Requirement,
            ["seasonPassItems"] = new JsonArray([.. page.Items.Select(item => Cell(warbond, item, lookup, sells, claimed)).OfType<JsonNode>()]),
        })]);
        var total = warbond.Pages.Sum(p => p.Items.Where(i => sells.ContainsKey(i.Id) && i.CountsAsSpent(warbond.CurrencyMixId)).Sum(i => i.Cost));
        if (sells.Count + claimed.Count == 0)
        {
            Report(warbond, "", "none of its items can be sold or shown this session");
            return null;
        }
        // Each page's requirement within what the pages before it sell this session (items left out cannot be bought). A
        // page that sells nothing (every item shown claimed or left out) opens with the page before it: locked, the game
        // would read it as a premium warbond's ("PREMIUM WARBOND NEEDED", 0x1975132: its own amount left is 0, run 11).
        long before = 0, previous = 0;
        var requirements = new JsonArray();
        for (var i = 0; i < warbond.Pages.Count; i++)
        {
            var requirement = warbond.Pages[i].Items.Any(item => sells.ContainsKey(item.Id)) ? Math.Min(warbond.Pages[i].Requirement, before) : previous;
            previous = requirement;
            requirements.Add((JsonNode)requirement);
            pages[i]!["totalMedalsRequired"] = requirement;
            before += warbond.Pages[i].Items.Where(item => sells.ContainsKey(item.Id) && item.CountsAsSpent(warbond.CurrencyMixId)).Sum(item => item.Cost);
        }
        var entry = Head(warbond, total);
        entry["pageRequirements"] = requirements.DeepClone();
        entry["premiumTokenMixIds"] = Tokens(warbond);
        var detail = Head(warbond, total);
        detail["layoutSize"] = new JsonObject { ["x"] = ModWarbonds.GridWidth, ["y"] = ModWarbonds.GridHeight };
        detail["layoutType"] = 1;
        detail["seasonPassPages"] = pages;
        detail["pageRequirements"] = requirements;
        detail["premiumTokenMixIds"] = Tokens(warbond);
        return new Served(warbond, entry, detail, sells, total) { Claimed = claimed };
    }

    // A warbond with a price or a premium look has a token, which owns it (game.dll 0x136E470); one without either has none,
    // and the game takes it as owned. A premium look without a token would read WARBOND LOCKED under forge_credits.c.
    static JsonArray Tokens(ModWarbonds.Warbond warbond) => warbond.HasToken ? [(JsonNode)warbond.Token] : [];

    static JsonObject Head(ModWarbonds.Warbond warbond, long total) => new()
    {
        ["id32"] = warbond.Id32, ["medalsTotalMixId"] = MedalsTotal, ["medalsConsumableMixId"] = warbond.CurrencyMixId,
        ["medalsSpentMixId"] = warbond.Spent, ["totalCost"] = Math.Max(total, 1), /* 0 reads complete (held spent == totalCost, 0x1963587): the card moves last */
    };

    // An item's cell in the game's detail shape, or null (reported) when it cannot be sold or shown.
    JsonObject? Cell(ModWarbonds.Warbond warbond, ModWarbonds.Item item, Lookup lookup, Dictionary<long, uint> sells, HashSet<long> claimed)
    {
        if (lookup.Caps.TryGetValue(CurrencyOf(warbond, item), out var cap) && item.Cost > cap)
        {
            Report(warbond, item.Sells.ToString(), $"it costs {item.Cost} {(item.CurrencyMixId != 0 ? item.Currency : warbond.Currency)}, and a player holds at most {cap}");
            return null;
        }
        if (ItemOf(item.Sells, lookup.Catalogue, lookup.Registered, out var why) is not { } mixId)
        {
            Report(warbond, item.Sells.ToString(), why);
            return null;
        }
        // A repeatable item is bought again and again: only a consumable adds up (an armor or a weapon is held once).
        if (item.Repeatable && lookup.Consumables != null && !lookup.Consumables.Contains(mixId))
        {
            Report(warbond, item.Sells.ToString(), "it is \"repeatable\", and only a consumable can be bought again");
            return null;
        }
        // An item held once (not a consumable) the player has already, from another warbond or bought on another mod's, is
        // shown claimed (its id in the served purchased list, Claim) and not sold: its price would buy nothing. It counts
        // towards no page, as an item left out. One bought here is sold as any (bought, so claimed).
        if (!UnlockAll && lookup.Consumables != null && !item.Repeatable && !lookup.Bought.Contains(item.Id) && lookup.Owned.Contains(mixId) &&
            !lookup.Consumables.Contains(mixId))
            claimed.Add(item.Id);
        else
            sells[item.Id] = mixId;
        return new JsonObject
        {
            ["seasonPassItemId"] = item.Id, ["itemMixId"] = mixId, ["medalCost"] = item.Cost,
            ["gridSize"] = new JsonObject { ["x"] = item.Width, ["y"] = item.Height },
            ["gridPosition"] = new JsonObject { ["x"] = item.X, ["y"] = item.Y },
            ["listSortOrder"] = 0, ["isPremium"] = false, ["isRepeatable"] = item.Repeatable,
        };
    }

    // The mixId an item sells, or null with the reason.
    uint? ItemOf(ModWarbonds.Ref sells, HashSet<uint>? catalogue, List<RegisteredItems.Item> registered, out string why)
    {
        why = "";
        if (sells.Kind == "item")
        {
            if (catalogue != null && !catalogue.Contains(sells.MixId)) why = "the game's catalogue has no such item";
            else if (PaidContentSet.IsSuperCredits(sells.MixId)) why = "Super Credits are never sold";
            else if (IsPaid?.Invoke(sells.MixId) == true || IsAllowed?.Invoke(sells.MixId) == false) why = "it is premium content, which only its own warbond sells";
            return why.Length == 0 ? sells.MixId : null;
        }
        var match = registered.FirstOrDefault(i => (sells.Kind == "weapon" ? i.IsWeapon : i.IsStratagem) &&
            string.Equals(i.Name, sells.Name, StringComparison.Ordinal) && string.Equals(i.Mod, sells.Mod, StringComparison.Ordinal));
        if (match == null) why = $"no {sells.Kind} by that name is registered this session (its mod is not loaded, is locked, or ran out of room)";
        return match?.MixId;
    }

    void Report(ModWarbonds.Warbond warbond, string item, string why)
    {
        var line = $"{warbond.Mod}'s warbond \"{warbond.Key}\"{(item.Length > 0 ? ": " + item + " is left out" : " is left out")}: {why}.";
        lock (reported)
            if (!reported.Add(line)) return;
        Log(line);
    }

    // A startup body with the mods' warbonds added where they belong.
    public JsonElement Apply(string path, JsonElement body)
    {
        if (warbonds.Count == 0 || path is not ("/SeasonPass" or "/Progression/items" or "/Progression/inventory" or "/Progression")) return body;
        var served = Resolve();
        if (served.Count == 0 || (path == "/Progression" && !UnlockAll && served.All(w => w.Claimed.Count == 0))) return body;
        var node = JsonNode.Parse(body.GetRawText());
        var changed = (path, node) switch
        {
            ("/SeasonPass", JsonArray list) => AddEntries(list, served),
            ("/Progression/items", JsonArray catalogue) => AddCounters(catalogue, served),
            ("/Progression/inventory", JsonObject { } inventory) when inventory["items"] is JsonArray held => HoldTokens(held, served) | (UnlockAll && FillCounters(held, served)),
            ("/Progression", JsonObject progression) => Claim(progression, served, UnlockAll),
            _ => false,
        };
        if (!changed) return body;
        using var document = JsonDocument.Parse(node!.ToJsonString());
        return document.RootElement.Clone();
    }

    // After all of the game's: the Acquisition Center shows the list in order (0x1963470). Only when the number of warbonds
    // differs from the one it stored when it last closed (+0x897B4 of its settings) does it show the last one first, once,
    // as it does with a new warbond; with mods, that is the last mod's.
    static bool AddEntries(JsonArray list, IReadOnlyList<Served> served)
    {
        foreach (var warbond in served) list.Add((JsonNode)warbond.ListEntry.DeepClone());
        return true;
    }

    // Each warbond's spent counter (a consumable like the game's medalsSpent items) and its token, priced in Super Credits as
    // the game's are (the warbond button reads that price, 0x136C270; Purchases takes it in Forge Credits).
    static bool AddCounters(JsonArray catalogue, IReadOnlyList<Served> served)
    {
        foreach (var warbond in served.Select(w => w.Spec).Where(w => w.HasToken))
            catalogue.Add((JsonNode)new JsonObject
            {
                ["itemId"] = warbond.Token, ["mixId"] = warbond.Token, ["isConsumable"] = false, ["requiredLevel"] = 0,
                ["progressionCategory"] = 0, ["tags"] = new JsonArray(), ["requiredItems"] = new JsonArray(),
                ["buyPrice"] = new JsonArray(new JsonObject { ["mixId"] = PaidContentSet.SuperCreditPrice, ["amount"] = Math.Max(warbond.UnlockCost, 1) }),
                ["sellPrice"] = new JsonArray(),
            });
        foreach (var warbond in served)
            catalogue.Add((JsonNode)new JsonObject
            {
                ["itemId"] = warbond.Spec.Spent, ["mixId"] = warbond.Spec.Spent, ["isConsumable"] = true, ["requiredLevel"] = 0,
                ["progressionCategory"] = 0, ["tags"] = new JsonArray(), ["requiredItems"] = new JsonArray(),
                ["buyPrice"] = new JsonArray(), ["sellPrice"] = new JsonArray(),
            });
        return true;
    }

    // The inventory entries the game keeps this session (more once the mod loader has moved its arrays).
    int InventoryKept => Registered?.InventoryKept ?? RegisteredItems.InventoryCap;

    // The tokens of open warbonds (a premium look, no price) are everyone's; on unlock-all every token is. So is the token of
    // a warbond with nothing left to sell (every item shown claimed): its price would buy nothing. A token held owns its
    // warbond (0x136E470), so the button reads unlocked; never saved.
    bool HoldTokens(JsonArray held, IReadOnlyList<Served> served)
    {
        var have = held.OfType<JsonObject>().Select(i => (uint?)i["mixId"]).ToHashSet();
        foreach (var warbond in served.Where(w => w.Spec.HasToken && (w.Spec.UnlockCost == 0 || UnlockAll || w.Sells.Count == 0)).Select(w => w.Spec)
            .Where(w => !have.Contains(w.Token)))
            if (held.Count < InventoryKept) held.Add((JsonNode)new JsonObject { ["mixId"] = warbond.Token, ["amount"] = 1, ["isDiscovered"] = true });
        return true;
    }

    // Unlock-all: each spent counter at its warbond's whole cost, so every page is open.
    bool FillCounters(JsonArray held, IReadOnlyList<Served> served)
    {
        foreach (var warbond in served)
        {
            var counter = held.OfType<JsonObject>().FirstOrDefault(i => (uint?)i["mixId"] == warbond.Spec.Spent);
            if (counter != null) counter["amount"] = Math.Max((long?)counter["amount"] ?? 0, warbond.TotalCost);
            else if (held.Count < InventoryKept) held.Add((JsonNode)new JsonObject { ["mixId"] = warbond.Spec.Spent, ["amount"] = warbond.TotalCost, ["isDiscovered"] = true });
        }
        return true;
    }

    // The purchased list the game is served (it fills its own from it alone, 0x1038BF0; never saved): the items the player
    // has already shown claimed, and on unlock-all every item of the mods' warbonds bought.
    static bool Claim(JsonObject progression, IReadOnlyList<Served> served, bool unlockAll)
    {
        var bought = progression["purchasedSeasonPassItems"] as JsonArray ?? [];
        var have = bought.OfType<JsonValue>().Select(v => v.TryGetValue<long>(out var id) ? id : -1).ToHashSet();
        // a repeatable item is never bought for good: it stays for sale, as after a purchase
        var repeatable = served.SelectMany(w => w.Spec.Pages.SelectMany(p => p.Items)).Where(i => i.Repeatable).Select(i => (long)i.Id).ToHashSet();
        var shown = served.SelectMany(w => unlockAll ? w.Sells.Keys.Concat(w.Claimed) : w.Claimed);
        foreach (var id in shown.Where(id => !repeatable.Contains(id) && have.Add(id))) bought.Add((JsonNode)id);
        progression["purchasedSeasonPassItems"] = bought;
        return true;
    }

    // GET /SeasonPass/<id32> for a mod's warbond.
    public JsonElement? Detail(string id32)
    {
        if (warbonds.Count == 0 || !uint.TryParse(id32, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id)) return null;
        var warbond = Resolve().FirstOrDefault(w => w.Spec.Id32 == id);
        return warbond == null ? null : JsonSerializer.SerializeToElement(warbond.Detail, BackendJson.Default.JsonNode);
    }

    // A mod warbond's item by its seasonPassItemId: the warbond, page and item as the game's detail has them, for Purchases.
    public (JsonElement Warbond, JsonElement Page, JsonElement Item)? Find(long itemId)
    {
        if (warbonds.Count == 0 || itemId < ModWarbonds.FirstItemId) return null;
        foreach (var warbond in Resolve())
        {
            if (!warbond.Sells.ContainsKey(itemId)) continue;
            var detail = JsonSerializer.SerializeToElement(warbond.Detail, BackendJson.Default.JsonNode);
            foreach (var page in detail.GetProperty("seasonPassPages").EnumerateArray())
                foreach (var item in page.GetProperty("seasonPassItems").EnumerateArray())
                    if (item.GetProperty("seasonPassItemId").GetInt64() == itemId) return (detail, page, item);
        }
        return null;
    }

    public bool IsModWarbond(uint id32) => warbonds.Any(w => w.Id32 == id32);

    // Whether a mod warbond is the player's: open to everyone, unlock-all, or its token bought (held is the saved inventory).
    public bool Owns(uint id32, Func<uint, bool> held) =>
        warbonds.FirstOrDefault(w => w.Id32 == id32) is { } warbond && (warbond.UnlockCost == 0 || UnlockAll || held(warbond.Token));

    // A mod warbond's token sold for its price (in Forge Credits), or null for any other item. One with nothing left to sell
    // is never sold (HoldTokens serves it held): 0, which Purchases refuses.
    public long? UnlockPrice(uint mixId) =>
        mixId == 0 ? null : Resolve().FirstOrDefault(w => w.Spec.HasToken && w.Spec.UnlockCost > 0 && w.Spec.Token == mixId) is { } warbond
            ? warbond.Sells.Count == 0 ? 0 : warbond.Spec.UnlockCost : null;

    // Mods' weapons and stratagems a warbond sells: the player holds one only once an item selling it is bought (an
    // unlock-all profile has bought them all). An item no warbond sells is the mod's to give, as before.
    public IReadOnlySet<uint> Withheld()
    {
        if (UnlockAll || warbonds.Count == 0 || Registered == null) return new HashSet<uint>();
        var bought = Purchased?.Invoke() ?? new HashSet<long>();
        var offers = new Dictionary<uint, bool>();
        foreach (var warbond in Resolve())
            foreach (var page in warbond.Spec.Pages)
                foreach (var item in page.Items.Where(i => i.Sells.Kind != "item" && warbond.Sells.ContainsKey(i.Id)))
                {
                    var mixId = warbond.Sells[item.Id];
                    offers[mixId] = offers.GetValueOrDefault(mixId) || bought.Contains(item.Id);
                }
        return offers.Where(o => !o.Value).Select(o => o.Key).ToHashSet();
    }

    static uint CurrencyOf(ModWarbonds.Warbond warbond, ModWarbonds.Item item) => item.CurrencyMixId != 0 ? item.CurrencyMixId : warbond.CurrencyMixId;

    // A mod warbond item's currency (its own, else its warbond's); null for any other item.
    public uint? ItemCurrency(long itemId) =>
        warbonds.SelectMany(w => w.Pages.SelectMany(p => p.Items).Select(i => (w, i))).FirstOrDefault(x => x.i.Id == itemId) is ({ } w, { } i) ? CurrencyOf(w, i) : null;

    // Whether a mixId is a mod's registered weapon or stratagem: a purchase grants it without saving it in the profile,
    // which RegisteredItems serves while the item stays bought.
    public bool IsRegistered(uint mixId) => Registered?.Load().Any(i => i.MixId == mixId && !i.IsDifficulty) == true;
}
