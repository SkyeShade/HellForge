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
        check(progress.Any(e => (uint)e!["id32"]! == (uint)mastery[0]["id32"]! && (int)e["level"]! == 7), "The registered weapon does not follow its base's level.");

        var held = JsonSerializer.SerializeToElement(new { items = new object[] { new { mixId = baseChildren[0], amount = 1, isDiscovered = true } } });
        var inventory = JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(Gold) && inventory.Contains(goldChildren[0]), "The inventory lacks the weapon or its copy of a held child.");

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
    }
}
