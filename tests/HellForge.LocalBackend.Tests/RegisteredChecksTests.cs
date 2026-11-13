using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;

// registered-items.json is the player's to write: an entry naming a paid id, or making one, is refused and its mod named
// in the log; a copy of premium content (through another mod's weapon too) is not held without it. The example mods'
// real ids still pass.
static class RegisteredChecksTests
{
    const uint Liberator = 0xAA035309, M = 0x5BD1E995;
    const string Refused = "is the game's or a paid item's";

    public static void Run(string root, Action<bool, string> check)
    {
        var fixtures = LocalBackend.LoadStartupFixtures();
        var paid = PaidContentSet.Load();
        var catalogue = fixtures["/Progression/items"].EnumerateArray().Select(e => JsonNode.Parse(e.GetRawText())!.AsObject()).ToList();
        var folder = Directory.CreateDirectory(Path.Combine(root, "registered-checks")).FullName;
        var file = Path.Combine(folder, "registered-items.json");
        var log = new List<string>();
        var filter = new OwnershipFilter(paid, []);
        RegisteredItems Items() => new(file) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = log.Add, Reserved = paid.IsReserved, OwnsBase = filter.IsAllowed };
        List<uint> Held(RegisteredItems items, params uint[] held) =>
            [.. JsonNode.Parse(items.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = held.Select(id => new { mixId = id, amount = 1, isDiscovered = true }) })).GetRawText())!["items"]!
                .AsArray().Select(i => (uint)i!["mixId"]!)];

        var store = paid.MixIds.First(id => !paid.WarbondTokens.Contains(id) && !paid.DlcItems.Contains(id) && !PaidContentSet.IsSuperCredits(id));
        // an attachment only premium weapons have, and one of those weapons
        var weapons = catalogue.Where(e => (int?)e["progressionCategory"] == 1 && e["parentId"] == null).Select(e => (uint)e["mixId"]!).ToHashSet();
        var premiumPaint = catalogue.Where(e => (int?)e["progressionCategory"] == 2 && e["parentId"] != null).GroupBy(e => (uint)e["itemId"]!)
            .First(g => g.All(e => paid.IsReserved((uint)e["parentId"]!)) && g.Any(e => weapons.Contains((uint)e["parentId"]!)));
        var premium = premiumPaint.Select(e => (uint)e["parentId"]!).First(weapons.Contains);
        Forged(file, Items, Held, log, (store, catalogue), check);
        Copies(file, Items, Held, (premium, premiumPaint.Key), catalogue, paid, check);
        Honest(file, Items, Held, log, check);
    }

    // Entries a player wrote by hand: each refused, named by its mod, and nothing paid held.
    static void Forged(string file, Func<RegisteredItems> items, Func<RegisteredItems, uint[], List<uint>> held, List<string> log,
        (uint Store, List<JsonObject> Catalogue) ids, Action<bool, string> check)
    {
        var (store, catalogue) = ids;
        var liberatorChild = catalogue.Where(e => (uint?)e["parentId"] == Liberator).Select(e => (uint)e["mixId"]!).First();
        var crafted = Crafted(RegisteredItems.Attachment(liberatorChild, Liberator), store);
        check(RegisteredItems.Rehome(liberatorChild, Liberator, crafted) == store, "The crafted weapon does not make the paid id: the checks below prove nothing.");
        var vanillaStable = catalogue.Where(e => (int?)e["progressionCategory"] == 10 && e["parentId"] != null).Select(e => (uint)e["itemId"]!).First();
        File.WriteAllText(file, $$"""
            {"format":1,"items":[
              {"mixId":{{store}},"copyOf":1,"mod":"test.evil","unit":"evil/a"},
              {"mixId":{{crafted}},"copyOf":{{Liberator}},"mod":"test.evil","unit":"evil/b"},
              {"kind":"stratagem","mixId":{{EngineHash.Id32("evil/s#item")}},"stable":{{vanillaStable}},"copyOf":{{vanillaStable}},"mod":"test.evil","name":"evil/s"},
              {"kind":"difficulty","mixId":{{store}},"copyOf":{{RegisteredItems.LevelTenItem}},"level":11,"mod":"test.evil","name":"evil/d"},
              {"kind":"difficulty","mixId":{{EngineHash.Id32("evil/e#item")}},"copyOf":1,"level":11,"mod":"test.evil","name":"evil/e"}],
             "attachments":[{"mixId":{{RegisteredItems.Attachment(store, Liberator)}},"copyOf":1,"mod":"test.evil","weapons":[{{Liberator}}]}],
             "patterns":[{"mixId":{{store}},"copyOf":1,"kind":"unit","mod":"test.evil"}]}
            """);
        log.Clear();
        var registered = items();
        var inventory = held(registered, [RegisteredItems.LevelTenItem, Liberator]);
        check(!inventory.Contains(store) && !inventory.Contains(crafted) && !inventory.Contains(EngineHash.Id32("evil/s#item")) &&
            !inventory.Contains(EngineHash.Id32("evil/e#item")) && registered.Served().Count == 0, "A forged entry was served: " + string.Join(",", registered.Served().Select(i => i.MixId)));
        check(log.Any(l => l.StartsWith($"test.evil's weapon {store} ", StringComparison.Ordinal) && l.Contains("its id " + Refused)) &&
            log.Any(l => l.StartsWith($"test.evil's weapon {crafted} ", StringComparison.Ordinal) && l.Contains($"its child's id {store}")) &&
            log.Any(l => l.StartsWith("test.evil's stratagem ", StringComparison.Ordinal) && l.Contains("game stratagem's")) &&
            log.Count(l => l.StartsWith("test.evil's difficulty ", StringComparison.Ordinal) && l.Contains("level copied from level 10")) == 2 &&
            log.Any(l => l.StartsWith("test.evil's attachment ", StringComparison.Ordinal)) && log.Any(l => l.StartsWith("test.evil's pattern ", StringComparison.Ordinal)),
            "A refusal is not logged with its mod: " + string.Join(" | ", log));
        var listed = JsonNode.Parse(registered.Apply("/Progression/items", JsonSerializer.SerializeToElement(catalogue)).GetRawText())!.AsArray();
        check(listed.Count == catalogue.Count, "A forged entry added to the catalogue.");
    }

    // A weapon copying another mod's weapon (registry_load.c PendingBase): held when the game's weapon at the end is, never
    // when it is premium and not owned; patterns and attachments of premium content likewise.
    static void Copies(string file, Func<RegisteredItems> items, Func<RegisteredItems, uint[], List<uint>> held, (uint Weapon, uint Paint) premiumIds,
        List<JsonObject> catalogue, PaidContentSet paid, Action<bool, string> check)
    {
        var (premium, premiumPaint) = premiumIds;
        uint a = EngineHash.Id32("test/a"), b = EngineHash.Id32("test/b"), c = EngineHash.Id32("test/c"), d = EngineHash.Id32("test/d");
        var premiumSkin = catalogue.Where(e => (int?)e["progressionCategory"] == 4 && e["parentId"] != null).GroupBy(e => (uint)e["itemId"]!)
            .First(g => g.All(e => paid.IsReserved((uint)e["mixId"]!))).First();
        uint pattern = EngineHash.Id32("test/p"), unitPattern = EngineHash.Id32("test/u"), attachment = EngineHash.Id32("test/x");
        File.WriteAllText(file, $$"""
            {"format":1,"items":[
              {"mixId":{{a}},"copyOf":{{premium}},"mod":"test","unit":"test/a"},{"mixId":{{b}},"copyOf":{{a}},"mod":"test","unit":"test/b"},
              {"mixId":{{c}},"copyOf":{{Liberator}},"mod":"test","unit":"test/c"},{"mixId":{{d}},"copyOf":{{c}},"mod":"test","unit":"test/d"}],
             "attachments":[{"mixId":{{attachment}},"copyOf":{{premiumPaint}},"mod":"test","weapons":[{{c}}]}],
             "patterns":[{"mixId":{{pattern}},"copyOf":{{premiumPaint}},"kind":"weapon","mod":"test","weapons":[]},
               {"mixId":{{unitPattern}},"copyOf":{{(uint)premiumSkin["itemId"]!}},"kind":"unit","mod":"test"}]}
            """);
        var registered = items();
        var inventory = held(registered, [Liberator, premium]);
        check(inventory.Contains(c) && inventory.Contains(d), "A copy of a mod's copy of a free weapon is not held.");
        check(!inventory.Contains(a) && !inventory.Contains(b), "A copy of a mod's copy of a premium weapon is held without it.");
        check(!inventory.Contains(RegisteredItems.WeaponChild(attachment, c)), "An attachment copied from premium weapons is held without them.");
        check(!inventory.Contains(RegisteredItems.WeaponChild(pattern, premium)) && !inventory.Any(id => id == RegisteredItems.WeaponChild(unitPattern, (uint)premiumSkin["parentId"]!)),
            "A pattern copied from premium content is held without it.");
    }

    // The example mods' own entries (golden-liberator, talon-liberator, hell-forge, stratagem-compendium) as the loader
    // writes them: each served and held.
    static void Honest(string file, Func<RegisteredItems> items, Func<RegisteredItems, uint[], List<uint>> held, List<string> log, Action<bool, string> check)
    {
        const uint Eat17 = 0xCB778090;
        string gold = "skyeshade/golden_liberator/assault_rifle_gold", talon = "skyeshade/talon_liberator/assault_rifle_talon",
            hell = "skyeshade/hell_forge/hell_forge", eat = "skyeshade/compendium/eat_gas";
        File.WriteAllText(file, $$"""
            {"format":1,"items":[
              {"mixId":{{EngineHash.Id32(gold)}},"copyOf":{{Liberator}},"mod":"skyeshade.golden-liberator","unit":"{{gold}}"},
              {"mixId":{{EngineHash.Id32(talon)}},"copyOf":{{Liberator}},"mod":"skyeshade.talon-liberator","unit":"{{talon}}"},
              {"kind":"stratagem","mixId":{{EngineHash.Id32(eat + "#item")}},"stable":{{EngineHash.Id32(eat)}},"copyOfType":147,"copyOf":{{Eat17}},"mod":"skyeshade.stratagem-compendium","name":"{{eat}}"},
              {"kind":"difficulty","mixId":{{EngineHash.Id32(hell + "#item")}},"copyOf":{{RegisteredItems.LevelTenItem}},"level":11,"mod":"skyeshade.hell-forge","name":"{{hell}}"}]}
            """);
        log.Clear();
        var inventory = held(items(), [RegisteredItems.LevelTenItem, Liberator]);
        check(new[] { EngineHash.Id32(gold), EngineHash.Id32(talon), EngineHash.Id32(eat + "#item"), EngineHash.Id32(hell + "#item") }.All(inventory.Contains) && log.Count == 0,
            "An example mod's entry was refused: " + string.Join(" | ", log));
    }

    // The weapon id whose child for this attachment is target: WeaponChild solved for the weapon.
    static uint Crafted(uint attachment, uint target)
    {
        uint inverse = M;
        for (var i = 0; i < 5; i++) inverse *= 2 - M * inverse;
        return (target ^ RegisteredItems.WeaponChild(attachment, 0)) * inverse;
    }
}
