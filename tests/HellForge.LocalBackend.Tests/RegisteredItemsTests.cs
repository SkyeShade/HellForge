using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.LocalBackend;

// A weapon a code mod registers (the gold Liberator) gets its own children, mastery package, progress and inventory.
static class RegisteredItemsTests
{
    const uint Liberator = 0xAA035309, Gold = 0x32895C2A;

    public static void Run(string root, Action<bool, string> check)
    {
        // The client's child hash, pinned to real catalogue data: Liberator child 2309820280 is attachment 3441477456.
        check(RegisteredItems.WeaponChild(3441477456, Liberator) == 2309820280, "Weapon child hash differs from the client's.");
        check(RegisteredItems.Attachment(2309820280, Liberator) == 3441477456, "Weapon child hash does not invert.");
        // The mod loader names the same package in the weapon's customization record (registry.c Rehome): 2800332670.
        check(RegisteredItems.PackageFor(3391943729, Liberator, Gold) == 2800332670, "Mastery package id differs from the mod loader's.");
        // Read where the mod loader writes them: the run, or a sandboxed game's run/game (its only writable folder)
        var run = Path.Combine(root, "run");
        check(RegisteredItems.PathFor(Path.Combine(run, "backend"), sandboxed: false) == Path.Combine(Path.GetFullPath(run), "registered-items.json") &&
              RegisteredItems.PathFor(Path.Combine(run, "backend"), sandboxed: true) == Path.Combine(Path.GetFullPath(run), "game", "registered-items.json"),
            "Registered items are read from the run, or sandboxed from its game folder.");

        var fixtures = LocalBackend.LoadStartupFixtures();
        var folder = Path.Combine(root, "registered-items");
        Directory.CreateDirectory(folder);
        var file = Path.Combine(folder, "registered-items.json");
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"}]}""");
        var items = new RegisteredItems(file) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null };

        var catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var baseChildren = catalogue.OfType<JsonObject>().Where(i => (uint?)i["parentId"] == Liberator).Select(i => (uint)i["mixId"]!).ToList();
        var at = catalogue.Select((item, index) => (item, index)).First(x => (uint)x.item!["mixId"]! == Gold).index;
        var goldChildren = catalogue.Skip(at + 1).TakeWhile(i => (uint?)i!["parentId"] == Gold).Select(i => (uint)i!["mixId"]!).ToList();
        check(baseChildren.Count > 0 && goldChildren.Count == baseChildren.Count, "The registered weapon's children were not copied after it.");
        check(goldChildren.SequenceEqual(baseChildren.Select(c => RegisteredItems.Rehome(c, Liberator, Gold))), "Children are not re-hashed under the weapon.");
        check(catalogue.Skip(at + 1).Take(goldChildren.Count).All(i => (uint)i!["itemId"]! == RegisteredItems.Attachment((uint)i["mixId"]!, Gold)),
            "A copied child's itemId is not its attachment (the client's attachment list matches on it).");

        var packages = (JsonArray)JsonNode.Parse(items.Apply("/Progression/ProgressionPackages", fixtures["/Progression/ProgressionPackages"]).GetRawText())!;
        var mastery = packages.OfType<JsonObject>().Where(p => (int)p["type"]! == 2 && ((JsonArray)p["requiredItems"]!).Any(r => (uint)r! == Gold)).ToList();
        check(mastery.Count == 1, "The registered weapon has no mastery package.");
        var unlocks = mastery[0]["levels"]!.AsArray().SelectMany(l => l!["unlockableItems"]!.AsArray()).Select(u => (uint)u!["mixId"]!).ToList();
        check(unlocks.Count > 0 && unlocks.All(goldChildren.Contains), "The mastery package does not unlock the weapon's own children.");

        var basePackage = packages.OfType<JsonObject>().First(p => (int)p["type"]! == 2 && ((JsonArray)p["requiredItems"]!).Any(r => (uint)r! == Liberator));
        var progression = JsonSerializer.SerializeToElement(new { experience = 1, packageProgress = new[] { new { id32 = (uint)basePackage["id32"]!, level = 7, progress = 3, sortOrder = 0, claimedLevels = Array.Empty<int>() } } });
        var progress = JsonNode.Parse(items.Apply("/Progression", progression).GetRawText())!["packageProgress"]!.AsArray();
        check(!progress.Any(e => (uint)e!["id32"]! == (uint)mastery[0]["id32"]!), "The registered weapon took its base's level: a whole copy starts at the lowest.");

        var held = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = baseChildren[0], amount = 1, isDiscovered = true } } });
        var inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(Gold) && !inventory.Contains(goldChildren[0]), "The inventory lacks the weapon, or mirrors what its base holds.");

        // An attachment a mod adds (attachments.c), a copy of Liberator attachment 3441477456 offered on the gold one: its
        // child of the gold Liberator in that weapon's run, held with the weapon; a weapon it is not offered on gets none.
        const uint Drum = 0x0D12A001;
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"}],"attachments":[{"mixId":{{Drum}},"copyOf":3441477456,"weapons":[{{Gold}}]}]}""");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        at = catalogue.Select((item, index) => (item, index)).First(x => (uint)x.item!["mixId"]! == Gold).index;
        var goldRun = catalogue.Skip(at + 1).TakeWhile(i => (uint?)i!["parentId"] == Gold).OfType<JsonObject>().ToList();
        var drumChild = goldRun.SingleOrDefault(i => (uint)i["itemId"]! == Drum);
        check(drumChild != null && (uint)drumChild["mixId"]! == RegisteredItems.WeaponChild(Drum, Gold) && goldRun.Count == baseChildren.Count + 1 &&
            !catalogue.OfType<JsonObject>().Any(i => (uint)i["itemId"]! == Drum && (uint?)i["parentId"] != Gold),
            "The attachment is not a child of the gold Liberator alone, in its run.");
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(RegisteredItems.WeaponChild(Drum, Gold)), "The attachment's child is not held with the weapon.");

        // A stratagem a code mod adds (stratagems.c): a copy of the Orbital Airburst Strike's item (stable id 1560416221)
        // and its child, the child's itemId being the new stable id. An entry whose base is not known yet waits.
        const uint Strike = 1560416221, Item = 0x1234ABCD, Stable = 0x0BADF00D;
        File.WriteAllText(file, $$"""{"format":1,"items":[{"kind":"stratagem","mixId":{{Item}},"stable":{{Stable}},"copyOfType":83,"copyOf":{{Strike}},"mod":"test","name":"test/s"},{"kind":"stratagem","mixId":7,"stable":8,"copyOfType":83,"copyOf":0,"mod":"test","name":"test/later"}]}""");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var baseChild = catalogue.OfType<JsonObject>().First(i => (int)i["progressionCategory"]! == 10 && (uint)i["itemId"]! == Strike);
        var newItem = catalogue.OfType<JsonObject>().SingleOrDefault(i => (uint)i["mixId"]! == Item);
        var newChild = catalogue.OfType<JsonObject>().SingleOrDefault(i => (uint?)i["parentId"] == Item);
        check(newItem != null && (int)newItem["progressionCategory"]! == 11 && (uint)newItem["itemId"]! == Item, "The stratagem's item was not added.");
        check(newChild != null && (uint)newChild["itemId"]! == Stable && (uint)newChild["mixId"]! == RegisteredItems.WeaponChild(Stable, Item) &&
            newChild["tags"]!.ToJsonString() == baseChild["tags"]!.ToJsonString(), "The stratagem's child is not its base's under the new stable id.");
        check(!catalogue.OfType<JsonObject>().Any(i => (uint)i["mixId"]! == 7), "A stratagem whose base is not known yet was added.");
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(Item), "The inventory lacks the stratagem.");

        // Difficulty 11 (difficulty.c): its unlock item is a copy of Super Helldive's, held once Super Helldive is.
        const uint SuperHelldive = 2695783738, HellForge = 0x5EED0011;
        File.WriteAllText(file, $$"""{"format":1,"items":[{"kind":"difficulty","mixId":{{HellForge}},"copyOf":{{SuperHelldive}},"level":11,"mod":"test","name":"test/hell"}]}""");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var unlock = catalogue.OfType<JsonObject>().SingleOrDefault(i => (uint)i["mixId"]! == HellForge);
        var superUnlock = catalogue.OfType<JsonObject>().Single(i => (uint)i["mixId"]! == SuperHelldive);
        check(unlock != null && (uint)unlock["itemId"]! == HellForge && (int)unlock["progressionCategory"]! == (int)superUnlock["progressionCategory"]!,
            "The difficulty's unlock item is not a copy of Super Helldive's.");
        check(!catalogue.OfType<JsonObject>().Any(i => (uint?)i["parentId"] == HellForge), "The difficulty's unlock item got children.");
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(!inventory.Contains(HellForge), "Difficulty 11 was unlocked before Super Helldive.");
        var veteran = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = SuperHelldive, amount = 1, isDiscovered = true } } });
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", veteran).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(HellForge), "Difficulty 11 is not unlocked with Super Helldive.");
        var client = JsonNode.Parse(items.Apply("/Configuration/GameClient", fixtures["/Configuration/GameClient"]).GetRawText())!;
        var cap = client["onlineOverrideConfiguration"]!.AsArray().OfType<JsonObject>().Single(o => (uint)o["mixId"]! == RegisteredItems.DifficultyCapOverride);
        check((string)cap["data"]! == "11", "The live game's difficulty cap of 10 still applies.");
        check(MissionService.Rewards.Requisition(11) > MissionService.Rewards.Requisition(10) &&
            MissionService.Rewards.Experience(11, true, 0) > MissionService.Rewards.Experience(10, true, 0), "Difficulty 11 pays no more than 10.");

        // The client drops catalogue items past its capacity, so a weapon registered first (as mods load) still goes in
        // after the difficulty and the stratagem: a large weapon pack loses its last weapons, not level 11 or a stratagem.
        const uint Second = 0x32895C2B;
        File.WriteAllText(file, $$"""
            {"format":1,"items":[
              {"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"},
              {"kind":"stratagem","mixId":{{Item}},"stable":{{Stable}},"copyOfType":83,"copyOf":{{Strike}},"mod":"test","name":"test/s"},
              {"mixId":{{Second}},"copyOf":{{Liberator}},"mod":"test","unit":"test/second"},
              {"kind":"difficulty","mixId":{{HellForge}},"copyOf":{{SuperHelldive}},"level":11,"mod":"test","name":"test/hell"}]}
            """);
        check(items.Load().Select(i => i.MixId).SequenceEqual([Gold, Item, Second, HellForge]) &&
            RegisteredItems.Prioritized(items.Load()).Select(i => i.MixId).SequenceEqual([HellForge, Item, Gold, Second]),
            "Difficulties, then stratagems, then weapons, each kind in the order it registered.");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        int Index(uint mixId) => catalogue.Select((item, index) => (item, index)).Single(x => (uint)x.item!["mixId"]! == mixId).index;
        check(Index(HellForge) < Index(Item) && Index(Item) < Index(Gold) && Index(Gold) < Index(Second) &&
            Index(RegisteredItems.WeaponChild(Stable, Item)) == Index(Item) + 1 && (uint?)catalogue[Index(Gold) + 1]!["parentId"] == Gold,
            "In the catalogue: level 11, the stratagem and its child, then each weapon with its children.");

        // The game keeps 4,096 catalogue items (research\catalogue-cap): 25 Liberator copies registered before a stratagem
        // fit only partly. The stratagem goes in, and as many copies as fit whole, each with every child; the rest are
        // logged, never cut through.
        var log = new List<string>();
        var full = new RegisteredItems(file) { Source = items.Source, Log = log.Add };
        var copies = Enumerable.Range(0, 25).Select(i => 0x40000000u + (uint)i).ToList();
        File.WriteAllText(file, "{\"format\":1,\"items\":[" + string.Join(",", copies.Select(c => $$"""{"mixId":{{c}},"copyOf":{{Liberator}},"mod":"test.pack","unit":"test/{{c}}"}""")) +
            $$""",{"kind":"stratagem","mixId":{{Item}},"stable":{{Stable}},"copyOfType":83,"copyOf":{{Strike}},"mod":"test","name":"test/s"}]}""");
        var vanillaCount = fixtures["/Progression/items"].GetArrayLength();
        var fitting = (RegisteredItems.CatalogueCap - vanillaCount - 2) / (baseChildren.Count + 1);
        catalogue = (JsonArray)JsonNode.Parse(full.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var added = copies.Where(c => catalogue.Any(i => (uint)i!["mixId"]! == c)).ToList();
        check(fitting is > 0 and < 25 && catalogue.Count <= RegisteredItems.CatalogueCap && added.SequenceEqual(copies.Take(fitting)) &&
            catalogue.Any(i => (uint)i!["mixId"]! == Item), $"The stratagem and the first {fitting} copies fit ({added.Count} added, {catalogue.Count} items).");
        check(added.All(c => catalogue.Count(i => (uint?)i!["parentId"] == c) == baseChildren.Count), "Every copy that went in has all of its children.");
        check(log.Count == 25 - fitting && log.All(l => l.StartsWith("test.pack's weapon ", StringComparison.Ordinal) && l.Contains("4096 catalogue items")),
            "Each copy left out is logged once: " + string.Join(" | ", log.Take(2)));
        full.Apply("/Progression/items", fixtures["/Progression/items"]);
        check(log.Count == 25 - fitting, "Asked again, nothing is logged twice.");
        var heldAll = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = baseChildren[0], amount = 1, isDiscovered = true } } });
        inventory = JsonNode.Parse(full.Apply("/Progression/inventory", heldAll).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(copies[0]) && !inventory.Contains(copies[^1]) && inventory.Contains(Item), "Only the copies in the catalogue are held.");

        // 4,096 inventory entries: a full inventory takes nothing more, and says so.
        var crowded = JsonSerializer.SerializeToElement(new { items = Enumerable.Range(1, RegisteredItems.InventoryCap).Select(i => new { mixId = (uint)i, amount = 1, isDiscovered = true }) });
        log.Clear();
        var fullHeld = JsonNode.Parse(full.Apply("/Progression/inventory", crowded).GetRawText())!["items"]!.AsArray();
        check(fullHeld.Count == RegisteredItems.InventoryCap && log.Any(l => l.Contains("4096 inventory entries")), "A full inventory takes nothing more: " + string.Join(" | ", log.Take(1)));

        // 256 progression packages: with none left, the weapons stay out (a copy needs its mastery package); the stratagem
        // needs none.
        var packed = (JsonArray)JsonNode.Parse(fixtures["/Progression/ProgressionPackages"].GetRawText())!;
        while (packed.Count < RegisteredItems.PackageCap) packed.Add((JsonNode?)new JsonObject { ["id32"] = 0x7000000u + (uint)packed.Count, ["type"] = 1 });
        var packedElement = JsonSerializer.SerializeToElement(packed);
        log.Clear();
        var noPackages = new RegisteredItems(file) { Source = path => path == "/Progression/ProgressionPackages" ? packedElement : items.Source!(path), Log = log.Add };
        catalogue = (JsonArray)JsonNode.Parse(noPackages.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        check(!copies.Any(c => catalogue.Any(i => (uint)i!["mixId"]! == c)) && catalogue.Any(i => (uint)i!["mixId"]! == Item) &&
            log.Count == 25 && log.All(l => l.Contains("256 progression packages")), "No package left: no weapon copy, the stratagem still: " + string.Join(" | ", log.Take(1)));

        // The loader moved the game's arrays (native/mod-loader/state_moves.c) and says so: every copy goes in, past 4,096
        // items; inventory and packages follow.
        string Pack(string capacity) => "{\"format\":1," + capacity + "\"items\":[" + string.Join(",", copies.Select(c => $$"""{"mixId":{{c}},"copyOf":{{Liberator}},"mod":"test.pack","unit":"test/{{c}}"}""")) + "]}";
        const string Moved = "\"capacity\":{\"items\":40960,\"packages\":1024},";
        var boundary = (RegisteredItems.PackageCap - fixtures["/Progression/ProgressionPackages"].GetArrayLength()) + 5;
        copies = [.. Enumerable.Range(0, Math.Max(boundary, 25)).Select(i => 0x40000000u + (uint)i)];
        JsonArray Served(RegisteredItems registered, string path) => (JsonArray)JsonNode.Parse(registered.Apply(path, fixtures[path]).GetRawText())!;
        File.WriteAllText(file, Pack(Moved));
        log.Clear();
        var moved = new RegisteredItems(file) { Source = items.Source, Log = log.Add };
        catalogue = Served(moved, "/Progression/items");
        var movedPackages = Served(moved, "/Progression/ProgressionPackages");
        check(catalogue.Count == vanillaCount + copies.Count * (baseChildren.Count + 1) && catalogue.Count > RegisteredItems.CatalogueCap && log.Count == 0 &&
            movedPackages.Count > RegisteredItems.PackageCap, $"Moved: every copy is served whole ({catalogue.Count} items, {movedPackages.Count} packages): " + string.Join(" | ", log.Take(1)));
        var crowdedOld = JsonSerializer.SerializeToElement(new { items = Enumerable.Range(1, RegisteredItems.InventoryCap).Select(i => new { mixId = (uint)i, amount = 1, isDiscovered = true }) });
        var heldMoved = JsonNode.Parse(moved.Apply("/Progression/inventory", crowdedOld).GetRawText())!["items"]!.AsArray();
        check(heldMoved.Count == RegisteredItems.InventoryCap + copies.Count && log.Count == 0, "Moved: the inventory holds past 4,096 entries.");
        // A move refused says nothing: the session stays at what the game compiled, and so does any word it cannot trust.
        foreach (var refused in new[] { "", "\"capacity\":{\"items\":100,\"packages\":1024},", "\"capacity\":{\"items\":40960},", "\"capacity\":7," })
        {
            File.WriteAllText(file, Pack(refused));
            var stayed = new RegisteredItems(file) { Source = items.Source, Log = _ => { } };
            check(Served(stayed, "/Progression/items").Count <= RegisteredItems.CatalogueCap && Served(stayed, "/Progression/ProgressionPackages").Count <= RegisteredItems.PackageCap,
                $"No move, no more than the game compiled: '{refused}'.");
        }
        // The word comes after weapons registered (the game has not asked yet): the file's change is what counts.
        File.WriteAllText(file, Pack(""));
        var late = new RegisteredItems(file) { Source = items.Source, Log = _ => { } };
        var before = late.Load().Count;
        File.WriteAllText(file, Pack(Moved));
        check(late.Load().Count == before && Served(late, "/Progression/items").Count > RegisteredItems.CatalogueCap, "A loader's word that comes later counts.");

        Standalone(folder, fixtures, check);
    }

    // A standalone weapon names no base: its item, children and mastery package are authored from its entry, beside a copy.
    static void Standalone(string folder, Dictionary<string, JsonElement> fixtures, Action<bool, string> check)
    {
        const uint Rifle = 0x5A0D0001, Package = 0x5A0D0F01, Sight = 0x5A0D0A01, Brake = 0x5A0D0A02, Drum = 0x5A0D0A03, Far = 0x5A0D0A04;
        var file = Path.Combine(folder, "standalone.json");
        File.WriteAllText(file, $$"""
            {"format":1,"items":[
              {"mixId":{{Rifle}},"standalone":true,"class":"primary","mod":"test.rifle","unit":"test/rifle","package":{{Package}}},
              {"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"}],
             "attachments":[
              {"mixId":{{Sight}},"standalone":true,"weapons":[{{Rifle}}],"level":0},
              {"mixId":{{Brake}},"standalone":true,"weapons":[{{Rifle}},{{Gold}}],"level":1},
              {"mixId":{{Drum}},"standalone":true,"weapons":[{{Rifle}}],"level":5},
              {"mixId":{{Far}},"standalone":true,"weapons":[{{Rifle}}],"level":26}]}
            """);
        var log = new List<string>();
        // Every base refused: a standalone weapon has none, so no premium rule applies to it.
        var items = new RegisteredItems(file) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = log.Add, OwnsBase = _ => false };
        var vanillaCatalogue = fixtures["/Progression/items"];
        check(!vanillaCatalogue.EnumerateArray().Any(i => i.GetProperty("mixId").GetUInt32() is Rifle or Package), "The test ids are vanilla ones.");

        var catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", vanillaCatalogue).GetRawText())!;
        var at = catalogue.Select((item, index) => (item, index)).Single(x => (uint)x.item!["mixId"]! == Rifle).index;
        // The class's tags are those every vanilla weapon (a category 1 item a type 2 package names) shares; the game's differ.
        var weapons = fixtures["/Progression/ProgressionPackages"].EnumerateArray().Where(p => p.GetProperty("type").GetInt32() == 2)
            .SelectMany(p => p.GetProperty("requiredItems").EnumerateArray().Select(r => r.GetUInt32())).ToHashSet();
        var shared = vanillaCatalogue.EnumerateArray().Where(i => weapons.Contains(i.GetProperty("mixId").GetUInt32()) && !i.TryGetProperty("parentId", out _))
            .Select(i => i.GetProperty("tags").GetRawText()).Distinct().ToList();
        var tags = shared.Count == 1 ? JsonNode.Parse(shared[0])!.ToJsonString() : "[]";
        check(shared.Count == 1 || log.Count(l => l.Contains("do not share one set of tags")) == 1, "Tags the vanilla weapons do not share were not logged.");
        check(catalogue[at]!.ToJsonString() == $$"""{"itemId":{{Rifle}},"mixId":{{Rifle}},"isConsumable":false,"requiredLevel":0,"progressionCategory":1,"tags":{{tags}},"requiredItems":[],"buyPrice":[],"sellPrice":[]}""",
            "The standalone weapon's item is not the authored one: " + catalogue[at]!.ToJsonString());
        var run = catalogue.Skip(at + 1).TakeWhile(i => (uint?)i!["parentId"] == Rifle).OfType<JsonObject>().ToList();
        check(run.Count == 4 && run.Select(c => (uint)c["itemId"]!).SequenceEqual([Sight, Brake, Drum, Far]) &&
            run.All(c => (uint)c["mixId"]! == RegisteredItems.WeaponChild((uint)c["itemId"]!, Rifle) && (int)c["progressionCategory"]! == 2 &&
                c["buyPrice"]!.AsArray().Count == 0 && c["tags"]!.AsArray().Count == 0),
            "The standalone weapon's run is not one authored child per standalone attachment.");
        check(!catalogue.OfType<JsonObject>().Any(i => (uint)i["itemId"]! == Brake && (uint?)i["parentId"] == Gold),
            "A standalone attachment became a child of a copy.");
        var goldAt = catalogue.Select((item, index) => (item, index)).Single(x => (uint)x.item!["mixId"]! == Gold).index;
        check(catalogue.Skip(goldAt + 1).TakeWhile(i => (uint?)i!["parentId"] == Gold).Count() == catalogue.Count(i => (uint?)i!["parentId"] == Liberator),
            "The copy beside the standalone weapon lost its base's children.");

        var packages = (JsonArray)JsonNode.Parse(items.Apply("/Progression/ProgressionPackages", fixtures["/Progression/ProgressionPackages"]).GetRawText())!;
        var own = packages.OfType<JsonObject>().SingleOrDefault(p => (uint)p["id32"]! == Package);
        check(own != null && (int)own["type"]! == 2 && own["requiredItems"]!.ToJsonString() == $"[{Rifle}]", "The standalone weapon has no package of its own.");
        var vanillaLevels = packages.OfType<JsonObject>().First(p => (int)p["type"]! == 2 && (uint)p["id32"]! != Package)["levels"]!.AsArray();
        var levels = own!["levels"]!.AsArray().OfType<JsonObject>().ToList();
        check(levels.Select(l => (l["level"]!.ToJsonString(), l["requirementType"]!.ToJsonString(), l["requirement"]!.ToJsonString()))
                .SequenceEqual(vanillaLevels.Select(l => (l!["level"]!.ToJsonString(), l["requirementType"]!.ToJsonString(), l["requirement"]!.ToJsonString()))) &&
            levels.All(l => l["cost"]!.AsArray().Count == 0 && l["unlockableItems"]!.AsArray().Count == 0),
            "The standalone package's levels are not the vanilla weapons' shared ones.");
        string Rewards(int level) => levels.Single(l => (int)l["level"]! == level)["rewards"]!.ToJsonString();
        check(Rewards(1) == $$"""[{"type":1,"id32":{{RegisteredItems.WeaponChild(Brake, Rifle)}},"amount":1}]""" &&
            Rewards(5) == $$"""[{"type":1,"id32":{{RegisteredItems.WeaponChild(Drum, Rifle)}},"amount":1}]""" &&
            levels.Where(l => (int)l["level"]! is not (1 or 5)).All(l => l["rewards"]!.AsArray().Count == 0),
            "The standalone attachments are not their levels' rewards.");
        check(log.Any(l => l.Contains($"attachment {Far} unlocks at level 26")), "An attachment past the last level was not logged.");
        check(packages.Count(p => ((JsonArray)p!["requiredItems"]!).Any(r => (uint)r! == Gold)) == 1, "The copy beside it has no mastery package.");

        var inventory = JsonNode.Parse(items.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = Array.Empty<object>() })).GetRawText())!["items"]!
            .AsArray().Select(i => (uint)i!["mixId"]!).ToHashSet();
        check(inventory.Contains(Rifle) && inventory.Contains(RegisteredItems.WeaponChild(Sight, Rifle)) &&
            !inventory.Contains(RegisteredItems.WeaponChild(Brake, Rifle)) && !inventory.Contains(RegisteredItems.WeaponChild(Drum, Rifle)),
            "The standalone weapon is not held with only its level 0 attachment, despite a premium rule that refuses every base.");
        check(!inventory.Contains(Gold), "The copy escaped its base's premium rule.");

        // Its level 1 passes at the first mission with XP and gives the level 1 attachment, as a vanilla weapon's defaults.
        var track = items.Track(Package);
        var progression = new JsonObject();
        if (track != null) WeaponMastery.Earn(track, progression, [], 100);
        var earned = track != null ? WeaponMastery.Owned(track, track.Entry(progression)) : [];
        check(track is { IsMod: true } && earned.SetEquals([RegisteredItems.WeaponChild(Brake, Rifle)]), "A first mission does not give the level 1 attachment.");

        // A mod that locks its premium list still locks a standalone weapon; another class or no package is malformed.
        var locked = new RegisteredItems(file) { Source = items.Source, Log = _ => { }, LockedMods = new HashSet<string> { "test.rifle" } };
        check(locked.IsLocked(locked.Served().Single(i => i.MixId == Rifle)), "A mod's own premium list does not lock its standalone weapon.");
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Rifle}},"standalone":true,"class":"support","package":{{Package}}},{"mixId":{{Rifle + 1}},"standalone":true,"class":"primary"}]}""");
        check(new RegisteredItems(file) { Source = items.Source }.Load().Count == 0, "A standalone entry with another class or no package was read.");
        // A package id the game already uses would share its progress: refused and logged.
        var taken = fixtures["/Progression/ProgressionPackages"].EnumerateArray().First().GetProperty("id32").GetUInt32();
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Rifle}},"standalone":true,"class":"primary","mod":"test.rifle","package":{{taken}}}]}""");
        log.Clear();
        var clash = new RegisteredItems(file) { Source = items.Source, Log = log.Add };
        check(clash.Served().Count == 0 && log.Any(l => l.Contains("another package's id")), "A standalone weapon took a vanilla package's id.");

        // Two standalone weapons (two mods, or one mod's two) are both served, each with the class's tags.
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Rifle}},"standalone":true,"class":"primary","mod":"test.rifle","package":{{Package}}},{"mixId":{{Rifle + 1}},"standalone":true,"class":"primary","mod":"test.other","package":{{Package + 1}}}]}""");
        var pair = new RegisteredItems(file) { Source = items.Source, Log = _ => { } };
        var both = (JsonArray)JsonNode.Parse(pair.Apply("/Progression/items", vanillaCatalogue).GetRawText())!;
        check(both.OfType<JsonObject>().Count(i => (uint)i["mixId"]! is Rifle or Rifle + 1 && i["tags"]!.ToJsonString() == tags) == 2,
            "Two standalone weapons are not both served with the class's tags.");
    }
}
