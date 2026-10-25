using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The in-game editor's catalogue (native/editor, docs/sdk-roadmap.md phase 5a): the arsenal pages of StatsCatalog for the
// categories the editor shows, laid out as the HD2R Editor lays out a page (ui/sections.lua there), with what the editor
// needs per field to read it (get_live), change it (set_live) and save it (Local Stats): editor-catalog.json, written next
// to the editor's DLL by Build-Editor.ps1.
//
// A page is categories (Projectile, Damage, Handling / Fire, Ammunition...), each with its parts (Flight, Direct hit...).
//   flat    the category shows its one part's rows directly (no part header)
//   plain   the category is a single row: no header at all, the row alone, `prefix` before its label
//   fold    a part of a single row: shown in its category without a header, `prefix` before its label
// Keys are the HD2R Editor's ("c|<category>", "c|<category>|<part>"): a closed header stays closed on every page.
public static class EditorCatalog
{
    public const string FileName = "editor-catalog.json";
    public const int Format = 1;
    // The categories the editor shows, in the HD2R Editor's groups; more come with their fields.
    // (id, label, group, tone: the game's stratagem category colour, or none)
    static readonly (string Id, string Label, string Group, string? Tone)[] Categories =
    [
        ("primary", "Primary", "WEAPONS", null), ("secondary", "Secondary", "WEAPONS", null), ("throwables", "Throwables", "WEAPONS", null),
        ("offensive", "Offensive", "STRATAGEMS", "offensive"), ("defensive", "Defensive", "STRATAGEMS", "defensive"),
        ("support", "Support Weapons", "STRATAGEMS", "support"), ("backpacks", "Support Backpacks", "STRATAGEMS", "support"),
        ("vehicles", "Vehicles", "STRATAGEMS", "support"), ("mission", "Mission", "STRATAGEMS", "support"),
        ("boosters", "Boosters", "EQUIPMENT", null),
        ("helldiver", "Helldiver", "HELLDIVER", null), ("perks", "Armor perks", "HELLDIVER", null),
        ("terminids", "Terminids", "ENEMIES", null), ("automatons", "Automatons", "ENEMIES", null), ("illuminate", "Illuminate", "ENEMIES", null),
        ("structures", "Structures", "ENEMIES", null), ("entities", "Other Entities", "ENEMIES", null),
        ("difficulty", "Mission Difficulty", "SPAWNING", null), ("rosters", "Spawn Rosters", "SPAWNING", null),
        ("compositions", "Spawn Compositions", "SPAWNING", null), ("demolition", "Demolition Templates", "SPAWNING", null),
        ("statuses", "Status Effects", "EFFECTS", null),
        ("explosions", "Explosions", "ADVANCED", null), ("projectiles", "Projectiles", "ADVANCED", null), ("damage", "Damage Types", "ADVANCED", null),
    ];
    // A stratagem's settings row is the stratagem's type (mappings/catalog.json), what set_live names a row by.
    static readonly Lazy<Dictionary<string, ulong>> StratagemTypes = new(() =>
        SdkMappings.Current.Content.Where(c => c.Kind == "stratagems").ToDictionary(c => c.Name, c => c.Id, StringComparer.Ordinal));
    static readonly Lazy<IReadOnlyDictionary<string, IReadOnlyDictionary<string, ulong>>> Sprites = new(GameData.EditorIcons.Sprites);
    // The armory's weapon groups, in its order (the HD2R Editor's filter chips).
    static readonly Dictionary<string, string[]> GroupOrder = new(StringComparer.Ordinal)
    {
        ["primary"] = ["Assault Rifle", "Marksman Rifle", "Submachine Gun", "Shotgun", "Explosive", "Energy-Based", "Special"],
        ["secondary"] = ["Pistol", "Melee", "Special"],
        // the HD2R Editor's filter by faction (StatsCatalog.FactionLabel)
        ["rosters"] = ["Terminids", "Automatons", "Illuminate", "Super Earth", "Unknown"],
    };
    const string Sep = " · ";

    public static JsonObject Build(StatsSchema? schema = null)
    {
        schema ??= StatsSchema.Current;
        var categories = new JsonArray();
        var objects = new JsonArray();
        foreach (var (id, label, group, tone) in Categories)
        {
            var items = StatsCatalog.Items.Where(i => i.Nav == id).ToList();
            var present = items.Select(i => i.Group ?? "").Where(g => g.Length > 0).Distinct().ToList();
            var order = GroupOrder.GetValueOrDefault(id) ?? [];
            var groups = order.Where(present.Contains).Concat(present.Where(g => !order.Contains(g)).Order(StringComparer.Ordinal)).ToList();
            categories.Add(new JsonObject
            {
                ["id"] = id, ["label"] = label, ["group"] = group, ["tone"] = tone,
                ["filters"] = GroupOrder.ContainsKey(id) ? new JsonArray(groups.Select(g => (JsonNode)g).ToArray()) : null,
            });
            foreach (var item in items) objects.Add(Object(item, id, tone, schema));
            if (id == "helldiver") objects.Add(ArmorPerks());
        }
        return new JsonObject
        {
            ["format"] = Format, ["build"] = schema.Build, ["schema"] = StatsSchema.Fingerprint, ["categories"] = categories, ["objects"] = objects,
            ["references"] = References(),
            ["hd2r"] = Hd2rValues(),
        };
    }

    // HD2R Editor presets (mappings/hd2r-keys.json): each key with the bytes it edits, how its value is stored, and the
    // ids HD2R stores for references (an attack output's id for a projectile, an explosion's catalogue id, a status's).
    sealed record Hd2rKey(string Key, string Kind, string Field)
    {
        public string Object => Key.Split('|')[1];
        // a value kind held as plain fields ("slots" in hd2r-keys.json): this field's slot and the value names
        public int Slot { get; init; } = -1;
        public JsonNode? Names { get; init; }
    }
    static readonly Lazy<JsonNode> Hd2rData = new(() =>
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.hd2r-keys.json")!;
        return JsonNode.Parse(stream)!;
    });
    static readonly Lazy<Dictionary<string, List<Hd2rKey>>> Hd2rByAddress = new(() =>
    {
        var byAddress = new Dictionary<string, List<Hd2rKey>>(StringComparer.Ordinal);
        foreach (var (key, v) in Hd2rData.Value["keys"]!.AsObject())
        {
            var a = v!.AsArray();
            var address = a[0]!.GetValue<string>();
            if (!byAddress.TryGetValue(address, out var list)) byAddress[address] = list = [];
            list.Add(new Hd2rKey(key, a[1]!.GetValue<string>(), a[2]!.GetValue<string>()));
        }
        // flags, enums, armor penetration labels and slot lists (fire rates, fire modes, traits): a key on each slot
        foreach (var (key, v) in Hd2rData.Value["slots"]!.AsObject())
        {
            var addresses = v!["addresses"]!.AsArray();
            for (var i = 0; i < addresses.Count; i++)
            {
                var address = addresses[i]!.GetValue<string>();
                if (!byAddress.TryGetValue(address, out var list)) byAddress[address] = list = [];
                list.Add(new Hd2rKey(key, v["kind"]!.GetValue<string>(), v["field"]!.GetValue<string>()) { Slot = i, Names = v["names"] });
            }
        }
        return byAddress;
    });

    // HD2R Editor rows with no native address that HellForge holds another way (hd2r-keys.json "unaddressed": pod payloads,
    // arm swaps, the overcharge explosion, the default magazine's reload): the key on an object whose key ends so.
    static string? Unaddressed(string objectName, string suffix) =>
        Hd2rData.Value["unaddressed"]?.AsObject().Select(k => k.Key).FirstOrDefault(k => k.Split('|')[1] == objectName && k.EndsWith(suffix, StringComparison.Ordinal));

    // A key on a choice row whose preset value is a name (a mounted weapon's id, a weapon's name) or a pickup.
    static void Hd2rNamed(JsonObject row, string key, string kind, JsonObject names)
    {
        row["hd2r"] = new JsonArray(key);
        row["hd2rKind"] = kind;
        row["hd2rAs"] = new JsonObject { [key] = new JsonObject { ["kind"] = kind, ["slot"] = 0, ["names"] = names } };
    }

    static readonly Lazy<JsonNode> ParityData = new(() =>
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.parity.json")!;
        return JsonNode.Parse(stream)!;
    });

    // A weapon's default magazine's reload row key (HD2R Editor's "reload.default_magazine"), or null.
    static readonly Lazy<Dictionary<string, string>> DefaultReload = new(() =>
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.attachments.json")!;
        var root = JsonNode.Parse(stream)!;
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var (weapon, slots) in root["weapons"]!.AsObject())
            if (slots!["magazine"]?["default"]?.GetValue<string>() is { } id)
                map[weapon] = $"attachment.magazine.{id.Split('/')[^2].Replace('-', '_')}.reload_duration";
        return map;
    });

    static JsonObject Hd2rValues()
    {
        var statuses = new JsonObject();
        foreach (var s in ReferenceData.Value["statuses"]!.AsArray()) statuses[s!["id"]!.GetValue<string>()] = s["type"]!.GetValue<long>();
        return new JsonObject
        {
            ["outputs"] = Hd2rData.Value["outputs"]!.DeepClone(), ["explosions"] = Hd2rData.Value["explosions"]!.DeepClone(), ["statuses"] = statuses,
            ["editor"] = Hd2rData.Value["editor"]!.DeepClone(), ["runtime"] = Hd2rData.Value["runtime"]!.DeepClone(),
        };
    }

    // The HD2R keys on a row's bytes, the row's own first (same item, same field id), and how their value is stored.
    static void Hd2rKeys(JsonObject json, StatsRow row, StatsItem item)
    {
        // An entity's rows are the HD2R Editor's Enemies rows already ('en|<entity>|<row id>', mappings/entities.json); the
        // Helldiver's main fields are its avatar entity's.
        string? entity = row.Part.Category == "enemies" ? $"en|{row.Part.Target}|{row.Key}" : row.Field.Hd2rKey;
        if (!Hd2rByAddress.Value.TryGetValue(row.Field.Hd2rAddress, out var keys))
        {
            if (entity == null && row.Part.Category == "helldiver" && row.Key.StartsWith("entity.", StringComparison.Ordinal))
                entity = "en|avatar_helldiver|entity::" + row.Key;
            if (entity == null) return;
            json["hd2r"] = new JsonArray(entity);
            json["hd2rKind"] = "number";
            return;
        }
        keys = keys.Where(k => k.Kind != "ap").ToList(); /* the displayed armor penetration: its own row (ApLabelRow) */
        if (keys.Count == 0)
        {
            if (entity != null) (json["hd2r"], json["hd2rKind"]) = (new JsonArray(entity), "number");
            return;
        }
        var names = new HashSet<string>(item.Parts.Select(p => p.Target).Append(item.Name), StringComparer.Ordinal);
        var ordered = keys.OrderBy(k => names.Contains(k.Object) ? 0 : 1).ThenBy(k => k.Field == row.Key ? 0 : 1).ThenBy(k => k.Key, StringComparer.Ordinal).ToList();
        // bytes many items share (a melee hit volume, ~100 weapons): each item's row carries its own keys only
        if (ordered.Count > 8 && ordered.Any(k => names.Contains(k.Object))) ordered = ordered.Where(k => names.Contains(k.Object)).ToList();
        json["hd2r"] = new JsonArray((entity != null ? [entity] : Array.Empty<string>()).Concat(ordered.Select(k => k.Key)).Select(k => (JsonNode)k).ToArray());
        json["hd2rKind"] = ordered[0].Kind;
        // keys whose values are not the field's number (a flag, a name, a list): how each reads
        var structured = ordered.Where(k => k.Slot >= 0).ToList();
        if (structured.Count > 0)
            json["hd2rAs"] = new JsonObject(structured.Select(k => KeyValuePair.Create(k.Key, (JsonNode?)new JsonObject
            {
                ["kind"] = k.Kind, ["slot"] = k.Slot, ["names"] = k.Names?.DeepClone(),
            })));
    }

    // The references a pick row offers (mappings/references.json; HD2R Editor's pickers), each list once: projectile
    // donors by what fires them, explosions in the HD2R Editor's groups, status effects by family. No game values.
    static JsonObject References()
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        var root = JsonNode.Parse(stream)!;
        JsonObject Option(long value, string label, string group, string note) => new() { ["value"] = value, ["label"] = label, ["group"] = group, ["note"] = note };
        var payloadGroups = root["payload"]!["groups"]!.AsArray().Select(g => g!).ToDictionary(g => g["id"]!.GetValue<string>(), g => g["label"]!.GetValue<string>());
        var groupOrder = payloadGroups.Values.ToList();
        static string Kind(string owner) => owner switch
        {
            "player_weapon" => "Weapons", "support_weapon" => "Support weapons", "stratagem" => "Stratagems", "vehicle_weapon" => "Vehicles",
            "mounted_weapon" => "Mounted weapons", _ => "Other",
        };
        string[] projectileGroups = ["Weapons", "Support weapons", "Stratagems", "Vehicles", "Mounted weapons", "Other"];
        // A weapon's programmable round is another weapon's shot at times (the GR-8 Recoilless Rifle's is the TD-220 Bastion's
        // shell): the note names it too.
        var programmedBy = root["programmable"]!.AsObject().ToLookup(w => w.Value!["projectile"]!.GetValue<long>(), w => w.Key + " (programmable ammo)");
        var projectiles = root["projectiles"]!.AsArray().Select(p => p!).Select(p =>
        {
            var owners = p["owners"]!.AsArray().Select(o => o!.GetValue<string>()).Concat(programmedBy[p["type"]!.GetValue<long>()]).ToList();
            var others = owners.Count > 1 ? $", also {string.Join(", ", owners.Skip(1).Take(2))}{(owners.Count > 3 ? $" +{owners.Count - 3}" : "")}" : "";
            return Option(p["type"]!.GetValue<long>(), owners[0], Kind(p["owner"]!.GetValue<string>()), $"projectile {p["type"]}{others}");
        }).OrderBy(o => Array.IndexOf(projectileGroups, o["group"]!.GetValue<string>())).ThenBy(o => o["label"]!.GetValue<string>(), StringComparer.OrdinalIgnoreCase);
        // The explosions a payload slot offers: HD2R Editor's payload options, in its groups, with its warnings.
        var explosions = root["explosions"]!.AsArray().Select(x => x!).Where(x => x["payload"] != null).Select(x =>
        {
            var notes = new List<string> { $"explosion {x["type"]}" };
            if (x["mission"]!.GetValue<bool>()) notes.Add("a mission's effects (in missions)");
            else if (x["packages"]!.AsArray().Count > 0) notes.Add("loads its package first");
            var option = Option(x["type"]!.GetValue<long>(), x["label"]!.GetValue<string>(), payloadGroups[x["payload"]!["group"]!.GetValue<string>()], string.Join(" · ", notes));
            option["warnings"] = x["payload"]!["warnings"]!.DeepClone();
            return option;
        }).OrderBy(o => groupOrder.IndexOf(o["group"]!.GetValue<string>())).ThenBy(o => o["label"]!.GetValue<string>(), StringComparer.OrdinalIgnoreCase);
        var statuses = root["statuses"]!.AsArray().Select(s => s!).Where(s => s["attachable"]!.GetValue<bool>()).Select(s => Option(s["type"]!.GetValue<long>(),
            s["label"]!.GetValue<string>(), StatsCatalog.Humanize(s["family"]!.GetValue<string>()),
            $"status {s["type"]}" + (s["tier"]?.GetValue<string>() == "player_attack" ? "" : " · unverified on a weapon (HD2R: allow_unverified_effect)")))
            .OrderBy(o => o["group"]!.GetValue<string>(), StringComparer.Ordinal)
            .ThenBy(o => o["label"]!.GetValue<string>(), StringComparer.OrdinalIgnoreCase);
        return new JsonObject
        {
            ["warnings"] = root["payload"]!["warnings"]!.DeepClone(),
            ["projectile"] = new JsonArray(projectiles.Select(o => (JsonNode)o).ToArray()),
            ["explosion"] = new JsonArray(explosions.Select(o => (JsonNode)o).ToArray()),
            ["status"] = new JsonArray(statuses.Select(o => (JsonNode)o).ToArray()),
        };
    }

    static readonly Lazy<JsonNode> ReferenceData = new(() =>
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        return JsonNode.Parse(stream)!;
    });

    static readonly Lazy<Dictionary<string, JsonObject>> Ammunition = new(() =>
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        return JsonNode.Parse(stream)!["ammunition"]!.AsArray().Select(a => a!.AsObject()).ToDictionary(a => a["weapon"]!.GetValue<string>(), a => a, StringComparer.Ordinal);
    });

    // What a reference row picks (projectile, explosion, status), from its mapping's "ref"; a weapon's ammunition delta
    // is its projectile.
    static string? Pick(StatsField f) => f.Kind == "EntityDelta" ? (f.Component is "321:0" or "117:64" or "117:68" ? "projectile" : null) :
        SdkMappings.Current.FieldAt(f.Kind == "component" ? f.Component : f.Kind, f.Offset, f.Storage)?.Ref is { } r && r != "damage" ? r : null;

    // The page's attack branch of a key ("" its main round), as StatsCatalog places it.
    static bool MainRound(string key)
    {
        var segments = key.Split('.');
        var middle = segments.Skip(1).Take(segments.Length - 2).Where(s => s != "damage").ToList();
        return middle.Count == 0 || middle[0] is "primary" or "feed_primary" or "impact" or "expiry" or "detonation" ||
            middle[0].StartsWith("primary_", StringComparison.Ordinal);
    }

    public static string Text(StatsSchema? schema = null) => Build(schema).ToJsonString(new JsonSerializerOptions { WriteIndented = false }) + "\n";

    // A vehicle's mount slots (HD2R Editor's "Mounted Weapons", its arm swap): a choice row per slot, kept in the
    // editor's settings and applied live (MountComponentData slot k +24k, a u64: the mounted entity), offering the weapons
    // HD2Runtime allows there (the vanilla one first), each named by its stats page where it has one. The rows of a
    // mount's own weapon say which slot holds it (mountSlot), so a swap can lock them.
    static readonly Lazy<Dictionary<string, string>> WeaponByResource = new(() =>
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (name, t) in StatsSchema.Current.Targets["weapons"])
            foreach (var f in t.Fields.Values.Where(f => f.Kind == "component"))
                map.TryAdd(f.Resource, name);
        return map;
    });

    static JsonObject? MountCategory(StatsItem item)
    {
        var vehicle = item.Parts.FirstOrDefault(p => p.Category == "vehicles")?.Target;
        if (vehicle == null) return null;
        var slots = ReferenceData.Value["mountSlots"]!.AsArray().Select(s => s!).Where(s => s["vehicle"]!.GetValue<string>() == vehicle).ToList();
        if (slots.Count == 0) return null;
        var weapons = ReferenceData.Value["mountedWeapons"]!.AsObject();
        var ids = weapons.Select(w => w.Key).ToList();
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.vehicles.json")!;
        var mounts = JsonNode.Parse(stream)!["vehicles"]![vehicle]!["mounts"]!.AsArray().Select(m => m!).ToList();
        string Name(string id)
        {
            var w = weapons[id]!;
            var resource = w["resource"]!.GetValue<string>();
            if (!WeaponByResource.Value.TryGetValue(resource, out var page)) return StatsCatalog.Humanize(w["name"]!.GetValue<string>());
            var at = page.LastIndexOf(" / ", StringComparison.Ordinal); /* "EXO-49 Emancipator Exosuit / left_gun": its vehicle · Left gun */
            return at < 0 ? page : page[..at] + Sep + StatsCatalog.Humanize(page[(at + 3)..]);
        }
        var rows = new JsonArray();
        foreach (var s in slots)
        {
            var slot = s["slot"]!.GetValue<int>();
            var label = mounts.FirstOrDefault(m => m["slot"]?.GetValue<int>() == slot)?["label"]?.GetValue<string>() ?? $"Slot {slot}";
            var options = new JsonArray();
            var first = true;
            foreach (var id in s["allowed"]!.AsArray().Select(a => a!.GetValue<string>()))
            {
                var w = weapons[id]!;
                var resource = w["resource"]!.GetValue<string>();
                options.Add(new JsonObject
                {
                    ["value"] = ids.IndexOf(id), ["label"] = Name(id) + (first ? " (vanilla)" : ""), ["resource"] = resource,
                    ["note"] = (WeaponByResource.Value.TryGetValue(resource, out var page) ? "stats editable: " + page : "stats not editable") +
                        (w["package"] != null ? " · loads its package first" : ""),
                    ["target"] = page, // its stats page: shown under the slot while it is swapped in
                });
                first = false;
            }
            AddSlot(slot, s["untested"] != null ? "Shield arm" : label, s["resource"]!.GetValue<string>(), options, s["untested"]?.GetValue<string>());
            if (Unaddressed(vehicle, $"-mount-slot-{slot}:mount.weapon") is { } mountKey) /* a preset names the mounted weapon */
            {
                var names = new JsonObject();
                foreach (var id in s["allowed"]!.AsArray().Select(a => a!.GetValue<string>())) names[id] = ids.IndexOf(id);
                Hd2rNamed((JsonObject)rows[^1]!, mountKey, "named", names);
            }
        }
        void AddSlot(int slot, string label, string resource, JsonArray options, string? warning)
        {
            rows.Add(new JsonObject
            {
                ["label"] = label, ["cat"] = "mounts", ["target"] = vehicle, ["key"] = $"mount.slot_{slot}", ["choice"] = "mount",
                ["component"] = "MountComponentData", ["ref"] = resource, ["offset"] = slot * 24, ["type"] = "u64",
                ["min"] = null, ["max"] = null, ["unit"] = null, ["applies"] = "respawn", ["verified"] = false, ["unverified"] = true,
                ["shared"] = false, ["also"] = null, ["link"] = $"mount|{vehicle}|{slot}",
                ["note"] = (warning != null ? warning + " " : "") +
                    "Swaps the weapon this slot holds (HD2R Editor's arm swap): vehicles called in after it carry it. Firing and look of a swapped mount are not proven.",
                ["options"] = options,
            });
        }
        var part = new JsonObject { ["title"] = "Mounted Weapons", ["key"] = "c|mounts|Mounted Weapons", ["fold"] = false, ["prefix"] = null, ["rows"] = rows };
        return new JsonObject { ["title"] = "Mounted Weapons", ["key"] = "c|mounts", ["flat"] = true, ["plain"] = false, ["prefix"] = null, ["subs"] = new JsonArray(part) };
    }

    // A call-in's hellpod (HD2R Editor's pod payload rows): what its pod opens with, a choice row per slot 1-4 of its rack
    // (HellpodRackComponentData slot k +64k, a u64: the item's entity) offering its vanilla item, nothing, and every
    // pickup HD2Runtime reviewed (support weapons, backpacks, ammo, stims, grenades, supplies) by kind, and how many of
    // the slots spawn (+556, 1-4). Kept in the editor's settings like the arm swaps, applied live (pods called in after
    // it) and shared with friends in co-op. A rack several call-ins use (the EAT-17's, the Surplus EAT booster's) is one.
    static readonly Dictionary<string, string> PickupGroups = new(StringComparer.Ordinal)
    {
        ["support_weapon"] = "Support weapons", ["backpack"] = "Backpacks", ["ammo"] = "Ammo", ["stim"] = "Stims", ["grenade"] = "Grenades",
        ["supply"] = "Supplies",
    };

    static JsonObject? PodCategory(StatsItem item)
    {
        var racks = ReferenceData.Value["podRacks"]?.AsArray().Select(r => r!).Where(r => r["consumers"]!.AsArray()
            .Any(c => c!.GetValue<string>() is var n && (n == item.Name || n == item.Name + " (granted stratagem)"))).ToList() ?? [];
        if (racks.Count == 0) return null;
        var pickups = ReferenceData.Value["pickups"]!.AsObject();
        var ids = pickups.Select(p => p.Key).ToList();
        var packages = ReferenceData.Value["packages"]!.AsObject();
        var subs = new JsonArray();
        foreach (var rack in racks)
        {
            var name = rack["rack"]!.GetValue<string>();
            var resource = rack["resource"]!.GetValue<string>();
            var consumers = rack["consumers"]!.AsArray().Select(c => c!.GetValue<string>()).ToList();
            var shared = rack["shared"]!.GetValue<bool>();
            var also = consumers.Where(c => c != item.Name && c != item.Name + " (granted stratagem)").ToList();
            var rows = new JsonArray();
            JsonObject Row(string label, string key, int offset, string type, JsonArray options, string note) => new()
            {
                ["label"] = label, ["cat"] = "pods", ["target"] = name, ["key"] = key, ["choice"] = key == "pod.count" ? "podcount" : "pod",
                ["component"] = "HellpodRackComponentData", ["ref"] = resource, ["offset"] = offset, ["type"] = type,
                ["min"] = null, ["max"] = null, ["unit"] = null, ["applies"] = "respawn", ["verified"] = false, ["unverified"] = true,
                ["shared"] = shared, ["also"] = also.Count > 0 ? new JsonArray(also.Select(a => (JsonNode)a).ToArray()) : null,
                ["link"] = $"pod|{name}|{key}", ["note"] = note, ["options"] = options,
            };
            JsonObject Option(int value, string label, string? resourceHex, string group, string note) => new()
            {
                ["value"] = value, ["label"] = label, ["resource"] = resourceHex, ["group"] = group, ["note"] = note,
            };
            foreach (var slot in rack["slots"]!.AsArray().Select(s => s!))
            {
                var k = slot["slot"]!.GetValue<int>();
                var vanilla = slot["default"]!.GetValue<string>();
                var tested = slot["tested"]!.AsArray().Select(t => t!.GetValue<string>()).ToHashSet(StringComparer.Ordinal);
                var options = new JsonArray();
                string NoteOf(string id)
                {
                    var p = pickups[id]!;
                    var notes = new List<string>();
                    if (tested.Contains(id)) notes.Add("proven in this slot in game");
                    else notes.Add(p["compatibility"]!.GetValue<string>() == "UNVERIFIED_REFERENCE" ? "no vanilla pod delivers it: untested" : "untested in this pod");
                    if (p["package"]?.GetValue<string>() is { } package && !p["alwaysResident"]!.GetValue<bool>())
                        notes.Add("loads its package first" + (packages[package]?.GetValue<string>() is { } pn && !pn.StartsWith("unnamed", StringComparison.Ordinal) ? $" ({pn.Split('/')[^1]})" : ""));
                    return string.Join(" · ", notes);
                }
                if (vanilla == "empty") options.Add(Option(0, "Empty (vanilla)", "0x0", "", "the slot delivers nothing"));
                else
                {
                    var p = pickups[vanilla]!;
                    options.Add(Option(ids.IndexOf(vanilla) + 1, p["name"]!.GetValue<string>() + " (vanilla)", p["resource"]!.GetValue<string>(), "", "what this pod opens with"));
                    options.Add(Option(0, "Empty", "0x0", "", "the slot delivers nothing"));
                }
                foreach (var (id, p) in pickups.Where(p => p.Key != vanilla))
                    options.Add(Option(ids.IndexOf(id) + 1, p!["name"]!.GetValue<string>(), p["resource"]!.GetValue<string>(),
                        PickupGroups.GetValueOrDefault(p["category"]!.GetValue<string>(), "Other"), NoteOf(id)));
                var podRow = Row($"Slot {k}", $"pod.slot_{k}", (k - 1) * 64, "u64", options,
                    "What the pod opens with in this slot (HD2R Editor's pod payload): pods called in after it carry it." +
                    (shared ? $" The rack is shared: {string.Join(", ", consumers)} change with it." : ""));
                if (Unaddressed(item.Name, $"|pod|payload.slot_{k}") is { } podKey)
                {
                    var names = new JsonObject { ["empty"] = 0 };
                    foreach (var id in ids) names[id] = ids.IndexOf(id) + 1;
                    Hd2rNamed(podRow, podKey, "pickup", names);
                }
                rows.Add(podRow);
            }
            var count = rack["spawnCount"]!.GetValue<int>();
            var counts = new JsonArray(new[] { count }.Concat(Enumerable.Range(1, 4).Where(n => n != count))
                .Select(n => (JsonNode)Option(n, n + (n == 1 ? " item" : " items") + (n == count ? " (vanilla)" : ""), null, "",
                    n == count ? "the game's own" : $"slots 1 to {n} spawn")).ToArray());
            rows.Add(Row("Items spawned", "pod.count", 556, "u32", counts,
                "How many of the slots above spawn (the first N): untested in game (HD2Runtime: vanilla racks use 1, 2, 4 and 6)."));
            subs.Add(new JsonObject
            {
                ["title"] = racks.Count > 1 ? name : "Hellpod Payload", ["key"] = "c|pods|" + name, ["fold"] = false, ["prefix"] = null, ["rows"] = rows,
            });
        }
        return new JsonObject { ["title"] = "Hellpod Payload", ["key"] = "c|pods", ["flat"] = racks.Count == 1, ["plain"] = false, ["prefix"] = null, ["subs"] = subs };
    }

    // Whole-row copies (HD2R Editor's G-109 Urchin "Pulse explosion"; mappings/references.json "rowCopies"): a choice of the
    // explosions whose settings replace the item's own explosion row (SDK 9 copy_row), its own first. Kept in the editor's
    // settings with the arm swaps and pods: applied live, back at every boot, the host's in co-op.
    static JsonObject? RowCopyCategory(StatsItem item)
    {
        var copies = ReferenceData.Value["rowCopies"]?.AsArray().Select(r => r!)
            .Where(r => r["target"]!.GetValue<string>() == item.Name && item.Parts.Any(p => p.Category == r["category"]!.GetValue<string>())).ToList() ?? [];
        if (copies.Count == 0) return null;
        var labels = ReferenceData.Value["explosions"]!.AsArray().Select(x => x!).GroupBy(x => x["type"]!.GetValue<long>())
            .ToDictionary(g => g.Key, g => g.First()["label"]!.GetValue<string>());
        var rows = new JsonArray();
        foreach (var c in copies)
        {
            var own = c["default"]!.GetValue<long>();
            var options = new JsonArray();
            foreach (var type in new[] { own }.Concat(c["allowed"]!.AsArray().Select(a => a!.GetValue<long>()).Where(t => t != own)))
                options.Add(new JsonObject
                {
                    ["value"] = type, ["label"] = labels.GetValueOrDefault(type, $"Explosion {type}") + (type == own ? " (vanilla)" : ""),
                    ["note"] = type == own ? "its own explosion" : $"explosion {type}'s settings copied over its own (loads its packages first)",
                });
            rows.Add(new JsonObject
            {
                ["label"] = c["label"]!.DeepClone(), ["cat"] = c["category"]!.DeepClone(), ["target"] = item.Name, ["key"] = c["key"]!.DeepClone(),
                ["choice"] = "rowcopy", ["component"] = c["table"]!.DeepClone(), ["ref"] = c["row"]!.GetValue<long>().ToString(CultureInfo.InvariantCulture),
                ["offset"] = c["offset"]!.DeepClone(), ["size"] = c["size"]!.DeepClone(), ["type"] = "u32", ["min"] = null, ["max"] = null, ["unit"] = null,
                ["applies"] = "now", ["verified"] = false, ["unverified"] = true, ["shared"] = true, ["also"] = null,
                ["link"] = $"rowcopy|{item.Name}|{c["key"]}", ["options"] = options,
                ["note"] = "Another explosion's whole settings over its own (HD2R Editor's pulse explosion): its explosion rows show the copy. Untested in game.",
            });
            if (Hd2rData.Value["keys"]!.AsObject().Select(k => k.Key).FirstOrDefault(k => k.Split('|')[1] == item.Name && k.EndsWith(":" + c["key"]!.GetValue<string>(), StringComparison.Ordinal)) is { } pulseKey)
            {
                rows[^1]!["hd2r"] = new JsonArray(pulseKey);
                rows[^1]!["hd2rKind"] = "explosion"; /* a preset's {ref: explosion, id}: its type, the option's value */
            }
        }
        var part = new JsonObject { ["title"] = "Pulse", ["key"] = "c|rowcopy|Pulse", ["fold"] = false, ["prefix"] = null, ["rows"] = rows };
        return new JsonObject { ["title"] = "Pulse", ["key"] = "c|rowcopy", ["flat"] = true, ["plain"] = false, ["prefix"] = null, ["subs"] = new JsonArray(part) };
    }

    // The armory's displayed armor penetration (HD2R Editor's "Displayed armor penetration"): which of the armor
    // penetration traits the weapon's trait slots hold (LoadoutEntryComponentData +12, five u32), or none. A choice row
    // after the slots; the editor stages a pick on the slot that holds one, else the first empty slot, so the trait
    // slots stay what is saved. HD2R presets' AP label key is on it (by the AP trait's name), for every weapon.
    static readonly (string Name, uint Trait)[] ApTraits =
        [("light", 300892437), ("medium", 3085090887), ("heavy", 658390125), ("light_anti_tank", 612466998), ("anti_tank", 3591930318)];

    static JsonObject? ApLabelRow(JsonObject trait5, StatsItem item, StatsRow row)
    {
        if (row.Part.Category != "weapons" || trait5["values"] is not JsonArray values) return null;
        var labels = values.Select(v => v!).ToDictionary(v => v["value"]!.GetValue<double>(), v => v["label"]!.GetValue<string>());
        var options = new JsonArray(new JsonObject { ["value"] = 0, ["label"] = "None", ["note"] = "no armor penetration trait in the armory" });
        foreach (var (name, trait) in ApTraits)
            if (labels.TryGetValue(trait, out var label))
                options.Add(new JsonObject { ["value"] = trait, ["label"] = label, ["note"] = "the armory shows " + label.ToLowerInvariant() });
        var target = row.Part.Target;
        var family = StatsSchema.Current.Targets["weapons"][target].Slot == "support" ? "sp|" + target + "|sw|" : "pw|" + target + "|";
        var key = Hd2rData.Value["slots"]!.AsObject().Where(s => s.Value!["kind"]!.GetValue<string>() == "ap" && s.Key.Split('|')[1] == target)
            .Select(s => s.Key).FirstOrDefault() ?? family + "presentation.armor_penetration";
        var names = new JsonObject { ["none"] = 0 };
        foreach (var (name, trait) in ApTraits) names[name] = trait;
        return new JsonObject
        {
            ["label"] = "Displayed armor penetration", ["cat"] = trait5["cat"]!.DeepClone(), ["target"] = target, ["key"] = "presentation.armor_penetration",
            ["choice"] = "aplabel", ["component"] = trait5["component"]?.DeepClone(), ["ref"] = trait5["ref"]?.DeepClone(), ["offset"] = 12, ["type"] = "u32",
            ["min"] = null, ["max"] = null, ["unit"] = null, ["applies"] = trait5["applies"]!.DeepClone(), ["verified"] = false, ["unverified"] = true,
            ["shared"] = trait5["shared"]!.DeepClone(), ["also"] = null, ["link"] = "aplabel|" + target,
            ["note"] = "What armor penetration the armory shows (a trait only: the weapon's damage decides the real one). Written into the trait slot that holds one, else the first empty slot.",
            ["options"] = options,
            ["hd2r"] = new JsonArray(key), ["hd2rKind"] = "ap",
            ["hd2rAs"] = new JsonObject { [key] = new JsonObject { ["kind"] = "ap", ["slot"] = 0, ["names"] = names } },
        };
    }

    // The mount slot holding a vehicle page's weapon part (its own weapon), or null.
    static string? MountSlotOf(StatsItem item, StatsPart part)
    {
        var vehicle = item.Parts.FirstOrDefault(p => p.Category == "vehicles")?.Target;
        if (vehicle == null || part.Category != "weapons") return null;
        var resources = StatsSchema.Current.Targets["weapons"][part.Target].Fields.Values.Where(f => f.Kind == "component").Select(f => f.Resource)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        var weapons = ReferenceData.Value["mountedWeapons"]!.AsObject();
        var slot = ReferenceData.Value["mountSlots"]!.AsArray().Select(s => s!).FirstOrDefault(s => s["vehicle"]!.GetValue<string>() == vehicle &&
            resources.Contains(weapons[s["default"]!.GetValue<string>()]?["resource"]?.GetValue<string>() ?? ""));
        return slot == null ? null : $"mount.slot_{slot["slot"]!.GetValue<int>()}";
    }

    static JsonObject Object(StatsItem item, string category, string? tone, StatsSchema schema)
    {
        var main = item.Parts[0];
        var page = new JsonArray();
        foreach (var section in StatsCatalog.Sections(item, schema))
        {
            var classified = !section.Key.Contains(':');
            var rows = section.Rows.ToList();
            // an attachment category keeps every attachment as its own group, one field or many (HD2R Editor's)
            var attachments = section.Key.StartsWith("att:", StringComparison.Ordinal) || section.Key == "melee" ||
                section.Rows.Any(r => r.Key.StartsWith("attachment.", StringComparison.Ordinal)); /* magazines inside Ammunition */
            var flat = section.Groups.Count == 1 && !attachments; /* a single group: its rows, no sub-header (Hellpod / Call-in) */
            var plain = rows.Count == 1 && !attachments;
            string? prefix = null;
            if (plain)
            {
                var g = section.Groups[0];
                var own = FoldPrefix(g.Title, rows[0].Label);
                prefix = classified ? (own != null && g.Title != section.Title ? own : FoldPrefix(section.Title, rows[0].Label)) : own;
            }
            var key = "c|" + section.Key;
            var subs = new JsonArray();
            foreach (var g in section.Groups)
            {
                var fold = g.Rows.Count == 1 && !flat && !plain && !attachments;
                subs.Add(new JsonObject
                {
                    ["title"] = g.Title, ["key"] = key + "|" + g.Title, ["fold"] = fold,
                    ["prefix"] = fold ? FoldPrefix(g.Title, g.Rows[0].Label) : null,
                    ["rows"] = new JsonArray(g.Rows.SelectMany(r => r.Key == "presentation.traits.5" && ApLabelRow(Row(r, item), item, r) is { } ap
                        ? new JsonNode[] { WithMount(Row(r, item), item, r), ap } : [WithMount(Row(r, item), item, r)]).ToArray()),
                });
            }
            page.Add(new JsonObject { ["title"] = section.Title, ["key"] = key, ["flat"] = flat, ["plain"] = plain, ["prefix"] = prefix, ["subs"] = subs });
        }
        if (MountCategory(item) is { } mountCategory)
            page.Insert(page.Count > 0 && page[0]!["key"]!.GetValue<string>() == "c|call_in" ? 1 : 0, mountCategory);
        if (RowCopyCategory(item) is { } rowCopyCategory) page.Add(rowCopyCategory);
        if (PodCategory(item) is { } podCategory)
        {
            var callIn = page.Select((c, i) => (c, i)).FirstOrDefault(p => p.c!["key"]!.GetValue<string>() == "c|call_in");
            page.Insert(callIn.c != null ? callIn.i + 1 : page.Count, podCategory); /* right after the call-in */
        }
        // A weapon HD2R Editor offers no projectile swap for: why (shown on its page).
        var refused = item.Parts.Where(p => p.Category == "weapons" && !StatsSchema.Current.Targets["weapons"][p.Target].Fields.Keys.Any(k => k.EndsWith(".projectile", StringComparison.Ordinal)))
            .Select(p => ReferenceData.Value["refused"]![p.Target]).FirstOrDefault(r => r != null);
        // a mission difficulty: its level's icon and "D10 · Super Helldive" (the game's icons, levels 1 to 10; level 11's
        // is its mod's)
        var level = item.Nav == "difficulty" ? item.Parts[0].Target : null;
        return new JsonObject
        {
            ["id"] = item.Id, ["name"] = level != null && item.Name.StartsWith("Difficulty ", StringComparison.Ordinal) ? "D" + item.Name["Difficulty ".Length..] : item.Name,
            ["category"] = category, ["subtitle"] = item.Line ?? item.Group,
            // the filter chip it is under, where the line under its name is not it (a roster row's faction)
            ["group"] = item.Line != null ? item.Group : null,
            ["icon"] = level != null ? "difficulty" : Icon(item, main), ["tone"] = tone, ["search"] = item.Search, ["page"] = page,
            ["notOffered"] = refused?.GetValue<string>(),
            // an armor perk: its passive's icon (by the game's name), and what it does (HD2R Editor's "Perk details");
            // a difficulty: its level
            ["sprite"] = item.Nav == "perks" ? PerkSprite(item.Name) : level,
            // an entity's faction, whose emblem the editor draws (the HD2R Editor's: the Helldivers' is Super Earth's)
            ["faction"] = item.Faction switch
            {
                "terminids" or "automatons" or "illuminate" or "super_earth" => item.Faction,
                "helldivers" => "super_earth",
                _ => null
            },
            ["detail"] = item.Nav == "perks" ? PerkDetail(item, page.Count) : null,
        };
    }

    static string? PerkSprite(string title)
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.passives.json")!;
        return JsonNode.Parse(stream)!["passives"]!.AsArray().FirstOrDefault(p => p!["title"]!.GetValue<string>() == title)?["name"]!.GetValue<string>();
    }

    static string PerkDetail(StatsItem item, int categories)
    {
        var target = StatsSchema.Current.Targets["perks"][item.Name];
        return (target.Class ?? "") + (categories == 0 ? "   No numeric gameplay fields mapped for this perk." : "");
    }

    // The armor perks (HD2R Editor's "Armor perks", SDK set_passives): the local player's armor passive and a second passive
    // on top, two choice rows; -1 is the kits' own (HF_PASSIVE_KIT). Passives whose effect needs a package HellForge does
    // not load are not offered (mappings/passives.json), nor STANDARD ISSUE as the second (it gives nothing).
    static JsonObject ArmorPerks()
    {
        using var stream = typeof(EditorCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.passives.json")!;
        var passives = JsonNode.Parse(stream)!["passives"]!.AsArray().Select(p => p!.AsObject())
            .Where(p => p["package"] == null).OrderBy(p => p["title"]!.GetValue<string>(), StringComparer.Ordinal).ToList();
        JsonArray Options(string none, bool second) => new(new JsonNode?[] { new JsonObject { ["value"] = -1, ["label"] = none } }.Concat(
            passives.Where(p => !(second && (p["id"]!.GetValue<int>() == 0 || p["armorOnly"]!.GetValue<bool>()))).Select(p => (JsonNode)new JsonObject
            {
                ["value"] = p["id"]!.GetValue<int>(), ["label"] = p["title"]!.GetValue<string>(), ["icon"] = p["name"]!.GetValue<string>(),
                ["note"] = p["description"]!.GetValue<string>() + (p["follows"]!.GetValue<string>() == "partial" ? " (Some of it applies only at spawn.)" : ""),
            })).ToArray());
        JsonObject Choice(string choice, string label, string none, bool second, string note) => new()
        {
            ["label"] = label, ["cat"] = "", ["target"] = "", ["key"] = "passives." + choice, ["choice"] = choice, ["component"] = null,
            ["ref"] = null, ["offset"] = 0, ["type"] = "i32", ["min"] = null, ["max"] = null, ["unit"] = null, ["applies"] = "now",
            ["verified"] = false, ["unverified"] = true, ["shared"] = false, ["also"] = null, ["link"] = "passives." + choice,
            ["note"] = note, ["options"] = Options(none, second),
            ["hd2r"] = new JsonArray("hd|Armor perks|" + choice), ["hd2rKind"] = "passive",
        };
        var rows = new JsonArray(
            Choice("armor", "Armor passive", "The armor's own", false, "Your own armor, solo only; follows armor changes"),
            Choice("second", "Second passive", "None", true, "Added to the armor passive (no stacking of the same bonus)"));
        var part = new JsonObject { ["title"] = "Armor perks", ["key"] = "c|perks|Armor perks", ["fold"] = false, ["prefix"] = null, ["rows"] = rows };
        var category = new JsonObject
        {
            ["title"] = "Armor perks", ["key"] = "c|perks", ["flat"] = true, ["plain"] = false, ["prefix"] = null, ["subs"] = new JsonArray(part),
        };
        return new JsonObject
        {
            ["id"] = "helldiver/armor-perks", ["name"] = "Armor perks", ["category"] = "helldiver", ["subtitle"] = "Your Helldiver",
            ["icon"] = null, ["tone"] = null, ["search"] = "armor perks passive passives helldiver", ["page"] = new JsonArray(category),
        };
    }

    static JsonObject WithMount(JsonObject row, StatsItem item, StatsRow r)
    {
        if (MountSlotOf(item, r.Part) is { } slot) row["mountSlot"] = slot;
        return row;
    }

    // Which of the game's icons the object shows (by its name in that group of mappings/hud-icons.json), or none.
    static string? Icon(StatsItem item, StatsPart main)
    {
        if (item.Id.StartsWith("stratagems/", StringComparison.Ordinal)) return Sprites.Value["stratagems"].ContainsKey(item.Name) ? "stratagem" : null;
        if (item.Id.StartsWith("boosters/", StringComparison.Ordinal)) return Sprites.Value["boosters"].ContainsKey(item.Name) ? "booster" : null;
        if (item.Id.StartsWith("perks/", StringComparison.Ordinal)) return "passive";
        if (main.Category == "throwables") return "throwable";
        return main.Category == "weapons" && Sprites.Value["weapons"].ContainsKey(item.Name) ? "weapon" : null;
    }

    // The part's title before a folded row's label, unless the label starts with its first word already
    // ("Ammunition · Spare magazines", but "Recoil" alone under "Recoil").
    static string? FoldPrefix(string title, string label)
    {
        var word = title.Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault()?.ToLowerInvariant() ?? "";
        return word.Length > 0 && !label.ToLowerInvariant().StartsWith(word, StringComparison.Ordinal) ? title : null;
    }

    static JsonObject Row(StatsRow row, StatsItem item)
    {
        var f = row.Field;
        var applies = LiveApplies.Of(f);
        var type = f.LiveId ?? (f.Kind == "StratagemDefinition" && StratagemTypes.Value.TryGetValue(row.Part.Target, out var t) ? t : (ulong?)null);
        var live = applies != null && (f.Kind != "StratagemDefinition" || type != null);
        var also = StatsCatalog.AlsoChanges(row, item.Nav is "projectiles" or "damage");
        var json = new JsonObject
        {
            ["label"] = row.Label, ["cat"] = row.Part.Category, ["target"] = row.Part.Target, ["key"] = row.Key,
            // set_live / get_live: the component record (or settings table) and the target, an entity's resource id or a row id
            ["component"] = live ? (f.Kind == "component" ? f.Component : f.Kind) : null,
            ["ref"] = live ? (f.Kind == "component" ? f.Resource : (type ?? (ulong)f.Record).ToString(CultureInfo.InvariantCulture)) : null,
            ["offset"] = f.Offset, ["type"] = f.Storage,
            ["min"] = f.Min, ["max"] = f.Max, ["unit"] = StatsCatalog.Unit(f.Unit) is { Length: > 0 } unit ? unit : null,
            ["applies"] = live ? applies!.When : "boot", ["verified"] = live && applies!.Verified, ["unverified"] = f.Unverified,
            ["shared"] = f.Shared, // set_live with HF_LIVE_SHARED, as the launcher sends it
            ["also"] = also.Count > 0 ? new JsonArray(also.Select(a => (JsonNode)a).ToArray()) : null,
            ["link"] = StatsCatalog.LinkKey(f),
        };
        // weapons this row reaches only when they equip the same ammunition (bug-report-0035), and that ammunition
        if (StatsCatalog.AlsoIfEquipped(row) is { Item: { } ammo, Names.Count: > 0 } equipped)
        {
            json["alsoIf"] = new JsonArray(equipped.Names.Select(n => (JsonNode)n).ToArray());
            json["alsoAmmo"] = ammo;
        }
        // get_stat: a field get_live cannot read (entity deltas, say), by the address the session's stats snapshot reads
        if (!live) json["stat"] = new JsonArray(f.Kind, f.Component, f.Resource, f.Record);
        // a field with named values (a flag, an enum, a trait or fire mode slot): picked from them
        if (f.Options.ValueKind == JsonValueKind.Array)
            json["values"] = new JsonArray(f.Choices.Select(c => (JsonNode)new JsonObject { ["value"] = c.Value, ["label"] = c.Label }).ToArray());
        // References (HD2R Editor's payload swaps): what the row picks; a status slot's number (its strength's too) and
        // the slot group; the rows a projectile swap locks (the weapon's own round), named by the swap's key.
        var pick = Pick(f);
        if (pick != null) json["pick"] = pick;
        if (StatsCatalog.Unlimited(f)) { json["unlimited"] = true; json["label"] = row.Label + " (next launch)"; } /* UINT32_MAX: unlimited, -1 in the stats file */
        // a roster row's weight at one difficulty: "D10 weight", with that difficulty's icon beside it (the HD2R Editor's)
        if (row.Part.Category == "rosters" && System.Text.RegularExpressions.Regex.Match(row.Key, @"^spawn\.weight_(\d+)$") is { Success: true } weight)
        {
            json["difficulty"] = int.Parse(weight.Groups[1].Value, CultureInfo.InvariantCulture);
            json["label"] = "D" + weight.Groups[1].Value + " weight";
        }
        var slot = System.Text.RegularExpressions.Regex.Match(row.Key, @"^(.*?)status_([1-4])_(type|strength)$");
        if (slot.Success && f.Kind == "DamageInfo") { json["slot"] = int.Parse(slot.Groups[2].Value, CultureInfo.InvariantCulture); json["slots"] = slot.Groups[1].Value; }
        if (pick == "explosion" && ReferenceData.Value["payload"]!["excluded"]![$"{row.Part.Category}/{row.Part.Target}/{row.Key}"] is JsonArray excluded)
            json["exclude"] = excluded.DeepClone();
        if (row.Key == "function_ammo.projectile" && ReferenceData.Value["programmable"]![row.Part.Target] is { } programmed)
            json["note"] = "While programmable ammunition is on, this projectile replaces the fired one. " + programmed["note"]!.GetValue<string>();
        if (f.Kind == "EntityDelta" && pick == "projectile" && Ammunition.Value.TryGetValue(row.Part.Target, out var ammunition))
        {
            json["base"] = ammunition["projectile"]!.GetValue<long>();
            json["note"] = $"Swapped on the default ammunition ({ammunition["item"]}), which every weapon that equips it shares";
        }
        if (f.Note != null && json["note"] == null) json["note"] = f.Note;
        Hd2rKeys(json, row, item);
        if (row.Key == "charge.overcharge_explosion" && Unaddressed(row.Part.Target, "|charge.overcharge_explosion") is { } over)
            Hd2rNamed(json, over, "named", (JsonObject)ParityData.Value["overchargeExplosions"]!.DeepClone()); /* a preset names the weapon */
        if (row.Part.Category == "weapons" && DefaultReload.Value.GetValueOrDefault(row.Part.Target) == row.Key &&
            Unaddressed(row.Part.Target, "|reload.default_magazine") is { } reload)
        {
            if (json["hd2r"] is not JsonArray list) { json["hd2r"] = list = []; json["hd2rKind"] = "number"; }
            list.Add(reload);
        }
        var swap = SwapKey(row.Part);
        if (swap != null && row.Key != swap && f.Kind is "ProjectileSettings" or "DamageInfo" or "ExplosionSettings" or "StatusEffectSettings" or "ArcSettings" &&
            MainRound(row.Key))
            json["fires"] = swap;
        return json;
    }

    // The key of the part's projectile swap row, if it has one.
    static string? SwapKey(StatsPart part)
    {
        var fields = StatsSchema.Current.Targets[part.Category][part.Target].Fields;
        return new[] { "attack.primary.projectile", "attack.feed_primary.projectile", "ammunition.projectile" }.FirstOrDefault(fields.ContainsKey);
    }
}
