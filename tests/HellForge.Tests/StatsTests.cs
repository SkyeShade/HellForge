using HellForge.Core;
using System.Text.Json.Nodes;

static class StatsTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var profile = EntityProfile.Current;
        const string rpm = """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}}}""";
        test("Stats resolve armory aliases to reviewed scalar layouts", () =>
        {
            var edits = StatsData.Parse(rpm, profile);
            check(edits.Count == 1 && edits[0].Component == "ProjectileWeaponComponentData" && edits[0].Offset == 8 &&
                  BitConverter.UInt32BitsToSingle(edits[0].Value) == 900 && edits[0].Record == 192, "RPM mapping differs from the reviewed layout.");
            var fuse = StatsData.Parse("""{"format":1,"throwables":{"G-12 High Explosive":{"fuse":2}}}""", profile).Single();
            check(fuse.Component == "ExplosiveComponentData" && fuse.Offset == 12, "Fuse mapping is wrong.");
            var damage = StatsData.Parse("""{"format":1,"damage":{"54":{"standard":150,"armorPenetration":[4,4,4,4]}}}""", profile);
            check(damage.Count == 5 && damage.All(e => e.Shared && e.Kind == "DamageInfo"), "Shared damage/array resolution failed.");
            var uses = StatsData.Parse("""{"format":1,"stratagems":{"Orbital Precision Strike":{"uses":-1}}}""", profile).Single();
            check(uses.Kind == "StratagemDefinition" && uses.Offset == 80 && uses.Value == uint.MaxValue, "Unlimited uses sentinel is wrong.");
        });
        test("Stats reject unknown, blocked, duplicate and out-of-range fields", () =>
        {
            foreach (var json in new[]
            {
                rpm.Replace("900", "\"900\""), rpm.Replace("900", "1e100"), rpm.Replace("rpm", "typo"),
                rpm.Replace("AR-23 Liberator", "unknown"), rpm.Replace("\"rpm\":900", "\"rpm\":900,\"weapon.fire_rate\":800"),
                rpm.Replace("\"format\":1", "\"format\":1,\"format\":1"), rpm.Replace("\"rpm\":900", "\"weapon.slot\":2"),
                """{"format":1,"throwables":{"G-12 High Explosive":{"fuse":-1}}}""",
                """{"format":1,"damage":{"54":{"standard":1.5}}}""",
                """{"format":1,"damage":{"54":{"own":true,"standard":10}}}""",
                """{"format":1,"damage":{"54":{"armorPenetration":[4,4]}}}""",
                """{"format":1,"weapons":{"AR-23 Liberator":{"own":true}}}""",
                """{"format":1,"raw":[{"component":"DepositComponentData","resource":"0x1","offset":0,"type":"u32","value":10}]}"""
            }) reject(() => StatsData.Parse(json, profile));
            reject(() => StatsData.Parse(rpm, profile with { GameDllSha256 = "mismatch" }));
        });
        test("Stats-only mods load, conflict in mod order, and affect the digest", () =>
        {
            var modsRoot = Path.Combine(root, "stats-mods");
            foreach (var id in new[] { "a", "b" })
            {
                var folder = Path.Combine(modsRoot, id); Directory.CreateDirectory(folder);
                File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), $$"""{"format":1,"id":"{{id}}","name":"Stats","version":"1","gameplay":false}""");
                File.WriteAllText(Path.Combine(folder, StatsData.FileName), id == "a" ? rpm : rpm.Replace("900", "800"));
            }
            var (mods, problems) = ModCatalog.Load(modsRoot);
            check(mods.Count == 2 && mods.All(m => m.NeedsLoader && !m.HasCode && m.Gameplay), "Stats-only mods were omitted.");
            // listed a, b (new mods by id): b loads first, a later, so a's value wins
            check(problems.Count == 1 && problems[0].Contains("a applies later"), "Stats conflict not reported in load order: " + string.Join(" | ", problems));
            var plan = ModLoaderPlan.Text(ModCatalog.LoadOrder(mods), root, profile, "25480438");
            check(plan.Contains("\t-\nstat\ta\tcomponent") && plan.IndexOf("stat\tb\t", StringComparison.Ordinal) < plan.IndexOf("stat\ta\t", StringComparison.Ordinal), "Plan order/data-only marker wrong.");
            var digest = ModCatalog.Digest(mods);
            File.WriteAllText(Path.Combine(modsRoot, "b", StatsData.FileName), rpm.Replace("900", "700"));
            check(ModCatalog.Digest(ModCatalog.Load(modsRoot).Mods) != digest, "Stats edit did not affect digest.");
            var disabled = mods.Select(m => m with { Enabled = false }).ToList();
            check(!ModLoaderPlan.Text(disabled, root, profile, "25480438").Contains("stat\t"), "Disabled stats leaked into plan.");
        });
        test("Legacy component writes are emitted with expected values in the stats plan", () =>
        {
            var mod = new ModManifest("legacy", "Legacy", "1", "", true, root, [],
                [new ComponentWrite("DepositComponentData", 1, [new ComponentFieldEdit("capacity", 0, 10, 20)])], []);
            var plan = ModLoaderPlan.Text([mod], root, profile, "25480438");
            check(plan.Contains("stat\tlegacy\tcomponent\tDepositComponentData\t0000000000000001\t-1\t0\tu32\t00000014\t0000000A"), "Legacy guard was lost.");
        });
        test("The shipped stats example resolves without game files", () =>
        {
            var example = ModCatalog.Parse(Path.GetFullPath("examples/mods/stats-example"), profile);
            check(example.Stats.Count == 15 && example.NeedsLoader, "Example edit count or loader flag is wrong.");
        });
        test("Default magazine, every settings family and native stratagem names resolve", () =>
        {
            var magazine = StatsData.Parse(rpm.Replace("\"rpm\":900", "\"magazine\":60"), profile).Single();
            check(magazine.Kind == "EntityDelta" && magazine.Resource == 0x7BE1E04A7738E673 && magazine.Offset == 168144 && magazine.Shared,
                "Default magazine did not resolve to its reviewed attachment delta.");
            var fields = StatsData.Parse("""{"format":1,"projectiles":{"276":{"speed":1200,"damage":54}},"stratagems":{"OrbitalShell":{"cooldown":60,"uses":-1}}}""", profile);
            check(fields.Count == 4 && fields.Any(f => f.Kind == "ProjectileSettings" && f.Offset == 60 && f.Value == 54), "Plan example reference or native name is unresolved.");
            check(StatsSchema.Current.Targets["projectiles"].Count == 350 && StatsSchema.Current.Targets["damage"].Count == 649, "Direct catalogs omit vanilla rows.");
            foreach (var kind in new[] { "ExplosionSettings", "ArcSettings", "BeamSettings", "StatusEffectSettings" })
                check(StatsSchema.Current.Targets.Values.SelectMany(c => c.Values).SelectMany(t => t.Fields.Values).Any(f => f.Kind == kind), "Missing adapter " + kind);
            reject(() => StatsData.Parse("""{"format":1,"projectiles":{"276":{"damage":999}}}""", profile));
        });
        test("Secondary, support, mounted weapon, backpack and vehicle stats resolve", () =>
        {
            var edits = StatsData.Parse("""
                {"format":1,"weapons":{
                  "P-2 Peacemaker":{"rpm":600},
                  "RS-422 Railgun":{"charge.level_1":0.5},
                  "EXO-45 Patriot Exosuit / right_gun":{"weapon.capacity":1500}},
                 "backpacks":{"M-1000 Maxigun Backpack":{"deposit.capacity":1000}},
                 "vehicles":{"EXO-45 Patriot Exosuit":{"entity.health":2000}}}
                """, profile);
            check(edits.Count == 5 && edits.All(e => e.Kind == "component" && profile.Components.ContainsKey(e.Component)),
                "An arsenal category lacks a reviewed component adapter.");
        });
        test("Snapshot values are schema-pinned, typed and reject malformed or duplicate rows", () =>
        {
            var field = StatsSchema.Current.Targets["weapons"]["AR-23 Liberator"].Fields["weapon.fire_rate"];
            var snapshot = Path.Combine(root, "stats-values.tsv");
            var header = $"stats_snapshot\t2\t25480438\t{StatsSchema.Fingerprint}\n";
            var row = $"{field.AddressKey}\tok\t{BitConverter.SingleToUInt32Bits(640):X8}\n";
            File.WriteAllText(snapshot, header + row);
            check(StatsEditorData.ReadSnapshot(snapshot).Value(field) == "640", "Observed float was not decoded.");
            File.WriteAllText(snapshot, header.Replace("25480438", "other") + row); reject(() => StatsEditorData.ReadSnapshot(snapshot));
            File.WriteAllText(snapshot, header + row + row); reject(() => StatsEditorData.ReadSnapshot(snapshot));
            File.WriteAllText(snapshot, header + row.Replace("ok", "unavailable"));
            check(StatsEditorData.ReadSnapshot(snapshot).Value(field) == null, "Unavailable observation was presented as current.");
        });
        test("Stats editor saves a valid mod, detects external edits and clears its own last override", () =>
        {
            var folder = Path.Combine(root, "editor-mods");
            var (empty, revision) = StatsEditorData.Load(folder); check(revision == "missing" && empty.Count == 1, "New editor state wrong.");
            var document = (JsonObject)JsonNode.Parse(rpm)!;
            revision = StatsEditorData.Save(folder, document, revision);
            check(ModCatalog.Load(folder).Mods.Single().Stats.Count == 1, "Editor did not create a loadable mod.");
            var loaded = StatsEditorData.Load(folder); check(loaded.Revision == revision, "Editor revision unstable.");
            File.AppendAllText(Path.Combine(folder, StatsEditorData.ModId, StatsData.FileName), " ");
            reject(() => StatsEditorData.Save(folder, document, revision));
            revision = StatsEditorData.Load(folder).Revision;
            check(StatsEditorData.Save(folder, StatsEditorData.Empty, revision) == "missing" && ModCatalog.Load(folder).Mods.Count == 0, "Clearing the last override left an invalid mod.");
            Directory.CreateDirectory(Path.Combine(folder, StatsEditorData.ModId));
            File.WriteAllText(Path.Combine(folder, StatsEditorData.ModId, ModCatalog.ManifestName), """{"id":"someone.else"}""");
            reject(() => StatsEditorData.Save(folder, document, "missing"));
        });
        test("Editor catalog lists every target once, as the armory and stratagem menus do", () =>
        {
            var items = StatsCatalog.Items;
            var parts = items.SelectMany(i => i.Parts).Select(p => (p.Category, p.Target)).ToList();
            check(parts.Count == parts.Distinct().Count() && parts.Count == StatsSchema.Current.Targets.Sum(c => c.Value.Count), "A target is missing or listed twice.");
            StatsItem Item(string name) => items.Single(i => i.Name == name);
            check(Item("AR-23 Liberator") is { Nav: "primary", Group: "Assault Rifle" }, "Liberator is not an assault rifle.");
            check(Item("AR-11 Arbitrator").Parts.Any(p => p.Title == "Underbarrel"), "Underbarrel is not part of its host.");
            check(Item("MG-43 Machine Gun") is { Nav: "support" } mg && mg.Parts.Select(p => p.Category).SequenceEqual(["weapons", "stratagems"]), "Support weapon lacks its call-in.");
            check(Item("TD-220 Bastion MK XVI").Parts.Count(p => p.Category == "weapons") == 2 && Item("Orbital Laser").Nav == "offensive", "Vehicle mounts or stratagem family wrong.");
            check(Item("Projectile 276").Subtitle != null && items.Where(i => i.Nav == "damage").All(i => i.Name.StartsWith("Damage type ", StringComparison.Ordinal)), "Type ids are not named by their users.");
            var sections = StatsCatalog.Sections(Item("AR-23 Liberator"));
            check(sections.Select(s => s.Title).SequenceEqual(["Projectile", "Handling / Fire", "Ammunition", "Armory", "Melee", "Muzzles", "Optics", "Underbarrels"]), "Liberator page categories: " + string.Join(", ", sections.Select(s => s.Title)));
            check(sections[2].Rows.First().Key == "magazine.capacity" &&
                sections[0].Groups.Select(g => g.Title).SequenceEqual(["Swap", "Flight", "Direct hit", "Explosion · Impact", "Explosion · Expiry"]),
                "Ammunition or round order wrong: " + string.Join(", ", sections[0].Groups.Select(g => g.Title)));
            check(StatsCatalog.Sections(Item("MG-43 Machine Gun"))[^1].Title == "Hellpod / Call-in" && StatsCatalog.Sections(Item("Orbital Laser"))[0].Title == "Hellpod / Call-in",
                "Call-in should close a weapon page and open a stratagem page.");
            var railgun = StatsCatalog.Sections(Item("RS-422 Railgun")).SelectMany(s => s.Rows).ToList();
            check(railgun.GroupBy(r => (r.Part, StatsCatalog.LinkKey(r.Field))).All(g => g.Count() == 1) && railgun.All(r => !r.Label.Contains("deprecated")), "Duplicate ids shown twice.");
            var words = StatsCatalog.SearchWords("liberator fire rate");
            var match = StatsCatalog.Match(Item("AR-23 Liberator"), words, i => StatsCatalog.Sections(i).SelectMany(s => s.Rows));
            check(!match.Item && match.Fields.Count >= 1 && match.Fields.Any(f => f.EndsWith("weapon.fire_rate", StringComparison.Ordinal)) && StatsCatalog.Match(Item("AR-23 Liberator"), StatsCatalog.SearchWords("23 lib"), _ => []).Item, "Search did not match a field or an item.");
        });
        test("References (payload swaps) are stats fields placed by the target's own records", () =>
        {
            var weapons = StatsSchema.Current.Targets["weapons"];
            var eruptor = weapons["R-36 Eruptor"].Fields;
            var swap = eruptor["attack.primary.projectile"];
            var rate = eruptor["weapon.fire_rate"];
            check(swap is { Kind: "component", Component: "ProjectileWeaponComponentData", Offset: 0, Storage: "u32" } && swap.Record == rate.Record &&
                swap.Resource == rate.Resource, "A weapon's projectile is its ProjectileWeapon +0.");
            var round = eruptor["projectile.velocity"].Record;
            check(eruptor["terminal.primary.impact.explosion"] is { Kind: "ProjectileSettings", Offset: 144 } impact && impact.Record == round &&
                eruptor["terminal.primary.expiry.explosion"].Offset == 156, "Its projectile's impact and expiry explosions.");
            var hit = eruptor["damage.standard_damage"].Record;
            check(Enumerable.Range(1, 4).All(k => eruptor[$"damage.status_{k}_type"] is { Kind: "DamageInfo", Storage: "u32" } t && t.Record == hit &&
                t.Offset == 44 + 8 * (k - 1) && eruptor[$"damage.status_{k}_strength"].Offset == 48 + 8 * (k - 1)) &&
                eruptor["explosion.primary.impact.damage.status_4_type"].Record == eruptor["explosion.primary.impact.damage.standard_damage"].Record,
                "Four status slots on each damage type it shows.");
            var liberator = weapons["AR-23 Liberator"];
            check(liberator.Fields["ammunition.projectile"] is { Kind: "EntityDelta", Component: "321:0", Record: 60, Offset: 162676 } &&
                !liberator.Fields.ContainsKey("attack.primary.projectile") && liberator.Blocked.ContainsKey("attack.primary.projectile"),
                "The Liberator's shot is its default ammunition's (an entity delta); ProjectileWeapon +0 stays refused.");
            check(!weapons["P-92 Warrant"].Fields.ContainsKey("attack.primary.projectile"), "A missile weapon has no projectile swap.");
            check(StatsSchema.Current.Targets["projectiles"]["40"].Fields.ContainsKey("terminal.impact.explosion") &&
                StatsSchema.Current.Targets["damage"]["149"].Fields.ContainsKey("status_3_type"), "Projectile and damage pages have their own.");
            var edits = StatsData.Parse("{\"format\":1,\"weapons\":{\"R-36 Eruptor\":{\"terminal.primary.impact.explosion\":0,\"damage.status_1_type\":5," +
                "\"attack.primary.projectile\":239},\"AR-23 Liberator\":{\"ammunition.projectile\":239}}}", profile);
            check(edits.Count == 4 && edits.Any(e => e is { Kind: "ProjectileSettings", Offset: 144, Value: 0 }) &&
                edits.Any(e => e is { Kind: "EntityDelta", Component: "321:0", Value: 239 }), "A stats file sets references by type id.");
            var catalog = EditorCatalog.Build();
            check(catalog["references"]!["explosion"]!.AsArray().Count == 139 && catalog["references"]!["status"]!.AsArray().Count == 35 &&
                catalog["references"]!["projectile"]!.AsArray().Count > 90 && catalog["references"]!["warnings"]!["very_large"] != null &&
                catalog["references"]!["explosion"]!.AsArray().Any(o => o!["warnings"]!.AsArray().Any(w => w!.GetValue<string>() == "very_large")),
                "The editor offers HD2R Editor's payload options (with their warnings), the attachable statuses and the donors.");
            var objects = catalog["objects"]!.AsArray().Select(o => o!).ToList();
            JsonNode Object(string name) => objects.Single(o => o["name"]!.GetValue<string>() == name);
            check(Object("P-92 Warrant")["notOffered"] != null && Object("R-36 Eruptor")["notOffered"] == null, "Weapons with no swap say why.");
            var wasp = Object("StA-X3 W.A.S.P. Launcher")["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Select(r => r!).ToList();
            check(wasp.Single(r => r["key"]!.GetValue<string>() == "function_ammo.projectile")["pick"]!.GetValue<string>() == "projectile",
                "The programmable ammunition's projectile.");
            check(weapons["AC-8 Autocannon"].Fields["function_ammo.projectile"] is { Component: "ProjectileWeaponComponentData", Offset: 576 } &&
                weapons["R-2124 Constitution"].Fields["attack.feed_primary.projectile"] is { Component: "WeaponRoundsComponentData", Offset: 64 } &&
                weapons["AR-2 Coyote"].Fields.ContainsKey("attack.primary.projectile"),
                "Programmable ammunition, rounds-fed weapons and HD2Runtime 0.40's direct sources swap.");
            // the rest, where their rounds really come from (mappings/projectile-sources.json)
            JsonNode SourceRow(string name, string key) => Object(name)["page"]!.AsArray().SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray())
                .Select(r => r!).Single(r => r["key"]!.GetValue<string>() == key);
            check(weapons["M-1000 Maxigun"].Fields["attack.primary.projectile"] is { Component: "ProjectileWeaponComponentData", Record: 257, Offset: 0 } &&
                weapons["PLAS-101 Purifier"].Fields["charge.level_1.projectile"] is { Component: "WeaponChargeComponentData", Record: 4, Offset: 4 } &&
                !weapons["PLAS-101 Purifier"].Fields.ContainsKey("charge.level_3.projectile") &&
                weapons["LAS-17 Double-Edge Sickle"].Fields["heat.level_2.projectile"] is { Component: "WeaponHeatComponentData", Record: 21, Offset: 28 } &&
                weapons["MG-43 Machine Gun"].Fields["magazine.pattern_5.projectile"] is { Component: "WeaponMagazineComponentData", Record: 255, Offset: 20 } &&
                weapons["SG-8 Punisher"].Fields["ammunition.projectile"] is { Kind: "EntityDelta", Component: "117:64", Record: 96, Offset: 165124 } &&
                weapons["SG-20 Halt"].Fields["ammunition.stun_alternate.projectile"] is { Kind: "EntityDelta", Component: "117:68", Record: 121 } &&
                weapons["SG-88 Break-Action Shotgun"].Fields["attack.feed_primary.projectile"] is { Component: "WeaponRoundsComponentData", Record: 12, Offset: 64 },
                "Charge and heat levels, magazine patterns, ammunition options, rounds feeds and the Maxigun's own round.");
            check(SourceRow("MG-43 Machine Gun", "magazine.pattern_5.projectile")["pick"]!.GetValue<string>() == "projectile" &&
                SourceRow("SG-8 Punisher", "ammunition.birdshot.projectile")["pick"]!.GetValue<string>() == "projectile" &&
                SourceRow("PLAS-15 Loyalist", "charge.level_3.projectile")["pick"]!.GetValue<string>() == "projectile" &&
                Object("MG-43 Machine Gun")["notOffered"] == null && Object("PLAS-101 Purifier")["notOffered"] == null,
                "They pick projectiles, and their weapons no longer say no swap is offered.");
            var fire = StatsSchema.Current.Targets["statuses"]["Fire"].Fields;
            check(StatsSchema.Current.Targets["statuses"].Count == 71 && fire["status.duration"] is { Kind: "StatusEffectSettings", Record: 5, Offset: 40 } &&
                fire["damage.standard_damage"].Record == 519 && StatsCatalog.Items.Count(i => i.Nav == "statuses") == 71,
                "Status effects: duration, and the damage they deal while active.");
            var rows = catalog["objects"]!.AsArray().Single(o => o!["name"]!.GetValue<string>() == "R-36 Eruptor")!["page"]!.AsArray()
                .SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Select(r => r!).ToList();
            JsonNode Row(string key) => rows.Single(r => r["key"]!.GetValue<string>() == key);
            check(Row("attack.primary.projectile")["pick"]!.GetValue<string>() == "projectile" && Row("terminal.primary.impact.explosion")["pick"]!.GetValue<string>() == "explosion" &&
                Row("damage.status_2_type")["slot"]!.GetValue<int>() == 2 && Row("projectile.velocity")["fires"]!.GetValue<string>() == "attack.primary.projectile" &&
                Row("weapon.fire_rate")["fires"] == null, "Pick rows, slots and the rows a swap locks.");
        });
        test("Vehicle pages are laid out as HD2R Editor's: call-in, health, named damage zones, a category per mount", () =>
        {
            StatsItem Vehicle(string name) => StatsCatalog.Items.Single(i => i.Name == name && i.Nav == "vehicles");
            var patriot = StatsCatalog.Sections(Vehicle("EXO-45 Patriot Exosuit"));
            check(patriot.Select(s => s.Title).SequenceEqual(["Hellpod / Call-in", "Weapon · Left gun", "Weapon · Right gun", "Health & Armor", "Damage Zones"]),
                "Patriot page: " + string.Join(", ", patriot.Select(s => s.Title)));
            check(patriot[4].Groups.Select(g => g.Title).Take(4).SequenceEqual(["Zone 0", "Zone 1", "Cockpit rear", "Hips"]) &&
                patriot[4].Groups.All(g => g.Rows.Count == 5), "Zones by their native names, armor, health and share each, and their explosion share.");
            var fields = StatsSchema.Current.Targets["vehicles"]["EXO-45 Patriot Exosuit"].Fields;
            check(fields["zone_2.armor"] is { Component: "HealthComponentData", Offset: 1840 } && fields["rotation.turn_speed"].Component == "RotationComponentData",
                "Zone fields at +736 + 552 per zone; the exosuit's rotation.");
            check(StatsCatalog.Sections(Vehicle("TD-110 Maelstrom")).Any(s => s.Title == "Weapon · Main cannon") &&
                StatsCatalog.Sections(Vehicle("EXO-55 Breakthrough Exosuit")).Any(s => s.Title == "Shield · Left gun"), "Mount labels as HD2R Editor's.");
        });
        test("HD2R Editor's last rows: the displayed armor penetration, overcharge and pulse explosions, one-off fields, preset keys", () =>
        {
            var objects = EditorCatalog.Build()["objects"]!.AsArray().Select(o => o!).ToList();
            List<JsonNode> RowsOf(string name) => objects.Single(o => o["name"]!.GetValue<string>() == name)["page"]!.AsArray()
                .SelectMany(c => c!["subs"]!.AsArray()).SelectMany(s => s!["rows"]!.AsArray()).Select(r => r!).ToList();
            var torcher = RowsOf("FLAM-66 Torcher");
            var ap = torcher.Single(r => r["key"]!.GetValue<string>() == "presentation.armor_penetration");
            check(ap["choice"]!.GetValue<string>() == "aplabel" && ap["options"]!.AsArray().Count == 6 && ap["hd2r"]![0]!.GetValue<string>() == "pw|FLAM-66 Torcher|presentation.armor_penetration" &&
                torcher.Where(r => r["key"]!.GetValue<string>().StartsWith("presentation.traits.")).All(r => r["hd2rAs"] == null ||
                    !r["hd2rAs"]!.AsObject().Any(k => k.Value!["kind"]!.GetValue<string>() == "ap")),
                "Every weapon's displayed armor penetration is a row of its own, with HD2R's key (the Torcher had none to link to).");
            var epoch = RowsOf("PLAS-45 Epoch").Single(r => r["key"]!.GetValue<string>() == "charge.overcharge_explosion");
            check(epoch["pick"]!.GetValue<string>() == "explosion" && epoch["offset"]!.GetValue<int>() == 200 &&
                epoch["hd2rAs"]!.AsObject().Single().Value!["names"]!["RS-422 Railgun"]!.GetValue<long>() == 326, "The Epoch's overcharge explosion: a picker, its preset naming a charge weapon.");
            var pulse = RowsOf("G-109 Urchin").Single(r => r["key"]!.GetValue<string>() == "throwable.pulse_explosion");
            check(pulse["choice"]!.GetValue<string>() == "rowcopy" && pulse["size"]!.GetValue<int>() == 148 && pulse["ref"]!.GetValue<string>() == "369" &&
                pulse["options"]!.AsArray().Count == 3 && pulse["hd2rKind"]!.GetValue<string>() == "explosion", "The Urchin's pulse explosion: a whole-row copy from its three donors.");
            check(RowsOf("SH-51 Directional Shield").Count(r => r["key"]!.GetValue<string>() is "entity.armor" or "entity.armor_deployed" or "shield.radius") == 3 &&
                RowsOf("B-100 Portable Hellbomb").Any(r => r["key"]!.GetValue<string>() == "deposit.capacity") &&
                RowsOf("AR/GL-21 One-Two").Single(r => r["key"]!.GetValue<string>() == "weapon.default_fire_mode")["values"]!.AsArray().Count == 2 &&
                RowsOf("A/G-16 Gatling Sentry").Any(r => r["key"]!.GetValue<string>() == "attack.primary.projectile") &&
                RowsOf("AX/ARC-3 K-9").Any(r => r["key"]!.GetValue<string>() == "entity.armor"), "The one-off fields: shields, the Hellbomb's deposit, the One-Two's default mode, a sentry's projectile, drone armor.");
            var patriotSlot = RowsOf("EXO-45 Patriot Exosuit").First(r => r["key"]!.GetValue<string>() == "mount.slot_0");
            var podSlot = RowsOf("M-105 Stalwart").Single(r => r["key"]!.GetValue<string>() == "pod.slot_1");
            check(patriotSlot["hd2rKind"]!.GetValue<string>() == "named" && podSlot["hd2rKind"]!.GetValue<string>() == "pickup" &&
                podSlot["hd2rAs"]!.AsObject().Single().Value!["names"]!["empty"]!.GetValue<int>() == 0, "Arm swaps and pod slots carry HD2R's preset keys.");
            var shear = StatsSchema.Current.Targets["weapons"]["LAS-22 Shear"];
            check(shear.Fields.ContainsKey("attachment.magazine.laser_heatsink_charge_standard_heatsink.heat.cool_per_second") &&
                shear.Fields.ContainsKey("attachment.magazine.laser_heatsink_charge_standard_heatsink.heat.heat_per_second") &&
                shear.Aliases["attachment.magazine.laser_heatsink_charge_standard_heatsink.er_second"] == "attachment.magazine.laser_heatsink_charge_standard_heatsink.heat.heat_per_second",
                "A heatsink's fields keep their whole ids (the cooling rate no longer shares a key); old keys stay aliases.");
        });
        test("Hellpod payloads: a call-in's pod slots and spawn count, with the reviewed pickups by kind", () =>
        {
            var objects = EditorCatalog.Build()["objects"]!.AsArray().Select(o => o!).ToList();
            JsonArray Rows(string name) => objects.Single(o => o["name"]!.GetValue<string>() == name)["page"]!.AsArray()
                .Single(c => c!["key"]!.GetValue<string>() == "c|pods")!["subs"]!.AsArray().SelectMany(s => s!["rows"]!.AsArray()).Aggregate(new JsonArray(), (a, r) => { a.Add(r!.DeepClone()); return a; });
            var stalwart = Rows("M-105 Stalwart");
            var slot = stalwart.Single(r => r!["key"]!.GetValue<string>() == "pod.slot_1")!;
            var options = slot["options"]!.AsArray();
            check(slot["component"]!.GetValue<string>() == "HellpodRackComponentData" && slot["type"]!.GetValue<string>() == "u64" && slot["choice"]!.GetValue<string>() == "pod" &&
                options[0]!["label"]!.GetValue<string>().EndsWith("(vanilla)") && options.Any(o => o!["label"]!.GetValue<string>() == "Empty") &&
                options.Any(o => o!["group"]?.GetValue<string>() == "Backpacks") && options.Count > 60, "Slot 1: its vanilla item first, empty, and the pickups by kind.");
            var count = stalwart.Single(r => r!["key"]!.GetValue<string>() == "pod.count")!;
            check(count["offset"]!.GetValue<int>() == 556 && count["type"]!.GetValue<string>() == "u32" && count["options"]!.AsArray().Count == 4, "Items spawned: 1 to 4.");
            var eat = Rows("Surplus EAT Allocation").Single(r => r!["key"]!.GetValue<string>() == "pod.slot_1")!;
            check(eat["shared"]!.GetValue<bool>() && eat["target"]!.GetValue<string>() == "EAT-17 Expendable Anti-Tank pod" &&
                eat["also"]!.AsArray().Any(a => a!.GetValue<string>() == "EAT-17 Expendable Anti-Tank"), "The booster's granted stratagem shares the EAT-17's rack.");
            var tested = Rows("Resupply").Single(r => r!["key"]!.GetValue<string>() == "pod.slot_1")!["options"]!.AsArray()
                .Single(o => o!["label"]!.GetValue<string>() == "Grenade Box")!;
            check(tested["note"]!.GetValue<string>().Contains("proven in this slot"), "A pickup a live test proved in that slot says so.");
            check(!objects.Any(o => o["name"]!.GetValue<string>() == "MS-11 Solo Silo" && o["page"]!.AsArray().Any(c => c!["key"]!.GetValue<string>() == "c|pods")),
                "A rack that delivers a deployable (the Solo Silo) has none.");
        });
        test("Field parity: what the catalogues lack, on the item's own records and rows (mappings/parity.json)", () =>
        {
            var weapons = StatsSchema.Current.Targets["weapons"];
            var arbitrator = weapons["AR-11 Arbitrator / underbarrel"].Fields;
            check(arbitrator["projectile.pellet_count"] is { Kind: "ProjectileSettings", Record: 198 } && arbitrator["damage.standard_damage"] is { Kind: "DamageInfo", Record: 161 } &&
                arbitrator["damage.status_1_type"].Record == 161 && arbitrator["weapon.sway"] is { Component: "WeaponDataComponentData", Record: 251, Offset: 104 },
                "The Arbitrator's underbarrel: its buckshot's projectile and damage rows, status slots, and its handling.");
            check(weapons["AR/GL-21 One-Two / underbarrel"].Fields["explosion.primary.impact.inner_radius"] is { Kind: "ExplosionSettings", Record: 327 } &&
                weapons["SMG/FLAM-34 Stoker / underbarrel"].Fields.ContainsKey("damage.standard_damage"),
                "The One-Two's grenade has the GP-31's rows; the Stoker's spray its damage type.");
            check(weapons["PLAS-101 Purifier"].Fields["charge.level_2"] is { Component: "WeaponChargeComponentData", Record: 4, Offset: 24 } &&
                weapons["PLAS-15 Loyalist"].Fields.ContainsKey("charge.damage_multiplier_min") && !weapons["PLAS-1 Scorcher"].Fields.Keys.Any(k => k.StartsWith("charge.")),
                "Charge on the plasma weapons that charge (not the Scorcher).");
            check(weapons["AX/ARC-3 K-9 / gun"].Fields["weapon.fire_rate"] is { Component: "ArcWeaponComponentData", Offset: 4 } &&
                !weapons["ARC-3 Arc Thrower"].Fields.Values.Any(f => f.Component == "ArcWeaponComponentData"), "The K-9's rate of fire; the Arc Thrower has none (charge).");
            foreach (var name in new[] { "EXO-45 Patriot Exosuit / right_gun", "E/MG-101 HMG Emplacement / weapon", "M-102 Gunner FRV / gun", "TD-220 Bastion MK XVI / attach_tank_gun" })
                check(weapons[name].Fields.ContainsKey("weapon.sway") && weapons[name].Fields["weapon.recoil_climb_vertical"].Offset == 32, name + ": sway and recoil.");
            var bastion = StatsSchema.Current.Targets["vehicles"]["TD-220 Bastion MK XVI"].Fields;
            check(bastion["entity.explosive_damage_percentage"] is { Offset: 388, Note: not null } && bastion["zone_0.explosive_damage_percentage"].Offset == 844 &&
                bastion["zone_0.affected_by_explosions"] is { Offset: 843, Storage: "u8" }, "Explosion shares: the default zone's and each zone's own, with its flag.");
            var shield = StatsSchema.Current.Targets["backpacks"]["SH-20 Ballistic Shield Backpack"].Fields;
            check(shield["zone.explosive_damage_percentage"].Offset == 844 && shield["entity.explosive_damage_percentage"].Record == 292, "The ballistic shield's explosion shares.");
            var row = EditorCatalog.Build()["objects"]!.AsArray().Single(o => o!["name"]!.GetValue<string>() == "SH-20 Ballistic Shield Backpack")!["page"]!.AsArray()
                .SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Single(r => r!["key"]!.GetValue<string>() == "zone.affected_by_explosions")!;
            check(row["note"] != null && row["values"]!.AsArray().Count == 2 && row["component"]!.GetValue<string>() == "HealthComponentData", "Its editor row: live, No / Yes, with its note.");
        });
        test("Weapon pages carry their attachments; the explosions have pages of their own", () =>
        {
            var liberator = StatsCatalog.Sections(StatsCatalog.Items.Single(i => i.Name == "AR-23 Liberator" && i.Nav == "primary"));
            var titles = liberator.Select(s => s.Title).ToList();
            check(!titles.Contains("Magazines") && titles.Contains("Muzzles") && titles.Contains("Underbarrels"),
                "Liberator page: " + string.Join(", ", titles));
            var magazines = liberator.Single(s => s.Title == "Ammunition").Groups.Select(g => g.Title).ToList(); /* the magazines are in Ammunition */
            check(magazines.Contains("Drum Magazine") && magazines.Contains("Short Magazine") && liberator.Single(s => s.Title == "Muzzles").Groups.Any(g => g.Title == "Compensator"),
                "Attachments by the names the armory shows: " + string.Join(", ", magazines));
            var fields = StatsSchema.Current.Targets["weapons"]["AR-23 Liberator"].Fields;
            check(fields.Where(f => f.Key.StartsWith("attachment.")).All(f => f.Value.Kind == "EntityDelta") &&
                fields.Values.GroupBy(f => f.Hd2rAddress).All(g => g.Count() == 1), "Attachment stats are entity deltas, each byte once per page.");
            var explosions = StatsSchema.Current.Targets["explosions"];
            check(explosions.Count == 241 && explosions.Values.All(e => e.Fields["explosion.inner_radius"] is { Kind: "ExplosionSettings", Offset: 16 }) &&
                explosions.Values.Any(e => e.Fields.TryGetValue("explosion.damage.standard_damage", out var d) && d is { Kind: "DamageInfo", Offset: 4 }),
                "241 explosion pages: their ExplosionSettings radii and their damage type's damage.");
            check(StatsCatalog.Items.Count(i => i.Nav == "explosions") == 241, "Each explosion is an item.");
        });
        test("Weapons that fire an entity edit what they fire: the Breacher's charge, the Maelstrom's rockets, the missiles' flight", () =>
        {
            var weapons = StatsSchema.Current.Targets["weapons"];
            var breacher = weapons["P-34 Breacher"];
            check(!breacher.Fields.Keys.Any(k => k.Split('.')[0] is "projectile" or "damage" || k.StartsWith("explosion.primary.")) &&
                  breacher.Retired.ContainsKey("projectile.velocity") && breacher.Retired.ContainsKey("explosion.primary.impact.inner_radius"),
                "Projectile 270 and its explosion 167 never reach the game: " + string.Join(", ", breacher.Fields.Keys.Where(k => k.StartsWith("projectile."))));
            check(breacher.Fields["fired.explosion"] is { Kind: "component", Component: "ExplosiveComponentData", Resource: "0x87A8A187B6FD68FD", Record: 48, Offset: 36, Shared: false } &&
                  breacher.Fields["explosion.detonation.inner_radius"] is { Kind: "ExplosionSettings", Record: 127 } &&
                  breacher.Fields["explosion.detonation.damage.standard_damage"] is { Kind: "DamageInfo", Record: 342 }, "Charge fuse and detonation 127 (damage 342).");
            var groups = StatsCatalog.Sections(StatsCatalog.Items.Single(i => i.Name == "P-34 Breacher")).SelectMany(s => s.Groups).Select(g => g.Title).ToList();
            check(groups.Contains("Breaching charge") && groups.Contains("Explosion · Detonation") && !groups.Any(g => g.StartsWith("Flight")),
                "Breacher groups: " + string.Join(", ", groups));
            var payload = EditorCatalog.Build()["objects"]!.AsArray().Single(o => o!["name"]!.GetValue<string>() == "P-34 Breacher")!["page"]!.AsArray()
                .SelectMany(c => c!["subs"]!.AsArray()).SelectMany(p => p!["rows"]!.AsArray()).Single(r => r!["key"]!.GetValue<string>() == "fired.explosion")!;
            check(payload["pick"]?.GetValue<string>() == "explosion", "The in-game editor offers the charge's payload as an explosion pick: " + payload.ToJsonString());
            // the Maelstrom's rockets explode by their own impact explosion 11, not the Commando's rows (projectile 245, explosion 148)
            foreach (var slot in new[] { "TD-110 Maelstrom / slot_3", "TD-110 Maelstrom / slot_4" })
            {
                var f = weapons[slot].Fields;
                check(!f.Values.Any(x => x.Kind == "ProjectileSettings" || x.Record is 148 or 237 or 392 && x.Kind != "component") &&
                      f["explosion.detonation.inner_radius"].Record == 11 && f["explosion.detonation.damage.standard_damage"].Record == 394 &&
                      f["fired.arming_delay"] is { Record: 26, Resource: "0xD24156BD98FFF3FE" } && !f.ContainsKey("fired.explosion") &&
                      f["missile.preferred_speed"] is { Component: "SeekingMissileComponentData", Record: 14, Offset: 76 } && f.ContainsKey("entity.health"),
                    slot + ": " + string.Join(", ", f.Keys));
            }
            // a missile flies by its SeekingMissile record; the carried row's damage and impact explosion stay
            foreach (var (name, row) in new[] { ("P-33 Missile Pistol", 152L), ("P-92 Warrant", 326L), ("EXO-45 Patriot Exosuit / left_gun", 128L) })
            {
                var f = weapons[name].Fields;
                check(!f.Values.Any(x => x.Kind == "ProjectileSettings" && x.Record == row && x.Offset is 28 or 32 or 36 or 40 or 44 or 64) &&
                      f.Values.Any(x => x.Kind == "DamageInfo") && f.Keys.Any(k => k.StartsWith("missile.")), name + ": " + string.Join(", ", f.Keys));
            }
            check(weapons["P-33 Missile Pistol"].Fields.ContainsKey("explosion.primary.impact.inner_radius") &&
                  weapons["EXO-45 Patriot Exosuit / left_gun"].Fields["missile.starting_speed"] is { Record: 12, Resource: "0xA526DC7D89FF696D", Shared: false } &&
                  weapons["FAF-14 Spear"].Fields["missile.starting_speed"].Record == 7, "The Patriot's missile fields on its own record; the Spear's unchanged.");
            // a saved edit that never applied still loads and is skipped
            var edits = StatsData.Parse("""{"format":1,"weapons":{"P-34 Breacher":{"projectile.velocity":500,"fired.explosion_delay":1}}}""", profile);
            check(edits.Count == 1 && edits[0].Offset == 12 && StatsData.Parse("""{"format":1,"weapons":{"P-34 Breacher":{"projectile.velocity":500}}}""", profile).Count == 0,
                "Retired edits are skipped, not refused.");
        });
        test("Editor values format, parse and step as the stats file stores them", () =>
        {
            var weapon = StatsSchema.Current.Targets["weapons"]["AR-23 Liberator"];
            var rpmField = weapon.Fields["weapon.fire_rate"];
            check(StatsCatalog.Format(rpmField, BitConverter.SingleToUInt32Bits(640)) == "640" && StatsCatalog.Format(rpmField, BitConverter.SingleToUInt32Bits(0.1f)) == "0.1", "Float formatting.");
            check(StatsCatalog.TryParse(rpmField, "900,5", out var value, out var bits, out _) && value!.ToJsonString() == "900.5" && bits == BitConverter.SingleToUInt32Bits(900.5f), "Comma decimal or JSON value.");
            var damage = StatsSchema.Current.Targets["damage"]["54"].Fields;
            check(!StatsCatalog.TryParse(damage["standard_damage"], "1.5", out _, out _, out _) && StatsCatalog.TryParse(damage["standard_damage"], "-3", out _, out bits, out _) && bits == unchecked((uint)-3),
                "Whole-number or signed parsing.");
            check(!StatsCatalog.TryParse(damage["ap_direct"], "-1", out _, out _, out _) && !StatsCatalog.TryParse(rpmField, "fast", out _, out _, out _), "Negative unsigned or text accepted.");
            var uses = StatsSchema.Current.Targets["stratagems"]["Orbital Precision Strike"].Fields["stratagem.max_uses"];
            check(StatsCatalog.TryParse(uses, "-1", out _, out bits, out _) && bits == uint.MaxValue && StatsCatalog.Format(uses, uint.MaxValue) == "-1", "Unlimited uses.");
            check(StatsCatalog.Step(rpmField, 640) == 10 && StatsCatalog.Step(damage["standard_damage"], 90) == 1 && StatsCatalog.Unit("degrees_per_second") == "°/s", "Steps or units.");
        });
        test("Editor drafts keep one value per shared record and save a valid stats file", () =>
        {
            StatsRow Row(string name, string key) => StatsCatalog.Sections(StatsCatalog.Items.Single(i => i.Name == name)).SelectMany(s => s.Rows).First(r => r.Key == key);
            var draft = new StatsDraft((JsonObject)JsonNode.Parse("""{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900,"spread":2}},"damage":{"54":{"armorPenetration":[1,2,3,4]}},"stratagems":{"OrbitalShell":{"cooldown":60}}}""")!);
            check(draft.Find(Row("AR-23 Liberator", "weapon.fire_rate"))?.Value.ToJsonString() == "900" && draft.Entries().Count() == 8 &&
                  draft.Document["stratagems"]!.AsObject().Single().Key != "OrbitalShell", "Aliases were not expanded.");
            var liberator = Row("AR-23 Liberator", "damage.standard_damage"); var carbine = Row("AR-23A Liberator Carbine", "damage.standard_damage");
            draft.Set(liberator, JsonValue.Create(120));
            check(draft.Find(carbine) is { Target: "AR-23 Liberator" }, "Shared record not shown on the other item.");
            draft.Set(carbine, JsonValue.Create(130));
            check(draft.Find(liberator) is { Target: "AR-23A Liberator Carbine" } && draft.Entries().Count(e => e.Key.EndsWith("standard_damage", StringComparison.Ordinal)) == 1, "Shared record stored twice.");
            StatsData.Parse(draft.Clean().ToJsonString(), profile);
            draft.SetOwn("weapons", "AR-23A Liberator Carbine", true);
            check(draft.Find(carbine) is { Category: "damage" } && draft.Own("weapons", "AR-23A Liberator Carbine"), "Own records kept a shared definition edit.");
            draft.Set(carbine, JsonValue.Create(140));
            check(draft.Find(carbine) is { Category: "damage", Value: var v } && v.ToJsonString() == "140", "A shared definition edit went into an own-records entry.");
            StatsData.Parse(draft.Clean().ToJsonString(), profile);
            draft.Set(carbine, null); draft.SetOwn("weapons", "AR-23A Liberator Carbine", false);
            check(draft.Find(liberator) == null && draft.Clean()["weapons"]?["AR-23A Liberator Carbine"] == null, "Clearing left a value or an own-only entry.");
        });
        test("Editor defaults: the newest snapshot under other mods' values, in mod order", () =>
        {
            var field = StatsSchema.Current.Targets["weapons"]["AR-23 Liberator"].Fields["weapon.fire_rate"];
            var game = new StatsObservation("x", DateTime.UtcNow, new Dictionary<string, uint> { [field.AddressKey] = BitConverter.SingleToUInt32Bits(640) });
            ModManifest Mod(string id, int value) => new(id, id.ToUpperInvariant(), "1", "", true, root, [], [], []) { Stats = StatsData.Parse(rpm.Replace("900", value.ToString()), profile) };
            var baseline = StatsBaseline.From(game, [Mod("b", 800), Mod("a", 700), Mod(StatsEditorData.ModId, 900)]);
            check(baseline.Mod(field, false) is { Mod: "B" } && baseline.Base(field, false) == BitConverter.SingleToUInt32Bits(800), "Later mod should win; the editor's own mod is not a default.");
            check(StatsBaseline.From(game, []).Base(field, false) == BitConverter.SingleToUInt32Bits(640), "Game value missing.");
        });
        if (Environment.GetEnvironmentVariable("HELLFORGE_STATS_TEST_EXE") is { Length: > 0 } nativeFixture)
            test("Native loader consumes the complete Core plan and Core reads its snapshot", () =>
            {
                var session = Path.Combine(root, "native-stats-session"); Directory.CreateDirectory(session);
                var plan = Path.Combine(session, "plan.tsv");
                File.WriteAllText(plan, ModLoaderPlan.Text([], session, profile, "25480438"));
                var start = new System.Diagnostics.ProcessStartInfo(nativeFixture) { UseShellExecute = false, CreateNoWindow = true };
                start.ArgumentList.Add(plan); start.ArgumentList.Add(session);
                using var process = System.Diagnostics.Process.Start(start)!;
                check(process.WaitForExit(30000) && process.ExitCode == 0, "Native fixture failed to read the full plan.");
                var snapshot = StatsEditorData.ReadSnapshot(Path.Combine(session, "stats-values.tsv"));
                var damage = StatsSchema.Current.Targets["damage"]["54"].Fields["standard_damage"];
                check(snapshot.Value(damage) == "150" && snapshot.Values.Count == 1, "Native snapshot did not round-trip typed values/unavailable probes.");
            });
    }
}
