using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// Weapons code mods register with the HellForge weapon registry (native/mod-loader/registry.c) for this session: the mod
// loader writes <session>/registered-items.json ({"items":[{"mixId","copyOf"}]}) as mods register them, and the
// game's progression data then carries each one as a copy of its base weapon:
//   /Progression/items        the base's catalogue entry under the new id, then copies of its children (attachments etc.);
//   /Progression/ProgressionPackages  a copy of the base's weapon mastery package (type 2) for the new weapon;
//   /Progression              progress in that package, the copy's own (progression.modPackageProgress, ModPackages);
//   /Progression/inventory    the weapon and the children its own progress holds (WeaponMastery.Owned); a base without a
//                             mastery package has nothing to earn them by, so its copy mirrors what the player holds.
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
// A copy's mastery is all that is saved of a registered item, apart from vanilla data, so a removed mod leaves nothing.
// Premium content (docs/modding/concepts.md "Premium content"): a weapon or stratagem copied from content the player does
// not own (OwnsBase: OwnershipFilter's rules), or registered by a mod whose declared premium list the player does not own
// (LockedMods, from mods-stage), stays out of /Progression/inventory: the game knows it, the player cannot equip it.
public sealed class RegisteredItems(string path)
{
    // Name: the name its mod registered it by (a weapon's "unit", a stratagem's "name"), which a mod's warbond names it by.
    public sealed record Item(uint MixId, uint CopyOf, uint Stable = 0, bool IsDifficulty = false, uint Level = 0, string Mod = "", string Name = "")
    {
        public bool IsStratagem => Stable != 0;
        // Weapons: the items with children, mastery packages and progress of their own.
        public bool IsWeapon => !IsStratagem && !IsDifficulty;
    }

    // The online override capping the difficulty level (its mixId: the client's hash of keys AF218CAC, FAABBEA9).
    public const uint DifficultyCapOverride = 48726272;

    // Responses the copies are made from (the backend's startup bodies), set by LocalBackend.
    public Func<string, JsonElement?>? Source
    {
        get => source;
        set
        {
            lock (gate) (source, vanilla, session) = (value, null, null);
        }
    }

    // The saved progress of mod weapons (ProfileStore.ModProgress), set by LocalBackend.
    public Func<JsonArray?>? ModProgress { get; set; }

    Func<string, JsonElement?>? source;
    readonly object gate = new();
    Vanilla? vanilla;
    Session? session;
    readonly Dictionary<string, (Session Session, string Body, JsonElement Served)> served = [];

    // What the vanilla catalogue and packages say, read once: the game's data does not change in a session, and a large
    // weapon set asked it for every item of every request (research\unlimited-weapons\walls, section 3).
    sealed class Vanilla
    {
        public int Items, Packages;
        public readonly Dictionary<uint, int> Children = [];          // parent -> how many children it has
        public readonly Dictionary<uint, uint> Parents = [];          // child -> parent
        public readonly Dictionary<uint, uint> StratagemItems = [];   // a stratagem's stable id -> its catalogue item
        public readonly Dictionary<uint, uint> Mastery = [];          // weapon -> its mastery package

        public Vanilla(JsonElement catalogue, JsonElement? packages)
        {
            Items = catalogue.GetArrayLength();
            foreach (var entry in catalogue.EnumerateArray()) AddChild(entry);
            if (packages is not { ValueKind: JsonValueKind.Array } list) return;
            Packages = list.GetArrayLength();
            foreach (var package in list.EnumerateArray()) AddMastery(package);
        }

        static uint? Field(JsonElement entry, string name) => entry.TryGetProperty(name, out var value) && value.TryGetUInt32(out var id) ? id : null;

        void AddChild(JsonElement entry)
        {
            if (Field(entry, "parentId") is not { } parent) return;
            Children[parent] = Children.GetValueOrDefault(parent) + 1;
            if (Field(entry, "mixId") is { } mixId) Parents[mixId] = parent;
            if (Field(entry, "progressionCategory") == 10 && Field(entry, "itemId") is { } stable) StratagemItems.TryAdd(stable, parent);
        }

        void AddMastery(JsonElement package)
        {
            if (Field(package, "type") != 2 || Field(package, "id32") is not { } id32 ||
                !package.TryGetProperty("requiredItems", out var required) || required.ValueKind != JsonValueKind.Array) return;
            foreach (var weapon in required.EnumerateArray())
                if (weapon.TryGetUInt32(out var id)) Mastery.TryAdd(id, id32);
        }
    }

    // A registered-items.json as read, and what follows from it. The mod loader adds to the file during a session (and it
    // may hold it open, so its timestamp can lag): the bytes say whether it changed.
    sealed record Session(byte[] File, List<Item> Loaded, IReadOnlyList<Item> Admitted, Dictionary<uint, MasteryTrack> Tracks, Dictionary<uint, MasteryTrack> ByWeapon, Capacity Keeps);

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

    // Mods' weapons and stratagems a warbond sells that the player has not bought (ModWarbondService.Withheld): left out of
    // the inventory like a locked item.
    public Func<IReadOnlySet<uint>>? Withheld { get; set; }

    // Catalogue items others add besides these (mods' warbonds' spent counters), which the room counts.
    public int ReservedCatalogue { get; set; }
    // Inventory entries kept for others too (mods' warbonds: a spent counter and a token each).
    public int ReservedInventory { get; set; }

    public IReadOnlyList<Item> Load() => Current().Loaded;
    // The items this session serves (Admitted within what the game keeps now), and the inventory entries the game keeps.
    public IReadOnlyList<Item> Served() => Current().Admitted;
    public int InventoryKept => Current().Keeps.Inventory;

    Vanilla? Game()
    {
        if (vanilla != null) return vanilla;
        if (source?.Invoke("/Progression/items") is not { ValueKind: JsonValueKind.Array } catalogue) return null;
        return vanilla = new Vanilla(catalogue, source("/Progression/ProgressionPackages"));
    }

    Session Current()
    {
        var bytes = ReadFile();
        lock (gate)
        {
            if (session != null && bytes.AsSpan().SequenceEqual(session.File)) return session;
            var loaded = Parse(bytes);
            var keeps = Keeps(bytes);
            var admitted = Admitted(Prioritized(loaded), keeps);
            Dictionary<uint, MasteryTrack> tracks = [], byWeapon = [];
            if (Game() is { } game)
                foreach (var item in admitted.Where(i => i.IsWeapon))
                    if (game.Mastery.TryGetValue(item.CopyOf, out var basePackage) && WeaponMastery.Definition(basePackage) is { } definition)
                    {
                        var track = new MasteryTrack(WeaponIds.PackageFor(basePackage, item.CopyOf, item.MixId), definition,
                            id => game.Parents.TryGetValue(id, out var parent) && parent == item.CopyOf ? Rehome(id, item.CopyOf, item.MixId) : id, item);
                        tracks[track.Package] = track;
                        byWeapon[item.MixId] = track;
                    }
            return session = new Session(bytes, loaded, admitted, tracks, byWeapon, keeps);
        }
    }

    byte[] ReadFile()
    {
        try
        {
            if (!File.Exists(path)) return [];
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var copy = new MemoryStream();
            stream.CopyTo(copy);
            return copy.ToArray();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    static List<Item> Parse(byte[] file)
    {
        if (file.Length == 0) return [];
        try
        {
            using var document = JsonDocument.Parse(file);
            List<Item> items = [];
            foreach (var item in document.RootElement.GetProperty("items").EnumerateArray())
                if (Read(item) is { } registered) items.Add(registered);
            return items;
        }
        catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException) { return []; }
    }

    // What the file says the game keeps this session: state_moves.c writes "capacity" once it has moved the arrays, before
    // the game can ask for anything. No word of it (the move refused, an older loader, a file cut short) is the game's own.
    static Capacity Keeps(byte[] file)
    {
        if (file.Length == 0) return Capacity.Game;
        try
        {
            using var document = JsonDocument.Parse(file);
            if (!document.RootElement.TryGetProperty("capacity", out var capacity) || !capacity.TryGetProperty("items", out var items) ||
                !capacity.TryGetProperty("packages", out var packages) || !items.TryGetInt32(out var kept) || !packages.TryGetInt32(out var progression))
                return Capacity.Game;
            return kept >= CatalogueCap && progression >= PackageCap ? new Capacity(kept, kept, progression) : Capacity.Game;
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException) { return Capacity.Game; }
    }

    // A mod weapon's mastery package, among the weapons this session's game knows; null for any other package.
    public MasteryTrack? Track(uint package) => Current().Tracks.GetValueOrDefault(package);

    // Whether the player may not have an item (a premium rule or a locked mod; mods' warbonds sell only what passes this).
    public bool IsLocked(Item item) => Locked(item);

    // Whether the inventory leaves an item out: locked, or sold on a mod's warbond the player has not bought it from.
    // Purchases and earning refuse what it leaves out.
    public bool IsLeftOut(Item item) => Locked(item) || Withheld?.Invoke().Contains(item.MixId) == true;

    // One entry of the file: a weapon, a stratagem or a difficulty, or null for one that is malformed or waits (copyOf 0).
    static Item? Read(JsonElement item)
    {
        if (!item.TryGetProperty("mixId", out var id) || !id.TryGetUInt32(out var mixId) || mixId == 0 ||
            !item.TryGetProperty("copyOf", out var copy) || !copy.TryGetUInt32(out var copyOf))
            return null;
        if (item.TryGetProperty("kind", out var kind) && kind.ValueKind == JsonValueKind.String && kind.GetString() == "stratagem")
            return copyOf != 0 && item.TryGetProperty("stable", out var stable) && stable.TryGetUInt32(out var stableId) && stableId != 0
                ? new Item(mixId, copyOf, stableId, Mod: ModOf(item), Name: NameOf(item, "name")) : null;
        if (item.TryGetProperty("kind", out kind) && kind.ValueKind == JsonValueKind.String && kind.GetString() == "difficulty")
            return copyOf != 0 && item.TryGetProperty("level", out var level) && level.TryGetUInt32(out var levelValue) && levelValue is > 10 and <= 15
                ? new Item(mixId, copyOf, IsDifficulty: true, Level: levelValue, Mod: ModOf(item)) : null;
        return new Item(mixId, copyOf, Mod: ModOf(item), Name: NameOf(item, "unit"));
    }

    // The client's ids (HellForge.Core WeaponIds).
    public static uint WeaponChild(uint attachment, uint weapon) => WeaponIds.WeaponChild(attachment, weapon);
    public static uint Attachment(uint child, uint weapon) => WeaponIds.Attachment(child, weapon);
    public static uint Rehome(uint child, uint from, uint to) => WeaponIds.Rehome(child, from, to);
    public static uint PackageFor(uint basePackage, uint from, uint to) => WeaponIds.PackageFor(basePackage, from, to);

    // What the game keeps (build 25480438; research\catalogue-cap): the first 4,096 /Progression/items (game.dll+0x107EA50
    // `mov eax,0x1000; cmp; cmovb` caps the parse, into a buffer of 4,096 records, +0x12CA837), the first 4,096 inventory
    // entries (+0x103927B) and the first 256 progression packages (+0x1080093). The rest is dropped, unsaid: with 64
    // Liberator copies (6,573 items) the cut fell inside the 21st copy, and every mod stratagem after it was lost.
    // HellForge can move those arrays and raise those caps at start-up (native/mod-loader/state_moves.c): then the loader
    // says what the game keeps now (Keeps), and only then is more served.
    public const int CatalogueCap = 4096, InventoryCap = 4096, PackageCap = 256;

    public sealed record Capacity(int Items, int Inventory, int Packages)
    {
        public static readonly Capacity Game = new(CatalogueCap, InventoryCap, PackageCap);
    }

    // The order the items are added in: difficulties and stratagems (one or two catalogue items each) before weapons (the
    // base's item and every one of its children: 57 for the AR-23 Liberator), each kind in the order mods registered it.
    // Weapons register as mods load and stratagems only once the game has loaded its own, so in registration order a large
    // weapon pack pushed out every stratagem and level 11; in this order, only its last weapons.
    public static IReadOnlyList<Item> Prioritized(IReadOnlyList<Item> items) =>
        [.. items.OrderBy(i => i.IsDifficulty ? 0 : i.IsStratagem ? 1 : 2)];

    // Where a line goes when a registered item does not fit (the backend's log); each item is reported once.
    public Action<string> Log { get; set; } = line => Console.Error.WriteLine("[registered items] " + line);
    readonly HashSet<uint> reported = [];

    // The items that fit the game's catalogue and packages whole, in priority order (Prioritized): a weapon with every one of
    // its base's children and its mastery package, or not at all, never cut through. Without the vanilla catalogue to count
    // (Source), every item.
    public IReadOnlyList<Item> Admitted(IReadOnlyList<Item> items) => Admitted(items, Capacity.Game);

    public IReadOnlyList<Item> Admitted(IReadOnlyList<Item> items, Capacity keeps)
    {
        if (Game() is not { } game) return items;
        int room = keeps.Items - game.Items - ReservedCatalogue, packageRoom = keeps.Packages - game.Packages;
        var admitted = new List<Item>();
        foreach (var item in items)
        {
            var cost = item.IsStratagem ? 2 : 1 + game.Children.GetValueOrDefault(item.CopyOf);
            var package = item.IsWeapon && game.Mastery.ContainsKey(item.CopyOf) ? 1 : 0;
            if (cost <= room && package <= packageRoom)
            {
                (room, packageRoom) = (room - cost, packageRoom - package);
                admitted.Add(item);
            }
            else Report(item, cost > room ? $"the game keeps {keeps.Items} catalogue items, and it needs {cost} more where {room} are left"
                : $"the game keeps {keeps.Packages} progression packages, and none is left for its mastery package");
        }
        return admitted;
    }

    void Report(Item item, string why)
    {
        lock (reported)
            if (!reported.Add(item.MixId)) return;
        var kind = item.IsDifficulty ? "difficulty" : item.IsStratagem ? "stratagem" : "weapon";
        Log($"{(item.Mod.Length > 0 ? item.Mod + "'s " : "")}{kind} {item.MixId} is left out: {why}.");
    }

    // The response body for a startup path, with the registered items added where they belong.
    public JsonElement Apply(string path, JsonElement body)
    {
        if (path is not ("/Progression/items" or "/Progression/inventory" or "/Progression/ProgressionPackages" or "/Progression" or
            "/Configuration/GameClient"))
            return body;
        var current = Current();
        var items = current.Admitted;
        if (items.Count == 0) return body;
        // The catalogue and the packages change only with the file, and each is a long body: the same answer is kept.
        var cached = path is "/Progression/items" or "/Progression/ProgressionPackages";
        var raw = body.GetRawText();
        if (cached)
            lock (gate)
                if (served.TryGetValue(path, out var kept) && ReferenceEquals(kept.Session, current) && kept.Body == raw) return kept.Served;
        var node = JsonNode.Parse(raw);
        switch (path)
        {
            case "/Progression/items" when node is JsonArray catalogue:
                AddCatalogue(catalogue, items);
                break;
            case "/Progression/ProgressionPackages" when node is JsonArray packages:
                AddPackages(packages, items);
                break;
            case "/Progression" when node is JsonObject progression:
                AddProgress(progression, current);
                break;
            case "/Progression/inventory" when node is JsonObject inventory && inventory["items"] is JsonArray held:
                AddInventory(held, current);
                break;
            case "/Configuration/GameClient" when node is JsonObject client && items.Any(i => i.IsDifficulty):
                RaiseDifficultyCap(client, items.Where(i => i.IsDifficulty).Max(i => i.Level));
                break;
            default:
                return body;
        }
        using var document = JsonDocument.Parse(node!.ToJsonString());
        var result = document.RootElement.Clone();
        if (cached)
            lock (gate) served[path] = (current, raw, result);
        return result;
    }

    // A uint field, however its node was made (a JsonValue made from a uint does not convert to long).
    static uint? Id(JsonNode? node, string name) =>
        node is JsonObject o && o[name] is JsonValue v && v.GetValueKind() == JsonValueKind.Number &&
        uint.TryParse(v.ToJsonString(), out var id) ? id : null;

    static void AddCatalogue(JsonArray catalogue, IReadOnlyList<Item> items)
    {
        var known = catalogue.Select(i => Id(i, "mixId")).ToHashSet();
        var index = new CatalogueIndex(catalogue);
        foreach (var item in items.Where(i => !known.Contains(i.MixId)))
            if (item.IsStratagem) AddStratagem(catalogue, item, known, index.Stratagems, index.Parents);
            else AddWeapon(catalogue, item, known, index);
    }

    // The catalogue by id: a copy of each of 500 weapons searched all of it for its base and its children.
    sealed class CatalogueIndex
    {
        public readonly Dictionary<uint, JsonObject> Parents = [], Stratagems = [];
        public readonly Dictionary<uint, List<JsonObject>> ChildrenOf = [];

        public CatalogueIndex(JsonArray catalogue)
        {
            foreach (var entry in catalogue.OfType<JsonObject>())
            {
                if (Id(entry, "parentId") is { } parent) (ChildrenOf.TryGetValue(parent, out var list) ? list : ChildrenOf[parent] = []).Add(entry);
                else if (Id(entry, "mixId") is { } mixId) Parents.TryAdd(mixId, entry);
                if (Id(entry, "progressionCategory") == 10 && Id(entry, "itemId") is { } stable) Stratagems.TryAdd(stable, entry);
            }
        }
    }

    // A weapon's item, and its children right after it: the client links a child to the preceding parent run.
    static void AddWeapon(JsonArray catalogue, Item item, HashSet<uint?> known, CatalogueIndex index)
    {
        if (!index.Parents.TryGetValue(item.CopyOf, out var source)) return;
        var copy = source.DeepClone().AsObject();
        copy["itemId"] = item.MixId;
        copy["mixId"] = item.MixId;
        catalogue.Add((JsonNode?)copy);
        index.Parents.TryAdd(item.MixId, copy);
        foreach (var child in index.ChildrenOf.GetValueOrDefault(item.CopyOf) ?? [])
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

    // A stratagem's item and its child, copied from its base's.
    static void AddStratagem(JsonArray catalogue, Item item, HashSet<uint?> known, Dictionary<uint, JsonObject> stratagems, Dictionary<uint, JsonObject> parents)
    {
        if (!stratagems.TryGetValue(item.CopyOf, out var baseChild) || Id(baseChild, "parentId") is not { } parent ||
            !parents.TryGetValue(parent, out var baseItem)) return;
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

    void AddPackages(JsonArray packages, IReadOnlyList<Item> items)
    {
        // Each weapon's package, by weapon, and the base's children, which a copy's package names by its own ids.
        Dictionary<uint, JsonObject> mastery = [];
        foreach (var package in packages.OfType<JsonObject>().Where(p => Id(p, "type") == 2))
            foreach (var weapon in (package["requiredItems"] as JsonArray ?? []).OfType<JsonValue>())
                if (uint.TryParse(weapon.ToJsonString(), out var id)) mastery.TryAdd(id, package);
        var children = Game()?.Parents ?? [];
        foreach (var item in items.Where(i => i.IsWeapon))
        {
            if (!mastery.TryGetValue(item.CopyOf, out var source) || mastery.ContainsKey(item.MixId)) continue;
            var copy = source.DeepClone().AsObject();
            copy["id32"] = PackageFor(Id(source, "id32")!.Value, item.CopyOf, item.MixId);
            copy["requiredItems"] = new JsonArray(item.MixId);
            RehomeLevels(copy, children, item);
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

    // Saved mod progress, by package.
    Dictionary<uint, JsonObject> SavedProgress()
    {
        Dictionary<uint, JsonObject> saved = [];
        foreach (var entry in (ModProgress?.Invoke() ?? []).OfType<JsonObject>())
            if (Id(entry, "id32") is { } id) saved.TryAdd(id, entry);
        return saved;
    }

    // A copy's own level, for the weapons this session has: nothing for a copy still at level 0, as for a vanilla weapon
    // not yet levelled. Only the fields of a packageProgress entry (ProfileStore.CompletePackageEntry) reach the client.
    void AddProgress(JsonObject progression, Session current)
    {
        var saved = SavedProgress();
        if (saved.Count == 0) return;
        if (progression["packageProgress"] is not JsonArray progress) progression["packageProgress"] = progress = [];
        var listed = progress.Select(e => Id(e, "id32")).ToHashSet();
        foreach (var (package, entry) in saved)
        {
            if (!current.Tracks.ContainsKey(package) || !listed.Add(package) || ModPackages.Whole(entry["level"]) is not > 0) continue;
            var copy = entry.DeepClone().AsObject();
            copy.Remove(ModPackages.HeldItems);
            copy.Remove(ModPackages.GrantedThrough);
            ProfileStore.CompletePackageEntry(copy);
            progress.Add((JsonNode?)copy);
        }
    }

    // Each item with the child items that are its own, while the game's inventory entries (Capacity.Inventory) hold all of
    // them: past that the game ignores the rest, and a weapon would be held without its attachments.
    void AddInventory(JsonArray held, Session current)
    {
        var have = held.Select(i => Id(i, "mixId")).ToHashSet();
        var saved = SavedProgress();
        var heldChildren = HeldChildren(held);
        var withheld = Withheld?.Invoke() ?? new HashSet<uint>();
        var keeps = current.Keeps.Inventory - ReservedInventory;
        foreach (var item in current.Admitted.Where(i => !Locked(i) && !withheld.Contains(i.MixId) && (!i.IsDifficulty || have.Contains(i.CopyOf)))) /* a difficulty not before level 10 is unlocked */
        {
            var own = OwnItems(item, current, saved);
            var mirrored = item.IsWeapon && !current.ByWeapon.ContainsKey(item.MixId) ? heldChildren.GetValueOrDefault(item.CopyOf) ?? [] : [];
            var entries = (have.Contains(item.MixId) ? 0 : 1) + own.Count + mirrored.Count;
            if (held.Count + entries > keeps)
            {
                Report(item, $"the game keeps {current.Keeps.Inventory} inventory entries, and it needs {entries} more where {keeps - held.Count} are left");
                continue;
            }
            foreach (var mixId in own.Prepend(item.MixId))
                if (have.Add(mixId)) held.Add((JsonNode?)new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
            AddMirrored(held, have, mirrored, item);
        }
    }

    static HashSet<uint> OwnItems(Item item, Session current, Dictionary<uint, JsonObject> saved) =>
        current.ByWeapon.TryGetValue(item.MixId, out var track) && saved.TryGetValue(track.Package, out var progress) ? WeaponMastery.Owned(track, progress) : [];

    // For the copies of bases without a package: what the inventory holds of each base's children.
    Dictionary<uint, List<JsonObject>> HeldChildren(JsonArray held)
    {
        var parents = Game()?.Parents ?? [];
        Dictionary<uint, List<JsonObject>> children = [];
        foreach (var entry in held.OfType<JsonObject>())
            if (Id(entry, "mixId") is { } id && parents.TryGetValue(id, out var parent))
                (children.TryGetValue(parent, out var list) ? list : children[parent] = []).Add(entry);
        return children;
    }

    static void AddMirrored(JsonArray held, HashSet<uint?> have, List<JsonObject> mirrored, Item item)
    {
        foreach (var entry in mirrored)
        {
            var mixId = Rehome(Id(entry, "mixId")!.Value, item.CopyOf, item.MixId);
            if (!have.Add(mixId)) continue;
            var copy = entry.DeepClone().AsObject();
            copy["mixId"] = mixId;
            held.Add((JsonNode?)copy);
        }
    }

    // The mod that registered an entry (registry.c writes its id).
    static string ModOf(JsonElement item) => NameOf(item, "mod");

    static string NameOf(JsonElement item, string key) => item.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()! : "";

    // A premium mod the player does not own, or a copy of a premium base they do not own; a stratagem whose base cannot be
    // found yet stays out too, until the game has loaded its stratagems.
    bool Locked(Item item)
    {
        if (LockedMods.Contains(item.Mod)) return true;
        if (OwnsBase == null || item.IsDifficulty) return false;
        return BaseItem(item) is not { } baseItem || !OwnsBase(baseItem);
    }

    // The base's own item: a weapon's is copyOf; a stratagem's is the parent of the catalogue child whose itemId is its stable id.
    uint? BaseItem(Item item)
    {
        if (!item.IsStratagem) return item.CopyOf;
        return Game()?.StratagemItems.TryGetValue(item.CopyOf, out var parent) == true ? parent : null;
    }

    static void RaiseDifficultyCap(JsonObject client, uint level)
    {
        if (client["onlineOverrideConfiguration"] is not JsonArray overrides) return;
        foreach (var entry in overrides.OfType<JsonObject>().Where(o => Id(o, "mixId") == DifficultyCapOverride && Id(o, "dataType") == 6))
            if (int.TryParse(entry["data"]?.GetValue<string>(), out var cap) && cap < level) entry["data"] = level.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }
}
