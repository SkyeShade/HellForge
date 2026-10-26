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
public sealed class RegisteredItems(string path)
{
    public sealed record Item(uint MixId, uint CopyOf, uint Stable = 0, bool IsDifficulty = false, uint Level = 0)
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

    public IReadOnlyList<Item> Load()
    {
        try
        {
            if (!File.Exists(path)) return [];
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var document = JsonDocument.Parse(stream);
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
                ? new Item(mixId, copyOf, stableId) : null;
        if (item.TryGetProperty("kind", out kind) && kind.ValueKind == JsonValueKind.String && kind.GetString() == "difficulty")
            return copyOf != 0 && item.TryGetProperty("level", out var level) && level.TryGetUInt32(out var levelValue) && levelValue is > 10 and <= 15
                ? new Item(mixId, copyOf, IsDifficulty: true, Level: levelValue) : null;
        return new Item(mixId, copyOf);
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
        switch (path)
        {
            case "/Progression/items" when node is JsonArray catalogue:
                AddCatalogue(catalogue, items);
                break;
            case "/Progression/ProgressionPackages" when node is JsonArray packages:
                AddPackages(packages, items);
                break;
            case "/Progression" when node is JsonObject progression:
                AddProgress(progression, items);
                break;
            case "/Progression/inventory" when node is JsonObject inventory && inventory["items"] is JsonArray held:
                AddInventory(held, items);
                break;
            case "/Configuration/GameClient" when node is JsonObject client && items.Any(i => i.IsDifficulty):
                RaiseDifficultyCap(client, items.Where(i => i.IsDifficulty).Max(i => i.Level));
                break;
            default:
                return body;
        }
        using var document = JsonDocument.Parse(node!.ToJsonString());
        return document.RootElement.Clone();
    }

    // A uint field, however its node was made (a JsonValue made from a uint does not convert to long).
    static uint? Id(JsonNode? node, string name) =>
        node is JsonObject o && o[name] is JsonValue v && v.GetValueKind() == JsonValueKind.Number &&
        uint.TryParse(v.ToJsonString(), out var id) ? id : null;

    static void AddCatalogue(JsonArray catalogue, IReadOnlyList<Item> items)
    {
        var known = catalogue.Select(i => Id(i, "mixId")).ToHashSet();
        foreach (var item in items)
        {
            if (known.Contains(item.MixId)) continue;
            if (item.IsStratagem)
            {
                AddStratagem(catalogue, item, known);
                continue;
            }
            var source = catalogue.OfType<JsonObject>().FirstOrDefault(i => Id(i, "mixId") == item.CopyOf && i["parentId"] == null);
            if (source == null) continue;
            var copy = source.DeepClone().AsObject();
            copy["itemId"] = item.MixId;
            copy["mixId"] = item.MixId;
            catalogue.Add((JsonNode?)copy);
            // Its children right after it: the client links a child to the preceding parent run.
            foreach (var child in catalogue.OfType<JsonObject>().Where(i => Id(i, "parentId") == item.CopyOf).ToList())
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
    static void AddStratagem(JsonArray catalogue, Item item, HashSet<uint?> known)
    {
        var baseChild = catalogue.OfType<JsonObject>().FirstOrDefault(i => Id(i, "progressionCategory") == 10 && Id(i, "itemId") == item.CopyOf);
        var baseItem = baseChild == null ? null : catalogue.OfType<JsonObject>().FirstOrDefault(i => Id(i, "mixId") == Id(baseChild, "parentId"));
        if (baseChild == null || baseItem == null) return;
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

    static JsonObject? MasteryPackage(JsonArray packages, uint weapon) =>
        packages.OfType<JsonObject>().FirstOrDefault(p => Id(p, "type") == 2 &&
            p["requiredItems"] is JsonArray required && required.Any(r => r is JsonValue v && uint.TryParse(v.ToJsonString(), out var id) && id == weapon));

    void AddPackages(JsonArray packages, IReadOnlyList<Item> items)
    {
        var children = ChildIds();
        foreach (var item in items.Where(i => i.IsWeapon))
        {
            var source = MasteryPackage(packages, item.CopyOf);
            if (source == null || MasteryPackage(packages, item.MixId) != null) continue;
            var copy = source.DeepClone().AsObject();
            copy["id32"] = PackageFor(Id(source, "id32")!.Value, item.CopyOf, item.MixId);
            copy["requiredItems"] = new JsonArray(item.MixId);
            RehomeLevels(copy, children, item);
            packages.Add((JsonNode?)copy);
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

    void AddProgress(JsonObject progression, IReadOnlyList<Item> items)
    {
        if (progression["packageProgress"] is not JsonArray progress || Source?.Invoke("/Progression/ProgressionPackages") is not { } body) return;
        var packages = (JsonArray)JsonNode.Parse(body.GetRawText())!;
        var entries = progress.OfType<JsonObject>().ToDictionary(e => Id(e, "id32") ?? 0, e => e);
        foreach (var item in items.Where(i => i.IsWeapon))
        {
            if (MasteryPackage(packages, item.CopyOf) is not { } source) continue;
            var basePackage = Id(source, "id32")!.Value;
            var own = PackageFor(basePackage, item.CopyOf, item.MixId);
            if (entries.ContainsKey(own) || !entries.TryGetValue(basePackage, out var baseEntry)) continue;
            var copy = baseEntry.DeepClone().AsObject();
            copy["id32"] = own;
            progress.Add((JsonNode?)copy);
            entries[own] = copy;
        }
    }

    void AddInventory(JsonArray held, IReadOnlyList<Item> items)
    {
        var have = held.Select(i => Id(i, "mixId")).ToHashSet();
        var children = ChildIds();
        foreach (var item in items)
        {
            if (item.IsDifficulty && !have.Contains(item.CopyOf)) continue; /* not before level 10 is unlocked */
            if (have.Add(item.MixId)) held.Add((JsonNode?)new JsonObject { ["mixId"] = item.MixId, ["amount"] = 1, ["isDiscovered"] = true });
            if (item.IsWeapon) AddChildren(held, have, children, item);
        }
    }

    // The copy of every child of a weapon's base the inventory holds.
    static void AddChildren(JsonArray held, HashSet<uint?> have, Dictionary<uint, uint> children, Item item)
    {
        foreach (var entry in held.OfType<JsonObject>().ToList())
            if (Id(entry, "mixId") is { } id && children.TryGetValue(id, out var parent) && parent == item.CopyOf)
            {
                var mixId = Rehome(id, item.CopyOf, item.MixId);
                if (!have.Add(mixId)) continue;
                var copy = entry.DeepClone().AsObject();
                copy["mixId"] = mixId;
                held.Add((JsonNode?)copy);
            }
    }

    static void RaiseDifficultyCap(JsonObject client, uint level)
    {
        if (client["onlineOverrideConfiguration"] is not JsonArray overrides) return;
        foreach (var entry in overrides.OfType<JsonObject>().Where(o => Id(o, "mixId") == DifficultyCapOverride && Id(o, "dataType") == 6))
            if (int.TryParse(entry["data"]?.GetValue<string>(), out var cap) && cap < level) entry["data"] = level.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    // Catalogue children: child id -> parent id.
    Dictionary<uint, uint> ChildIds()
    {
        Dictionary<uint, uint> children = [];
        if (Source?.Invoke("/Progression/items") is not { ValueKind: JsonValueKind.Array } catalogue) return children;
        foreach (var item in catalogue.EnumerateArray())
            if (item.TryGetProperty("parentId", out var parent) && parent.TryGetUInt32(out var parentId) &&
                item.TryGetProperty("mixId", out var mix) && mix.TryGetUInt32(out var mixId))
                children[mixId] = parentId;
        return children;
    }

    static uint Inverse(uint a)
    {
        uint x = a; // Newton's iteration for the inverse modulo 2^32 (a odd)
        for (var i = 0; i < 5; i++) x *= 2 - a * x;
        return x;
    }
}
