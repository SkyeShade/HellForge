using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Weapons code mods register with the HellForge weapon registry (native/mod-loader/registry.c) for this session: the mod
// loader writes <session>/registered-items.json ({"items":[{"mixId","copyOf"}]}) as mods register them, and the
// game's progression data then carries each one as a copy of its base weapon:
//   /Progression/items        the base's catalogue entry under the new id, followed by copies of the base's children
//                             (attachments and other customizations);
//   /Progression/ProgressionPackages  a copy of the base's weapon mastery package (type 2) for the new weapon;
//   /Progression              progress in that package: the profile's own entry if it has one, else the base's;
//   /Progression/inventory    the weapon, and the copy of every child of the base the player holds.
// A weapon's children are its own items: a child's itemId is the attachment and its mixId WeaponChild(attachment,
// weapon), the hash the armory's customization screen recomputes from the attachment and the weapon (game.dll
// 0x1493770); the attachment list (0x1370420) matches a child's itemId to the attachment and its parent's itemId to
// the weapon. So the base's children are copied with the same itemId and the mixId re-hashed under the new weapon. A weapon without its own children and mastery package crashed that screen.
// Stratagems code mods add (native/mod-loader/stratagems.c) are entries {"kind":"stratagem","mixId","stable","copyOf"}:
// a stratagem is a catalogue item (progressionCategory 11) with one child (10) whose itemId is the stratagem's stable id
// (StratagemInfo +4, what the loadout checks and saves) and whose mixId is WeaponChild(stable id, item). The base's two
// items (found by its stable id, copyOf) are copied under the new ids, so the new one has the base's level, price and
// traits; the inventory holds the item. copyOf is 0 until the game has loaded its stratagems, and such entries wait.
// A difficulty a mod adds (native/mod-loader/difficulty.c) is {"kind":"difficulty","mixId","copyOf"}: its unlock item,
// a copy of level 10's (copyOf); the inventory holds it only when it holds level 10's, so it unlocks with Super Helldive.
// /Configuration/GameClient carries the live game's cap on the difficulty level (online override 48726272, an int, "10";
// game.dll 0x11EBB40 returns min(its own maximum, the cap)): it is raised to the level the mod added.
// Nothing is saved to the profile, so a removed mod leaves nothing behind.
// Premium content (docs/modding/concepts.md "Premium content"): a weapon or stratagem copied from content the player does
// not own (OwnsBase: OwnershipFilter's rules), or registered by a mod whose declared premium list the player does not own
// (LockedMods, from mods-stage), stays out of /Progression/inventory: the game knows it, the player cannot equip it.
// The client keeps only so much of each list and drops the rest without a word (research content-limits\catalogue):
//   /Progression/items                its first 4096 items (game.dll 0x107EA50, min(count, 0x1000)): 4096 inline records
//                                     behind it, so nothing else guards them;
//   /Progression/inventory            its first 4096 held items (0x103927B);
//   /Progression/ProgressionPackages  any number, but a package's progress lands in slot <its position> of 256
//                                     (0x1038CDF, unchecked): a package at position 256 or more overwrites the catalogue;
//   /Progression packageProgress      its first 256 entries (0x1038C54); the rest show level 0.
// So the registered items go in a fixed order (the difficulty, then stratagems, then weapons, each in registration
// order), each one whole (a weapon with all of its children and its mastery package) while it fits; one that doesn't is
// left out of every list, and the backend says so (Log). A mod's stratagems no longer vanish behind its weapons.
public sealed class RegisteredItems(string path)
{
    public const int CatalogueLimit = 4096, InventoryLimit = 4096, PackageLimit = 256, ProgressLimit = 256;

    // Where the backend says what it left out (stderr, in the session's logs); each report once.
    public Action<string> Log { get; set; } = line => Console.Error.WriteLine(line);
    string? reported;

    public sealed record Item(uint MixId, uint CopyOf, uint Stable = 0, bool IsDifficulty = false, uint Level = 0, string Mod = "")
    {
        public bool IsStratagem => Stable != 0;
        // Weapons: the items with children, mastery packages and progress of their own.
        public bool IsWeapon => !IsStratagem && !IsDifficulty;
    }

    const uint M = 0x5BD1E995;
    // The online override capping the difficulty level (its mixId: the client's hash of keys AF218CAC, FAABBEA9).
    public const uint DifficultyCapOverride = 48726272;
    static readonly uint InverseM = Inverse(M);

    // Responses the copies are made from (the backend's startup bodies), set by LocalBackend.
    public Func<string, JsonElement?>? Source { get; set; }

    // Where the mod loader writes them: the game's log folder, which is the session's run (the backend evidence folder's
    // parent) or, sandboxed, the run's game folder, the one the game may write (PlaySession's gameOut). Read from the run,
    // a sandboxed session lost them all: mods' weapons and stratagems, level 11 and the raised difficulty cap.
    public static string PathFor(string evidence, bool sandboxed)
    {
        var run = Path.GetDirectoryName(Path.GetFullPath(evidence))!;
        return Path.Combine(sandboxed ? Path.Combine(run, "game") : run, "registered-items.json");
    }

    // Whether the player may hold a base item; null: every base (no premium rules, as in tests and the development backend).
    public Func<uint, bool>? OwnsBase { get; set; }

    // Mods whose items stay out of the player's inventory (mods-stage's --premium-locked).
    public IReadOnlySet<string> LockedMods { get; set; } = new HashSet<string>();

    // The loader's copies use their bases' attachments (native/mod-loader/shared_attachments.c: the game reads a copy's id as
    // its base's where it finds a weapon's attachments): a weapon copy is then its own item only, with no children, mastery
    // package or progress of its own, so it costs one catalogue item instead of one per attachment (57 for a Liberator).
    public bool SharedChildren { get; private set; }

    public IReadOnlyList<Item> Load()
    {
        SharedChildren = false;
        try
        {
            if (!File.Exists(path)) return [];
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var document = JsonDocument.Parse(stream);
            SharedChildren = document.RootElement.TryGetProperty("sharedChildren", out var shared) && shared.ValueKind == JsonValueKind.True;
            List<Item> items = [];
            foreach (var item in document.RootElement.GetProperty("items").EnumerateArray())
                if (Read(item) is { } registered) items.Add(registered);
            return items;
        }
        catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException or InvalidOperationException) { return []; }
    }

    // One entry of the file: a weapon, a stratagem or a difficulty, or null for one that is malformed or waits (copyOf 0).
    static Item? Read(JsonElement item)
    {
        if (!item.TryGetProperty("mixId", out var id) || !id.TryGetUInt32(out var mixId) || mixId == 0 ||
            !item.TryGetProperty("copyOf", out var copy) || !copy.TryGetUInt32(out var copyOf))
            return null;
        if (item.TryGetProperty("kind", out var kind) && kind.ValueKind == JsonValueKind.String && kind.GetString() == "stratagem")
            return copyOf != 0 && item.TryGetProperty("stable", out var stable) && stable.TryGetUInt32(out var stableId) && stableId != 0
                ? new Item(mixId, copyOf, stableId, Mod: ModOf(item)) : null;
        if (item.TryGetProperty("kind", out kind) && kind.ValueKind == JsonValueKind.String && kind.GetString() == "difficulty")
            return copyOf != 0 && item.TryGetProperty("level", out var level) && level.TryGetUInt32(out var levelValue) && levelValue is > 10 and <= 15
                ? new Item(mixId, copyOf, IsDifficulty: true, Level: levelValue, Mod: ModOf(item)) : null;
        return new Item(mixId, copyOf, Mod: ModOf(item));
    }

    // The id of a weapon's child item (an attachment's id for that weapon), as the client computes it.
    public static uint WeaponChild(uint attachment, uint weapon)
    {
        var k = attachment * M;
        k ^= k >> 24;
        return (k * M) ^ (weapon * M);
    }

    // The attachment a weapon's child item stands for (WeaponChild inverted).
    public static uint Attachment(uint child, uint weapon)
    {
        var k = (child ^ (weapon * M)) * InverseM;
        k ^= k >> 24;
        return k * InverseM;
    }

    // A base child's id under the new weapon.
    public static uint Rehome(uint child, uint from, uint to) => WeaponChild(Attachment(child, from), to);

    // The id of the new weapon's mastery package.
    public static uint PackageFor(uint basePackage, uint from, uint to) => Rehome(basePackage, from, to);

    // The response body for a startup path, with the registered items added where they belong.
    public JsonElement Apply(string path, JsonElement body)
    {
        if (path is not ("/Progression/items" or "/Progression/inventory" or "/Progression/ProgressionPackages" or "/Progression" or
            "/Configuration/GameClient"))
            return body;
        var items = Load();
        if (items.Count == 0) return body;
        var node = JsonNode.Parse(body.GetRawText());
        // The vanilla lists every response is planned from, so all four agree on what goes in.
        var vanilla = new Catalogue(Vanilla("/Progression/items", path, node));
        var vanillaPackages = Vanilla("/Progression/ProgressionPackages", path, node);
        var plan = Admit(items, vanilla, vanillaPackages, SharedChildren);
        List<string> leftOut = [.. plan.LeftOut];
        switch (path)
        {
            case "/Progression/items" when node is JsonArray catalogue:
                AddCatalogue(catalogue, plan.Admitted, vanilla, SharedChildren);
                break;
            case "/Progression/ProgressionPackages" when node is JsonArray packages && !SharedChildren:
                AddPackages(packages, plan.Admitted, vanilla);
                break;
            case "/Progression" when node is JsonObject progression && !SharedChildren:
                AddProgress(progression, plan.Admitted, vanillaPackages, leftOut);
                break;
            case "/Progression/inventory" when node is JsonObject inventory && inventory["items"] is JsonArray held:
                AddInventory(held, plan.Admitted, vanilla, leftOut, SharedChildren);
                break;
            case "/Configuration/GameClient" when node is JsonObject client && items.Any(i => i.IsDifficulty):
                RaiseDifficultyCap(client, items.Where(i => i.IsDifficulty).Max(i => i.Level));
                break;
            default:
                return body;
        }
        Report(leftOut);
        using var document = JsonDocument.Parse(node!.ToJsonString());
        return document.RootElement.Clone();
    }

    // A vanilla list: the startup body (Source), or the response itself for its own path (as in tests without a Source).
    JsonArray Vanilla(string list, string path, JsonNode? response) =>
        Source?.Invoke(list) is { ValueKind: JsonValueKind.Array } body ? (JsonArray)JsonNode.Parse(body.GetRawText())!
            : path == list && response is JsonArray own ? (JsonArray)own.DeepClone() : [];

    void Report(List<string> leftOut)
    {
        var text = leftOut.Count == 0 ? null
            : $"[registered items] {leftOut.Count} left out, the game would drop them: " + string.Join("; ", leftOut);
        if (text == null || text == reported) return;
        reported = text;
        Log(text);
    }

    // The vanilla catalogue, looked up by id: items (no parent) by mixId, children by their parent, a stratagem's child
    // (progressionCategory 10) by its itemId, the stratagem's stable id. The first of each, as the client reads them.
    sealed class Catalogue
    {
        public readonly int Count;
        public readonly Dictionary<uint, JsonObject> Items = [];
        public readonly Dictionary<uint, List<JsonObject>> Children = [];
        public readonly Dictionary<uint, uint> ParentOf = [];
        public readonly Dictionary<uint, JsonObject> StratagemChildren = [];
        public readonly HashSet<uint> Known = [];

        public Catalogue(JsonArray catalogue)
        {
            Count = catalogue.Count;
            foreach (var item in catalogue.OfType<JsonObject>())
            {
                if (Id(item, "mixId") is not { } mixId) continue;
                Known.Add(mixId);
                if (Id(item, "parentId") is not { } parent)
                {
                    Items.TryAdd(mixId, item);
                    continue;
                }
                ParentOf[mixId] = parent;
                if (!Children.TryGetValue(parent, out var children)) Children[parent] = children = [];
                children.Add(item);
                if (Id(item, "progressionCategory") == 10 && Id(item, "itemId") is { } stable) StratagemChildren.TryAdd(stable, item);
            }
        }

        // A stratagem's base: its child (by the base's stable id, copyOf) and the item that child belongs to.
        public (JsonObject Item, JsonObject Child)? StratagemBase(Item item) =>
            StratagemChildren.TryGetValue(item.CopyOf, out var child) && Id(child, "parentId") is { } parent &&
            Items.TryGetValue(parent, out var baseItem) ? (baseItem, child) : null;
    }

    sealed record Plan(List<Item> Admitted, List<string> LeftOut);

    // The difficulty first, then stratagems, then weapons, each in registration order.
    static IEnumerable<Item> Ordered(IReadOnlyList<Item> items) =>
        items.Where(i => i.IsDifficulty).Concat(items.Where(i => i.IsStratagem)).Concat(items.Where(i => i.IsWeapon));

    // What goes in while it fits: each item's catalogue items (a stratagem's two, a weapon's copy and its children's
    // copies) within CatalogueLimit, and a weapon's mastery package within PackageLimit positions.
    static Plan Admit(IReadOnlyList<Item> items, Catalogue vanilla, JsonArray packages, bool shared)
    {
        var known = new HashSet<uint>(vanilla.Known);
        var mastery = MasteryPackages(packages);
        int used = vanilla.Count, positions = packages.Count;
        List<Item> admitted = [];
        List<string> leftOut = [];
        foreach (var item in Ordered(items))
        {
            List<uint> ids = [];
            if (!known.Contains(item.MixId))
            {
                if (item.IsStratagem)
                {
                    if (vanilla.StratagemBase(item) != null) ids.AddRange([item.MixId, WeaponChild(item.Stable, item.MixId)]);
                }
                else if (vanilla.Items.ContainsKey(item.CopyOf))
                {
                    ids.Add(item.MixId);
                    foreach (var child in shared ? [] : vanilla.Children.GetValueOrDefault(item.CopyOf) ?? [])
                        ids.Add(Rehome(Id(child, "mixId")!.Value, item.CopyOf, item.MixId));
                }
            }
            var fresh = ids.Where(id => !known.Contains(id)).Distinct().ToList();
            var package = !shared && item.IsWeapon && mastery.ContainsKey(item.CopyOf) && !mastery.ContainsKey(item.MixId) ? 1 : 0;
            if (used + fresh.Count > CatalogueLimit)
                leftOut.Add($"{Describe(item)} (its {fresh.Count} catalogue items: the game keeps {CatalogueLimit})");
            else if (positions + package > PackageLimit)
                leftOut.Add($"{Describe(item)} (its mastery package: the game keeps progress for {PackageLimit} packages)");
            else
            {
                admitted.Add(item);
                known.UnionWith(fresh);
                used += fresh.Count;
                positions += package;
            }
        }
        return new Plan(admitted, leftOut);
    }

    static string Describe(Item item) =>
        (item.IsStratagem ? "stratagem " : item.IsDifficulty ? "difficulty " : "weapon ") + item.MixId + (item.Mod == "" ? "" : " of " + item.Mod);

    // A uint field, however its node was made (a JsonValue made from a uint does not convert to long).
    static uint? Id(JsonNode? node, string name) =>
        node is JsonObject o && o[name] is JsonValue v && v.GetValueKind() == JsonValueKind.Number &&
        uint.TryParse(v.ToJsonString(), out var id) ? id : null;

    static void AddCatalogue(JsonArray catalogue, List<Item> items, Catalogue vanilla, bool shared)
    {
        var known = new HashSet<uint>(vanilla.Known);
        foreach (var item in items)
        {
            if (known.Contains(item.MixId)) continue;
            if (item.IsStratagem)
            {
                AddStratagem(catalogue, item, vanilla, known);
                continue;
            }
            if (!vanilla.Items.TryGetValue(item.CopyOf, out var source)) continue;
            var copy = source.DeepClone().AsObject();
            copy["itemId"] = item.MixId;
            copy["mixId"] = item.MixId;
            catalogue.Add((JsonNode?)copy);
            // Its children right after it: the client links a child to the preceding parent run. Shared, it has none.
            foreach (var child in shared ? [] : vanilla.Children.GetValueOrDefault(item.CopyOf) ?? [])
            {
                var mixId = Rehome(Id(child, "mixId")!.Value, item.CopyOf, item.MixId);
                if (!known.Add(mixId)) continue;
                var childCopy = child.DeepClone().AsObject(); // itemId: the same attachment
                childCopy["mixId"] = mixId;
                childCopy["parentId"] = item.MixId;
                catalogue.Add((JsonNode?)childCopy);
            }
            known.Add(item.MixId);
        }
    }

    // A stratagem's item and its child, copied from its base's.
    static void AddStratagem(JsonArray catalogue, Item item, Catalogue vanilla, HashSet<uint> known)
    {
        if (vanilla.StratagemBase(item) is not ({ } baseItem, { } baseChild)) return;
        var copy = baseItem.DeepClone().AsObject();
        copy["itemId"] = item.MixId;
        copy["mixId"] = item.MixId;
        catalogue.Add((JsonNode?)copy);
        var child = baseChild.DeepClone().AsObject();
        child["itemId"] = item.Stable;
        child["parentId"] = item.MixId;
        child["mixId"] = WeaponChild(item.Stable, item.MixId);
        catalogue.Add((JsonNode?)child);
        known.Add(item.MixId);
        known.Add(WeaponChild(item.Stable, item.MixId));
    }

    // Each weapon's mastery package (type 2, the first that requires it).
    static Dictionary<uint, JsonObject> MasteryPackages(JsonArray packages)
    {
        Dictionary<uint, JsonObject> mastery = [];
        foreach (var package in packages.OfType<JsonObject>().Where(p => Id(p, "type") == 2))
            foreach (var required in package["requiredItems"] as JsonArray ?? [])
                if (required is JsonValue v && uint.TryParse(v.ToJsonString(), out var weapon)) mastery.TryAdd(weapon, package);
        return mastery;
    }

    static void AddPackages(JsonArray packages, List<Item> items, Catalogue vanilla)
    {
        var mastery = MasteryPackages(packages);
        foreach (var item in items.Where(i => i.IsWeapon))
        {
            if (!mastery.TryGetValue(item.CopyOf, out var source) || mastery.ContainsKey(item.MixId)) continue;
            var copy = source.DeepClone().AsObject();
            copy["id32"] = PackageFor(Id(source, "id32")!.Value, item.CopyOf, item.MixId);
            copy["requiredItems"] = new JsonArray(item.MixId);
            RehomeLevels(copy, vanilla.ParentOf, item);
            packages.Add((JsonNode?)copy);
            mastery[item.MixId] = copy;
        }
    }

    // A copied mastery package's rewards and unlocks that are the base's children become the new weapon's.
    static void RehomeLevels(JsonObject package, Dictionary<uint, uint> children, Item item)
    {
        uint Map(uint id) => children.TryGetValue(id, out var parent) && parent == item.CopyOf ? Rehome(id, item.CopyOf, item.MixId) : id;
        foreach (var level in (package["levels"] as JsonArray ?? []).OfType<JsonObject>())
        {
            foreach (var reward in (level["rewards"] as JsonArray ?? []).OfType<JsonObject>())
                if (Id(reward, "type") == 1 && Id(reward, "id32") is { } id) reward["id32"] = Map(id);
            foreach (var unlock in (level["unlockableItems"] as JsonArray ?? []).OfType<JsonObject>())
                if (Id(unlock, "mixId") is { } id) unlock["mixId"] = Map(id);
        }
    }

    // A weapon's progress follows its base's; past ProgressLimit entries the game drops it (the weapon shows level 0).
    static void AddProgress(JsonObject progression, List<Item> items, JsonArray packages, List<string> leftOut)
    {
        if (progression["packageProgress"] is not JsonArray progress) return;
        var mastery = MasteryPackages(packages);
        var entries = progress.OfType<JsonObject>().GroupBy(e => Id(e, "id32") ?? 0).ToDictionary(g => g.Key, g => g.First());
        var full = 0;
        foreach (var item in items.Where(i => i.IsWeapon))
        {
            if (!mastery.TryGetValue(item.CopyOf, out var source)) continue;
            var basePackage = Id(source, "id32")!.Value;
            var own = PackageFor(basePackage, item.CopyOf, item.MixId);
            if (entries.ContainsKey(own) || !entries.TryGetValue(basePackage, out var baseEntry)) continue;
            if (progress.Count >= ProgressLimit) { full++; continue; }
            var copy = baseEntry.DeepClone().AsObject();
            copy["id32"] = own;
            progress.Add((JsonNode?)copy);
            entries[own] = copy;
        }
        if (full > 0) leftOut.Add($"the levels of {full} weapons (the game keeps {ProgressLimit} weapon levels; they show level 0)");
    }

    // Each item with the copies of its base's children the player holds, whole, while the game keeps them (InventoryLimit).
    void AddInventory(JsonArray held, List<Item> items, Catalogue vanilla, List<string> leftOut, bool shared)
    {
        var have = held.Select(i => Id(i, "mixId")).OfType<uint>().ToHashSet();
        // The base children the player holds, by base.
        Dictionary<uint, List<JsonObject>> heldChildren = [];
        foreach (var entry in held.OfType<JsonObject>())
            if (Id(entry, "mixId") is { } id && vanilla.ParentOf.TryGetValue(id, out var parent))
            {
                if (!heldChildren.TryGetValue(parent, out var list)) heldChildren[parent] = list = [];
                list.Add(entry);
            }
        foreach (var item in items.Where(i => !Locked(i, vanilla)))
        {
            if (item.IsDifficulty && !have.Contains(item.CopyOf)) continue; /* not before level 10 is unlocked */
            List<JsonObject> adds = [];
            if (!have.Contains(item.MixId)) adds.Add(new JsonObject { ["mixId"] = item.MixId, ["amount"] = 1, ["isDiscovered"] = true });
            if (item.IsWeapon && !shared)
                foreach (var entry in heldChildren.GetValueOrDefault(item.CopyOf) ?? [])
                {
                    var mixId = Rehome(Id(entry, "mixId")!.Value, item.CopyOf, item.MixId);
                    if (have.Contains(mixId) || adds.Any(a => Id(a, "mixId") == mixId)) continue;
                    var copy = entry.DeepClone().AsObject();
                    copy["mixId"] = mixId;
                    adds.Add(copy);
                }
            if (held.Count + adds.Count > InventoryLimit)
            {
                leftOut.Add($"{Describe(item)} from the inventory (the game keeps {InventoryLimit} owned items)");
                continue;
            }
            foreach (var add in adds)
            {
                have.Add(Id(add, "mixId")!.Value);
                held.Add((JsonNode?)add);
            }
        }
    }

    // The mod that registered an entry (registry.c writes its id).
    static string ModOf(JsonElement item) => item.TryGetProperty("mod", out var owner) && owner.ValueKind == JsonValueKind.String ? owner.GetString()! : "";

    // A premium mod the player does not own, or a copy of a premium base they do not own; a stratagem whose base cannot be
    // found yet stays out too, until the game has loaded its stratagems.
    bool Locked(Item item, Catalogue vanilla)
    {
        if (LockedMods.Contains(item.Mod)) return true;
        if (OwnsBase == null || item.IsDifficulty) return false;
        return BaseItem(item, vanilla) is not { } baseItem || !OwnsBase(baseItem);
    }

    // The base's own item: a weapon's is copyOf; a stratagem's is the parent of the catalogue child whose itemId is its stable id.
    static uint? BaseItem(Item item, Catalogue vanilla) =>
        !item.IsStratagem ? item.CopyOf
        : vanilla.StratagemChildren.TryGetValue(item.CopyOf, out var child) ? Id(child, "parentId") : null;

    static void RaiseDifficultyCap(JsonObject client, uint level)
    {
        if (client["onlineOverrideConfiguration"] is not JsonArray overrides) return;
        foreach (var entry in overrides.OfType<JsonObject>().Where(o => Id(o, "mixId") == DifficultyCapOverride && Id(o, "dataType") == 6))
            if (int.TryParse(entry["data"]?.GetValue<string>(), out var cap) && cap < level) entry["data"] = level.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    static uint Inverse(uint a)
    {
        uint x = a; // Newton's iteration for the inverse modulo 2^32 (a odd)
        for (var i = 0; i < 5; i++) x *= 2 - a * x;
        return x;
    }
}
