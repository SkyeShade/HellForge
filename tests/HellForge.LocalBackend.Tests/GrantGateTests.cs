using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;
using ProfileStore = HellForge.LocalBackend.ProfileStore;

// The last gate (OwnershipFilter.Gate.cs) on each reply that can give an item, with no account snapshot: every path is
// sent a paid or premium id it would otherwise give, and gives nothing paid.
static class GrantGateTests
{
    const uint Liberator = 0xAA035309, LiberatorPackage = 3391943729, Requisition = 0xD7150EEC, CommonSamples = 0xEDF6EAF5;

    public static async Task Run(string root, Action<bool, string> check)
    {
        var paid = PaidContentSet.Load();
        var fixtures = LocalBackend.LoadStartupFixtures();
        var packages = fixtures["/Progression/ProgressionPackages"].EnumerateArray().Select(p => JsonNode.Parse(p.GetRawText())!.AsObject()).ToList();
        // a premium weapon's mastery package whose first level rewards its (premium) children
        var premiumPackage = packages.First(p => (int?)p["type"] == 2 && p["requiredItems"]!.AsArray().Any(w => paid.IsReserved((uint)w!)) &&
            Rewards(p, 1).Any(paid.IsReserved) && p["levels"]!.AsArray().Any(l => l!["unlockableItems"]!.AsArray().Count > 0));
        var store = paid.MixIds.First(id => !paid.WarbondTokens.Contains(id) && !paid.DlcItems.Contains(id) && !PaidContentSet.IsSuperCredits(id));
        var premiumItem = paid.Warbonds.Where(w => w.Tokens.Length > 0).SelectMany(w => w.Items).Select(i => i.MixId)
            .First(id => paid.TryGetWarbondTokens(id, out _) && !paid.IsPaid(id));
        await Served(root, paid, store, check);
        Purchased(root, paid, ((uint)premiumPackage["id32"]!, premiumPackage, premiumItem), check);
        Mission(paid, ((uint)premiumPackage["id32"]!, premiumPackage, store), check);
    }

    static IEnumerable<uint> Rewards(JsonObject package, int level) =>
        package["levels"]!.AsArray().OfType<JsonObject>().Where(l => (int)l["level"]! == level).SelectMany(l => l["rewards"]!.AsArray())
            .Where(r => (int)r!["type"]! == 1).Select(r => (uint)r!["id32"]!);

    // Served to an unlock-all profile (it holds every item): inventory, packages and progression after every addition. A
    // mod weapon's saved heldItems in profile.json (C-3) name a paid id; the gate drops it.
    static async Task Served(string root, PaidContentSet paid, uint store, Action<bool, string> check)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "grant-gate")).FullName;
        var file = Path.Combine(folder, "registered-items.json");
        var gold = EngineHash.Id32("test/gate_gold");
        File.WriteAllText(file, $$"""{"format":1,"items":[{"mixId":{{gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gate_gold"}]}""");
        using var profile = ProfileStore.Open(null, "unlock-all");
        var filter = new OwnershipFilter(paid, null);
        profile.Ownership = filter;
        var copyPackage = WeaponIds.PackageFor(LiberatorPackage, Liberator, gold);
        profile.TryChange((progression, _) =>
        {
            ModPackages.Hold(ModPackages.Entry(progression, copyPackage), store);
            return true;
        });
        var fixtures = LocalBackend.LoadStartupFixtures();
        var registered = new RegisteredItems(file) { OwnsBase = filter.IsAllowed, Reserved = paid.IsReserved, ModProgress = profile.ModProgress };
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(folder, "backend"), LoginMode.LocalSession)
            { StartupFixtures = true, Profile = profile, RegisteredItems = registered });
        await app.StartAsync();
        using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(30) };
        async Task<JsonNode> Get(string path) => JsonNode.Parse(await client.GetStringAsync(path))!;

        var inventory = (await Get("/Progression/inventory"))["items"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        check(inventory.Contains(gold) && inventory.Contains(Liberator), "The gate dropped what the player may hold.");
        check(!inventory.Contains(store), "A paid id saved in a mod weapon's heldItems (profile.json) was served.");
        check(inventory.All(filter.IsAllowed), "An unlock-all profile was served paid items without a snapshot: " +
            string.Join(",", inventory.Where(id => !filter.IsAllowed(id)).Take(5)));
        // The loader's paid-item guard takes back what this list names, so nothing served may be on it.
        var refused = (await Get("/hellforge/paid-guard"))["refused"]!.AsArray().Select(i => (uint)i!).ToList();
        check(refused.SequenceEqual(refused.Order().Distinct()), "The paid guard's list is not sorted and distinct.");
        check(refused.Contains(store) && refused.All(paid.IsReserved) && !refused.Contains(PaidContentSet.ForgeCreditsWallet) && !refused.Contains(Liberator),
            "The paid guard's list misses a paid item, or names a free item or the Forge Credits wallet.");
        check(!inventory.Any(refused.Contains), "The paid guard would take back a served item: " + string.Join(",", inventory.Where(refused.Contains).Take(5)));
        var served = (await Get("/Progression/ProgressionPackages")).AsArray().OfType<JsonObject>().ToList();
        var levels = served.SelectMany(p => p["levels"]!.AsArray()).OfType<JsonObject>().ToList();
        check(levels.SelectMany(l => l["rewards"]!.AsArray()).Where(r => (int)r!["type"]! == 1).All(r => filter.IsAllowed((uint)r!["id32"]!)) &&
            levels.SelectMany(l => l["unlockableItems"]!.AsArray()).All(u => filter.IsAllowed((uint)u!["mixId"]!)),
            "A package definition offers a paid reward or unlockable.");
        check(Rewards(served.First(p => (uint)p["id32"]! == LiberatorPackage), 1).Any(), "The gate emptied a free weapon's mastery rewards.");
        var bought = (await Get("/Progression"))["purchasedSeasonPassItems"]!.AsArray().Select(i => (long)i!).ToHashSet();
        var locked = paid.Warbonds.Where(w => w.Id != PremiumContent.FreeWarbond).SelectMany(w => w.Items.Select(i => i.ItemId));
        check(!locked.Any(bought.Contains), "A premium warbond's page purchases were served without its token.");
    }

    // Each purchase that grants: a level of a premium weapon's package, its unlockable, a gated item given a requisition
    // price, and a premium item put on Helldivers Mobilize's page in an edited catalogue. Each is refused.
    static void Purchased(string root, PaidContentSet paid, (uint Id, JsonObject Package, uint PremiumItem) ids, Action<bool, string> check)
    {
        var (packageId, package, premiumItem) = ids;
        using var profile = ProfileStore.Open(null, "starter");
        profile.Ownership = new OwnershipFilter(paid, null);
        var unlock = package["levels"]!.AsArray().OfType<JsonObject>().First(l => l["unlockableItems"]!.AsArray().Count > 0);
        profile.TryChange((progression, inventory) =>
        {
            ProfileStore.AddItem(inventory, Requisition, 1_000_000);
            ProfileStore.AddItem(inventory, MissionService.MedalsMixId, 100);
            ProfileStore.AddItem(inventory, CommonSamples, 10_000);
            return true;
        });
        JsonElement? Priced(uint id) => id == premiumItem
            ? JsonSerializer.SerializeToElement(new { mixId = id, buyPrice = new[] { new { mixId = Requisition, amount = 1 } } }) : null;
        var shop = new Purchases(profile, Priced);
        int Status(string path, string body)
        {
            using var document = JsonDocument.Parse(body);
            return shop.Handle(path, document.RootElement).Status;
        }
        var before = profile.Current()["inventory"]!.ToJsonString();
        check(Status("/Progression/ProgressionPackage/AcquireLevel", $$"""{"id32":{{packageId}},"level":1,"expectedCost":[]}""") == 400,
            "A premium weapon's package level was sold with its paid rewards.");
        profile.TryChange((progression, _) => { ProfileStore.PackageEntry(progression, packageId)["level"] = 25; return true; });
        var offer = (uint)unlock["unlockableItems"]![0]!["mixId"]!;
        check(Status("/Progression/ProgressionPackage/AcquireUnlockableItem", $$"""{"packageId32":{{packageId}},"targetLevel":{{(int)unlock["level"]!}},"itemMixId":{{offer}},"expectedCost":[]}""") == 400,
            "A premium weapon's unlockable was sold.");
        check(Status("/Progression/buy/v2", $$"""{"mixId":{{premiumItem}},"amount":1,"warId":801,"expectedCost":[]}""") == 400, "A premium item given a requisition price was sold.");
        check(profile.Current()["inventory"]!.ToJsonString() == before, "A refused purchase changed the inventory.");
        EditedWarbond(root, profile, (premiumItem, paid), check);
    }

    // A premium item on Helldivers Mobilize's page in an edited catalogue: at a medal, and free while a forged
    // registered-items.json entry names it (RegisteredItems refuses that entry, so it is no mod item to grant).
    static void EditedWarbond(string root, ProfileStore profile, (uint Id, PaidContentSet Paid) premium, Action<bool, string> check)
    {
        var (premiumItem, paid) = premium;
        var suite = LocalBackend.CatalogueDirectory!;
        var folder = Directory.CreateDirectory(Path.Combine(root, "grant-gate-catalogue")).FullName;
        var progression = JsonNode.Parse(File.ReadAllText(Path.Combine(suite, GameCatalog.ProgressionDataFile)))!.AsObject();
        var page = progression["seasonPassDetails"]![PremiumContent.FreeWarbond.ToString()]!["seasonPassPages"]![0]!;
        page["seasonPassItems"]!.AsArray().Add(new JsonObject { ["seasonPassItemId"] = 999101, ["itemMixId"] = premiumItem, ["medalCost"] = 1 });
        page["seasonPassItems"]!.AsArray().Add(new JsonObject { ["seasonPassItemId"] = 999102, ["itemMixId"] = premiumItem, ["medalCost"] = 0 });
        var forged = Path.Combine(folder, "registered-items.json");
        File.WriteAllText(forged, $$"""{"format":1,"items":[{"mixId":{{premiumItem}},"copyOf":1,"mod":"test.evil","unit":"evil/a"}]}""");
        var fixtures = LocalBackend.LoadStartupFixtures();
        var mods = new ModWarbondService([]) { Log = _ => { },
            Registered = new RegisteredItems(forged) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Reserved = paid.IsReserved, Log = _ => { } } };
        File.WriteAllText(Path.Combine(folder, GameCatalog.ProgressionDataFile), progression.ToJsonString());
        File.Copy(Path.Combine(suite, GameCatalog.ItemCatalogFile), Path.Combine(folder, GameCatalog.ItemCatalogFile), true);
        try
        {
            LocalBackend.CatalogueDirectory = folder;
            var shop = new Purchases(profile, _ => null);
            using var body = JsonDocument.Parse("""{"expectedCost":[{"mixId":1276729503,"amount":1}],"seasonPassItemId":999101,"warId":801}""");
            var status = shop.Handle("/SeasonPass/item/buy", body.RootElement).Status;
            check(status == 400 && ProfileStore.Held(profile.Current()["inventory"]!["items"]!.AsArray(), premiumItem) == 0,
                "A premium item put on Helldivers Mobilize's page was sold.");
            using var free = JsonDocument.Parse("""{"expectedCost":[],"seasonPassItemId":999102,"warId":801}""");
            var reply = new Purchases(profile, _ => null, mods).Handle("/SeasonPass/item/buy", free.RootElement);
            check(!mods.IsRegistered(premiumItem) && reply.Reply.ToJsonString().IndexOf(premiumItem.ToString(), StringComparison.Ordinal) < 0 &&
                ProfileStore.Held(profile.Current()["inventory"]!["items"]!.AsArray(), premiumItem) == 0,
                "A premium item a forged registered entry names was given free from Helldivers Mobilize's page: " + reply.Reply.ToJsonString());
        }
        finally { LocalBackend.CatalogueDirectory = suite; }
    }

    // A mission end that reports a premium weapon's package and loot naming a paid item and a ship module: the package's
    // paid rewards are not given and only samples, requisition and medals are loot. Pickups pay only their piles, and
    // consume takes nothing.
    static void Mission(PaidContentSet paid, (uint Id, JsonObject Package, uint Store) ids, Action<bool, string> check)
    {
        var (packageId, package, store) = ids;
        using var profile = ProfileStore.Open(null, "starter");
        profile.Ownership = new OwnershipFilter(paid, null);
        var missions = new MissionService(profile);
        var module = 0xCD183602u;
        var before = profile.Current()["inventory"]!["items"]!.AsArray();
        using var end = JsonDocument.Parse($$"""
            {"missionId":"gate-1","missionSuccess":true,"difficultyType":4,"objectives":[],
             "participants":[{"platformAccountId":"1","lootedItems":[{"mixId":{{store}},"amount":1},{"mixId":{{module}},"amount":1},{"mixId":{{CommonSamples}},"amount":2}],
               "progressedPackages":[{"id32":{{packageId}},"factor":1}]}]}
            """);
        var reply = (JsonObject)missions.Handle("/Operation/Mission/end", end.RootElement).Reply;
        var looted = reply["lootedItemRewards"]!.AsArray().Select(i => (uint)i!["mixId"]!).ToList();
        var after = profile.Current()["inventory"]!["items"]!.AsArray();
        check(looted.SequenceEqual([CommonSamples]) && ProfileStore.Held(after, store) == 0 &&
            ProfileStore.Held(after, module) == ProfileStore.Held(before, module), "A mission report's loot granted something other than samples: " + string.Join(",", looted));
        check(Rewards(package, 1).All(id => ProfileStore.Held(after, id) == ProfileStore.Held(before, id)) &&
            reply["progressionPackageUpdates"]!.AsArray().Any(u => (uint)u!["id32"]! == packageId),
            "A premium weapon's mastery level gave its paid rewards.");
        using var activity = JsonDocument.Parse("""{"missionId":"gate-1","activityId32":123,"objectId":5,"participants":["1"]}""");
        check(((JsonObject)missions.Handle("/Operation/Mission/Activity", activity.RootElement).Reply)["rewards"]!.AsArray().Count == 0, "An unknown pickup paid something.");
        var kept = profile.Current()["inventory"]!.ToJsonString();
        using var consume = JsonDocument.Parse($$"""{"consumedItems":[{"mixId":{{CommonSamples}},"amount":1}]}""");
        missions.Handle("/Progression/inventory/consume", consume.RootElement);
        check(profile.Current()["inventory"]!.ToJsonString() == kept, "consume changed the inventory.");
    }
}
