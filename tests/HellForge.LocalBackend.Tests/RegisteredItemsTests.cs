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

        Patterns(file, fixtures, check);

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
        const uint SuperHelldive = 2695783738;
        var Hell = HellForge.Core.EngineHash.Id32("test/hell#item");
        File.WriteAllText(file, $$"""{"format":1,"items":[{"kind":"difficulty","mixId":{{Hell}},"copyOf":{{SuperHelldive}},"level":11,"mod":"test","name":"test/hell"}]}""");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var unlock = catalogue.OfType<JsonObject>().SingleOrDefault(i => (uint)i["mixId"]! == Hell);
        var superUnlock = catalogue.OfType<JsonObject>().Single(i => (uint)i["mixId"]! == SuperHelldive);
        check(unlock != null && (uint)unlock["itemId"]! == Hell && (int)unlock["progressionCategory"]! == (int)superUnlock["progressionCategory"]!,
            "The difficulty's unlock item is not a copy of Super Helldive's.");
        check(!catalogue.OfType<JsonObject>().Any(i => (uint?)i["parentId"] == Hell), "The difficulty's unlock item got children.");
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(!inventory.Contains(Hell), "Difficulty 11 was unlocked before Super Helldive.");
        var veteran = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = SuperHelldive, amount = 1, isDiscovered = true } } });
        inventory = JsonNode.Parse(items.Apply("/Progression/inventory", veteran).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(Hell), "Difficulty 11 is not unlocked with Super Helldive.");
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
              {"kind":"difficulty","mixId":{{Hell}},"copyOf":{{SuperHelldive}},"level":11,"mod":"test","name":"test/hell"}]}
            """);
        check(items.Load().Select(i => i.MixId).SequenceEqual([Gold, Item, Second, Hell]) &&
            RegisteredItems.Prioritized(items.Load()).Select(i => i.MixId).SequenceEqual([Hell, Item, Gold, Second]),
            "Difficulties, then stratagems, then weapons, each kind in the order it registered.");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        int Index(uint mixId) => catalogue.Select((item, index) => (item, index)).Single(x => (uint)x.item!["mixId"]! == mixId).index;
        check(Index(Hell) < Index(Item) && Index(Item) < Index(Gold) && Index(Gold) < Index(Second) &&
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
    }
    // Patterns a mod adds (patterns.c): a weapon pattern, a copy of Arctic Camo (1561483157) offered on every weapon
    // offering it, gets a child at the end of each weapon's run (the gold Liberator's too), a level 1 reward in each
    // mastery package and is held with its weapon; a unit pattern, a copy of the Pelican's Forest Camo (3520082317), is a
    // copy of its catalogue item right after it under the Pelican, always held. A base the catalogue does not have is said.
    static void Patterns(string file, Dictionary<string, JsonElement> fixtures, Action<bool, string> check)
    {
        const uint Tiger = 0x7163E501, PelicanTiger = 0x7163E502, ArcticCamo = 1561483157, PelicanCamo = 3520082317, Pelican = 2057541119, Unknown = 0x0BAD0001;
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"}],"attachments":[],"patterns":[{"mixId":{{Tiger}},"copyOf":{{ArcticCamo}},"kind":"weapon","weapons":[]},{"mixId":{{PelicanTiger}},"copyOf":{{PelicanCamo}},"kind":"unit"},{"mixId":7,"copyOf":{{Unknown}},"kind":"unit"}]}""");
        var log = new List<string>();
        var items = new RegisteredItems(file) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = log.Add };
        var catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var entries = catalogue.OfType<JsonObject>().ToList();
        var offering = entries.Where(e => (uint?)e["itemId"] == ArcticCamo && e["parentId"] != null).Select(e => (uint)e["parentId"]!).Distinct().ToList();
        var tigers = entries.Where(e => (uint?)e["itemId"] == Tiger).ToList();
        check(offering.Contains(Liberator) && offering.Contains(Gold) && tigers.Count == offering.Count &&
            tigers.All(t => (uint)t["mixId"]! == RegisteredItems.WeaponChild(Tiger, (uint)t["parentId"]!)), $"The weapon pattern is not a child of each of the {offering.Count} weapons offering its base.");
        foreach (var weapon in new[] { Liberator, Gold })
        {
            var at = entries.FindIndex(e => (uint?)e["mixId"] == RegisteredItems.WeaponChild(Tiger, weapon));
            var run = entries.FindLastIndex(e => (uint?)e["parentId"] == weapon);
            check(at == run, $"Weapon {weapon}'s pattern child is not at the end of its run.");
        }
        var skinAt = entries.FindIndex(e => (uint?)e["itemId"] == PelicanCamo && (int?)e["progressionCategory"] == 4);
        var pelicanTiger = RegisteredItems.WeaponChild(PelicanTiger, Pelican);
        check(skinAt >= 0 && (uint?)entries[skinAt + 1]["itemId"] == PelicanTiger && (uint?)entries[skinAt + 1]["mixId"] == pelicanTiger &&
            (int?)entries[skinAt + 1]["progressionCategory"] == 4, "The unit pattern is not a copy of its base skin right after it.");
        check(!entries.Any(e => (uint?)e["itemId"] == 7) && log.Any(l => l.Contains($"the catalogue has no skin {Unknown}")), "A pattern of an unknown skin was served, or not said.");

        var packages = (JsonArray)JsonNode.Parse(items.Apply("/Progression/ProgressionPackages", fixtures["/Progression/ProgressionPackages"]).GetRawText())!;
        foreach (var weapon in new[] { Liberator, Gold })
        {
            var mastery = packages.OfType<JsonObject>().First(p => (int)p["type"]! == 2 && ((JsonArray)p["requiredItems"]!).Any(r => (uint)r! == weapon));
            var first = mastery["levels"]!.AsArray().OfType<JsonObject>().First(l => (int)l["level"]! == 1);
            check(first["rewards"]!.AsArray().Count(r => (int)r!["type"]! == 1 && (uint)r["id32"]! == RegisteredItems.WeaponChild(Tiger, weapon)) == 1,
                $"Weapon {weapon}'s mastery package does not reward its pattern child at level 1, once.");
        }

        var held = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = Liberator, amount = 1, isDiscovered = true } } });
        var inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(RegisteredItems.WeaponChild(Tiger, Liberator)) && inventory.Contains(RegisteredItems.WeaponChild(Tiger, Gold)) &&
            inventory.Contains(pelicanTiger), "The patterns are not held with their weapons, or the unit pattern is not held.");
        check(offering.Where(w => w != Liberator && w != Gold).All(w => !inventory.Contains(RegisteredItems.WeaponChild(Tiger, w))), "A pattern is held on a weapon the player does not hold.");

        // named weapons: only those, and one that does not offer the base is said
        File.WriteAllText(file, $$"""{"format":1,"items":[],"patterns":[{"mixId":{{Tiger}},"copyOf":{{ArcticCamo}},"kind":"weapon","weapons":[{{Liberator}},{{Unknown}}]}]}""");
        log.Clear();
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        check(catalogue.OfType<JsonObject>().Where(e => (uint?)e["itemId"] == Tiger).Select(e => (uint)e["parentId"]!).SequenceEqual([Liberator]) &&
            log.Any(l => l.Contains($"weapon {Unknown} does not offer")), "A pattern named on weapons is not on those alone.");
    }
}
