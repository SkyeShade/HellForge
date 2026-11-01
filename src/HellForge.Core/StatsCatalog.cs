using System.Globalization;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The stats editor's view of the schema: the arsenal as players know it (one item per weapon or stratagem, holding
// its call-in, weapon, backpack, mounts and underbarrel), each item's fields in page categories, and how values are
// labelled, formatted and entered. Layout follows the HD2R Editor's.

// Tone: offensive, defensive or support (stratagem colours); null for weapons.
public sealed record StatsNav(string Id, string Label, string Section, string? Tone);
// One schema target inside an item. Title names it on the page when several parts share a category ("Backpack").
public sealed record StatsPart(string Category, string Target, string Title);
public sealed record StatsItem(string Id, string Name, string Nav, string? Group, string? Subtitle, IReadOnlyList<StatsPart> Parts)
{
    public string Search { get; init; } = "";
    // An entity's faction (StatsTarget.Faction), for its emblem in the editor.
    public string? Faction { get; init; }
    // The editor's second line under the item's name, where it is not its group (a roster row's "Automatons · Row 1";
    // the group still picks its filter chip).
    public string? Line { get; init; }
}
public sealed record StatsRow(StatsPart Part, string Key, StatsField Field, string Label)
{
    public string Id => Part.Category + "\t" + Part.Target + "\t" + Key;
}
public sealed record StatsGroup(string Title, IReadOnlyList<StatsRow> Rows);
public sealed record StatsSection(string Key, string Title, IReadOnlyList<StatsGroup> Groups)
{
    public IEnumerable<StatsRow> Rows => Groups.SelectMany(g => g.Rows);
}

public static class StatsCatalog
{
    public static readonly IReadOnlyList<StatsNav> Navs =
    [
        new("primary", "Primary", "Weapons", null), new("secondary", "Secondary", "Weapons", null),
        new("throwables", "Throwables", "Weapons", null),
        new("offensive", "Offensive", "Stratagems", "offensive"), new("defensive", "Defensive", "Stratagems", "defensive"),
        new("support", "Support weapons", "Stratagems", "support"), new("backpacks", "Backpacks", "Stratagems", "support"),
        new("vehicles", "Vehicles", "Stratagems", "support"), new("mission", "Mission", "Stratagems", "support"),
        new("boosters", "Boosters", "Equipment", null),
        new("projectiles", "Projectiles", "Advanced", null), new("damage", "Damage types", "Advanced", null),
        new("statuses", "Status effects", "Advanced", null), new("perks", "Armor perks", "Equipment", null),
        new("explosions", "Explosions", "Advanced", null),
        new("terminids", "Terminids", "Enemies", null), new("automatons", "Automatons", "Enemies", null),
        new("illuminate", "Illuminate", "Enemies", null), new("structures", "Structures", "Enemies", null),
        new("entities", "Other entities", "Enemies", null),
        new("difficulty", "Mission difficulty", "Spawning", null), new("rosters", "Spawn rosters", "Spawning", null),
        new("compositions", "Spawn compositions", "Spawning", null), new("demolition", "Demolition templates", "Spawning", null)
    ];

    // The armory's own group order; groups it does not list follow alphabetically.
    static readonly string[] GroupOrder = ["Assault Rifle", "Marksman Rifle", "Submachine Gun", "Shotgun", "Explosive", "Energy-Based",
        "Pistol", "Melee", "Standard", "Special", "Orbital", "Eagle", "Sentry", "Emplacement", "Mines", "Other", "Used", "Unused"];
    static readonly Dictionary<string, (string Nav, string? Group)> Families = new(StringComparer.Ordinal)
    {
        ["orbital"] = ("offensive", "Orbital"), ["eagle"] = ("offensive", "Eagle"), ["sentry"] = ("defensive", "Sentry"),
        ["emplacement"] = ("defensive", "Emplacement"), ["mine"] = ("defensive", "Mines"), ["support"] = ("support", null),
        ["backpack"] = ("backpacks", null), ["vehicle"] = ("vehicles", null), ["mission"] = ("mission", null)
    };

    static readonly Lazy<IReadOnlyList<StatsItem>> AllItems = new(() => Build(StatsSchema.Current));
    public static IReadOnlyList<StatsItem> Items => AllItems.Value;

    public static IReadOnlyList<StatsItem> Build(StatsSchema schema)
    {
        var t = schema.Targets;
        Dictionary<string, StatsTarget> Section(string name) => t.GetValueOrDefault(name) ?? [];
        var weapons = Section("weapons");
        var used = new HashSet<(string, string)>();
        var items = new List<StatsItem>();
        StatsPart Part(string category, string target, string title) { used.Add((category, target)); return new(category, target, title); }
        List<StatsPart> Mounted(string host) => weapons.Where(w => w.Value.Host == host && w.Value.Slot is "mounted" or "underbarrel")
            .OrderBy(w => w.Key, StringComparer.OrdinalIgnoreCase)
            .Select(w => Part("weapons", w.Key, w.Value.Slot == "underbarrel" ? "Underbarrel" : MountLabel(host, w.Value.Mount ?? w.Key))).ToList();

        foreach (var (name, target) in weapons.Where(w => w.Value.Slot is "primary" or "secondary"))
            items.Add(new("weapons/" + name, name, target.Slot!, target.Class ?? "Other", null, [Part("weapons", name, "Weapon"), .. Mounted(name)]));
        foreach (var (name, target) in Section("throwables"))
            items.Add(new("throwables/" + name, name, "throwables", target.Class ?? "Other", null, [Part("throwables", name, "Throwable")]));
        foreach (var (name, target) in Section("stratagems")) AddStratagem(name, target);
        foreach (var (name, target) in Section("statuses"))
            items.Add(new("statuses/" + name, name, "statuses", target.Class, Humanize(target.Family ?? ""), [Part("statuses", name, "Status effect")]));
        foreach (var (name, target) in Section("explosions"))
            items.Add(new("explosions/" + name, name, "explosions", Humanize(target.Family ?? "other"), null, [Part("explosions", name, "Explosion")]));
        foreach (var name in Section("helldiver").Keys)
            items.Add(new("helldiver/" + name, name, "helldiver", null, "Your Helldiver", [Part("helldiver", name, "Helldiver")]));
        foreach (var name in Section("perks").Keys)
            items.Add(new("perks/" + name, name, "perks", null, "Armor perks", [Part("perks", name, "Armor perk")]));
        foreach (var name in Section("boosters").Keys)
            items.Add(new("boosters/" + name, name, "boosters", null, null, [Part("boosters", name, "Booster")]));
        AddUnmatched();
        foreach (var (name, target) in Section("enemies").Where(e => !used.Contains(("enemies", e.Key)))) AddEnemy(name, target);
        AddSpawning();

        // Rows reached by type id are named by the items that use them.
        var users = Users(items, t);
        foreach (var (category, title) in new[] { ("projectiles", "Projectile"), ("damage", "Damage type") }) AddTypeRows(category, title, users);

        return items.Select(i => i with { Search = $"{i.Name} {i.Group} {NavLabel(i.Nav)} {string.Join(" ", i.Parts.Select(p => p.Target))} {i.Search}".ToLowerInvariant() })
            .OrderBy(i => Navs.Select(n => n.Id).ToList().IndexOf(i.Nav)).ThenBy(i => GroupRank(i.Group)).ThenBy(i => i.Group, StringComparer.Ordinal)
            .ThenBy(i => i.Nav is "projectiles" or "damage" ? i.Name.Length : 0)
            // the mission difficulties by their level (1 to 11), not as text ("Difficulty 10" before "Difficulty 2")
            .ThenBy(i => i.Nav == "difficulty" && int.TryParse(i.Parts[0].Target, NumberStyles.None, CultureInfo.InvariantCulture, out var level) ? level : 0)
            .ThenBy(i => i.Name, StringComparer.OrdinalIgnoreCase).ToList();

        // A stratagem's item: its call-in with the support weapon, vehicle, backpack and mounts that are its.
        void AddStratagem(string name, StatsTarget target)
        {
            var (nav, group) = Families.GetValueOrDefault(target.Family ?? "", ("mission", null));
            List<StatsPart> parts = [];
            if (weapons.TryGetValue(name, out var weapon) && weapon.Slot == "support") parts.Add(Part("weapons", name, "Weapon"));
            if (Section("vehicles").ContainsKey(name)) parts.Add(Part("vehicles", name, "Vehicle"));
            if (Section("backpacks").ContainsKey(name)) parts.Add(Part("backpacks", name, "Backpack"));
            else if (Section("backpacks").ContainsKey(name + " Backpack")) parts.Add(Part("backpacks", name + " Backpack", "Backpack"));
            parts.AddRange(Mounted(name));
            parts.Insert(nav is "support" or "backpacks" && parts.Count > 0 ? parts.Count : 0, Part("stratagems", name, "Call-in"));
            // the entities it deploys that have a page part of their own (the Solo Silo's rack and missile)
            foreach (var (entity, (_, title)) in StatsSchema.EntityParts.Where(p => p.Value.Item == name))
                if (Section("enemies").ContainsKey(entity)) parts.Add(Part("enemies", entity, title));
            items.Add(new("stratagems/" + name, name, nav, group, null, parts));
        }

        // Items with no stratagem of their own: mission weapons, unmatched backpacks, scenery vehicles.
        void AddUnmatched()
        {
            foreach (var (name, target) in weapons.Where(w => w.Value.Slot == "support" && !used.Contains(("weapons", w.Key))))
                items.Add(new("weapons/" + name, name, "support", "Other", null, [Part("weapons", name, "Weapon")]));
            foreach (var name in Section("backpacks").Keys.Where(n => !used.Contains(("backpacks", n))).ToList())
                items.Add(new("backpacks/" + name, name, "backpacks", "Other", null, [Part("backpacks", name, "Backpack")]));
            foreach (var name in Section("vehicles").Keys.Where(n => !used.Contains(("vehicles", n))).ToList())
                items.Add(new("vehicles/" + name, name, "vehicles", "Other", null, [Part("vehicles", name, "Vehicle"), .. Mounted(name)]));
            foreach (var (name, target) in weapons.Where(w => !used.Contains(("weapons", w.Key))).ToList())
                items.Add(new("weapons/" + name, name, "support", "Other", null, [Part("weapons", name, "Weapon")]));
        }

        // The HD2R Editor's Enemies pages: an item per entity, under its faction's (or Structures', or Other entities')
        // nav; the HD2R Editor's unnamed entities last. Search finds an entity by its label or its HD2R name.
        void AddEnemy(string name, StatsTarget target)
        {
            var nav = target.Family is "terminids" or "automatons" or "illuminate" or "structures" ? target.Family : "entities";
            var unnamed = name.StartsWith("unnamed_entity", StringComparison.Ordinal);
            items.Add(new("enemies/" + name, target.Label ?? name, nav, unnamed ? "Unnamed" : null, unnamed ? name : null, [Part("enemies", name, "Entity")])
                { Search = name, Faction = target.Faction });
        }

        // The HD2R Editor's Spawning pages: a difficulty's budgets, an enemy's spawn weights, a squad composition (by faction),
        // a demolition template's force.
        void AddSpawning()
        {
            foreach (var (name, target) in Section("difficulty"))
                items.Add(new("difficulty/" + name, target.Label ?? name, "difficulty", null, null, [Part("difficulty", name, "Difficulty")]));
            // a roster row as the HD2R Editor shows it: the enemy's name, its faction's emblem and filter chip, and its roster
            // and row ("automatons/row_1": "Automatons · Row 1") under the name
            foreach (var (name, target) in Section("rosters"))
            {
                var (roster, row) = name.Split('/') is [var r, var w] ? (r, w.StartsWith("row_", StringComparison.Ordinal) ? w[4..] : w) : (name, "");
                items.Add(new("rosters/" + name, target.Label ?? name, "rosters", FactionLabel(target.Family), name, [Part("rosters", name, "Roster row")])
                    { Search = name, Faction = target.Family, Line = Humanize(roster) + (row.Length > 0 ? " · Row " + row : "") });
            }
            foreach (var (name, target) in Section("compositions"))
                items.Add(new("compositions/" + name, target.Label ?? name, "compositions", FactionLabel(target.Family), Humanize(target.Class ?? ""),
                    [Part("compositions", name, "Composition")]) { Search = name + " " + target.Class });
            foreach (var (name, target) in Section("demolition"))
                items.Add(new("demolition/" + name, target.Label ?? name, "demolition", null, null, [Part("demolition", name, "Demolition template")])
                    { Search = name });
        }

        // A projectile's or damage type's own item, by its id, subtitled with the items that use it.
        void AddTypeRows(string category, string title, Dictionary<(string, long), SortedSet<string>> users)
        {
            foreach (var id in Section(category).Keys.OrderBy(k => long.Parse(k, CultureInfo.InvariantCulture)))
            {
                var names = users.GetValueOrDefault((category, long.Parse(id, CultureInfo.InvariantCulture)));
                var subtitle = names == null ? "Not used by a listed item" : string.Join(", ", names.Take(3)) + (names.Count > 3 ? $" +{names.Count - 3}" : "");
                items.Add(new(category + "/" + id, $"{title} {id}", category, names == null ? "Unused" : "Used", subtitle, [Part(category, id, title)])
                    { Search = string.Join(" ", names ?? []) });
            }
        }
    }

    // The items whose rows reach each projectile and damage type (by kind and id).
    static Dictionary<(string, long), SortedSet<string>> Users(List<StatsItem> items, Dictionary<string, Dictionary<string, StatsTarget>> t)
    {
        var users = new Dictionary<(string, long), SortedSet<string>>();
        foreach (var item in items)
            foreach (var part in item.Parts)
                foreach (var field in t[part.Category][part.Target].Fields.Values.Where(f => f.Kind is "ProjectileSettings" or "DamageInfo"))
                {
                    var key = (field.Kind == "ProjectileSettings" ? "projectiles" : "damage", field.Record);
                    if (!users.TryGetValue(key, out var names)) users[key] = names = new(StringComparer.OrdinalIgnoreCase);
                    names.Add(item.Name);
                }
        return users;
    }

    // A vehicle's mounts by what they are (HD2R Editor's mount_label): the Maelstrom's and the Bastion's guns, else the
    // humanized mount node ("Left gun").
    static readonly Dictionary<string, Dictionary<string, string>> MountLabels = new(StringComparer.Ordinal)
    {
        ["TD-110 Maelstrom"] = new() { ["attach_tank_gun"] = "Main cannon", ["slot_2"] = "Driver gun", ["slot_3"] = "Missile pods (slot 3)", ["slot_4"] = "Missile pods (slot 4)" },
        ["TD-220 Bastion MK XVI"] = new() { ["attach_tank_gun"] = "Cannon", ["attach_tank_gun_mg"] = "Coaxial MG" },
    };
    public static string MountLabel(string host, string mount) =>
        MountLabels.TryGetValue(host, out var labels) && labels.TryGetValue(mount, out var label) ? label : Humanize(mount);

    static int GroupRank(string? group) => group == null ? -1 : Array.IndexOf(GroupOrder, group) is var i and >= 0 ? i : GroupOrder.Length;
    public static string NavLabel(string id) => Navs.FirstOrDefault(n => n.Id == id)?.Label ?? id;
    public static StatsNav Nav(string id) => Navs.First(n => n.Id == id);
    public static StatsItem? Find(string category, string target) =>
        Items.FirstOrDefault(i => i.Parts.Any(p => p.Category == category && p.Target == target));

    // ---- Page categories ----

    static readonly Dictionary<string, string> Titles = new(StringComparer.Ordinal)
    {
        ["projectile"] = "Projectile", ["damage"] = "Damage", ["fire"] = "Handling / Fire", ["ammo"] = "Ammunition",
        ["heat"] = "Heat / Charge", ["beam"] = "Beam / Arc", ["health"] = "Health & Armor", ["call_in"] = "Hellpod / Call-in",
        ["zones"] = "Damage Zones", ["attacks"] = "Attacks", ["presentation"] = "Armory", ["melee"] = "Melee", ["att:magazine"] = "Magazines", ["att:muzzle"] = "Muzzles", ["att:optics"] = "Optics",
        ["att:underbarrel"] = "Underbarrels"
    };
    static readonly string[] Order = ["projectile", "damage", "fire", "ammo", "heat", "beam", "health"];
    // Sections that describe what the item is come before its weapon sections.
    static readonly Dictionary<string, string> Identity = new(StringComparer.Ordinal)
    {
        ["throwable"] = "Throwable", ["eagle"] = "Eagle", ["orbital"] = "Orbital strike", ["minefield"] = "Minefield",
        ["jump"] = "Jump pack", ["hover"] = "Hover pack", ["shield"] = "Shield", ["vehicle"] = "Vehicle", ["deposit"] = "Supply",
        ["booster"] = "Booster"
    };
    // The Fire mode group's order: the modes, the burst, then the rate-of-fire slots X, Y (the weapon's own fire rate), Z.
    static readonly Dictionary<string, int> FireRank = new(StringComparer.Ordinal)
    {
        ["fire_mode.modes.1"] = 0, ["fire_mode.modes.2"] = 1, ["fire_mode.modes.3"] = 2, ["fire_mode.burst_rounds"] = 3,
        ["fire_rate.modes.x"] = 4, ["weapon.fire_rate"] = 5, ["fire_rate.modes.z"] = 6,
    };

    static readonly Dictionary<string, int> AmmoRank = new(StringComparer.Ordinal)
    {
        ["weapon.capacity"] = 1, ["weapon.feed_capacity_1"] = 1, ["weapon.feed_capacity_2"] = 1, ["magazine.capacity"] = 1,
        ["rounds.feed_capacity_1"] = 1, ["rounds.feed_capacity_2"] = 1, ["magazine.spare_magazines"] = 2, ["rounds.spare_rounds"] = 2,
        ["magazine.starting_magazines"] = 3, ["rounds.starting_rounds"] = 3, ["magazine.magazines_from_supply"] = 4,
        ["rounds.rounds_from_supply"] = 4, ["reload.duration"] = 5
    };
    static readonly string[] HandlingWords = ["recoil", "spread", "sway", "ergonomics", "reticle", "aim", "zoom"];

    // (category, group title, rank within the category) for one field.
    static (string Category, string Group, double Rank) Place(string partCategory, string key, ISet<string> roundRoles, string? section = null)
    {
        if (PlaceMeleeOrAttachment(partCategory, key, section) is { } weaponPart) return weaponPart;
        if (partCategory == "vehicles" && PlaceVehicle(key, section) is { } vehicle) return vehicle;
        // The Spawning pages: their sections in the order of their fields (Budgets, Objectives, Patrols; Spawn weights; a
        // composition's groups, Selection, Eligibility).
        if (partCategory is "difficulty" or "rosters" or "compositions" or "demolition") return ("spawn:" + (section ?? "Other"), section ?? "Other", 0);
        if (partCategory == "enemies") return PlaceEntity(key, section);
        if (partCategory == "helldiver") return PlaceHelldiver(key, section);
        if (PlaceOwnPage(partCategory, key, section) is { } own) return own;
        return PlaceByDomain(partCategory, key, roundRoles, section);
    }

    // A weapon's melee attack (HD2R Editor's Melee rows): its tuning, then a group per hit volume; a weapon's attachments
    // (HD2R Editor's groups): a category per slot, a part per attachment. Null for any other field.
    static (string Category, string Group, double Rank)? PlaceMeleeOrAttachment(string partCategory, string key, string? section)
    {
        if (key.StartsWith("tuning:", StringComparison.Ordinal)) return ("melee", "Melee", -1);
        if (key.StartsWith("zone_", StringComparison.Ordinal) && key.Contains(':') && partCategory == "weapons")
            return ("melee", section ?? "Hit volume", int.TryParse(key[5..key.IndexOf(':')], NumberStyles.None, CultureInfo.InvariantCulture, out var volume) ? volume : 0);
        if (!key.StartsWith("attachment.", StringComparison.Ordinal)) return null;
        var slot = key.Split('.')[1];
        // the magazines join the weapon's Ammunition (its default one's fields in its first group, the others after)
        if (slot == "magazine") return ("ammo", section ?? Humanize(key.Split('.')[2]), section == "Ammunition" ? 0 : 10);
        return ("att:" + slot, section ?? Humanize(key.Split('.')[2]), 0);
    }

    // A vehicle's damage zones, shield, and rotation and vehicle fields; null for the others, placed as any target's.
    static (string Category, string Group, double Rank)? PlaceVehicle(string key, string? section)
    {
        if (key.StartsWith("zone_", StringComparison.Ordinal))
            return ("zones", section ?? Humanize(key[..key.IndexOf('.')]),
                int.TryParse(key[5..key.IndexOf('.')], NumberStyles.None, CultureInfo.InvariantCulture, out var zone) ? zone : 0);
        if (key.StartsWith("shield.", StringComparison.Ordinal)) return ("shield", section ?? "Shield", 0);
        if (key.StartsWith("rotation.", StringComparison.Ordinal) || key.StartsWith("vehicle.", StringComparison.Ordinal)) return ("health", "Health & Armor", 1);
        return null;
    }

    // An entity's page, in the HD2R Editor's sections (its row ids: "<path>:<zone or attack>:<field>"): Health & Armor,
    // a group per damage zone in slot order, a group per attack, then the rest (Spawning, Shield, Size & Geometry...).
    static (string Category, string Group, double Rank) PlaceEntity(string key, string? section)
    {
        var path = key[..Math.Max(0, key.IndexOf(':'))];
        var heading = section ?? "Other";
        if (path == "damage_zone")
        {
            var slot = key.Split(':')[1];
            return ("zones", heading, int.TryParse(slot.Replace("zone_", ""), NumberStyles.None, CultureInfo.InvariantCulture, out var zone) ? zone : 0);
        }
        if (path == "attack") return ("attacks", heading, 0);
        return heading == "Health & Armor" ? ("health", heading, 0) : ("other:" + heading, heading, 0);
    }

    // HD2R Editor's Helldiver page: Health, Speed, Stamina, Player refill, Damage, Size & Geometry, then the six body zones
    static (string Category, string Group, double Rank) PlaceHelldiver(string key, string? section)
    {
        if (section != null)
            return ("zones", section, Array.IndexOf(new[] { "Head", "Body", "Left arm", "Right arm", "Left leg", "Right leg" }, section));
        return key.Split('.') switch
        {
            ["entity", not "explosive_damage_percentage"] => ("identity:health", "Health", -1), // its avatar's main fields
            [_, "speed", ..] => ("identity:speed", "Speed", 0),
            [_, "stamina", ..] => ("identity:stamina", "Stamina", 1),
            [_, "supply", ..] => ("identity:refill", "Player refill", 2),
            ["unit", ..] => ("identity:size", "Size & Geometry", 4), // its UnitComponent (scale, radius)
            _ => ("identity:damage", "Damage", 3),
        };
    }

    // The pages with categories of their own: an explosion's, an armor perk's, a booster's, a projectile's, a status
    // effect's, a damage type's. Null for the others.
    static (string Category, string Group, double Rank)? PlaceOwnPage(string partCategory, string key, string? section) => partCategory switch
    {
        "explosions" => key.StartsWith("explosion.damage.", StringComparison.Ordinal) ? ("damage", "Damage", 0) : ("identity:explosion", "Explosion", 0),
        "perks" => section == "Stats applied at spawn" ? ("identity:perk_stats", "Stats applied at spawn", 0) : ("identity:perk", "Perk bonuses", 0),
        "boosters" => PlaceBooster(section),
        "projectiles" => ("projectile", "Projectile", 0),
        "statuses" => key.StartsWith("damage.", StringComparison.Ordinal) ? ("damage", "Damage while active", 0) : ("identity:status", "Status effect", 0),
        "damage" => ("damage", "Damage", 0),
        _ => null,
    };

    // A booster's page, in HD2R Editor's sections: an explosion under Projectile, status damage under Damage, the
    // granted stratagem under Hellpod / Call-in (first), the others categories of their own.
    static (string Category, string Group, double Rank) PlaceBooster(string? section) => section switch
    {
        "Explosion" => ("projectile", "Explosion", 50),
        "Status Damage" => ("damage", "Status Damage", 0),
        "Granted Stratagem" => ("call_in", "Granted Stratagem", 0),
        "Status Effect" => ("identity:status_effect", "Status Effect", 0),
        "Deployed Entity" => ("identity:deployed_entity", "Deployed Entity", 0),
        _ => ("identity:booster", "Tuning", 0),
    };

    // A weapon's, stratagem's, backpack's or vehicle's field, by its key's domain and attack branch.
    static (string Category, string Group, double Rank) PlaceByDomain(string partCategory, string key, ISet<string> roundRoles, string? section)
    {
        var segments = key.Split('.');
        var domain = segments[0];
        var middle = segments.Skip(1).Take(segments.Length - 2).Where(s => s != "damage").ToList();
        var role = Role(middle);
        switch (domain)
        {
            case "projectile": return ("projectile", Join("Flight", role), RoleRank(role) + 0.1);
            case "attack" or "ammunition": return ("projectile", Join("Swap", role), RoleRank(role) - 1);
            case "function_ammo": return ("projectile", "Swap · Programmable ammo", RoleRank(role) - 0.5);
            // a fired entity that explodes by itself (the Breacher's charge, the Maelstrom's rockets): its fuse, before its detonation
            case "fired": return ("projectile", section ?? "Fired entity", 40);
            case "terminal" when partCategory == "projectiles": return ("projectile", "Projectile", 0);
            case "terminal":
                var slot = Humanize(string.Join("_", middle).Replace("primary_", "").Replace("primary", "").Trim('_'));
                return ("projectile", "Explosion · " + slot, 50 + (slot.Contains("xpiry") ? 1 : 0));
            case "damage":
                return roundRoles.Contains(role) ? ("projectile", Join("Direct hit", role), RoleRank(role) + 0.2) : ("damage", Join("Damage", role), RoleRank(role));
            case "explosion":
                var phase = middle.Count == 0 ? "" : Humanize(string.Join("_", middle).Replace("primary_", "").Replace("primary", "").Trim('_'));
                return ("projectile", phase.Length == 0 ? "Explosion" : "Explosion · " + phase, 50 + (phase.Contains("xpiry") ? 1 : 0));
            case "status": return ("damage", "Status effects", 90);
            case "arc": return ("beam", Join("Arc", role), RoleRank(role));
            case "beam": return ("beam", Join("Beam", role), RoleRank(role));
            // HD2R Editor's Fire Mode section: the fire mode and rate-of-fire slots, and the weapon functions
            case "fire_mode" when key.StartsWith("fire_mode.modes.", StringComparison.Ordinal): return ("fire", "Fire mode", 1.5);
            case "fire_rate" or "weapon_function": return ("fire", "Fire mode", 1.5);
            case "fire_mode": return ("fire", "Fire", 1);
            case "presentation": return ("presentation", "Armory", 0); // the armory's traits
            case "turret": return ("fire", "Turret", 2);
            case "targeting": return ("fire", "Targeting", 3);
            case "weapon" when AmmoRank.ContainsKey(key): return ("ammo", "Ammunition", 0);
            case "weapon" when HandlingWords.Any(key.Contains): return ("fire", "Handling", 0);
            case "weapon": return ("fire", "Fire", 1);
            case "magazine" or "rounds" or "reload": return ("ammo", "Ammunition", 0);
            case "heatsink": return ("heat", "Heatsinks", 3);
            case "heat" when key is "heat.firing_charge" or "heat.charge_gain_per_second" or "heat.charge_loss_per_second" or "heat.reset_charge_after_shot":
                return ("heat", "Wind-up", 2);
            case "heat": return ("heat", "Heat", 0);
            case "charge": return ("heat", "Charge", 1);
            case "windup": return ("heat", "Wind-up", 2);
            case "recharge": return ("heat", "Recharge", 4);
            case "entity": return ("health", "Health & Armor", 0);
            case "zone": return ("health", "Damage zones", 1);
            case "stratagem": return ("call_in", "Call-in", 0);
            case "payload": return ("call_in", "Hellpod", 1);
        }
        return Identity.TryGetValue(domain, out var title) ? ("identity:" + domain, title, 0) : ("other:" + domain, Humanize(domain), 0);
    }

    // The attack branch a field belongs to: "" for the weapon's main round.
    static string Role(IReadOnlyList<string> middle) => middle.Count == 0 ? "" :
        middle[0] is "primary" or "feed_primary" ? "" : Humanize(middle[0].Replace("primary_", ""));
    static double RoleRank(string role) => role.Length == 0 ? 0 : 10 + (role.Length % 7);
    static string Join(string title, string role) => role.Length == 0 ? title : title + " · " + role;

    public static IReadOnlyList<StatsSection> Sections(StatsItem item, StatsSchema? schema = null)
    {
        schema ??= StatsSchema.Current;
        var weaponPage = item.Nav is "primary" or "secondary" or "throwables" or "support";
        var vehiclePage = item.Nav == "vehicles";
        var titles = new Dictionary<string, string>(StringComparer.Ordinal);
        var placed = new List<(string Category, string Group, double Rank, StatsRow Row)>();
        var shown = new HashSet<string>(StringComparer.Ordinal);
        foreach (var part in item.Parts) PlaceRows(part, schema, vehiclePage, shown, titles, placed);
        // A group that several parts fill is split per part, named after it: "Backpack · Ammunition". An entity a stratagem
        // deploys always says which it is ("Rack · Health & Armor"), its groups being the same as the others'.
        var groupParts = placed.GroupBy(p => (p.Category, p.Group)).ToDictionary(g => g.Key, g => g.Select(p => p.Row.Part).Distinct().ToList());
        bool Named(StatsPart part, string category, string group) =>
            part.Category == "enemies" && item.Parts.Count > 1 || groupParts[(category, group)].Count > 1 && groupParts[(category, group)][0] != part;
        var sections = new List<StatsSection>();
        var firstSeen = placed.Select(p => p.Category).Distinct().ToList();
        foreach (var category in firstSeen.OrderBy(c => vehiclePage ? VehicleRank(c, firstSeen) : CategoryRank(c, weaponPage, firstSeen)))
        {
            var rows = placed.Where(p => p.Category == category).ToList();
            var groups = rows.GroupBy(p => (p.Group, p.Row.Part))
                .OrderBy(g => item.Parts.ToList().IndexOf(g.Key.Part) * (category == "call_in" ? 0 : 1)).ThenBy(g => g.Min(p => p.Rank)).ThenBy(g => g.Min(p => p.Row.Field.Order))
                .Select(g => new StatsGroup(Named(g.Key.Part, category, g.Key.Group) ? g.Key.Part.Title + " · " + g.Key.Group : g.Key.Group,
                    g.OrderBy(p => AmmoRank.GetValueOrDefault(p.Row.Key, p.Row.Key.StartsWith("reload.", StringComparison.Ordinal) ? 5 : 3))
                        .ThenBy(p => FireRank.GetValueOrDefault(p.Row.Key, FireRank.Count)).ThenBy(p => p.Row.Field.Order).ThenBy(p => p.Row.Key, StringComparer.Ordinal).Select(p => p.Row).ToList()))
                .ToList();
            var title = titles.GetValueOrDefault(category) ?? Titles.GetValueOrDefault(category) ?? rows[0].Group;
            // A projectile category with no round of its own holds explosions only.
            if (category == "projectile" && !groups.Any(g => g.Title.Contains("Flight") || g.Title.Contains("Direct hit") || g.Title == "Projectile")) title = "Explosion";
            sections.Add(new(category, title, groups));
        }
        return sections;
    }

    // A part's rows, each placed in its category and group.
    static void PlaceRows(StatsPart part, StatsSchema schema, bool vehiclePage, HashSet<string> shown, Dictionary<string, string> titles,
        List<(string Category, string Group, double Rank, StatsRow Row)> placed)
    {
        var target = schema.Targets[part.Category][part.Target];
        var roles = target.Fields.Keys.Where(k => k.StartsWith("projectile.", StringComparison.Ordinal))
            .Select(k => Role(k.Split('.').Skip(1).SkipLast(1).ToList())).ToHashSet();
        // A catalog may name the same bytes twice (a deprecated id); one row each. An entity a stratagem deploys leaves
        // out the bytes the page shows already (the Solo Silo missile's attack is the weapon's explosion).
        foreach (var (key, field) in target.Fields.GroupBy(f => LinkKey(f.Value))
                     .Select(g => g.OrderBy(f => f.Value.Name.Contains("deprecated", StringComparison.OrdinalIgnoreCase)).ThenBy(f => f.Value.Order).First())
                     .Where(f => shown.Add(LinkKey(f.Value)) || part.Category != "enemies")
                     // a weapon's input bindings: the in-game editor binds rate of fire itself when a weapon has more
                     // than one rate (the editor's BindRateInput), as the HD2R Editor does
                     .Where(f => !f.Key.StartsWith("weapon_function.", StringComparison.Ordinal)))
        {
            var (category, group, rank) = Place(part.Category, key, roles, field.Section);
            var label = Label(key, field);
            if (category == "fire" && InFireModes(key, target))
            {
                (group, rank) = ("Fire mode", 1.5);
                if (key == "weapon.fire_rate") label = "Rate of fire Y";
            }
            if (vehiclePage && part.Category == "weapons")
            {
                // the mount's own category; its sections in the order a weapon page has them
                rank += WeaponSectionOrder(category) * 1000;
                category = "mount:" + part.Target;
                titles[category] = (schema.Targets["weapons"][part.Target].Class == "Shield" ? "Shield · " : "Weapon · ") + part.Title;
            }
            placed.Add((category, group, rank, new StatsRow(part, key, field, label)));
        }
    }

    // A weapon with fire modes: burst rounds under them, its fire rate as the Y slot between X and Z.
    static bool InFireModes(string key, StatsTarget target) =>
        key == "fire_mode.burst_rounds" && target.Fields.ContainsKey("fire_mode.modes.1") ||
        key == "weapon.fire_rate" && target.Fields.ContainsKey("fire_rate.modes.x");

    static int WeaponSectionOrder(string category) => Array.IndexOf(Order, category) is var order && order >= 0 ? order : Order.Length;

    // A vehicle's page: the call-in, (the editor's Mounted Weapons, EditorCatalog), each mount's weapon right below them,
    // then its health and armor, its damage zones and its shield.
    static int VehicleRank(string category, IList<string> firstSeen) => category switch
    {
        "call_in" => 0, "health" => 100, "zones" => 200, "shield" => 300,
        _ when category.StartsWith("mount:", StringComparison.Ordinal) => 10 + firstSeen.IndexOf(category),
        _ => 1000 + firstSeen.IndexOf(category),
    };

    static int CategoryRank(string category, bool weaponPage, IList<string> firstSeen) => category switch
    {
        "att:magazine" => 2000, "att:muzzle" => 2001, "att:optics" => 2002, "att:underbarrel" => 2003,
        "identity:health" => 0, "identity:speed" => 1, "identity:stamina" => 2, "identity:refill" => 3, "identity:damage" => 4, // the Helldiver's, in HD2R Editor's order
        "zones" => 200, "attacks" => 300, // an entity's, after its Health & Armor
        "call_in" => weaponPage ? 3000 : 0,
        _ when category.StartsWith("identity:", StringComparison.Ordinal) => 10 + firstSeen.IndexOf(category),
        _ when Array.IndexOf(Order, category) is var i and >= 0 => 100 + i,
        _ => 1000 + firstSeen.IndexOf(category)
    };

    // ---- Labels, units and values ----

    static readonly Dictionary<string, string> Words = new(StringComparer.Ordinal)
    {
        ["ap"] = "AP", ["rpm"] = "RPM", ["hp"] = "HP", ["id"] = "ID", ["ui"] = "UI", ["fov"] = "FOV", ["aoe"] = "AoE", ["ems"] = "EMS", ["mg"] = "MG"
    };

    // A faction's name for players (the roster pages' filter chips); "Other" for none.
    public static string FactionLabel(string? faction) => faction switch
    {
        "terminids" => "Terminids", "automatons" => "Automatons", "illuminate" => "Illuminate", "super_earth" => "Super Earth",
        null or "" => "Other", _ => Humanize(faction),
    };

    public static string Humanize(string text)
    {
        var words = text.Replace('/', ' ').Replace('.', ' ').Split(['_', ' '], StringSplitOptions.RemoveEmptyEntries)
            .Select(w => Words.GetValueOrDefault(w, w)).ToList();
        if (words.Count == 0) return text;
        var joined = string.Join(" ", words);
        return char.ToUpperInvariant(joined[0]) + joined[1..];
    }

    public static string Label(string key, StatsField field)
    {
        if (key == "damage" && field.Kind == "ProjectileSettings") return "Damage type";
        if (field.Name != key && !field.Name.Contains('.')) return field.Name;
        var segments = key.Split('.');
        var last = segments[^1];
        // "magazine.capacity" reads "Magazine capacity"; "magazine.spare_magazines" reads "Spare magazines".
        return Humanize(segments.Length > 1 && !last.Contains('_') && segments[^2] != "entity" ? segments[^2] + " " + last : last);
    }

    static readonly Dictionary<string, string> Units = new(StringComparer.Ordinal)
    {
        ["rpm"] = "rpm", ["mrad"] = "mrad", ["seconds"] = "s", ["milliseconds"] = "ms", ["degrees"] = "°", ["degrees_per_second"] = "°/s",
        ["meters"] = "m", ["meters_per_second"] = "m/s", ["grams"] = "g", ["kilograms"] = "kg", ["multiplier"] = "×", ["factor"] = "×",
        ["ratio"] = "", ["percent"] = "%", ["damage"] = "", ["health"] = "hp", ["armor_class"] = "AP", ["force"] = "", ["rounds"] = "rnd",
        ["projectiles"] = "", ["count"] = "", ["throwables"] = "", ["magazines"] = "mag", ["uses"] = "uses", ["record_units"] = "",
        ["native_angular_velocity"] = "", ["projectiles_per_shot"] = "/shot", ["pellets"] = "", ["strength"] = "", ["targets"] = "",
        ["heat_units"] = "heat", ["value"] = "", ["heatsinks"] = "", ["shots"] = "", ["charge"] = "", ["exponent"] = "", ["pattern"] = "", ["ammo"] = ""
    };

    public static string Unit(string? unit) => unit == null ? "" :
        Units.TryGetValue(unit, out var known) ? known : unit.Replace("_per_", "/").Replace('_', ' ');

    // StratagemDefinition uses: UINT32_MAX means unlimited and is entered as -1.
    public static bool Unlimited(StatsField field) => field.Kind == "StratagemDefinition" && field.Offset == 80 && field.Storage == "u32" && field.LiveId == null;

    public static string Format(StatsField field, uint bits) => field.Storage switch
    {
        "f32" => BitConverter.UInt32BitsToSingle(bits) is var f && f == 0 ? "0" : f.ToString(CultureInfo.InvariantCulture),
        "i32" => unchecked((int)bits).ToString(CultureInfo.InvariantCulture),
        _ when Unlimited(field) && bits == uint.MaxValue => "-1",
        _ => bits.ToString(CultureInfo.InvariantCulture)
    };

    public static string Format(StatsField field, JsonNode node) =>
        TryParse(field, node.ToJsonString(), out _, out var bits, out _) ? Format(field, bits) : node.ToJsonString();

    // Typed input as the stats file stores it, and its four-byte value. A comma decimal separator is accepted.
    public static bool TryParse(StatsField field, string text, out JsonNode? value, out uint bits, out string? error)
    {
        value = null; bits = 0; error = null;
        text = text.Trim();
        if (!text.Contains('.') && text.Count(c => c == ',') == 1) text = text.Replace(',', '.');
        if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) || !double.IsFinite(number))
        { error = "Enter a number."; return false; }
        if (Unlimited(field) && number == -1) { value = JsonValue.Create(-1); bits = uint.MaxValue; return true; }
        if (field.Storage != "f32" && number != Math.Floor(number)) { error = "Whole numbers only."; return false; }
        if (number < field.Min || number > field.Max) { error = $"Allowed range: {Convert.ToString(field.Min, CultureInfo.InvariantCulture)} to {Convert.ToString(field.Max, CultureInfo.InvariantCulture)}."; return false; }
        switch (field.Storage)
        {
            case "f32" when float.IsFinite((float)number):
                bits = BitConverter.SingleToUInt32Bits((float)number); value = JsonValue.Create(number); return true;
            case "u32" when number is >= 0 and <= uint.MaxValue:
                bits = (uint)number; value = JsonValue.Create((long)number); return true;
            case "i32" when number is >= int.MinValue and <= int.MaxValue:
                bits = unchecked((uint)(int)number); value = JsonValue.Create((long)number); return true;
            case "u8" when number is >= 0 and <= byte.MaxValue:
                bits = (uint)number; value = JsonValue.Create((long)number); return true;
        }
        error = field.Storage is "u32" or "u8" && number < 0 ? "Must not be negative." : "Number is too large."; return false;
    }

    // A step for the arrow keys: 1 for whole numbers, one significant digit below the value's for decimals.
    public static double Step(StatsField field, double value) => field.Storage != "f32" ? 1 :
        value == 0 ? 0.1 : Math.Pow(10, Math.Floor(Math.Log10(Math.Abs(value))) - 1);

    // ---- Shared records ----

    // Fields that write the same bytes share a link key: a shared record seen from several items, or an alias.
    public static string LinkKey(StatsField field, bool own = false) =>
        StatsData.ConflictKey(new StatsEdit("", field.Kind, field.Component, Convert.ToUInt64(field.Resource[2..], 16), field.Record,
            field.Offset, field.Storage, 0, null, own && field.Kind == "component", field.Shared, field.Unverified));

    static readonly Lazy<Dictionary<string, List<(string Category, string Target, string Key)>>> Links = new(() =>
    {
        var links = new Dictionary<string, List<(string, string, string)>>(StringComparer.Ordinal);
        foreach (var (category, targets) in StatsSchema.Current.Targets)
            foreach (var (name, target) in targets)
                foreach (var (key, field) in target.Fields)
                {
                    var link = LinkKey(field);
                    if (!links.TryGetValue(link, out var list)) links[link] = list = [];
                    list.Add((category, name, key));
                }
        return links;
    });

    public static IReadOnlyList<(string Category, string Target, string Key)> Linked(StatsField field) =>
        Links.Value.TryGetValue(LinkKey(field), out var list) ? list : [];

    // The other items a change to this row reaches. A projectile or damage type's own entry is the record itself. Weapons
    // that reach it only by equipping the same ammunition are AlsoIfEquipped's, not these.
    public static IReadOnlyList<string> AlsoChanges(StatsRow row, bool own)
    {
        if (own && row.Field.Kind == "component") return [];
        var optional = AlsoIfEquipped(row).Names;
        var names = Linked(row.Field).Where(l => (l.Category, l.Target) != (row.Part.Category, row.Part.Target) &&
                (row.Part.Category is "projectiles" or "damage" || l.Category is not ("projectiles" or "damage")))
            .Select(l => Find(l.Category, l.Target)?.Name ?? l.Target).Concat(row.Field.SharedConsumers.Where(c => c != row.Part.Target));
        var self = Find(row.Part.Category, row.Part.Target)?.Name;
        return names.Where(n => n != self && !optional.Contains(n, StringComparer.OrdinalIgnoreCase))
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToList();
    }

    // A weapon's default ammunition's projectile and damage rows that other weapons reach only when they equip that
    // ammunition (it is in their unlock list, not their default: the P-2 Peacemaker's 9x20mm HOLLOW POINT, which the Knight
    // and Redeemer can equip; references.json ammunition optionalFor). The ammunition and those weapons, or none.
    public static (string? Item, IReadOnlyList<string> Names) AlsoIfEquipped(StatsRow row) =>
        row.Field.Kind is "ProjectileSettings" or "DamageInfo" && OptionalAmmunition.Value.TryGetValue((row.Field.Kind, row.Field.Record), out var a)
            ? (a.Item, a.Weapons.Select(w => Find("weapons", w)?.Name ?? w).Where(n => n != Find(row.Part.Category, row.Part.Target)?.Name).ToList())
            : (null, []);

    static readonly Lazy<Dictionary<(string Kind, long Record), (string Item, string[] Weapons)>> OptionalAmmunition = new(() =>
    {
        using var stream = typeof(StatsCatalog).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        var map = new Dictionary<(string, long), (string, string[])>();
        foreach (var a in JsonNode.Parse(stream)!["ammunition"]!.AsArray())
        {
            var weapons = a!["optionalFor"]?.AsArray().Select(w => w!.GetValue<string>()).ToArray() ?? [];
            if (weapons.Length == 0) continue;
            map[("ProjectileSettings", a["projectile"]!.GetValue<long>())] = (a["item"]!.GetValue<string>(), weapons);
            map[("DamageInfo", a["damage"]!.GetValue<long>())] = (a["item"]!.GetValue<string>(), weapons);
        }
        return map;
    });

    // ---- Search ----

    // Every word must match, in any order. Words of three or more letters also match field labels.
    public static (bool Item, IReadOnlySet<string> Fields) Match(StatsItem item, IReadOnlyList<string> words, Func<StatsItem, IEnumerable<StatsRow>> rows)
    {
        if (words.Count == 0 || words.All(w => item.Search.Contains(w, StringComparison.Ordinal))) return (true, new HashSet<string>());
        if (words.Any(w => w.Length < 3 && !item.Search.Contains(w, StringComparison.Ordinal))) return (false, new HashSet<string>());
        var fields = rows(item).Where(r =>
        {
            var text = (r.Label + " " + r.Key).ToLowerInvariant();
            return words.All(w => text.Contains(w, StringComparison.Ordinal) || item.Search.Contains(w, StringComparison.Ordinal)) &&
                   words.Any(w => text.Contains(w, StringComparison.Ordinal));
        }).Select(r => r.Id).ToHashSet();
        return (false, fields);
    }

    public static IReadOnlyList<string> SearchWords(string query) =>
        query.ToLowerInvariant().Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
