using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Security.Cryptography;

namespace HellForge.Core;

public sealed record StatsField(string Name, string Kind, string Component, string Resource, long Record,
    int Offset, string Storage, double? Min, double? Max, string? Unit, bool Shared, bool Unverified)
{
    public IReadOnlyList<string> SharedConsumers { get; init; } = [];
    // The editor's section for it, where the target's page has its own (a booster's: Tuning, Explosion...).
    public string? Section { get; init; }
    // The table row set_live names when it is not the record (a booster's granted stratagem: its StratagemType).
    public ulong? LiveId { get; init; }
    // The catalog's field order, for the editor.
    public int Order { get; init; }
    // Its named values, where it takes only those (a flag's No / Yes, an enum's members, a trait or fire mode slot's):
    // [[value, label], ...] (scripts/ExportStatsSchema.py expand()).
    public JsonElement Options { get; init; }
    public IEnumerable<(double Value, string Label)> Choices => Options.ValueKind == JsonValueKind.Array
        ? Options.EnumerateArray().Select(o => (o[0].GetDouble(), o[1].GetString() ?? "")) : [];
    // What the editor says about it under the page (when it applies, what a sentinel value means), or null.
    public string? Note { get; init; }
    // The HD2R Editor's preset key for it, where the mapping names one itself (the Spawning pages: "ed|composition/...").
    [System.Text.Json.Serialization.JsonIgnore] public string? Hd2rKey { get; init; }
    // The bytes as HD2R Editor's preset keys name them (mappings/hd2r-keys.json, scripts/ExportHd2rKeys.py).
    public string Hd2rAddress => Kind switch
    {
        "component" => $"component|{Component}|{Record.ToString(CultureInfo.InvariantCulture)}|{Offset.ToString(CultureInfo.InvariantCulture)}",
        "EntityDelta" => $"EntityDelta|{Component}|{Offset.ToString(CultureInfo.InvariantCulture)}",
        _ => $"{Kind}|{Record.ToString(CultureInfo.InvariantCulture)}|{Offset.ToString(CultureInfo.InvariantCulture)}",
    };
    public string AddressKey => $"{Kind}\t{(Component.Length == 0 ? "-" : Component)}\t{Convert.ToUInt64(Resource[2..], 16):X16}\t{Record.ToString(CultureInfo.InvariantCulture)}\t{Offset.ToString(CultureInfo.InvariantCulture)}\t{Storage}";
}
// Slot (primary, secondary, underbarrel, support, mounted, throwable), Class (the armory's weapon class), Host (the
// item an underbarrel or mounted weapon belongs to), Mount and stratagem Family place a target in the editor.
public sealed record StatsTarget(Dictionary<string, StatsField> Fields, Dictionary<string, string> Blocked,
    Dictionary<string, string> Aliases)
{
    public string? Slot { get; init; }
    public string? Class { get; init; }
    public string? Host { get; init; }
    public string? Mount { get; init; }
    public string? Family { get; init; }
    // The name the editor shows, where the target's key is not one (an entity's: "Bile Titan - Strider").
    [System.Text.Json.Serialization.JsonIgnore] public string? Label { get; init; }
    // An entity's faction (mappings/entities.json: terminids, automatons, illuminate, super_earth, helldivers or
    // unresolved), whatever page it is on: the editor draws its emblem on structures and other entities too.
    [System.Text.Json.Serialization.JsonIgnore] public string? Faction { get; init; }
    // Fields it no longer offers because they never reached the game (a fired entity's unused projectile row): a stats
    // file naming one still loads, and that edit is skipped.
    [System.Text.Json.Serialization.JsonIgnore] public Dictionary<string, string> Retired { get; } = new(StringComparer.Ordinal);
}
public sealed record StatsSchema(int Format, string Build, string GameDllSha256, Dictionary<string, Dictionary<string, StatsTarget>> Targets)
{
    public Dictionary<string, Dictionary<string, string>> TargetAliases { get; init; } = [];
    // Where each weapon's projectile swap is written, by its edit's site (SwapSite): the weapons whose shots it changes
    // (an ammunition every weapon equipping it shares: several) and how the loader finds that weapon's round in co-op
    // (native/mod-loader/shots.c): "" its ProjectileWeapon +0, "/r" its WeaponRounds +64, "/f" its programmable round
    // (ProjectileWeapon +0x240, while its function is on), "/<type>" the round's stock type (an ammunition's, a magazine
    // pattern entry's, a charge or heat level's).
    [System.Text.Json.Serialization.JsonIgnore] public Dictionary<string, List<(ulong Weapon, string Native)>> ProjectileSwaps { get; } = new(StringComparer.Ordinal);
    void AddSwap(StatsField site, string weapon, string native)
    {
        var key = SwapSite(site.Kind, site.Component, Convert.ToUInt64(site.Resource[2..], 16), site.Offset);
        if (!ProjectileSwaps.TryGetValue(key, out var list)) ProjectileSwaps[key] = list = [];
        var entry = (Convert.ToUInt64(weapon[2..], 16), native);
        if (!list.Contains(entry)) list.Add(entry);
    }
    public static string SwapSite(string kind, string component, ulong resource, int offset) =>
        $"{kind}:{component}:{resource:X16}:{offset.ToString(CultureInfo.InvariantCulture)}";
    // Every field the loader observes for the editor. A snapshot is pinned to this set, not to editor metadata.
    static readonly Lazy<string[]> ObservedKeys = new(() => Current.Targets.Values.SelectMany(t => t.Values).SelectMany(t => t.Fields.Values)
        .Select(f => f.AddressKey).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray());
    public static IReadOnlyList<string> Observed => ObservedKeys.Value;
    static readonly Lazy<string> Hash = new(() => Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(string.Join("\n", Observed)))));
    public static string Fingerprint => Hash.Value;
    static readonly Lazy<StatsSchema> CurrentSchema = new(() =>
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.stats-schema.json")!;
        var schema = JsonSerializer.Deserialize(stream, CoreJson.Default.StatsSchema)
            ?? throw new InvalidDataException("Stats schema is missing.");
        schema.Targets["boosters"] = Boosters();
        schema.Targets["perks"] = Perks();
        Vehicles(schema);
        Attachments(schema);
        Melee(schema);
        Parity(schema);
        References(schema);
        ProjectileSources(schema);
        FiredEntities(schema);
        Entities(schema);
        Director(schema);
        return schema;
    });

    // The HD2R Editor's Spawning pages (mappings/director.json, scripts/ExportDirector.py; native/mod-loader/director.c):
    // "difficulty" (a level's DifficultySettings row, 1 to 11), "rosters" (an enemy's spawn weights by difficulty) and
    // "compositions" (a squad: its groups' counts, weight, cost and the difficulties and player counts it is drawn at) and
    // "demolition" (a demolition template: the force a destruction stage naming it requires).
    // Family is a faction; Class a composition's role (patrol, map, reinforcement).
    static void Director(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.director.json")!;
        using var json = JsonDocument.Parse(stream);
        foreach (var category in new[] { "difficulty", "rosters", "compositions", "demolition" })
        {
            var targets = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
            foreach (var t in json.RootElement.GetProperty(category).EnumerateObject())
            {
                var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal);
                var order = 0;
                foreach (var f in t.Value.GetProperty("fields").EnumerateArray())
                    fields[f.GetProperty("key").GetString()!] = new StatsField(f.GetProperty("label").GetString()!, f.GetProperty("kind").GetString()!, "",
                        "0x0", f.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(), f.GetProperty("storage").GetString()!,
                        Number(f, "min"), Number(f, "max"), Text(f, "unit"), true, true)
                    { Order = ++order, Section = Text(f, "section"), Note = Text(f, "note"), Hd2rKey = Text(f, "hd2r") };
                targets[t.Name] = new StatsTarget(fields, new Dictionary<string, string>(StringComparer.Ordinal), new Dictionary<string, string>(StringComparer.Ordinal))
                {
                    Label = Text(t.Value, "label"), Family = Text(t.Value, "faction"), Class = Text(t.Value, "role"),
                };
            }
            schema.Targets[category] = targets;
        }
    }

    // The entities another page shows instead (StatsCatalog: the MS-11 Solo Silo's rack and missile), and the one whose
    // main fields join the Helldiver page; the enemies category lists the rest.
    public static readonly IReadOnlyDictionary<string, (string Item, string Title)> EntityParts = new Dictionary<string, (string, string)>(StringComparer.Ordinal)
    {
        ["weapon_rack_mini_missile_silo"] = ("MS-11 Solo Silo", "Rack"),
        ["mini_missile_silo"] = ("MS-11 Solo Silo", "Missile"),
    };
    const string HelldiverEntity = "avatar_helldiver";

    // The HD2R Editor's Enemies pages (mappings/entities.json, scripts/ExportEntities.py): every enemy, structure and other
    // entity with the fields HellForge's engine writes, keyed by the HD2R Editor's row ids ("entity::entity.health",
    // "damage_zone:zone_3:zone.armor", "attack:mapped_4:damage.stagger"); Family is its page (terminids, automatons,
    // illuminate, structures, other). The Helldiver's own entity adds its main fields (main health) to the Helldiver page.
    static void Entities(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.entities.json")!;
        using var json = JsonDocument.Parse(stream);
        var enemies = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
        foreach (var e in json.RootElement.GetProperty("entities").EnumerateObject())
        {
            var resource = e.Value.GetProperty("resource").GetString()!;
            var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal);
            var order = 0;
            foreach (var f in e.Value.GetProperty("fields").EnumerateArray())
            {
                var kind = f.GetProperty("kind").GetString()!;
                fields[f.GetProperty("key").GetString()!] = new StatsField(f.GetProperty("label").GetString()!, kind, Text(f, "component") ?? "",
                    kind == "component" ? resource : "0x0", f.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(),
                    f.GetProperty("storage").GetString()!, Number(f, "min"), Number(f, "max"), Text(f, "unit"), f.GetProperty("shared").GetBoolean(),
                    !f.GetProperty("verified").GetBoolean())
                { Order = ++order, Section = Text(f, "section"), Note = Text(f, "note") };
            }
            if (e.Name == HelldiverEntity && schema.Targets.TryGetValue("helldiver", out var helldivers) && helldivers.TryGetValue("Helldiver", out var helldiver))
            {
                var taken = helldiver.Fields.Values.Select(f => f.Hd2rAddress).ToHashSet(StringComparer.Ordinal);
                var next = helldiver.Fields.Values.Select(f => f.Order).DefaultIfEmpty(0).Max();
                foreach (var (key, field) in fields.Where(f => f.Key.StartsWith("entity::", StringComparison.Ordinal)))
                    if (taken.Add(field.Hd2rAddress)) helldiver.Fields[key["entity::".Length..]] = field with { Section = null, Order = ++next };
                continue;
            }
            enemies[e.Name] = new StatsTarget(fields, new Dictionary<string, string>(StringComparer.Ordinal), new Dictionary<string, string>(StringComparer.Ordinal))
            {
                Family = e.Value.GetProperty("category").GetString(),
                Label = e.Value.GetProperty("label").GetString(),
                Class = e.Value.GetProperty("kind").GetString(),
                Faction = Text(e.Value, "faction"),
            };
        }
        schema.Targets["enemies"] = enemies;
    }

    // The references (HD2R Editor's payload swaps; mappings/references.json), which the exported schema lists as blocked,
    // placed by each target's own records: a weapon's projectile (ProjectileWeapon +0, or for six weapons their default
    // ammunition's entity delta, which every weapon equipping it shares), its projectiles' impact and expiry explosions
    // (ProjectileSettings +0x90 / +0x9C) and the four status slots of each damage type it shows (DamageInfo +0x2C + 8(k-1),
    // the type, then its strength). The loader takes only values its reference catalogue knows.
    static void References(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.references.json")!;
        using var json = JsonDocument.Parse(stream);
        var hosts = json.RootElement.GetProperty("hosts");
        var programmable = json.RootElement.GetProperty("programmable");
        schema.Targets["statuses"] = Statuses(json.RootElement, schema);
        schema.Targets["explosions"] = Explosions(json.RootElement);
        var ammunition = json.RootElement.GetProperty("ammunition").EnumerateArray().ToDictionary(a => a.GetProperty("weapon").GetString()!, a => a.Clone());
        foreach (var (category, targets) in schema.Targets)
            foreach (var (name, target) in targets)
            {
                var fields = target.Fields;
                void Add(string key, StatsField field)
                {
                    target.Blocked.Remove(key);
                    if (!fields.ContainsKey(key)) fields[key] = field;
                }
                // The projectile it fires. An underbarrel of rounds (the One-Two's and the Arbitrator's: ProjectileWeapon and
                // WeaponRounds, no magazine or heat) fires WeaponRounds +64: the type resolver (game.dll 0x7456D0) takes
                // +0x40 for a weapon whose flags have bit 8, as a live SG-8 Punisher's do (decoded snapshot of this build).
                var underbarrel = category == "weapons" && target.Slot == "underbarrel" &&
                    fields.Values.Any(f => f.Component == "WeaponRoundsComponentData") && !fields.Values.Any(f => f.Component is "WeaponMagazineComponentData" or "WeaponHeatComponentData");
                var mechanism = category != "weapons" ? null : hosts.TryGetProperty(name, out var host) ? host.GetProperty("mechanism").GetString() : underbarrel ? "rounds" : null;
                if (mechanism != null)
                {
                    var weapon = fields.Values.Where(f => f.Kind == "component" && f.Component == "ProjectileWeaponComponentData").OrderBy(f => f.Order).FirstOrDefault();
                    var order = fields.Values.Where(f => f.Kind == "ProjectileSettings").Select(f => f.Order).DefaultIfEmpty(0).Min() - 1;
                    string? swap = null, native = null;
                    if (mechanism == "ammunition" && ammunition.TryGetValue(name, out var a))
                    {
                        Add(swap = "ammunition.projectile", new StatsField("Projectile", "EntityDelta", $"{a.GetProperty("component").GetInt32()}:0",
                            a.GetProperty("resource").GetString()!, a.GetProperty("record").GetInt64(), a.GetProperty("offset").GetInt32(), "u32",
                            null, null, null, true, true) { Order = order });
                        native = "/" + a.GetProperty("projectile").GetInt32().ToString(CultureInfo.InvariantCulture);
                    }
                    else if (mechanism == "rounds" && fields.Values.Where(f => f.Kind == "component" && f.Component == "WeaponRoundsComponentData")
                                 .OrderBy(f => f.Order).FirstOrDefault() is { } rounds)
                    {
                        // by the name the exported schema blocked it under (a rounds feed's, or the weapon's primary attack)
                        swap = target.Blocked.ContainsKey("attack.primary.projectile") && !target.Blocked.ContainsKey("attack.feed_primary.projectile")
                            ? "attack.primary.projectile" : "attack.feed_primary.projectile";
                        Add(swap, new StatsField("Projectile", "component", rounds.Component, rounds.Resource, rounds.Record, 64, "u32",
                            null, null, null, rounds.Shared, true) { Order = order });
                        native = "/r";
                    }
                    else if (mechanism == "component" && weapon != null)
                    {
                        Add(swap = "attack.primary.projectile", new StatsField("Projectile", "component", weapon.Component, weapon.Resource, weapon.Record, 0, "u32",
                            null, null, null, weapon.Shared, true) { Order = order });
                        native = "";
                    }
                    // the weapon firing it, by its own resource (the shot's source); an ammunition's swap counts for the
                    // weapon it belongs to
                    if (swap != null && native != null && fields[swap] is var site && (weapon?.Resource ?? (native == "/r" ? site.Resource : null)) is { } fires)
                        schema.AddSwap(site, fires, native);
                }
                // The projectile its programmable ammunition fires (ProjectileWeapon +576), where it has one of its own.
                if (category == "weapons" && programmable.TryGetProperty(name, out _) &&
                    fields.Values.Where(f => f.Kind == "component" && f.Component == "ProjectileWeaponComponentData").OrderBy(f => f.Order).FirstOrDefault() is { } fed)
                {
                    Add("function_ammo.projectile", new StatsField("Programmable ammo projectile", "component", fed.Component, fed.Resource, fed.Record, 576, "u32",
                        null, null, null, fed.Shared, true) { Order = fields.Values.Where(f => f.Kind == "ProjectileSettings").Select(f => f.Order).DefaultIfEmpty(0).Min() - 1 });
                    var site = fields["function_ammo.projectile"];
                    schema.AddSwap(site, fed.Resource, "/f");
                }
                // Projectiles and damage types by id are their own rows.
                if (category == "projectiles" && long.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out var type))
                {
                    var order = fields.Values.Select(f => f.Order).DefaultIfEmpty(0).Max();
                    Add("terminal.impact.explosion", new StatsField("Impact payload", "ProjectileSettings", "", "0x0", type, 144, "u32", null, null, null, true, true) { Order = order + 1 });
                    Add("terminal.expiry.explosion", new StatsField("Expiry payload", "ProjectileSettings", "", "0x0", type, 156, "u32", null, null, null, true, true) { Order = order + 2 });
                }
                var slots = new HashSet<string>(StringComparer.Ordinal);
                if (category == "damage") slots.Add("");
                // a mounted weapon shows all four slots of every damage type it deals (HD2R Editor's vehicle weapons), and
                // so does an underbarrel (its damage rows come from mappings/parity.json, with no blocked slots to follow)
                if (category == "weapons" && target.Slot is "mounted" or "underbarrel")
                    foreach (var k in fields.Where(f => f.Value.Kind == "DamageInfo").Select(f => f.Key))
                        slots.Add(k.Contains('.') ? k[..(k.LastIndexOf('.') + 1)] : "");
                foreach (var key in target.Blocked.Keys.ToList())
                {
                    var terminal = Regex.Match(key, @"^terminal\.(\w+)\.(impact|expiry)\.explosion$");
                    if (terminal.Success)
                    {
                        var role = terminal.Groups[1].Value;
                        string[] prefixes = role == "primary" ? ["projectile.", "projectile.primary."] : ["projectile." + role + "."];
                        var round = fields.Where(f => f.Value.Kind == "ProjectileSettings" && prefixes.Any(p => f.Key.StartsWith(p, StringComparison.Ordinal) &&
                            !f.Key[p.Length..].Contains('.'))).Select(f => f.Value).OrderBy(f => f.Order).FirstOrDefault();
                        if (round == null) continue;
                        var impact = terminal.Groups[2].Value == "impact";
                        Add(key, new StatsField(impact ? "Impact payload" : "Expiry payload", "ProjectileSettings", "", "0x0", round.Record, impact ? 144 : 156, "u32",
                            null, null, null, true, true) { Order = -100 });
                        continue;
                    }
                    var status = Regex.Match(key, @"^(?:(.+)\.)?status_[1-4]_type$");
                    if (status.Success) slots.Add(status.Groups[1].Success ? status.Groups[1].Value + "." : "");
                }
                // A damage type's four status slots, after its other fields: type, then strength.
                foreach (var prefix in slots)
                {
                    var own = fields.Where(f => f.Value.Kind == "DamageInfo" && f.Key.StartsWith(prefix, StringComparison.Ordinal) &&
                        !f.Key[prefix.Length..].Contains('.')).ToList();
                    if (own.Count == 0) continue;
                    var record = own[0].Value.Record;
                    var after = own.Where(f => !f.Key[prefix.Length..].StartsWith("status_", StringComparison.Ordinal)).Select(f => f.Value.Order).DefaultIfEmpty(0).Max();
                    for (var k = 1; k <= 4; k++)
                    {
                        Add($"{prefix}status_{k}_type", new StatsField($"Status {k}", "DamageInfo", "", "0x0", record, 44 + 8 * (k - 1), "u32",
                            null, null, null, true, true) { Order = after + 2 * k - 1 });
                        var strength = $"{prefix}status_{k}_strength";
                        fields[strength] = fields.TryGetValue(strength, out var kept) ? kept with { Name = $"Status {k} strength", Order = after + 2 * k } :
                            new StatsField($"Status {k} strength", "DamageInfo", "", "0x0", record, 48 + 8 * (k - 1), "f32", 0, 1000, "factor", true, true) { Order = after + 2 * k };
                        target.Blocked.Remove(strength);
                    }
                }
            }
    }

    // The rounds of weapons the references leave without a swap, where they really come from (mappings/projectile-sources.json,
    // scripts/ExportProjectileSources.py from research/blocked-projectile-swaps): a rounds feed's WeaponRounds +64; the
    // ProjectileWeapon +0 that is the round after all (the Maelstrom gun, the Maxigun); an ammunition option's entity delta
    // (the SG-8's and SG-20's WeaponRounds +64 / +68, the Knight's and SMG-37's ProjectileWeapon +0); a magazine pattern's
    // entries (MG-43, MGX-42, the sentries); a charge level's or heat level's projectile. Each row keeps the round's stock
    // type, by which co-op recognises it ("/<type>").
    static void ProjectileSources(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.projectile-sources.json")!;
        using var json = JsonDocument.Parse(stream);
        var weapons = schema.Targets["weapons"];
        foreach (var w in json.RootElement.GetProperty("weapons").EnumerateObject())
        {
            var target = weapons.TryGetValue(w.Name, out var t) ? t : throw new InvalidDataException($"projectile-sources: no weapon {w.Name}.");
            var resource = w.Value.GetProperty("resource").GetString()!;
            var order = target.Fields.Values.Where(f => f.Kind == "ProjectileSettings").Select(f => f.Order).DefaultIfEmpty(0).Min() - 100;
            foreach (var r in w.Value.GetProperty("rows").EnumerateArray())
            {
                var key = r.GetProperty("key").GetString()!;
                target.Blocked.Remove(key);
                var shared = r.TryGetProperty("shared", out var s) ? s.EnumerateArray().Select(x => x.GetString()!).ToArray() : [];
                var kind = r.GetProperty("kind").GetString()!;
                var field = new StatsField(r.GetProperty("label").GetString()!, kind, r.GetProperty("component").GetString()!, r.GetProperty("resource").GetString()!,
                    r.GetProperty("record").GetInt64(), r.GetProperty("offset").GetInt32(), "u32", null, null, null, kind != "component" || shared.Length > 0, true)
                    { Order = ++order, Section = Text(r, "section"), Note = Text(r, "note"), SharedConsumers = shared };
                if (!target.Fields.ContainsKey(key)) target.Fields[key] = field;
                var coop = r.GetProperty("coop").GetString()!;
                var stock = r.GetProperty("stock").GetInt64().ToString(CultureInfo.InvariantCulture);
                schema.AddSwap(target.Fields[key], resource, coop switch { "" => "", "r" => "/r", "f" => "/f", _ => "/" + stock });
            }
        }
    }

    // Weapons whose shot spawns an entity instead of firing a projectile row (mappings/fired-entities.json,
    // research/wasp-rocket). Where the entity explodes by its own ExplosiveComponent (the P-34 Breacher's sticky charge,
    // the TD-110 Maelstrom's rockets), the row (ProjectileWeapon +0), its damage and its explosions never reach the
    // game: they are retired, and the entity's fuse and detonation (its explosion's and damage's rows) offered instead.
    // Where a missile carries the row as its hit row, only the row's flight is retired: the missile is unit-driven and
    // flies by its SeekingMissile record, offered as missile.* (the Spear's fields; on these missiles guidance is never
    // switched on by time, as on the Spear) where the catalogue lacks them.
    static readonly int[] FlightOffsets = [28, 32, 36, 40, 44, 64]; // pellets, velocity, mass, drag, gravity, penetration slowdown
    static void FiredEntities(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.fired-entities.json")!;
        using var json = JsonDocument.Parse(stream);
        var weapons = schema.Targets["weapons"];
        var spear = weapons["FAF-14 Spear"].Fields.Where(f => f.Key.StartsWith("missile.", StringComparison.Ordinal)).OrderBy(f => f.Value.Order).ToList();
        static long? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) ? v.GetInt64() : null;
        foreach (var w in json.RootElement.GetProperty("weapons").EnumerateObject())
        {
            var target = weapons.TryGetValue(w.Name, out var t) ? t : throw new InvalidDataException($"fired-entities: no weapon {w.Name}.");
            var v = w.Value;
            var entity = v.TryGetProperty("entity", out var e) ? e.GetString()! : null;
            void Retire(Func<KeyValuePair<string, StatsField>, bool> which, string reason)
            {
                foreach (var key in target.Fields.Where(which).Select(f => f.Key).ToList()) { target.Fields.Remove(key); target.Retired[key] = reason; }
            }
            if (Number(v, "unfired") is { } unfired)
            {
                var section = v.GetProperty("section").GetString()!;
                var reason = $"The shot spawns an entity that explodes by itself; projectile {unfired} is never fired. Edit its {section.ToLowerInvariant()} and detonation instead.";
                static bool Round(string key) => key.Split('.')[0] is "projectile" or "damage" or "explosion" or "terminal";
                Retire(f => Round(f.Key), reason);
                foreach (var key in target.Blocked.Keys.Where(Round).ToList()) { target.Blocked.Remove(key); target.Retired[key] = reason; }
                var explosive = v.GetProperty("explosive").GetInt64();
                var order = 0;
                foreach (var (key, name, offset, storage, min, max, unit) in new[]
                         {
                             ("fired.arming_delay", "Arming delay", 8, "f32", (double?)0, (double?)120, "seconds"),
                             ("fired.explosion_delay", "Fuse (explosion delay)", 12, "f32", 0, 120, "seconds"),
                             ("fired.explosion", "Explosion payload", (int)(Number(v, "payload") ?? -1), "u32", null, null, null),
                         })
                    if (offset >= 0)
                        target.Fields[key] = new StatsField(name, "component", "ExplosiveComponentData", entity!, explosive, offset, storage, min, max, unit, false, true)
                            { Order = order++, Section = section };
                // the detonation's own rows, as its Explosions page has them
                var detonation = v.GetProperty("detonation").GetInt64();
                var page = schema.Targets["explosions"].Values.FirstOrDefault(p => p.Fields.Values.Any(f => f.Kind == "ExplosionSettings" && f.Record == detonation))
                    ?? throw new InvalidDataException($"fired-entities: explosion {detonation} of {w.Name} is not in the reference catalogue.");
                foreach (var (key, field) in page.Fields.OrderBy(f => f.Value.Order))
                    target.Fields["explosion.detonation." + key["explosion.".Length..]] = field with { Order = order++ };
            }
            if (Number(v, "carried") is { } carried)
                Retire(f => f.Value.Kind == "ProjectileSettings" && f.Value.Record == carried && FlightOffsets.Contains(f.Value.Offset),
                    $"The shot is a missile: it flies by its own record (the missile fields), so projectile {carried}'s flight does nothing. Its damage and impact explosion still apply.");
            if (Number(v, "missile") is { } missile && !target.Fields.Keys.Any(k => k.StartsWith("missile.", StringComparison.Ordinal)))
                foreach (var (key, field) in spear)
                    target.Fields[key] = field with { Resource = entity!, Record = missile, Shared = false, SharedConsumers = [] };
        }
    }

    // The boosters (mappings/boosters.json, HD2R Editor's Boosters category): one target each, every field HD2Runtime
    // reviewed, in its section: the tuning value (BoosterSettings +0x08, the booster's row), and the explosions, status
    // effects and their damage, granted stratagem and deployed entity some act through. Keys are HD2Runtime's field ids.
    static readonly string[] BoosterSections = ["Granted Stratagem", "Tuning", "Explosion", "Status Effect", "Status Damage", "Deployed Entity"];
    static Dictionary<string, StatsTarget> Boosters()
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.boosters.json")!;
        using var json = JsonDocument.Parse(stream);
        var targets = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        static string? Text(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        foreach (var b in json.RootElement.GetProperty("boosters").EnumerateObject())
        {
            var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal);
            var order = 0;
            foreach (var f in b.Value.GetProperty("fields").EnumerateObject().OrderBy(f => Array.IndexOf(BoosterSections, Text(f.Value, "section"))))
            {
                var v = f.Value;
                var stratagemType = Number(v, "stratagemType");
                fields[f.Name] = new StatsField(v.GetProperty("label").GetString()!, v.GetProperty("kind").GetString()!, Text(v, "component") ?? "",
                    Text(v, "resource") ?? "0x0", v.GetProperty("record").GetInt64(), v.GetProperty("offset").GetInt32(), v.GetProperty("storage").GetString()!,
                    Number(v, "min"), Number(v, "max"), Text(v, "unit"), v.GetProperty("shared").GetBoolean(), true)
                {
                    Order = order++, Section = Text(v, "section"), LiveId = stratagemType is { } t ? (ulong)t : null,
                };
            }
            targets[b.Name] = new StatsTarget(fields, [], []) { Family = "booster" };
        }
        return targets;
    }
    // The armor perks' values (mappings/armor-perks.json, HD2R Editor's Armor perks): one target each, named by the
    // passive's title (mappings/passives.json), its bonuses (PassiveModifier) and its stats applied at spawn
    // (PassiveStat), rows named passive * 16 + entry. Shared by every wearer of the perk.
    public static readonly Lazy<Dictionary<int, string>> PerkTitles = new(() =>
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.passives.json")!;
        using var json = JsonDocument.Parse(stream);
        return json.RootElement.GetProperty("passives").EnumerateArray().ToDictionary(p => p.GetProperty("id").GetInt32(), p => p.GetProperty("title").GetString()!);
    });
    static Dictionary<string, StatsTarget> Perks()
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.armor-perks.json")!;
        using var json = JsonDocument.Parse(stream);
        var targets = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        foreach (var p in json.RootElement.GetProperty("perks").EnumerateArray())
        {
            var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal);
            var order = 0;
            foreach (var f in p.GetProperty("fields").EnumerateObject())
            {
                var v = f.Value;
                fields[f.Name] = new StatsField(v.GetProperty("label").GetString()!, v.GetProperty("kind").GetString()!, "", "0x0",
                    v.GetProperty("record").GetInt64(), v.GetProperty("offset").GetInt32(), v.GetProperty("storage").GetString()!,
                    Number(v, "min"), Number(v, "max"), v.GetProperty("unit").GetString(), true, true)
                { Order = order++, Section = v.GetProperty("section").GetString() };
            }
            var id = p.GetProperty("id").GetInt32();
            targets[PerkTitles.Value.GetValueOrDefault(id, p.GetProperty("name").GetString()!)] =
                new StatsTarget(fields, [], []) { Family = "perk", Class = p.GetProperty("description").GetString(), Mount = id.ToString(CultureInfo.InvariantCulture) };
        }
        return targets;
    }

    // The vehicles' pages as HD2R Editor lays them out (mappings/vehicles.json): their damage zones ("zone_N.<field>", in
    // the zone's section) and the exosuits' rotation, on bytes the exported schema lacks; a mount that is a shield (the
    // EXO-55's left arm) is marked so its page part reads "Shield".
    static void Vehicles(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.vehicles.json")!;
        using var json = JsonDocument.Parse(stream);
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        foreach (var v in json.RootElement.GetProperty("vehicles").EnumerateObject())
        {
            foreach (var shield in v.Value.GetProperty("shields").EnumerateArray().Select(s => s.GetString()!))
                if (schema.Targets["weapons"].TryGetValue(shield, out var arm)) schema.Targets["weapons"][shield] = arm with { Class = "Shield" };
            if (!schema.Targets["vehicles"].TryGetValue(v.Name, out var target)) continue;
            var taken = target.Fields.Values.Select(f => f.Hd2rAddress).ToHashSet(StringComparer.Ordinal);
            var order = target.Fields.Values.Select(f => f.Order).DefaultIfEmpty(0).Max();
            foreach (var f in v.Value.GetProperty("fields").EnumerateArray())
            {
                var field = new StatsField(f.GetProperty("label").GetString()!, "component", f.GetProperty("component").GetString()!,
                    f.GetProperty("resource").GetString()!, f.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(),
                    f.GetProperty("storage").GetString()!, Number(f, "min"), Number(f, "max"), null, false, true)
                { Order = ++order, Section = f.GetProperty("section").GetString(), Note = Text(f, "note") };
                var key = f.GetProperty("key").GetString()!;
                target.Blocked.Remove(key);
                if (taken.Add(field.Hd2rAddress) && !target.Fields.ContainsKey(key)) target.Fields[key] = field;
                else if (target.Fields.TryGetValue(key, out var kept)) target.Fields[key] = kept with { Section = field.Section, Note = kept.Note ?? field.Note };
            }
        }
    }

    // The fields HellForge adds where HD2Runtime's catalogues stop short (mappings/parity.json, scripts/ExportFieldParity.py):
    // a like item's fields on this item's own records and rows: an underbarrel's projectile and damage, a vehicle weapon's
    // sway and recoil, the Purifier's and Loyalist's charge, the K-9's rate of fire, and the explosion damage shares of
    // everything with a health record. A field whose bytes the target shows already is left out.
    static void Parity(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.parity.json")!;
        using var json = JsonDocument.Parse(stream);
        foreach (var category in json.RootElement.GetProperty("targets").EnumerateObject())
            foreach (var t in category.Value.EnumerateObject())
            {
                if (!schema.Targets.TryGetValue(category.Name, out var targets) || !targets.TryGetValue(t.Name, out var target)) continue;
                var taken = target.Fields.Values.Select(f => f.Hd2rAddress).ToHashSet(StringComparer.Ordinal);
                var order = target.Fields.Values.Select(f => f.Order).DefaultIfEmpty(0).Max();
                foreach (var f in t.Value.EnumerateArray())
                {
                    var field = new StatsField(f.GetProperty("label").GetString()!, f.GetProperty("kind").GetString()!, f.GetProperty("component").GetString()!,
                        f.GetProperty("resource").GetString()!, f.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(),
                        f.GetProperty("storage").GetString()!, Number(f, "min"), Number(f, "max"), Text(f, "unit"), f.GetProperty("shared").GetBoolean(), true)
                    {
                        Order = ++order, Section = Text(f, "section"), Note = Text(f, "note"),
                        Options = f.TryGetProperty("options", out var options) ? options.Clone() : default,
                    };
                    var key = f.GetProperty("key").GetString()!;
                    target.Blocked.Remove(key);
                    if (taken.Add(field.Hd2rAddress) && !target.Fields.ContainsKey(key)) target.Fields[key] = field;
                }
            }
    }

    static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
    static string? Text(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    // The weapons' attachments (mappings/attachments.json; HD2R Editor's Magazines, Muzzles, Optics and Underbarrels):
    // every attachment a weapon's slots offer, its stats as entity deltas ("attachment.<slot>.<id>.<field>"), shared by
    // every weapon that equips it and applied from the next boot. Bytes a weapon shows already (its default magazine's
    // magazine.* fields) stay where they are.
    static void Attachments(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.attachments.json")!;
        using var json = JsonDocument.Parse(stream);
        var all = json.RootElement.GetProperty("attachments");
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        static string? Text(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        foreach (var w in json.RootElement.GetProperty("weapons").EnumerateObject())
        {
            if (!schema.Targets["weapons"].TryGetValue(w.Name, out var target)) continue;
            var taken = target.Fields.Values.Select(f => f.Hd2rAddress).ToHashSet(StringComparer.Ordinal);
            var order = 10000;
            foreach (var slot in w.Value.EnumerateObject())
                foreach (var id in slot.Value.GetProperty("attachments").EnumerateArray().Select(a => a.GetString()!))
                {
                    var a = all.GetProperty(id);
                    var section = slot.Value.TryGetProperty("names", out var names) && names.TryGetProperty(id, out var shown) ? shown.GetString()! : a.GetProperty("name").GetString()!;
                    section = section.Split(". ")[^1];
                    // the default magazine is the weapon's own Ammunition: its fields sit with the magazine values it shows
                    if (slot.Name == "magazine" && slot.Value.TryGetProperty("default", out var standard) && standard.GetString() == id) section = "Ammunition"; // "Rifle 5,5x50mm. Extended Fastreload", "5,5mm. Compensator": the weapon's page names it by the last part
                    var slug = id.Split('/')[^2].Replace('-', '_');
                    foreach (var f in a.GetProperty("fields").EnumerateArray())
                    {
                        var field = new StatsField(f.GetProperty("label").GetString()!, "EntityDelta", f.GetProperty("component").GetString()!,
                            a.GetProperty("resource").GetString()!, a.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(),
                            f.GetProperty("storage").GetString()!, Number(f, "min"), Number(f, "max"), Text(f, "unit"), true, true)
                        { Order = order++, Section = section };
                        if (!taken.Add(field.Hd2rAddress)) continue;
                        // the field id past "attachment." (a magazine's "attachment.capacity"); a heatsink's own ids ("heat.cool_per_second")
                        // whole. Before 0.1.6 those lost their first 11 characters too, so two heat fields shared a key ("er_second",
                        // the last one kept): those keys stay aliases, so values saved under them still load.
                        var fieldId = f.GetProperty("id").GetString()!;
                        var key = $"attachment.{slot.Name}.{slug}.{(fieldId.StartsWith("attachment.", StringComparison.Ordinal) ? fieldId["attachment.".Length..] : fieldId)}";
                        target.Fields[key] = field;
                        if (!fieldId.StartsWith("attachment.", StringComparison.Ordinal) && fieldId.Length > "attachment.".Length)
                            target.Aliases[$"attachment.{slot.Name}.{slug}.{fieldId["attachment.".Length..]}"] = key;
                    }
                }
        }
    }

    // The explosions (HD2R Editor's explosion pages): one target per catalogued explosion, named by its label, with the
    // fields HD2R Editor shows: its ExplosionSettings row's radii and shrapnel, and its damage type's damage.
    static readonly Dictionary<string, string> ExplosionLabels = new(StringComparer.Ordinal)
    {
        ["explosion.inner_radius"] = "Inner radius", ["explosion.outer_radius"] = "Outer radius", ["explosion.shockwave_radius"] = "Shockwave radius",
        ["explosion.shrapnel_count"] = "Shrapnel count", ["explosion.damage.standard_damage"] = "Standard damage",
        ["explosion.damage.durable_damage"] = "Durable damage", ["explosion.damage.ap_direct"] = "Armor penetration: direct",
        ["explosion.damage.ap_slight"] = "Armor penetration: slight angle", ["explosion.damage.ap_large"] = "Armor penetration: large angle",
        ["explosion.damage.ap_extreme"] = "Armor penetration: extreme angle", ["explosion.damage.demolition"] = "Demolition",
        ["explosion.damage.stagger"] = "Stagger", ["explosion.damage.push_force"] = "Push force",
    };
    static Dictionary<string, StatsTarget> Explosions(JsonElement references)
    {
        var targets = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
        var defs = references.GetProperty("explosionFields").EnumerateArray().ToList();
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        foreach (var x in references.GetProperty("explosions").EnumerateArray())
        {
            var type = x.GetProperty("type").GetInt64();
            long? damage = x.TryGetProperty("damage", out var d) && d.ValueKind == JsonValueKind.Number ? d.GetInt64() : null;
            var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal);
            var order = 0;
            foreach (var f in defs)
            {
                var id = f.GetProperty("id").GetString()!;
                var onDamage = f.GetProperty("backing").GetString() == "damage";
                if (onDamage && damage == null) continue;
                fields[id] = new StatsField(ExplosionLabels.GetValueOrDefault(id, StatsCatalog.Humanize(id.Split('.')[^1])), onDamage ? "DamageInfo" : "ExplosionSettings",
                    "", "0x0", onDamage ? damage!.Value : type, f.GetProperty("offset").GetInt32(), f.GetProperty("storage").GetString()!,
                    Number(f, "min"), Number(f, "max"), f.TryGetProperty("unit", out var u) && u.ValueKind == JsonValueKind.String ? u.GetString() : null, true, true)
                { Order = order++ };
            }
            var name = x.GetProperty("label").GetString()!;
            if (targets.ContainsKey(name)) name += " (" + x.GetProperty("id").GetString() + ")";
            targets[name] = new StatsTarget(fields, [], []) { Family = x.GetProperty("family").GetString() };
        }
        return targets;
    }

    // A weapon's melee attack (mappings/melee.json; HD2R Editor's Melee rows): each hit volume its attack uses (a static
    // record in game.dll, MeleeHitVolume by RVA, shared by every weapon whose attack uses it) with the damage row it deals,
    // and a melee weapon's stamina cost. Keys as HD2R Editor's: "<hit volume>:<field>", "tuning:<field>".
    static void Melee(StatsSchema schema)
    {
        using var stream = typeof(StatsSchema).Assembly.GetManifestResourceStream("HellForge.Core.mappings.melee.json")!;
        using var json = JsonDocument.Parse(stream);
        static double? Number(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        static string? Text(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        var weapons = schema.Targets["weapons"];
        foreach (var w in json.RootElement.GetProperty("weapons").EnumerateObject())
        {
            if (!weapons.TryGetValue(w.Name, out var target)) continue;
            var taken = target.Fields.Values.Select(f => f.Hd2rAddress).ToHashSet(StringComparer.Ordinal);
            var order = 20000;
            void Add(string key, JsonElement f, string? section, bool shared)
            {
                var kind = f.GetProperty("kind").GetString()!;
                var field = new StatsField(f.GetProperty("label").GetString()!, kind, kind == "component" ? f.GetProperty("component").GetString()! : "",
                    kind == "component" ? f.GetProperty("resource").GetString()! : "0x0", f.GetProperty("record").GetInt64(), f.GetProperty("offset").GetInt32(),
                    f.GetProperty("storage").GetString()!, Number(f, "min"), Number(f, "max"), Text(f, "unit"), shared, true)
                { Order = order++, Section = section };
                if (taken.Add(field.Hd2rAddress) && !target.Fields.ContainsKey(key)) target.Fields[key] = field;
            }
            foreach (var f in w.Value.GetProperty("tuning").EnumerateArray()) Add("tuning:" + f.GetProperty("id").GetString(), f, "Melee", false);
            foreach (var zone in w.Value.GetProperty("zones").EnumerateArray())
            {
                var label = zone.GetProperty("label").GetString()!;
                var sharedBy = zone.GetProperty("consumers").GetInt32();
                foreach (var f in zone.GetProperty("fields").EnumerateArray())
                    Add(zone.GetProperty("key").GetString() + ":" + f.GetProperty("id").GetString(), f, sharedBy > 1 ? $"{label} (shared by {sharedBy} weapons)" : label, true);
            }
        }
    }

    // The status effects (mappings/references.json statuses): one target each, named by its label, its duration
    // (StatusEffectSettings +40, a row by the status type) and, for those that deal damage while active, that damage
    // type's fields (the damage page's, under "damage.").
    static Dictionary<string, StatsTarget> Statuses(JsonElement references, StatsSchema schema)
    {
        var targets = new Dictionary<string, StatsTarget>(StringComparer.Ordinal);
        var damage = schema.Targets["damage"];
        foreach (var s in references.GetProperty("statuses").EnumerateArray())
        {
            var type = s.GetProperty("type").GetInt64();
            var fields = new Dictionary<string, StatsField>(StringComparer.Ordinal)
            {
                ["status.duration"] = new("Duration", "StatusEffectSettings", "", "0x0", type, 40, "f32", 0, 100000, "seconds", true, true) { Order = 0 },
            };
            if (s.GetProperty("damage").ValueKind == JsonValueKind.Number &&
                damage.TryGetValue(s.GetProperty("damage").GetInt64().ToString(CultureInfo.InvariantCulture), out var tick))
                foreach (var (key, field) in tick.Fields.Where(f => !f.Key.StartsWith("status_", StringComparison.Ordinal)))
                    fields["damage." + key] = field with { Order = field.Order + 1 };
            targets[s.GetProperty("label").GetString()!] = new StatsTarget(fields, [], [])
            {
                Family = s.GetProperty("family").GetString(), Class = s.GetProperty("attachable").GetBoolean() ? "Applied by damage" : "Other",
            };
        }
        return targets;
    }
    public static StatsSchema Current => CurrentSchema.Value;
}

// Values in the plan are four-byte bit patterns, independent of the session's locale.
// A null Expect uses the exported record identity and layout as its guard. Raw/legacy edits require an expected value.
public sealed record StatsEdit(string Label, string Kind, string Component, ulong Resource, long Record, int Offset,
    string Storage, uint Value, uint? Expect, bool Own, bool Shared, bool Unverified)
{
    public string Key => $"{Kind}:{Component}:{(Own || Record < 0 ? $"entity-{Resource:X16}" : Record.ToString(CultureInfo.InvariantCulture))}:{Offset}";
}

public static class StatsData
{
    public const string FileName = "hellforge-stats.json";
    public const int MaxEdits = 8192;
    static readonly Lazy<Dictionary<string, long>> Records = new(() => StatsSchema.Current.Targets.Values.SelectMany(c => c.Values)
        .SelectMany(t => t.Fields.Values).Where(f => f.Kind == "component")
        .GroupBy(f => $"{f.Component}:{Resource(f.Resource):X16}")
        .Where(g => g.Select(f => f.Record).Distinct().Count() == 1).ToDictionary(g => g.Key, g => g.First().Record));
    public static string ConflictKey(StatsEdit edit) => edit.Record < 0 && !edit.Own && Records.Value.TryGetValue($"{edit.Component}:{edit.Resource:X16}", out var record)
        ? (edit with { Record = record }).Key : edit.Key;

    public static IReadOnlyList<StatsEdit> Load(string folder, EntityProfile profile, StatsSchema? schema = null)
    {
        var path = Path.Combine(folder, FileName);
        return File.Exists(path) ? Parse(File.ReadAllText(path), profile, schema) : [];
    }

    public static IReadOnlyList<StatsEdit> Parse(string json, EntityProfile profile, StatsSchema? schema = null)
    {
        schema ??= StatsSchema.Current;
        if (schema.Format != 1 || schema.Build != "25480438" || !schema.GameDllSha256.Equals(profile.GameDllSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Stats schema does not match the supported build.");
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        Object(root);
        if (!root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number || !format.TryGetInt32(out var version) || version != 1)
            throw new InvalidDataException("Stats format must be 1.");
        var edits = new List<StatsEdit>();
        var retired = 0;
        foreach (var section in root.EnumerateObject())
        {
            if (section.Name == "format") continue;
            if (section.Name == "raw")
            {
                if (section.Value.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Stats raw must be an array.");
                foreach (var raw in section.Value.EnumerateArray())
                {
                    Object(raw);
                    Allowed(raw, "component", "resource", "offset", "type", "value", "expect", "own");
                    var component = Text(raw, "component");
                    var resource = Resource(Text(raw, "resource"));
                    var offset = Offset(Required(raw, "offset"));
                    var storage = Text(raw, "type");
                    if (!profile.Components.TryGetValue(component, out var layout) || offset < 0 || offset % 4 != 0 || offset > layout.Stride - 4)
                        throw new InvalidDataException("Raw stats offset is outside a reviewed component layout.");
                    // Raw is an escape hatch, but never an unguarded write.
                    var expect = Bits(Required(raw, "expect"), storage, null, null);
                    edits.Add(new StatsEdit($"raw {component}+{offset:X}", "component", component, resource, -1, offset, storage,
                        Bits(Required(raw, "value"), storage, null, null), expect, Flag(raw, "own"), true, true));
                }
                continue;
            }
            if (!schema.Targets.TryGetValue(section.Name, out var targets)) throw new InvalidDataException($"Unknown stats category {section.Name}.");
            Object(section.Value);
            foreach (var entry in section.Value.EnumerateObject())
            {
                var targetName = schema.TargetAliases.GetValueOrDefault(section.Name)?.GetValueOrDefault(entry.Name, entry.Name) ?? entry.Name;
                if (!targets.TryGetValue(targetName, out var target)) throw new InvalidDataException($"Unknown {section.Name} target {entry.Name}.");
                Object(entry.Value);
                var own = Flag(entry.Value, "own");
                var (before, retiredBefore) = (edits.Count, retired);
                foreach (var property in entry.Value.EnumerateObject())
                {
                    if (property.Name == "own") continue;
                    var names = property.Name switch
                    {
                        "spread" => new[] { "weapon.horizontal_spread", "weapon.vertical_spread" },
                        "armorPenetration" when section.Name == "damage" => ["ap_direct", "ap_slight", "ap_large", "ap_extreme"],
                        _ => new[] { target.Aliases.GetValueOrDefault(property.Name, property.Name) }
                    };
                    if (property.Name == "armorPenetration" && (property.Value.ValueKind != JsonValueKind.Array || property.Value.GetArrayLength() != 4))
                        throw new InvalidDataException("armorPenetration needs four integer values.");
                    for (var index = 0; index < names.Length; index++)
                    {
                        var name = names[index];
                        // it never reached the game: still loads, and does nothing, as before
                        if (target.Retired.ContainsKey(name)) { retired++; continue; }
                        if (target.Blocked.TryGetValue(name, out var reason)) throw new InvalidDataException($"{entry.Name} {property.Name}: {reason}");
                        if (!target.Fields.TryGetValue(name, out var field)) throw new InvalidDataException($"{entry.Name}: unknown stat {property.Name}.");
                        if (own && field.Kind != "component") throw new InvalidDataException($"{entry.Name} {name}: own isolates entity component records; {field.Kind} is a shared definition. Put shared definition edits in a separate entry or mod without own.");
                        var value = property.Name == "armorPenetration" ? property.Value[index] : property.Value;
                        // StratagemDefinition uses UINT32_MAX for unlimited uses (the registry's reviewed sentinel).
                        var bits = field.Kind == "StratagemDefinition" && field.Offset == 0x50 && field.Storage == "u32" &&
                            value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var uses) && uses == -1
                            ? uint.MaxValue : Bits(value, field.Storage, field.Min, field.Max);
                        edits.Add(new StatsEdit($"{entry.Name} {name}", field.Kind, field.Component, Resource(field.Resource, allowZero: true), field.Record,
                            field.Offset, field.Storage, bits, null, own, field.Shared, field.Unverified));
                    }
                }
                if (before == edits.Count && retiredBefore == retired) throw new InvalidDataException($"{entry.Name}: no stats supplied.");
            }
        }
        if (edits.Count > MaxEdits || edits.Count == 0 && retired == 0) throw new InvalidDataException($"Stats file needs 1..{MaxEdits} edits.");
        if (edits.Select(ConflictKey).Distinct(StringComparer.Ordinal).Count() != edits.Count)
            throw new InvalidDataException("Stats file sets the same field more than once (possibly through an alias or shared record).");
        return edits;
    }

    public static IEnumerable<StatsEdit> Edits(ModManifest mod)
    {
        // Legacy native writes keep their expected-value guards and are applied by the same load-time pass.
        foreach (var write in mod.ComponentWrites)
            foreach (var field in write.Fields)
                yield return new StatsEdit(field.Name, "component", write.Component, write.Resource, -1, field.Offset, "u32", field.Value, field.Expect, false, true, false);
        foreach (var edit in mod.Stats) yield return edit;
    }

    static uint Bits(JsonElement value, string storage, double? min, double? max)
    {
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetDouble(out var number) || !double.IsFinite(number) || number < min || number > max)
            throw new InvalidDataException($"Stat must be a finite number within its reviewed range ({min}..{max}).");
        return storage switch
        {
            "f32" when float.IsFinite((float)number) => BitConverter.SingleToUInt32Bits((float)number),
            "u32" when value.TryGetUInt32(out var integer) => integer,
            "i32" when value.TryGetInt32(out var integer) => unchecked((uint)integer),
            "u8" when value.TryGetUInt32(out var integer) && integer <= byte.MaxValue => integer, // a component's one-byte flag
            _ => throw new InvalidDataException($"Stat does not fit {storage}.")
        };
    }

    static ulong Resource(string value, bool allowZero = false) => value.StartsWith("0x", StringComparison.Ordinal) &&
        ulong.TryParse(value.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var number) && (allowZero || number != 0)
        ? number : throw new InvalidDataException("Stats resource must be a hexadecimal entity id.");
    static int Offset(JsonElement value) => value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var n) ? n :
        value.ValueKind == JsonValueKind.String && value.GetString() is { } s && s.StartsWith("0x", StringComparison.Ordinal) &&
        int.TryParse(s.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out n) ? n : throw new InvalidDataException("Invalid stats offset.");
    static JsonElement Required(JsonElement value, string name) => value.TryGetProperty(name, out var result) ? result : throw new InvalidDataException($"Stats {name} is required.");
    static string Text(JsonElement value, string name) => Required(value, name) is { ValueKind: JsonValueKind.String } text ? text.GetString()! : throw new InvalidDataException($"Stats {name} must be text.");
    static bool Flag(JsonElement value, string name) => !value.TryGetProperty(name, out var flag) ? false : flag.ValueKind switch
    {
        JsonValueKind.True => true, JsonValueKind.False => false, _ => throw new InvalidDataException($"Stats {name} must be boolean.")
    };
    static void Object(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Stats entries must be objects.");
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var p in value.EnumerateObject()) if (!names.Add(p.Name)) throw new InvalidDataException($"Repeated stats property {p.Name}.");
    }
    static void Allowed(JsonElement value, params string[] names)
    {
        foreach (var p in value.EnumerateObject()) if (!names.Contains(p.Name, StringComparer.Ordinal)) throw new InvalidDataException($"Unknown raw stats property {p.Name}.");
    }
}
