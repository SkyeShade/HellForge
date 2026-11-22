using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Paints on weapons whose base has no mastery package (secondaries, support weapons). The customize screen makes its
// cards only from a weapon's mastery package (game.dll 0x1493770), so the mod loader gives each such vanilla weapon a
// customization record naming PaintPackage(weapon), the paint slot and the Default paint (registry_paints.c PaintWeapons),
// and lists them in "paintItems". Mods' copies of them inherit that record. For each of these weapons this serves:
//   /Progression/items                 the 12 game paints as its children, copies of the Liberator's;
//   /Progression/ProgressionPackages   PaintPackage(weapon): the Liberator's package with every reward and unlock taken
//                                      out, the Default paint a free unlockable of level 1 and the others its rewards;
//   /Progression                       that package at level 1, so the screen offers its rewards;
//   /Progression/inventory             the paints, held with the weapon.
// The package earns nothing: no MasteryTrack names it, so missions give it no XP.
public sealed partial class RegisteredItems
{
    // The game's weapon paints (itemIds): Default, Classic, then the solid and camo paints, Night and Venus.
    public static readonly uint[] GamePaints =
        [2163992125, 3171207820, 2489864848, 1561483157, 2193826825, 1184973026, 1744913886, 2406718068, 3320985152, 2532555742, 3096671213, 1535866827];

    // The AR-23 Liberator: its package is the template, and the catalogue has a child of every paint under it.
    const uint PaintTemplateWeapon = 2852344585;

    // The seed of a paint package's id (registry.c REG_PAINT_PACKAGE_SEED, the same derivation).
    public const uint PaintPackageSeed = 0x5EC0DA27;

    public static uint PaintPackage(uint weapon) => WeaponChild(PaintPackageSeed, weapon);

    // The file's "paintItems": the vanilla weapons the loader gave a paint package (none in an older file).
    static List<uint> PaintItems(byte[] file)
    {
        if (file.Length == 0) return [];
        try
        {
            using var document = JsonDocument.Parse(file);
            if (!document.RootElement.TryGetProperty("paintItems", out var list) || list.ValueKind != JsonValueKind.Array) return [];
            return [.. list.EnumerateArray().Where(w => w.TryGetUInt32(out var id) && id != 0).Select(w => w.GetUInt32()).Distinct()];
        }
        catch (JsonException) { return []; }
    }

    // The weapons that get paints: the listed ones and the admitted copies of them, each while the catalogue and the
    // packages have room for it (said when not). used: the catalogue items they take.
    List<uint> PlanPaints(IReadOnlyList<uint> listed, IReadOnlyList<Item> admitted, Capacity keeps, out int used)
    {
        used = 0;
        var candidates = listed.Concat(admitted.Where(i => i.IsWeapon && listed.Contains(i.CopyOf)).Select(i => i.MixId)).Distinct().ToList();
        if (Game() is not { } game) return candidates;
        var room = CatalogueRoom(game, admitted, keeps);
        var packageRoom = keeps.Packages - game.Packages - admitted.Count(i => i.IsWeapon && game.Mastery.ContainsKey(i.CopyOf));
        List<uint> painted = [];
        foreach (var weapon in candidates.Where(w => !game.Mastery.ContainsKey(w)))
        {
            var cost = GamePaints.Count(p => !game.Ids.Contains(WeaponChild(p, weapon))) + (Itemless(game, admitted, weapon) ? 1 : 0);
            if (cost > room - used || packageRoom < 1)
            {
                lock (reported)
                    if (reported.Add(weapon ^ 0x9A1E7u))
                        Log($"weapon {weapon} gets no paints: the game keeps {keeps.Items} catalogue items and {keeps.Packages} packages, and none are left.");
                continue;
            }
            used += cost;
            packageRoom--;
            painted.Add(weapon);
        }
        return painted;
    }

    // A painted weapon with no catalogue item, the game's or an arsenal copy's: a mod's entity copy of a support weapon
    // (HfEntity.own_all), which the mod loader gives a paint package of its own. It gets an item, held from the start,
    // as the customize screen finds a paint by its root item (0x1493770); its stratagem's item names another id.
    static bool Itemless(Vanilla? game, IReadOnlyList<Item> admitted, uint weapon) =>
        game is not null && !game.Ids.Contains(weapon) && !game.StratagemItems.ContainsKey(weapon) && admitted.All(i => i.MixId != weapon);

    static JsonObject WeaponItem(uint weapon) => new()
    {
        ["itemId"] = weapon, ["mixId"] = weapon, ["isConsumable"] = false, ["requiredLevel"] = 0, ["progressionCategory"] = 1,
        ["tags"] = new JsonArray(), ["requiredItems"] = new JsonArray(), ["buyPrice"] = new JsonArray(), ["sellPrice"] = new JsonArray(),
    };

    // The paints offered on a weapon of the plan, for mods' patterns copying one of them.
    static IEnumerable<uint> PaintedOffering(uint paint, IReadOnlyList<uint> painted) => GamePaints.Contains(paint) ? painted : [];

    // Each weapon's paint children, at the end of its run (the client links a child to the run before it). A support
    // weapon is its stratagem's child: the client links an entry whose parent differs from the one before to the entry
    // right before it (0x136EC00), so its paints follow it, and only where it ends its stratagem's run.
    void AddPaints(JsonArray catalogue, IReadOnlyList<uint> weapons, IReadOnlyList<Item> admitted)
    {
        if (weapons.Count == 0) return;
        var known = catalogue.Select(i => Id(i, "mixId")).ToHashSet();
        AddWeaponItems(catalogue, weapons, admitted, known);
        var index = new CatalogueIndex(catalogue);
        var (runEnd, supports) = PlacesAfterMoves(catalogue, weapons);
        List<(int After, JsonObject Entry)> inserts = [];
        foreach (var weapon in weapons)
            if (PaintPlace(weapon, runEnd, supports) is { } at)
                foreach (var paint in GamePaints)
                {
                    var mixId = WeaponChild(paint, weapon);
                    if (!index.ChildOfItem.TryGetValue(paint, out var template) || !known.Add(mixId)) continue;
                    var child = template.DeepClone().AsObject();
                    child["mixId"] = mixId;
                    child["parentId"] = at.Parent;
                    inserts.Add((at.After, child));
                }
        foreach (var group in inserts.Select((x, n) => (x.After, x.Entry, n)).GroupBy(x => x.After).OrderByDescending(g => g.Key))
            foreach (var x in group.OrderByDescending(x => x.n)) catalogue.Insert(group.Key + 1, x.Entry);
    }

    // The items of the painted weapons that have none (Itemless), at the end: each starts a run of its own.
    void AddWeaponItems(JsonArray catalogue, IReadOnlyList<uint> weapons, IReadOnlyList<Item> admitted, HashSet<uint?> known)
    {
        foreach (var weapon in weapons.Where(w => Itemless(Game(), admitted, w) && known.Add(w))) catalogue.Add((JsonNode?)WeaponItem(weapon));
    }

    // A support weapon with more of its stratagem's items after it moves behind them, so its paints can follow it and every
    // entry's link (0x136EC00) still finds its parent: the siblings link to the stratagem, the weapon to it through them.
    static bool MoveSupportsLast(JsonArray catalogue, IReadOnlyList<uint> weapons, Dictionary<uint, int> runEnd,
        Dictionary<uint, (int After, uint Parent)> supports)
    {
        var moved = false;
        foreach (var weapon in weapons)
        {
            if (runEnd.ContainsKey(weapon) || !supports.TryGetValue(weapon, out var support) || support.After >= 0) continue;
            var at = IndexOfMixId(catalogue, support.Parent);
            if (at < 0 || catalogue[at] is not JsonObject entry || Id(entry, "parentId") is not { } stratagem) continue;
            var end = Places(catalogue).RunEnd.GetValueOrDefault(stratagem, -1);
            if (end <= at) continue;
            catalogue.RemoveAt(at);
            catalogue.Insert(end, entry);
            moved = true;
        }
        return moved;
    }

    static (Dictionary<uint, int> RunEnd, Dictionary<uint, (int After, uint Parent)> Supports) PlacesAfterMoves(JsonArray catalogue, IReadOnlyList<uint> weapons)
    {
        var runEnd = Places(catalogue).RunEnd;
        if (!MoveSupportsLast(catalogue, weapons, runEnd, SupportPlaces(catalogue, runEnd))) return (runEnd, SupportPlaces(catalogue, runEnd));
        runEnd = Places(catalogue).RunEnd;
        return (runEnd, SupportPlaces(catalogue, runEnd));
    }

    static int IndexOfMixId(JsonArray catalogue, uint mixId)
    {
        for (var i = 0; i < catalogue.Count; i++) if (Id(catalogue[i], "mixId") == mixId) return i;
        return -1;
    }

    // Where a weapon's paints go and the parent they name; null, said once, for a support weapon with items after it.
    (int After, uint Parent)? PaintPlace(uint weapon, Dictionary<uint, int> runEnd, Dictionary<uint, (int After, uint Parent)> supports)
    {
        if (runEnd.TryGetValue(weapon, out var end)) return (end, weapon);
        if (!supports.TryGetValue(weapon, out var support)) return null;
        if (support.After >= 0) return support;
        lock (reported)
            if (reported.Add(weapon ^ 0x5C0A7u)) Log($"weapon {weapon} gets no paints: its stratagem has items after it.");
        return null;
    }

    // Support weapons (a stratagem's child, progressionCategory 10) by their itemId: where they are and their mixId,
    // After -1 when more of the stratagem's items follow.
    static Dictionary<uint, (int After, uint Parent)> SupportPlaces(JsonArray catalogue, Dictionary<uint, int> runEnd)
    {
        Dictionary<uint, (int, uint)> places = [];
        for (var i = 0; i < catalogue.Count; i++)
            if (catalogue[i] is JsonObject entry && Id(entry, "progressionCategory") == 10 && Id(entry, "parentId") is { } parent &&
                Id(entry, "itemId") is { } item && Id(entry, "mixId") is { } mixId)
                places.TryAdd(item, (runEnd.GetValueOrDefault(parent) == i ? i : -1, mixId));
        return places;
    }

    // Each weapon's paint package. Its 25 levels stay, so the armory's level display reads what it reads for a primary.
    static void AddPaintPackages(JsonArray packages, IReadOnlyList<uint> weapons)
    {
        if (weapons.Count == 0) return;
        var mastery = MasteryByWeapon(packages);
        if (!mastery.TryGetValue(PaintTemplateWeapon, out var template)) return;
        foreach (var weapon in weapons.Where(w => !mastery.ContainsKey(w)))
        {
            var package = template.DeepClone().AsObject();
            package["id32"] = PaintPackage(weapon);
            package["requiredItems"] = new JsonArray(weapon);
            foreach (var level in (package["levels"] as JsonArray ?? []).OfType<JsonObject>())
            {
                level["rewards"] = new JsonArray();
                level["unlockableItems"] = new JsonArray();
            }
            if (FirstLevel(package) is { } first)
            {
                // The armory shows CUSTOMIZE only for a package with an unlockable item on some level (game.dll 0x191D040
                // reads each level's unlockable count), so the Default paint is a free one there, the rest rewards.
                foreach (var paint in GamePaints.Skip(1)) AddReward(first, WeaponChild(paint, weapon));
                first["unlockableItems"] = new JsonArray(new JsonObject
                {
                    ["mixId"] = WeaponChild(GamePaints[0], weapon), ["amount"] = 1, ["sortOrder"] = 0, ["cost"] = new JsonArray(),
                });
            }
            packages.Add((JsonNode?)package);
            mastery[weapon] = package;
        }
    }

    // The paint packages at level 1 in the served progress (never saved: the profile does not change).
    static void AddPaintProgress(JsonObject progression, IReadOnlyList<uint> weapons)
    {
        if (weapons.Count == 0) return;
        if (progression["packageProgress"] is not JsonArray progress) progression["packageProgress"] = progress = [];
        var listed = progress.Select(e => Id(e, "id32")).ToHashSet();
        foreach (var weapon in weapons.Where(w => listed.Add(PaintPackage(w))))
        {
            var entry = new JsonObject { ["id32"] = PaintPackage(weapon), ["level"] = 1 };
            ProfileStore.CompletePackageEntry(entry);
            progress.Add((JsonNode?)entry);
        }
    }

    // What a painted weapon needs held besides its paints: nothing when it is held; its own item when it has one of
    // ours (Itemless) or is held with its stratagem; null when it is not held.
    static List<uint>? HeldItem(HashSet<uint?> have, uint weapon, Vanilla? game, IReadOnlyList<Item> admitted)
    {
        if (have.Contains(weapon)) return [];
        if (Itemless(game, admitted, weapon)) return [weapon];
        return game is not null && game.StratagemItems.TryGetValue(weapon, out var stratagem) && have.Contains(stratagem) &&
            game.ItemChildren.GetValueOrDefault(weapon) is [var own] ? [own] : null;
    }

    // The paints of each weapon the inventory holds, each weapon's whole or none, while the game's entries hold them. A
    // support weapon counts as held with its stratagem; its own item is held too, as the client links its paints to it.
    static void AddPaintsHeld(JsonArray held, HashSet<uint?> have, IReadOnlyList<uint> weapons, int keeps, Vanilla? game, IReadOnlyList<Item> admitted)
    {
        foreach (var weapon in weapons)
        {
            if (HeldItem(have, weapon, game, admitted) is not { } paints) continue;
            paints.AddRange(GamePaints.Select(p => WeaponChild(p, weapon)).Where(m => !have.Contains(m)));
            if (held.Count + paints.Count > keeps) continue;
            foreach (var mixId in paints)
                if (have.Add(mixId)) held.Add((JsonNode?)new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
        }
    }
}
