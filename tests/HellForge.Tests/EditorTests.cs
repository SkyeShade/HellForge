using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.Core.GameData;

static class EditorTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The in-game editor's catalogue lists the weapons, stratagems and boosters with fields it can read, change and save", () =>
        {
            var catalog = EditorCatalog.Build();
            check(catalog["format"]!.GetValue<int>() == EditorCatalog.Format && catalog["build"]!.GetValue<string>() == StatsSchema.Current.Build, "Format and build.");
            var navs = catalog["categories"]!.AsArray().Select(c => c!["id"]!.GetValue<string>()).ToList();
            check(navs.SequenceEqual(["primary", "secondary", "throwables", "offensive", "defensive", "support", "backpacks", "vehicles", "mission", "boosters", "helldiver", "perks",
                    "terminids", "automatons", "illuminate", "structures", "entities", "difficulty", "rosters", "compositions", "demolition", "statuses", "explosions",
                    "projectiles", "damage"]),
                "Weapons, stratagems, boosters, the Helldiver, the enemies, spawning: " + string.Join(", ", navs));
            var objects = catalog["objects"]!.AsArray().Select(o => o!.AsObject()).ToList();
            foreach (var nav in navs)
                check(objects.Count(o => o["category"]!.GetValue<string>() == nav && o["id"]!.GetValue<string>() != "helldiver/armor-perks") == StatsCatalog.Items.Count(i => i.Nav == nav), $"Every {nav} item is listed.");
            var sprites = EditorIcons.Sprites();
            var noIcon = objects.Where(o => o["category"]!.GetValue<string>() is "primary" or "secondary" or "throwables" or "offensive" or "defensive" &&
                    o["icon"] == null).Select(o => o["name"]!.GetValue<string>()).ToList();
            check(noIcon.Count == 0, "Every weapon, throwable and strike has a HUD sprite: " + string.Join(", ", noIcon));
            var groupOf = new Dictionary<string, string> { ["weapon"] = "weapons", ["throwable"] = "throwables", ["stratagem"] = "stratagems", ["booster"] = "boosters" };
            groupOf["passive"] = "passives";
            groupOf["difficulty"] = "difficulties";
            // level 11's icon is its mod's (the game has none), the others the game's
            check(objects.Where(o => o["icon"] != null && !(o["icon"]!.GetValue<string>() == "difficulty" && o["sprite"]?.GetValue<string>() == "11"))
                .All(o => sprites[groupOf[o["icon"]!.GetValue<string>()]].ContainsKey(o["sprite"]?.GetValue<string>() ?? o["name"]!.GetValue<string>())),
                "An icon names a sprite of its group.");
            var difficulties = objects.Where(o => o["category"]!.GetValue<string>() == "difficulty").ToList();
            check(difficulties.Select(o => o["name"]!.GetValue<string>()).Take(3).SequenceEqual(["D1 · Trivial", "D2 · Easy", "D3 · Medium"]) &&
                difficulties.All(o => o["icon"]?.GetValue<string>() == "difficulty"), "Mission difficulties are D1 to D11, in order, with their icons.");
            var roster = objects.First(o => o["id"]!.GetValue<string>() == "rosters/automatons/row_1");
            check(roster["subtitle"]!.GetValue<string>() == "Automatons · Row 1" && roster["group"]!.GetValue<string>() == "Automatons" &&
                roster["faction"]!.GetValue<string>() == "automatons", "A roster row shows its roster and row, its faction's chip and emblem.");

            var rows = 0;
            foreach (var o in objects)
                foreach (var c in o["page"]!.AsArray().Select(c => c!.AsObject()))
                {
                    var parts = c["subs"]!.AsArray().Select(s => s!.AsObject()).ToList();
                    var count = parts.Sum(p => p["rows"]!.AsArray().Count);
                    check(!c["plain"]!.GetValue<bool>() || count == 1, "A plain category is one row.");
                    check(!c["flat"]!.GetValue<bool>() || parts.Count == 1, "A flat category is one part.");
                    check(c["key"]!.GetValue<string>().StartsWith("c|", StringComparison.Ordinal), "Category keys are the HD2R Editor's.");
                    foreach (var p in parts)
                    {
                        check(!p["fold"]!.GetValue<bool>() || p["rows"]!.AsArray().Count == 1, "A folded part is one row.");
                        foreach (var r in p["rows"]!.AsArray().Select(r => r!.AsObject()))
                        {
                            rows++;
                            if (r["choice"] != null) { check(r["options"]!.AsArray().Count > (r["choice"]!.GetValue<string>() is "mount" or "podcount" or "aplabel" or "rowcopy" ? 1 : 20), "A choice row lists its options."); continue; }
                            var field = StatsSchema.Current.Targets[r["cat"]!.GetValue<string>()][r["target"]!.GetValue<string>()].Fields[r["key"]!.GetValue<string>()];
                            var component = r["component"]?.GetValue<string>();
                            if (component == null) { check(field.Kind is "EntityDelta" or "StatusMultiplier" && r["applies"]!.GetValue<string>() == "boot", "Only entity deltas and status multipliers are saved without live editing."); continue; }
                            check(SdkMappings.Current.FieldAt(component, field.Offset, field.Storage) != null, $"{component} +{field.Offset} is a mapped field.");
                            var stratagemType = field.Kind != "StratagemDefinition" ? null : field.LiveId?.ToString() ??
                                SdkMappings.Current.Content.Single(c => c.Kind == "stratagems" && c.Name == r["target"]!.GetValue<string>()).Id.ToString();
                            check(r["ref"]!.GetValue<string>() == (field.Kind == "component" ? field.Resource : stratagemType ?? field.Record.ToString()),
                                "The target is the entity, the row id, or the stratagem's type.");
                        }
                    }
                }
            check(rows > 3000, $"The fields are all there ({rows}).");
            var orbital = objects.Single(o => o["name"]!.GetValue<string>() == "Orbital Precision Strike");
            var cooldown = orbital["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray())
                .Single(r => r!["key"]!.GetValue<string>() == "stratagem.cooldown")!;
            check(orbital["tone"]!.GetValue<string>() == "offensive" && orbital["icon"]!.GetValue<string>() == "stratagem" &&
                cooldown["component"]!.GetValue<string>() == "StratagemDefinition" && cooldown["ref"]!.GetValue<string>() == "118",
                "A stratagem's settings row is its type (the Orbital Precision Strike's 118), in its category's tone.");
            var vitality = objects.Single(o => o["name"]!.GetValue<string>() == "Vitality Enhancement");
            var scale = vitality["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).Single()!;
            check(vitality["icon"]!.GetValue<string>() == "booster" && scale["component"]!.GetValue<string>() == "BoosterSettings" &&
                scale["ref"]!.GetValue<string>() == "1" && scale["offset"]!.GetValue<int>() == 8 && scale["key"]!.GetValue<string>() == "booster.damage_taken_scale" &&
                scale["min"]!.GetValue<double>() == 0 && scale["max"]!.GetValue<double>() == 4,
                "A booster's tuning value is its BoosterSettings row (Vitality Enhancement: 1), within its range.");
            var perkPages = objects.Where(o => o["category"]!.GetValue<string>() == "perks").ToList();
            var scout = perkPages.Single(o => o["name"]!.GetValue<string>() == "Scout");
            var scoutRows = scout["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).Select(r => r!).ToList();
            check(perkPages.Count == 32 && scout["icon"]!.GetValue<string>() == "passive" && scout["sprite"]!.GetValue<string>() == "SCOUT" &&
                scoutRows.Any(r => r["component"]!.GetValue<string>() == "PassiveModifier" && r["ref"]!.GetValue<string>() == "33" && r["offset"]!.GetValue<int>() == 8) &&
                perkPages.Single(o => o["name"]!.GetValue<string>() == "Standard Issue")["detail"]!.GetValue<string>().Contains("No numeric"),
                "Armor perks: 32 pages with their passive's icon, a value as its PassiveModifier entry (Scout's second: 2 * 16 + 1).");
            var allRows = objects.SelectMany(o => o["page"]!.AsArray()).SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Select(r => r!).ToList();
            var keyed = allRows.Where(r => r["hd2r"] != null).SelectMany(r => r["hd2r"]!.AsArray().Select(k => (Key: k!.GetValue<string>(), Row: r))).ToList();
            var eruptorImpact = keyed.FirstOrDefault(k => k.Key == "pw|R-36 Eruptor|terminal.primary.impact.explosion").Row;
            check(keyed.Count > 10000 && eruptorImpact != null && eruptorImpact["hd2rKind"]!.GetValue<string>() == "explosion" &&
                keyed.Any(k => k.Key == "hd|Armor perks|armor") && keyed.Any(k => k.Key == "ap|7|perk.stim_capacity") &&
                catalog["hd2r"]!["statuses"]!["fire"]!.GetValue<long>() == 5,
                "Rows carry HD2R Editor's preset keys on their bytes (a payload, an armor perk value, the passive choice), with the ids presets store.");
            var patriot = objects.Single(o => o["name"]!.GetValue<string>() == "EXO-45 Patriot Exosuit" && o["category"]!.GetValue<string>() == "vehicles");
            var mountsCategory = patriot["page"]!.AsArray().Select(c => c!).Single(c => c["key"]!.GetValue<string>() == "c|mounts");
            var slotRows = mountsCategory["subs"]!.AsArray()[0]!["rows"]!.AsArray().Select(r => r!).ToList();
            var patriotRows = patriot["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Select(r => r!).ToList();
            check(patriot["page"]!.AsArray().IndexOf(mountsCategory) == 1 && slotRows.Count == 2 &&
                slotRows.All(r => r["choice"]!.GetValue<string>() == "mount" && r["type"]!.GetValue<string>() == "u64" && r["options"]!.AsArray().Count > 50) &&
                slotRows[0]["label"]!.GetValue<string>() == "Left gun" && slotRows[1]["offset"]!.GetValue<int>() == 24 &&
                slotRows[0]["options"]!.AsArray()[0]!["label"]!.GetValue<string>().EndsWith("(vanilla)") &&
                patriotRows.Count(r => r["mountSlot"]?.GetValue<string>() == "mount.slot_1") > 10,
                "A vehicle's Mounted Weapons: a choice per slot (its allowed weapons, the vanilla one first), its weapon's rows tied to the slot.");
            check(slotRows[0]["options"]!.AsArray().Skip(1).Any(o => o!["target"]?.GetValue<string>() is { Length: > 0 } t &&
                    StatsSchema.Current.Targets["weapons"].ContainsKey(t)),
                "A mount option names its weapon's stats target: shown under the slot while it is swapped in.");
            var breakthrough = objects.Single(o => o["name"]!.GetValue<string>() == "EXO-55 Breakthrough Exosuit" && o["category"]!.GetValue<string>() == "vehicles");
            var arms = breakthrough["page"]!.AsArray().Select(c => c!).Single(c => c["key"]!.GetValue<string>() == "c|mounts")["subs"]!.AsArray()[0]!["rows"]!.AsArray();
            check(arms.Count == 2 && arms.Any(r => r!["offset"]!.GetValue<int>() == 0 && r["label"]!.GetValue<string>().StartsWith("Shield") &&
                    r["options"]!.AsArray()[0]!["label"]!.GetValue<string>().EndsWith("(vanilla)") && r["note"]!.GetValue<string>().Contains("untested")),
                "The EXO-55's shield arm can be swapped too (untested; HD2Runtime offers no swap for it).");
            JsonNode Booster(string name) => objects.Single(o => o["name"]!.GetValue<string>() == name);
            var firebomb = Booster("Firebomb Hellpods")["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).Select(r => r!).ToList();
            var eat = Booster("Surplus EAT Allocation")["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray())
                .Single(r => r!["choice"] == null)!; /* its granted stratagem's uses; its pod's rows beside it */
            check(objects.Count(o => o["category"]!.GetValue<string>() == "boosters") == 20 &&
                firebomb.Any(r => r["component"]!.GetValue<string>() == "ExplosionSettings" && r["ref"]!.GetValue<string>() == "83" && r["offset"]!.GetValue<int>() == 16) &&
                eat["component"]!.GetValue<string>() == "StratagemDefinition" && eat["ref"]!.GetValue<string>() == "15" && eat["unlimited"] == null,
                "All 20 boosters: an explosion booster's rows, a granted stratagem's uses (its StratagemType, a plain count).");
            var perks = objects.Single(o => o["id"]!.GetValue<string>() == "helldiver/armor-perks")["page"]!.AsArray()[0]!["subs"]!.AsArray()[0]!["rows"]!.AsArray();
            var armorOptions = perks[0]!["options"]!.AsArray().Select(o => o!["value"]!.GetValue<int>()).ToList();
            var secondOptions = perks[1]!["options"]!.AsArray().Select(o => o!["value"]!.GetValue<int>()).ToList();
            check(armorOptions[0] == -1 && armorOptions.Contains(2) && armorOptions.Contains(0) && !armorOptions.Contains(17) && !armorOptions.Contains(19) &&
                secondOptions[0] == -1 && !secondOptions.Contains(0) && secondOptions.Contains(7),
                "The armor perks offer the kits' own and every passive HellForge can give (no packaged ones, no STANDARD ISSUE as the second).");
            var liberator = objects.Single(o => o["name"]!.GetValue<string>() == "AR-23 Liberator");
            var fire = liberator["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray())
                .Single(r => r!["key"]!.GetValue<string>() == "weapon.fire_rate")!;
            check(fire["component"]!.GetValue<string>() == "ProjectileWeaponComponentData" && fire["offset"]!.GetValue<int>() == 8 &&
                fire["applies"]!.GetValue<string>() == "respawn" && fire["unit"]!.GetValue<string>() == "rpm", "The Liberator's fire rate.");
        });

        test("HD2R Editor's other value kinds are plain fields: flags, enums, fire mode, rate and trait slots, read through get_stat where get_live cannot", () =>
        {
            var weapons = StatsSchema.Current.Targets["weapons"];
            var maxigun = weapons["M-1000 Maxigun"].Fields;
            check(maxigun["weapon.stationary_while_firing"] is { Storage: "u8", Offset: 387, Component: "WeaponDataComponentData" } flag &&
                flag.Choices.Select(c => c.Label).SequenceEqual(["No", "Yes"]), "The stationary-while-firing flag, one byte with No / Yes.");
            check(maxigun["fire_rate.modes.x"] is { Storage: "f32", Offset: 4 } && maxigun["fire_rate.modes.z"].Offset == 12 &&
                !maxigun.ContainsKey("fire_rate.modes.y") && maxigun.ContainsKey("weapon.fire_rate"), "Rate slots X and Z; Y is the fire rate itself.");
            var liberator = weapons["AR-23 Liberator"].Fields;
            check(Enumerable.Range(1, 5).All(i => liberator[$"presentation.traits.{i}"] is { Storage: "u32", Component: "LoadoutEntryComponentData" } t &&
                t.Offset == 8 + 4 * i && t.Choices.First().Label == "None" && t.Choices.Any(c => c.Label == "Light armor penetrating")), "Five trait slots.");
            var catalog = EditorCatalog.Build();
            var rows = catalog["objects"]!.AsArray().Single(o => o!["name"]!.GetValue<string>() == "AR-23 Liberator")!["page"]!.AsArray()
                .SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Select(r => r!).ToList();
            JsonNode Row(string key) => rows.Single(r => r["key"]!.GetValue<string>() == key);
            var trait1 = Row("presentation.traits.1");
            check(trait1["values"]!.AsArray().Count > 10 && trait1["hd2rAs"]!["pw|AR-23 Liberator|presentation.traits"]!["kind"]!.GetValue<string>() == "slots" &&
                trait1["hd2rAs"]!["pw|AR-23 Liberator|presentation.armor_penetration"] == null &&
                Row("presentation.armor_penetration")["hd2rAs"]!["pw|AR-23 Liberator|presentation.armor_penetration"]!["kind"]!.GetValue<string>() == "ap" &&
                Row("presentation.traits.2")["hd2rAs"]!["pw|AR-23 Liberator|presentation.traits"]!["slot"]!.GetValue<int>() == 1,
                "Trait slots carry the preset's traits list (slot by slot); its armor penetration label is the displayed armor penetration row's.");
            check(Row("fire_mode.modes.1")["values"] != null && Row("weapon.fire_rate")["hd2rAs"]!["pw|AR-23 Liberator|fire_rate.modes"]!["slot"]!.GetValue<int>() == 1,
                "Fire mode slots pick from the weapon's modes; the fire rate is the rate list's middle slot.");
            var magazine = rows.First(r => r["key"]!.GetValue<string>().StartsWith("attachment.magazine.", StringComparison.Ordinal));
            check(magazine["stat"]!.AsArray()[0]!.GetValue<string>() == "EntityDelta" && magazine["component"] == null,
                "An attachment's field is read through get_stat: kind, component, resource, record.");
            var targets = StatsSchema.Current.Targets;
            var helldiver = targets["helldiver"]["Helldiver"].Fields;
            check(helldiver.Count == 66 && helldiver["unit.scale_min"].Component == "UnitComponentData" && helldiver["entity.health"] is { Component: "HealthComponentData", Record: 76, Offset: 0 } &&
                helldiver["helldiver.stamina.cost_jump"] is { Component: "AvatarComponentData", Offset: 76 } &&
                helldiver["arm_left:zone.health"] is { Component: "HealthComponentData", Record: 76, Offset: 1856, Storage: "i32", Section: "Left arm" } &&
                helldiver["head:zone.damage_multiplier"].Choices.Any(c => c.Label == "Critical") &&
                helldiver["helldiver.supply.stims_from_supply"].Component == "InventoryComponentData", "HD2R Editor's Helldiver page.");
            check(StatsCatalog.Sections(StatsCatalog.Items.Single(i => i.Nav == "helldiver")).Select(s => s.Title)
                .SequenceEqual(["Health", "Speed", "Stamina", "Player refill", "Damage", "Size & Geometry", "Damage Zones"]), "Its sections (main health first).");
            check(targets["stratagems"]["LIFT-850 Jump Pack"].Fields["payload.spawn_count"] is { Component: "HellpodRackComponentData", Offset: 556, Max: 4 } &&
                targets["stratagems"]["Orbital Laser"].Fields["orbital.duration"] is { Component: "OrbitalAbilityComponentData", Resource: "0xEC3575E7A93793BB", Offset: 460 } &&
                targets["boosters"]["Experimental Infusion"].Fields["status.incoming_damage_scale"] is { Kind: "StatusMultiplier", Component: "2", Offset: 4 },
                "A pod's spawn count, the Orbital Laser's duration, Experimental Infusion's multiplier.");
            var liberatorMelee = targets["weapons"]["AR-23 Liberator"].Fields;
            check(liberatorMelee["zone_1:melee.volume_x"] is { Kind: "MeleeHitVolume", Offset: 40, Shared: true } volume && volume.Record > 0x3000000 &&
                LiveApplies.Of(volume)?.When == "now" && targets["weapons"]["CQC-30 Stun Baton"].Fields["tuning:melee.stamina_cost"].Component == "MeleeWeaponComponentData",
                "Melee: a weapon's hit volumes (static in game.dll, by RVA; live) and a melee weapon's stamina cost.");
        });

        test("The HD2R Editor's Enemies pages: every entity with the fields the engine writes, by HD2R row id; deployed entities on their stratagem's page", () =>
        {
            var enemies = StatsSchema.Current.Targets["enemies"];
            check(enemies.Count > 1000 && enemies.Values.Count(e => e.Family == "terminids") > 30 && enemies.Values.Count(e => e.Family == "automatons") > 60,
                "Over a thousand entities, each on its faction's (or Structures', or Other entities') page: " + enemies.Count);
            check(enemies["Bile Titan"].Fields["entity::entity.health"] is { Kind: "component", Component: "HealthComponentData", Resource: "0x9E2E17F2CCCCAFDD", Offset: 0, Unverified: false }
                && enemies["Bile Titan"].Label == "Bile Titan - Strider", "An enemy's main health: its own HealthComponent record, proven in game (HD2Runtime's evidence).");
            check(enemies.Values.SelectMany(e => e.Fields).All(f => SdkMappings.Current.FieldAt(f.Value.Kind == "component" ? f.Value.Component : f.Value.Kind, f.Value.Offset, f.Value.Storage) != null),
                "Every field offered is one the engine maps (mappings/components.json).");
            check(!enemies.ContainsKey("avatar_helldiver"), "The Helldiver's own entity is the Helldiver page's, not an enemy.");
            var silo = StatsCatalog.Items.Single(i => i.Id == "stratagems/MS-11 Solo Silo");
            var siloRows = StatsCatalog.Sections(silo).SelectMany(s => s.Groups).SelectMany(g => g.Rows).ToList();
            check(silo.Parts.Any(p => p is { Category: "enemies", Target: "weapon_rack_mini_missile_silo", Title: "Rack" }) &&
                  silo.Parts.Any(p => p is { Category: "enemies", Target: "mini_missile_silo", Title: "Missile" }) &&
                  !StatsCatalog.Items.Any(i => i.Nav == "entities" && i.Parts.Any(p => p.Target == "mini_missile_silo")),
                "The Solo Silo's rack and missile are parts of its page, not Other entities.");
            check(siloRows.GroupBy(r => StatsCatalog.LinkKey(r.Field)).All(g => g.Count() == 1) &&
                  StatsCatalog.Sections(silo).SelectMany(s => s.Groups).Any(g => g.Title == "Rack · Health & Armor"),
                "No bytes twice on its page (the missile's attack is the weapon's explosion), and each part's groups say whose.");
            var catalog = EditorCatalog.Build();
            var titan = catalog["objects"]!.AsArray().Select(o => o!.AsObject()).Single(o => o["id"]!.GetValue<string>() == "enemies/Bile Titan");
            var health = titan["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).First(r => r!["key"]!.GetValue<string>() == "entity::entity.health")!;
            check(titan["category"]!.GetValue<string>() == "terminids" && health["hd2r"]![0]!.GetValue<string>() == "en|Bile Titan|entity::entity.health",
                "In the in-game editor's catalogue under Terminids, its rows keyed as HD2R Editor presets name them.");
            var saved = StatsData.Parse("""{"format":1,"enemies":{"Bile Titan":{"entity::entity.health":7000}},"helldiver":{"Helldiver":{"entity.health":200}}}""",
                EntityProfile.Current);
            check(saved.Any(e => e is { Component: "HealthComponentData", Resource: 0x9E2E17F2CCCCAFDD, Offset: 0, Value: 7000 }) &&
                  saved.Any(e => e is { Component: "HealthComponentData", Resource: 0x4D1C334D294DFA97, Record: 76, Offset: 0, Value: 200 }),
                "Saved in Local Stats, an enemy's and the Helldiver's main health come back at the next boot: " + string.Join(", ", saved.Select(e => e.Key)));
        });

        test("The HD2R Editor's Spawning pages: difficulties, spawn rosters and compositions, as the engine names their rows (director.c)", () =>
        {
            var targets = StatsSchema.Current.Targets;
            check(targets["difficulty"].Count == 11 && targets["rosters"].Count == 191 && targets["compositions"].Count == 452,
                "11 difficulties (11 with a mod that adds it), 191 roster rows, 452 compositions.");
            var scavenger = targets["rosters"]["terminids/row_0"];
            check(scavenger.Label == "Scavenger - Scavenger Tier 1" && scavenger.Fields["spawn.weight_3"] is { Kind: "EnemyRoster", Record: 0, Offset: 8, Storage: "f32" } &&
                  scavenger.Fields["spawn.weight_11"] is { Kind: "EnemyRosterLevel11", Offset: 0 } &&
                  targets["rosters"]["automatons/row_4"].Fields["spawn.weight_1"].Record == (2 << 16 | 4),
                "A roster row by (roster << 16) | row in the engine's roster order; its weights from 0, level 11's of its own.");
            var patrol = targets["compositions"]["automatons/family_1/row_0"];
            check(patrol.Class == "patrol" && patrol.Fields["composition.weight"] is { Kind: "EnemyComposition", Record: 2 << 16, Offset: 0xE0 } &&
                  targets["compositions"]["terminids/family_3/row_2"].Fields["composition.weight"].Record == (0 << 16 | 2 << 12 | 2),
                "A composition by (roster << 16) | (family - 1 << 12) | row.");
            var hellbomb = targets["demolition"]["Hellbomb"];
            check(targets["demolition"].Count == 9 && hellbomb.Fields["demolition.force"] is { Kind: "DemolitionTemplate", Record: 6, Offset: 4, Storage: "u32", Min: 1, Max: 255 } &&
                  !targets["demolition"].ContainsKey("none") && !targets["demolition"].ContainsKey("Count"),
                "A demolition template by its index, its force 1 to 255; none and Count are not offered.");
            check(targets.Where(t => t.Key is "difficulty" or "rosters" or "compositions" or "demolition").SelectMany(t => t.Value.Values).SelectMany(t => t.Fields.Values)
                    .All(f => SdkMappings.Current.FieldAt(f.Kind, f.Offset, f.Storage) != null),
                "Every field is one the engine maps (mappings/components.json).");
            var saved = StatsData.Parse("""{"format":1,"compositions":{"automatons/family_1/row_0":{"composition.group_0_count":4}},"rosters":{"terminids/row_0":{"spawn.weight_11":2}},"difficulty":{"7":{"difficulty.encounter_points":500}}}""",
                EntityProfile.Current);
            check(saved.Any(e => e is { Kind: "EnemyComposition", Record: 2 << 16, Offset: 4, Value: 4 }) && saved.Any(e => e is { Kind: "EnemyRosterLevel11", Record: 0 }) &&
                  saved.Any(e => e is { Kind: "DifficultySettings", Record: 7, Offset: 0xE4 }),
                "Saved in Local Stats, they come back at the next boot: " + string.Join(", ", saved.Select(e => e.Key)));
            var catalog = EditorCatalog.Build();
            var row = catalog["objects"]!.AsArray().Select(o => o!.AsObject()).Single(o => o["id"]!.GetValue<string>() == "compositions/automatons/family_1/row_0")["page"]!
                .AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).First(r => r!["key"]!.GetValue<string>() == "composition.weight")!;
            check(row["hd2r"]![0]!.GetValue<string>() == "ed|composition/automatons/family_1/row_0|composition.weight", "Keyed as HD2R Editor presets name them.");
        });

        test("The editor's icons are the game's sprite pixels: block formats decoded exactly, written in the editor's format", () =>
        {
            /* BC1: colour 0 white, colour 1 black, every texel index 0 except the last (1) */
            var bc1 = new byte[] { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40 };
            var rgba = EditorIcons.Decode(bc1, 4, 4, 71, 0, 0, 4, 4)!;
            check(rgba[0] == 255 && rgba[3] == 255 && rgba[15 * 4] == 0 && rgba[15 * 4 + 3] == 255, "BC1 texels.");
            /* BC4: values 200 and 100, eight-step palette; texel 1 takes index 1 */
            var bc4 = new byte[] { 200, 100, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };
            var gray = EditorIcons.Decode(bc4, 4, 4, 80, 0, 0, 2, 1)!;
            check(gray[0] == 200 && gray[4] == 100, "BC4 values.");
            check(EditorIcons.Decode(bc1, 4, 4, 71, 2, 2, 4, 4) == null, "A rectangle outside its page is refused.");
            check(EditorIcons.Decode(bc1, 4, 4, 99, 0, 0, 4, 4) == null, "An unknown format is refused.");
            var file = EditorIcons.Serialize([new EditorIcons.Icon(0, "AR-23 Liberator", 2, 1, 1, [0, 255]), new EditorIcons.Icon(4, "Vitality Enhancement", 1, 1, 4, [1, 2, 3, 4])]);
            check(file.AsSpan(0, 4).SequenceEqual("HFIC"u8) && BitConverter.ToUInt32(file, 4) == 2 && BitConverter.ToUInt32(file, 8) == 2, "Header.");
            check(file[12] == 0 && file[13] == 15 && System.Text.Encoding.UTF8.GetString(file, 14, 15) == "AR-23 Liberator" &&
                BitConverter.ToUInt16(file, 29) == 2 && BitConverter.ToUInt16(file, 31) == 1 && file[33] == 1 && file[34] == 0 && file[35] == 255, "A mask.");
            check(file[36] == 4 && file[37] == 20 && file[38 + 20 + 4] == 4 && file[38 + 20 + 5] == 1 && file[38 + 20 + 8] == 4, "A colour image.");
            check(file.Length == 12 + (2 + 15 + 4 + 1 + 2) + (2 + 20 + 4 + 1 + 4), "Nothing else.");
            reject(() => EditorIcons.Serialize([new EditorIcons.Icon(0, "x", 2, 2, 1, [0])]));
            /* the game's fonts: a ttf resource is a 16-byte header, then the TrueType file */
            var resource = new byte[16].Concat(new byte[] { 0, 1, 0, 0, 7, 7 }).ToArray();
            check(EditorFonts.TrueType(resource) is { Length: 6 } ttf && ttf[1] == 1 && ttf[4] == 7, "The TrueType file after the header.");
            check(EditorFonts.TrueType(new byte[24]) == null, "Anything else is not a font.");
            check(EditorFonts.Fonts.Select(f => f.File).SequenceEqual(["fs-sinclair.ttf", "fs-sinclair-medium.ttf"]), "Regular and medium.");
        });
    }
}
