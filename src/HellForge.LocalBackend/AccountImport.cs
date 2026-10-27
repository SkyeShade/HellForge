using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// Merges a verified account snapshot into a profile, category by category, as the player chose (HellForge.Core
// AccountImportSettings), and describes both sides for the launcher's review. Inventory items are sorted by
// PaidContentSet: warbond items, currencies, and everything else as armory items. Premium items are never merged:
// OwnershipFilter serves them straight from the snapshot. Super Credits are never taken, and none stay in a merged profile.
static class AccountImport
{
    enum Kind { Premium, SuperCredits, Currency, Warbond, Armory }

    static readonly (string Category, Kind Kind)[] ItemCategories =
        [("warbondItems", Kind.Warbond), ("armory", Kind.Armory), ("currencies", Kind.Currency)];

    const uint Medals = 1276729503, Requisition = 0xD7150EEC;
    static readonly uint[] Samples = [0xEDF6EAF5, 0xB1ED1C41, 0xDAC0E9DB];

    public static JsonObject Merge(JsonObject profile, AccountSnapshot snapshot, AccountImportSettings settings, PaidContentSet paid)
    {
        var merged = (JsonObject)profile.DeepClone();
        var items = ItemList(merged);
        foreach (var (category, kind) in ItemCategories)
            if (settings.Includes(category)) MergeItems(items, snapshot.Inventory.RootElement, kind, settings.RuleFor(category), paid);
        if (snapshot.Progression?.RootElement is { ValueKind: JsonValueKind.Object } vanilla)
            MergeProgression((JsonObject)merged["progression"]!, vanilla, settings);
        if (settings.Includes("warbondItems") && settings.RuleFor("warbondItems") == ImportRule.All)
            UnlockOwnedWarbonds(items, (JsonObject)merged["progression"]!, OwnedWarbondItems(snapshot, paid), paid);
        foreach (var superCredits in items.OfType<JsonObject>().Where(i => KindOf(i, paid) == Kind.SuperCredits).ToList()) items.Remove(superCredits);
        return merged;
    }

    // The once-bought items of every warbond the snapshot owns (PaidContentSet.OwnedWarbonds), by their unlock id.
    static List<(long ItemId, uint MixId)> OwnedWarbondItems(AccountSnapshot snapshot, PaidContentSet paid) =>
        [.. paid.OwnedWarbonds(snapshot.Items().Select(MixId).OfType<uint>().ToHashSet()).SelectMany(warbond => warbond.Items).DistinctBy(item => item.ItemId)];

    // All: each owned warbond item's unlock record and the item itself, besides what the profile holds already. An item
    // that is not a plain warbond item (Super Credits, a paid item, a currency) is never added.
    static void UnlockOwnedWarbonds(JsonArray items, JsonObject progression, List<(long ItemId, uint MixId)> owned, PaidContentSet paid)
    {
        var unlocked = new SortedSet<long>((progression["purchasedSeasonPassItems"] as JsonArray ?? []).OfType<JsonValue>().Select(Whole).OfType<long>());
        unlocked.UnionWith(owned.Select(item => item.ItemId));
        progression["purchasedSeasonPassItems"] = new JsonArray([.. unlocked.Select(id => (JsonNode?)id)]);
        var held = items.OfType<JsonObject>().Select(MixId).OfType<uint>().ToHashSet();
        foreach (var mixId in owned.Select(item => item.MixId))
        {
            var item = new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true };
            if (KindOf(item, paid) == Kind.Warbond && held.Add(mixId)) items.Add((JsonNode?)item);
        }
    }

    static void MergeProgression(JsonObject progression, JsonElement vanilla, AccountImportSettings settings)
    {
        if (settings.Includes("level") && Whole(vanilla, "experience") is { } experience)
            progression["experience"] = settings.RuleFor("level") == ImportRule.Vanilla ? experience
                : Math.Max(experience, Whole(progression, "experience") ?? 0);
        if (settings.Includes("warbondItems")) MergeWarbondUnlocks(progression, vanilla, settings.RuleFor("warbondItems"));
        if (settings.Includes("packages")) MergePackages(progression, vanilla, settings.RuleFor("packages"));
        if (settings.Includes("ship")) Copy(progression, vanilla, "shipName", "location");
        if (settings.Includes("onboarding")) Copy(progression, vanilla, "onboardingState", "onboardingFlags");
    }

    // Merge and Higher keep every item of the kind either side holds, at the larger amount; Vanilla takes the account's.
    static void MergeItems(JsonArray items, JsonElement inventory, Kind kind, ImportRule rule, PaidContentSet paid)
    {
        if (rule == ImportRule.Vanilla)
            foreach (var local in items.OfType<JsonObject>().Where(i => KindOf(i, paid) == kind).ToList()) items.Remove(local);
        // A profile edited by hand may list an item twice; the first entry is the one that counts.
        var held = new Dictionary<uint, JsonObject>();
        foreach (var local in items.OfType<JsonObject>())
            if (MixId(local) is { } localId) held.TryAdd(localId, local);
        foreach (var vanilla in inventory.GetProperty("items").EnumerateArray())
        {
            if (Item(vanilla) is not { } item || KindOf(item, paid) != kind) continue;
            var id = MixId(item)!.Value;
            if (!held.TryGetValue(id, out var local))
            {
                held[id] = item;
                items.Add((JsonNode?)item);
            }
            else
            {
                local["amount"] = Math.Max(Whole(local, "amount") ?? 0, Whole(item, "amount") ?? 0);
                local["isDiscovered"] = local["isDiscovered"]?.GetValueKind() == JsonValueKind.True || item["isDiscovered"]!.GetValue<bool>();
            }
        }
    }

    // The warbond items' purchase records, and which warbond earns medals.
    static void MergeWarbondUnlocks(JsonObject progression, JsonElement vanilla, ImportRule rule)
    {
        if (vanilla.TryGetProperty("purchasedSeasonPassItems", out var bought) && bought.ValueKind == JsonValueKind.Array)
        {
            var ids = new SortedSet<long>(bought.EnumerateArray().Where(i => i.ValueKind == JsonValueKind.Number && i.TryGetInt64(out _)).Select(i => i.GetInt64()));
            if (rule != ImportRule.Vanilla && progression["purchasedSeasonPassItems"] is JsonArray local)
                ids.UnionWith(local.OfType<JsonValue>().Select(v => Whole(v)).OfType<long>());
            progression["purchasedSeasonPassItems"] = new JsonArray([.. ids.Select(id => (JsonNode?)id)]);
        }
        if (vanilla.TryGetProperty("activeSeasonPass", out var active) && (rule == ImportRule.Vanilla || !progression.ContainsKey("activeSeasonPass")))
            progression["activeSeasonPass"] = JsonNode.Parse(active.GetRawText());
    }

    // Higher keeps, per package, whichever side reached the higher level (then the more progress); Vanilla takes the account's.
    static void MergePackages(JsonObject progression, JsonElement vanilla, ImportRule rule)
    {
        if (!vanilla.TryGetProperty("packageProgress", out var packages) || packages.ValueKind != JsonValueKind.Array) return;
        var result = new Dictionary<long, JsonObject>();
        if (rule != ImportRule.Vanilla)
            foreach (var local in (progression["packageProgress"] as JsonArray ?? []).OfType<JsonObject>())
                if (Whole(local, "id32") is { } id) result[id] = (JsonObject)local.DeepClone();
        foreach (var entry in packages.EnumerateArray().Where(p => p.ValueKind == JsonValueKind.Object))
        {
            var package = JsonNode.Parse(entry.GetRawText())!.AsObject();
            if (Whole(package, "id32") is not { } id) continue;
            if (!result.TryGetValue(id, out var local) || Further(package, local)) result[id] = package;
        }
        progression["packageProgress"] = new JsonArray([.. result.Values]);
    }

    static bool Further(JsonObject candidate, JsonObject current) =>
        (Whole(candidate, "level") ?? 0, Whole(candidate, "progress") ?? 0).CompareTo((Whole(current, "level") ?? 0, Whole(current, "progress") ?? 0)) > 0;

    static void Copy(JsonObject progression, JsonElement vanilla, params string[] fields)
    {
        foreach (var field in fields)
            if (vanilla.TryGetProperty(field, out var value)) progression[field] = JsonNode.Parse(value.GetRawText());
    }

    // The launcher's review: each category's HellForge and vanilla side as text, and the premium items always synced.
    public static JsonObject Preview(JsonObject profile, AccountSnapshot snapshot, PaidContentSet paid)
    {
        var progression = (JsonObject)profile["progression"]!;
        var vanilla = snapshot.Progression?.RootElement is { ValueKind: JsonValueKind.Object } captured
            ? JsonNode.Parse(captured.GetRawText())!.AsObject() : null;
        var localItems = ItemList(profile).OfType<JsonObject>().ToList();
        var vanillaItems = snapshot.Items().ToList();
        string Side(JsonObject? side, Func<JsonObject, string> describe) => side == null ? "not captured" : describe(side);
        var categories = new JsonObject
        {
            ["level"] = Pair(Experience(progression), Side(vanilla, Experience)),
            ["warbondItems"] = Pair(WarbondText(localItems, progression, paid), Side(vanilla, v => WarbondText(vanillaItems, v, paid))),
            ["armory"] = Pair(Count(localItems, Kind.Armory, paid), Count(vanillaItems, Kind.Armory, paid)),
            ["packages"] = Pair(Packages(progression), Side(vanilla, Packages)),
            ["currencies"] = Pair(Currencies(localItems), Currencies(vanillaItems)),
            ["ship"] = Pair(Ship(progression), Side(vanilla, Ship)),
            ["onboarding"] = Pair(Onboarding(progression), Side(vanilla, Onboarding))
        };
        var owned = vanillaItems.Select(MixId).OfType<uint>().Where(paid.IsPaid).ToList();
        var tokens = owned.Count(paid.WarbondTokens.Contains);
        var dlc = owned.Count(paid.DlcItems.Contains);
        return new JsonObject
        {
            ["categories"] = categories,
            ["premium"] = Premium(tokens, owned.Count - tokens - dlc, dlc),
            ["warbonds"] = OwnedWarbondsText(snapshot, paid),
            ["capturedUtc"] = snapshot.Seal.CapturedUtc.ToString("O", CultureInfo.InvariantCulture)
        };
    }

    // "23 warbonds you own hold 512 unlocks": what the All choice unlocks.
    static string OwnedWarbondsText(AccountSnapshot snapshot, PaidContentSet paid)
    {
        var warbonds = paid.OwnedWarbonds(snapshot.Items().Select(MixId).OfType<uint>().ToHashSet()).ToList();
        var unlocks = warbonds.SelectMany(warbond => warbond.Items).Select(item => item.ItemId).Distinct().Count();
        return Counted(warbonds.Count, "warbond you own holds", "warbonds you own hold") + " " + Counted(unlocks, "unlock", "unlocks");
    }

    static JsonObject Pair(string hellforge, string vanilla) => new() { ["hellforge"] = hellforge, ["vanilla"] = vanilla };

    static string Experience(JsonObject progression) => Format(Whole(progression, "experience") ?? 0) + " XP";

    static string WarbondText(List<JsonObject> items, JsonObject progression, PaidContentSet paid) =>
        Count(items, Kind.Warbond, paid) + ", " + Counted((progression["purchasedSeasonPassItems"] as JsonArray)?.Count ?? 0, "warbond unlock", "warbond unlocks");

    static string Count(List<JsonObject> items, Kind kind, PaidContentSet paid) => Counted(items.Count(i => KindOf(i, paid) == kind), "item", "items");

    static string Packages(JsonObject progression) =>
        Counted((progression["packageProgress"] as JsonArray)?.Count ?? 0, "ship module or weapon level", "ship modules and weapon levels");

    static string Currencies(List<JsonObject> items)
    {
        long Amount(params uint[] ids) => items.Where(i => MixId(i) is { } id && ids.Contains(id)).Sum(i => Whole(i, "amount") ?? 0);
        return $"{Format(Amount(Medals))} medals, {Format(Amount(Requisition))} requisition, {Format(Amount(Samples))} samples";
    }

    static string Ship(JsonObject progression)
    {
        var name = progression["shipName"] is JsonArray words ? "name " + string.Join("-", words.Select(w => w?.ToJsonString())) : "no name";
        var planet = progression["location"] is JsonObject location && Whole(location, "planetIndex") is { } index ? ", planet " + Format(index) : "";
        return name + planet;
    }

    static string Onboarding(JsonObject progression) => "state " + Format(Whole(progression, "onboardingState") ?? 0);

    static string Format(long number) => number.ToString("N0", CultureInfo.InvariantCulture);

    static string Counted(long number, string one, string many) => Format(number) + " " + (number == 1 ? one : many);

    // "2 warbond tokens, 3 Superstore items and 6 DLC items", leaving out kinds the account has none of.
    static string Premium(int tokens, int store, int dlc)
    {
        List<string> parts = [];
        if (tokens > 0) parts.Add(Counted(tokens, "warbond token", "warbond tokens"));
        if (store > 0) parts.Add(Counted(store, "Superstore item", "Superstore items"));
        if (dlc > 0) parts.Add(Counted(dlc, "DLC item", "DLC items"));
        return parts.Count switch
        {
            0 => "no premium items",
            1 => parts[0],
            _ => string.Join(", ", parts[..^1]) + " and " + parts[^1]
        };
    }

    static Kind KindOf(JsonObject item, PaidContentSet paid) => MixId(item) is not { } id ? Kind.Armory
        : PaidContentSet.IsSuperCredits(id) ? Kind.SuperCredits
        : paid.IsPaid(id) ? Kind.Premium
        : paid.Currencies.Contains(id) ? Kind.Currency
        : paid.WarbondItems.Contains(id) ? Kind.Warbond
        : Kind.Armory;

    static JsonArray ItemList(JsonObject profile)
    {
        var inventory = (JsonObject)profile["inventory"]!;
        if (inventory["items"] is not JsonArray items) inventory["items"] = items = [];
        return items;
    }

    // A captured item in the profile's shape: mixId, amount and isDiscovered, nothing else.
    static JsonObject? Item(JsonElement vanilla)
    {
        if (vanilla.ValueKind != JsonValueKind.Object || !vanilla.TryGetProperty("mixId", out var mix) || !mix.TryGetUInt32(out var id)) return null;
        var amount = vanilla.TryGetProperty("amount", out var a) && a.TryGetInt64(out var value) ? Math.Clamp(value, 0, uint.MaxValue) : 1;
        var discovered = !vanilla.TryGetProperty("isDiscovered", out var d) || d.ValueKind != JsonValueKind.False;
        return new JsonObject { ["mixId"] = id, ["amount"] = amount, ["isDiscovered"] = discovered };
    }

    static uint? MixId(JsonObject item) => Whole(item, "mixId") is { } id and >= 0 and <= uint.MaxValue ? (uint)id : null;

    static long? Whole(JsonObject owner, string property) => owner[property] is JsonValue value ? Whole(value) : null;

    static long? Whole(JsonValue value) =>
        value.GetValueKind() == JsonValueKind.Number && long.TryParse(value.ToJsonString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number)
            ? number : null;

    static long? Whole(JsonElement owner, string property) =>
        owner.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number) ? number : null;
}
