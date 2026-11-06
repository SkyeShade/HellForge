using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The warbonds mods add (HellForge.Core ModWarbonds, --mod-warbonds), served beside the game's: list entries after the
// game's while its list has room (32, game.dll 0x1080633), each warbond's pages, its spent counter in the catalogue (what
// opens its pages), and on unlock-all every item bought and every counter full (never saved). Its currency is its
// medalsConsumableMixId, which the page view checks (0x196D2E4) and charges (0x196BB0F). An item is left out when the game
// would not have it: a catalogue item missing or not the player's to hold (premium, Super Credits), a price above the
// currency's cap, or a mod's weapon or stratagem not registered this session. Only purchases are saved.
public sealed class ModWarbondService(IReadOnlyList<ModWarbonds.Warbond> warbonds)
{
    // The game's warbond list keeps 32 (game.dll 0x1080633).
    public const int ListCap = 32;
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
    // An unlock-all profile has bought everything.
    public bool UnlockAll { get; set; }
    // The profile's purchasedSeasonPassItems.
    public Func<IReadOnlySet<long>>? Purchased { get; set; }
    public Action<string> Log { get; set; } = line => Console.Error.WriteLine("[mod warbonds] " + line);
    readonly HashSet<string> reported = [];

    // A warbond as served this session: its spec, the items it sells (by seasonPassItemId) and its game-shaped bodies.
    public sealed record Served(ModWarbonds.Warbond Spec, JsonObject ListEntry, JsonObject Detail, IReadOnlyDictionary<long, uint> Sells, long TotalCost);

    // What a warbond's items are checked against: the catalogue's ids and currency caps, and the registered mod items.
    sealed record Lookup(HashSet<uint>? Catalogue, Dictionary<uint, long> Caps, List<RegisteredItems.Item> Registered);

    // The warbonds the game gets now, their items resolved.
    public IReadOnlyList<Served> Resolve()
    {
        if (warbonds.Count == 0) return [];
        var list = Source?.Invoke("/SeasonPass") is { ValueKind: JsonValueKind.Array } served
            ? served.EnumerateArray().Select(w => w.TryGetProperty("id32", out var id) && id.TryGetUInt32(out var value) ? value : 0).ToHashSet() : [];
        var lookup = Look();
        List<Served> result = [];
        foreach (var warbond in warbonds)
        {
            if (list.Contains(warbond.Id32) || TableIds.Contains(warbond.Id32) || lookup.Catalogue?.Contains(warbond.Spent) == true ||
                (warbond.HasToken && lookup.Catalogue?.Contains(warbond.Token) == true))
                Report(warbond, "", "its id is one the game uses; rename its key");
            else if (result.Count >= ListCap - list.Count)
                Report(warbond, "", $"the game lists {ListCap} warbonds, and {list.Count + result.Count} are listed already");
            else if (Serve(warbond, lookup) is { } ready)
                result.Add(ready);
        }
        return result;
    }

    Lookup Look()
    {
        var catalogue = Source?.Invoke("/Progression/items") is { ValueKind: JsonValueKind.Array } items
            ? items.EnumerateArray().Where(i => i.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out _)).ToList() : null;
        // What a player may hold of each currency (its maxCap): a price above it could never be paid.
        var caps = new Dictionary<uint, long>();
        foreach (var item in catalogue ?? [])
            if (item.TryGetProperty("maxCap", out var cap) && cap.TryGetInt64(out var most)) caps.TryAdd(item.GetProperty("mixId").GetUInt32(), most);
        var registered = Registered is { } r ? r.Admitted(RegisteredItems.Prioritized(r.Load())).Where(i => !r.Locked(i) && !i.IsDifficulty).ToList() : [];
        return new Lookup(catalogue?.Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet(), caps, registered);
    }

    // One warbond in the game's shapes, or null when none of its items can be sold.
    Served? Serve(ModWarbonds.Warbond warbond, Lookup lookup)
    {
        var sells = new Dictionary<long, uint>();
        var pages = new JsonArray([.. warbond.Pages.Select(page => (JsonNode)new JsonObject
        {
            ["totalMedalsRequired"] = page.Requirement,
            ["seasonPassItems"] = new JsonArray([.. page.Items.Select(item => Cell(warbond, item, lookup, sells)).OfType<JsonNode>()]),
        })]);
        var total = warbond.Pages.Sum(p => p.Items.Where(i => sells.ContainsKey(i.Id)).Sum(i => i.Cost));
        if (total == 0)
        {
            Report(warbond, "", "none of its items can be sold this session");
            return null;
        }
        var requirements = new JsonArray([.. warbond.Pages.Select(p => (JsonNode)p.Requirement)]);
        var entry = Head(warbond, total);
        entry["pageRequirements"] = requirements.DeepClone();
        entry["premiumTokenMixIds"] = Tokens(warbond);
        var detail = Head(warbond, total);
        detail["layoutSize"] = new JsonObject { ["x"] = ModWarbonds.GridWidth, ["y"] = ModWarbonds.GridHeight };
        detail["layoutType"] = 1;
        detail["seasonPassPages"] = pages;
        detail["pageRequirements"] = requirements;
        detail["premiumTokenMixIds"] = Tokens(warbond);
        return new Served(warbond, entry, detail, sells, total);
    }

    // A warbond with a price or a premium look has a token, which owns it (game.dll 0x136E470); one without either has none,
    // and the game takes it as owned. A premium look without a token would read WARBOND LOCKED under forge_credits.c.
    static JsonArray Tokens(ModWarbonds.Warbond warbond) => warbond.HasToken ? [(JsonNode)warbond.Token] : [];

    static JsonObject Head(ModWarbonds.Warbond warbond, long total) => new()
    {
        ["id32"] = warbond.Id32, ["medalsTotalMixId"] = MedalsTotal, ["medalsConsumableMixId"] = warbond.CurrencyMixId,
        ["medalsSpentMixId"] = warbond.Spent, ["totalCost"] = total,
    };

    // An item's cell in the game's detail shape, or null (reported) when it cannot be sold.
    JsonObject? Cell(ModWarbonds.Warbond warbond, ModWarbonds.Item item, Lookup lookup, Dictionary<long, uint> sells)
    {
        if (lookup.Caps.TryGetValue(warbond.CurrencyMixId, out var cap) && item.Cost > cap)
        {
            Report(warbond, item.Sells.ToString(), $"it costs {item.Cost} {warbond.Currency}, and a player holds at most {cap}");
            return null;
        }
        if (ItemOf(item.Sells, lookup.Catalogue, lookup.Registered, out var why) is not { } mixId)
        {
            Report(warbond, item.Sells.ToString(), why);
            return null;
        }
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
            else if (IsAllowed?.Invoke(sells.MixId) == false) why = "it is premium content the player does not own";
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
        if (path == "/Progression" && !UnlockAll) return body;
        var served = Resolve();
        if (served.Count == 0) return body;
        var node = JsonNode.Parse(body.GetRawText());
        var changed = (path, node) switch
        {
            ("/SeasonPass", JsonArray list) => AddEntries(list, served),
            ("/Progression/items", JsonArray catalogue) => AddCounters(catalogue, served),
            ("/Progression/inventory", JsonObject { } inventory) when inventory["items"] is JsonArray held => HoldTokens(held, served) | (UnlockAll && FillCounters(held, served)),
            ("/Progression", JsonObject progression) => BuyAll(progression, served),
            _ => false,
        };
        if (!changed) return body;
        using var document = JsonDocument.Parse(node!.ToJsonString());
        return document.RootElement.Clone();
    }

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

    // The tokens of open warbonds (a premium look, no price) are everyone's; on unlock-all every token is.
    bool HoldTokens(JsonArray held, IReadOnlyList<Served> served)
    {
        var have = held.OfType<JsonObject>().Select(i => (uint?)i["mixId"]).ToHashSet();
        foreach (var warbond in served.Select(w => w.Spec).Where(w => w.HasToken && (w.UnlockCost == 0 || UnlockAll) && !have.Contains(w.Token)))
            held.Add((JsonNode)new JsonObject { ["mixId"] = warbond.Token, ["amount"] = 1, ["isDiscovered"] = true });
        return true;
    }

    // Unlock-all: each spent counter at its warbond's whole cost, so every page is open.
    static bool FillCounters(JsonArray held, IReadOnlyList<Served> served)
    {
        foreach (var warbond in served)
        {
            var counter = held.OfType<JsonObject>().FirstOrDefault(i => (uint?)i["mixId"] == warbond.Spec.Spent);
            if (counter != null) counter["amount"] = Math.Max((long?)counter["amount"] ?? 0, warbond.TotalCost);
            else held.Add((JsonNode)new JsonObject { ["mixId"] = warbond.Spec.Spent, ["amount"] = warbond.TotalCost, ["isDiscovered"] = true });
        }
        return true;
    }

    // Unlock-all: every item of the mods' warbonds bought.
    static bool BuyAll(JsonObject progression, IReadOnlyList<Served> served)
    {
        var bought = progression["purchasedSeasonPassItems"] as JsonArray ?? [];
        var have = bought.OfType<JsonValue>().Select(v => v.TryGetValue<long>(out var id) ? id : -1).ToHashSet();
        foreach (var id in served.SelectMany(w => w.Sells.Keys).Where(have.Add)) bought.Add((JsonNode)id);
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

    // A mod warbond's token sold for its price (in Forge Credits), or null for any other item.
    public long? UnlockPrice(uint mixId) =>
        mixId == 0 ? null : Resolve().Select(w => w.Spec).FirstOrDefault(w => w.HasToken && w.UnlockCost > 0 && w.Token == mixId)?.UnlockCost;

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

    // Whether a mixId is a mod's registered weapon or stratagem: a purchase grants it without saving it in the profile,
    // which RegisteredItems serves while the item stays bought.
    public bool IsRegistered(uint mixId) => Registered?.Load().Any(i => i.MixId == mixId && !i.IsDifficulty) == true;
}
