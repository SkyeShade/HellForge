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

        Limits(folder, fixtures, check);
    }

    // The client keeps 4096 catalogue items and 4096 owned items (RegisteredItems' limits). The stress test mod's 64
    // Liberator copies (57 items each) and 64 stratagems (2 each) are more than fit: every stratagem goes in first,
    // weapons only whole and while they fit, and what is left out is said once and stays out of every list.
    static void Limits(string folder, Dictionary<string, JsonElement> fixtures, Action<bool, string> check)
    {
        const uint Strike = 1560416221, FirstWeapon = 0x7E570001, FirstStratagem = 0x7E571001, FirstStable = 0x7E572001;
        var file = Path.Combine(folder, "limits.json");
        var entries = Enumerable.Range(0, 64).Select(i => $$"""{"mixId":{{FirstWeapon + i}},"copyOf":{{Liberator}},"mod":"stress","unit":"t{{i}}"}""")
            .Concat(Enumerable.Range(0, 64).Select(i =>
                $$"""{"kind":"stratagem","mixId":{{FirstStratagem + i}},"stable":{{FirstStable + i}},"copyOfType":118,"copyOf":{{Strike}},"mod":"stress","name":"s{{i}}"}"""));
        File.WriteAllText(file, """{"format":1,"items":[""" + string.Join(",", entries) + "]}");
        List<string> logged = [];
        var items = new RegisteredItems(file) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = logged.Add };

        var vanilla = fixtures["/Progression/items"].GetArrayLength();
        var children = fixtures["/Progression/items"].EnumerateArray().Count(i => i.TryGetProperty("parentId", out var p) && p.GetUInt32() == Liberator);
        var catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        var ids = catalogue.Select(i => (uint)i!["mixId"]!).ToHashSet();
        var weapons = Enumerable.Range(0, 64).Select(i => FirstWeapon + (uint)i).Where(ids.Contains).ToList();
        var fit = (RegisteredItems.CatalogueLimit - vanilla - 64 * 2) / (1 + children);
        check(catalogue.Count <= RegisteredItems.CatalogueLimit, $"The catalogue has {catalogue.Count} items; the game keeps {RegisteredItems.CatalogueLimit}.");
        check(Enumerable.Range(0, 64).All(i => ids.Contains(FirstStratagem + (uint)i)), "Stratagems were left out behind weapons.");
        check(weapons.Count == fit && weapons.SequenceEqual(Enumerable.Range(0, fit).Select(i => FirstWeapon + (uint)i)),
            $"{weapons.Count} weapons went in, not the first {fit} that fit whole.");
        check(weapons.All(w => catalogue.Count(i => (uint?)i!["parentId"] == w) == children), "A weapon went in without all of its children.");
        check(logged.Count == 1 && logged[0].Contains($"{64 - fit} left out", StringComparison.Ordinal), "What was left out was not said: " + string.Join(" | ", logged));
        items.Apply("/Progression/items", fixtures["/Progression/items"]);
        check(logged.Count == 1, "The same report was logged twice.");

        var packages = (JsonArray)JsonNode.Parse(items.Apply("/Progression/ProgressionPackages", fixtures["/Progression/ProgressionPackages"]).GetRawText())!;
        var owners = packages.OfType<JsonObject>().SelectMany(p => p["requiredItems"]!.AsArray().Select(r => (uint)r!)).ToHashSet();
        check(packages.Count <= RegisteredItems.PackageLimit && weapons.All(owners.Contains) && !owners.Contains(FirstWeapon + 63),
            "Mastery packages are not those of the weapons that went in.");

        // An inventory near the game's limit: the stratagems first, then weapons with their held child, each whole.
        var baseChild = catalogue.OfType<JsonObject>().First(i => (uint?)i["parentId"] == Liberator);
        var filler = Enumerable.Range(0, 4000).Select(i => (object)new { mixId = 0x10000000u + (uint)i, amount = 1, isDiscovered = true })
            .Append(new { mixId = (uint)baseChild["mixId"]!, amount = 1, isDiscovered = true }).ToArray();
        var held = JsonNode.Parse(items.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = filler })).GetRawText())!["items"]!.AsArray();
        var owned = held.Select(i => (uint)i!["mixId"]!).ToHashSet();
        var room = (RegisteredItems.InventoryLimit - filler.Length - 64) / 2;
        check(held.Count <= RegisteredItems.InventoryLimit && Enumerable.Range(0, 64).All(i => owned.Contains(FirstStratagem + (uint)i)),
            $"The inventory has {held.Count} items, or lacks a stratagem.");
        check(weapons.Take(room).All(w => owned.Contains(w) && owned.Contains(RegisteredItems.Rehome((uint)baseChild["mixId"]!, Liberator, w))) &&
            !weapons.Skip(room).Any(owned.Contains) && !owned.Contains(FirstWeapon + 63), "Weapons were owned in part, past the limit, or without going in.");
        check(logged.Count == 2 && logged[1].Contains("owned items", StringComparison.Ordinal), "The inventory's limit was not said.");

        // The loader's copies sharing their bases' attachments (sharedChildren): each copy is its own item only, so all 64
        // fit beside the stratagems, with no children, mastery packages or progress of their own.
        File.WriteAllText(file, """{"format":1,"sharedChildren":true,"items":[""" + string.Join(",", entries) + "]}");
        catalogue = (JsonArray)JsonNode.Parse(items.Apply("/Progression/items", fixtures["/Progression/items"]).GetRawText())!;
        ids = catalogue.Select(i => (uint)i!["mixId"]!).ToHashSet();
        check(items.SharedChildren && Enumerable.Range(0, 64).All(i => ids.Contains(FirstWeapon + (uint)i) && ids.Contains(FirstStratagem + (uint)i)),
            "Shared, not every weapon copy and stratagem went in.");
        check(catalogue.Count == vanilla + 64 + 64 * 2 && !catalogue.Any(i => i!["parentId"] is { } p && (uint)p >= FirstWeapon && (uint)p < FirstWeapon + 64),
            "Shared, a weapon copy brought children.");
        packages = (JsonArray)JsonNode.Parse(items.Apply("/Progression/ProgressionPackages", fixtures["/Progression/ProgressionPackages"]).GetRawText())!;
        check(packages.Count == fixtures["/Progression/ProgressionPackages"].GetArrayLength(), "Shared, a weapon copy got a mastery package of its own.");
        held = JsonNode.Parse(items.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = filler })).GetRawText())!["items"]!.AsArray();
        owned = held.Select(i => (uint)i!["mixId"]!).ToHashSet();
        check(owned.Contains(FirstWeapon) && !owned.Contains(RegisteredItems.Rehome((uint)baseChild["mixId"]!, Liberator, FirstWeapon)),
            "Shared, the inventory holds a copy of a child, or not the copy.");
    }
}
