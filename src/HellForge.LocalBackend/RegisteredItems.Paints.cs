using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Paints on weapons whose base has no mastery package (the secondaries). The customize screen makes its cards only from
// a weapon's mastery package (game.dll 0x1493770), so the mod loader gives each vanilla secondary a customization record
// naming PaintPackage(weapon), the paint slot and the Default paint (registry_load.c PaintSecondaries), and lists them
// in "paintItems". Mods' copies of them inherit that record. For each of these weapons this serves:
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
            var cost = GamePaints.Count(p => !game.Ids.Contains(WeaponChild(p, weapon)));
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

    // The paints offered on a weapon of the plan, for mods' patterns copying one of them.
    static IEnumerable<uint> PaintedOffering(uint paint, IReadOnlyList<uint> painted) => GamePaints.Contains(paint) ? painted : [];

    // Each weapon's paint children, at the end of its run (the client links a child to the run before it).
    static void AddPaints(JsonArray catalogue, IReadOnlyList<uint> weapons)
    {
        if (weapons.Count == 0) return;
        var known = catalogue.Select(i => Id(i, "mixId")).ToHashSet();
        var index = new CatalogueIndex(catalogue);
        var (runEnd, _) = Places(catalogue);
        List<(int After, JsonObject Entry)> inserts = [];
        foreach (var weapon in weapons.Where(runEnd.ContainsKey))
            foreach (var paint in GamePaints)
            {
                var mixId = WeaponChild(paint, weapon);
                if (!index.ChildOfItem.TryGetValue(paint, out var template) || !known.Add(mixId)) continue;
                var child = template.DeepClone().AsObject();
                child["mixId"] = mixId;
                child["parentId"] = weapon;
                inserts.Add((runEnd[weapon], child));
            }
        foreach (var group in inserts.Select((x, n) => (x.After, x.Entry, n)).GroupBy(x => x.After).OrderByDescending(g => g.Key))
            foreach (var x in group.OrderByDescending(x => x.n)) catalogue.Insert(group.Key + 1, x.Entry);
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

    // The paints of each weapon the inventory holds, each weapon's whole or none, while the game's entries hold them.
    static void AddPaintsHeld(JsonArray held, HashSet<uint?> have, IReadOnlyList<uint> weapons, int keeps)
    {
        foreach (var weapon in weapons.Where(w => have.Contains(w)))
        {
            List<uint> paints = [.. GamePaints.Select(p => WeaponChild(p, weapon)).Where(m => !have.Contains(m))];
            if (held.Count + paints.Count > keeps) continue;
            foreach (var mixId in paints)
                if (have.Add(mixId)) held.Add((JsonNode?)new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
        }
    }
}
