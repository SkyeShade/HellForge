using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;
using ProfileStore = HellForge.LocalBackend.ProfileStore;

// Sync Inventory on the backend: the premium rules (PaidContentSet, OwnershipFilter), Super Credits on every path, sealed
// snapshots (AccountSnapshot) and the account import with its backups and restore (AccountImport, AccountCommands).
static class AccountSyncTests
{
    const ulong Player = 76561198000000001, Sibling = 76561198000000002;
    const uint MobilizeToken = 2032304616, Medals = 1276729503, Requisition = 0xD7150EEC, CommonSamples = 0xEDF6EAF5;
    const uint SuperCreditPrice = 0xCF875032, SuperCreditWallet = 0xCBFFC645, LocalArmoryItem = 9999, VanillaArmoryItem = 4242;
    // Steam DLC gear: the Super Citizen Edition's MP-98 Knight, DP-53 helmet and armour and cape; TR-117 Alpha Commander;
    // TR-40 Gold Eagle; and their packages. Not DLC: the TR-7, TR-9 and TR-62 sets of item package 3722765189.
    const uint Knight = 2161995568;
    static readonly uint[] DlcGear = [Knight, 1296068557, 556862721, 2007907056, 1945105139, 3066485739, 276679263, 3813318023, 1849408484,
        1542124658, 651643972, 1999963265, 4146558611];
    static readonly uint[] PromotionalArmour = [495842462, 1122513689, 2899116161];

    sealed record Ids(uint OwnedToken, uint OwnedWarbondItem, uint OtherWarbondItem, uint StoreItem, uint MobilizeItem);

    public static void Run(string root, Action<bool, string> check)
    {
        var paid = PaidContentSet.Load();
        var ids = FindIds(paid);
        PremiumRules(paid, ids, check);
        SuperCredits(root, check);
        Seals(root, paid, ids, check);
        Imports(root, ids, check);
        WarbondChoices(root, paid, ids, check);
        DuplicateItems(root, ids, check);
        ModItems(root, paid, check);
    }

    // Mods' weapons and stratagems (RegisteredItems): a copy of premium content is the player's only when they own the
    // original, and a mod mods-stage locked (its premium list not owned) adds nothing; free copies are untouched.
    static void ModItems(string root, PaidContentSet paid, Action<bool, string> check)
    {
        const uint Liberator = 0xAA035309, KnightCopy = 0x5EED0001, LiberatorCopy = 0x5EED0002, LockedCopy = 0x5EED0003, StrikeCopy = 0x5EED0004;
        const uint Strike = 1560416221, StrikeStable = 0x0BADF00D;
        var fixtures = LocalBackend.LoadStartupFixtures();
        var file = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "premium-mod-items")).FullName, "registered-items.json");
        File.WriteAllText(file, $$"""
            {"format":1,"items":[{"mixId":{{KnightCopy}},"copyOf":{{Knight}},"mod":"a.knight-variant"},{"mixId":{{LiberatorCopy}},"copyOf":{{Liberator}},"mod":"b.free"},
              {"mixId":{{LockedCopy}},"copyOf":{{Liberator}},"mod":"c.warbond-port"},
              {"kind":"stratagem","mixId":{{StrikeCopy}},"stable":{{StrikeStable}},"copyOf":{{Strike}},"mod":"b.free"}]}
            """);
        var items = new RegisteredItems(file)
        {
            Source = path => fixtures.TryGetValue(path, out var body) ? body : null,
            OwnsBase = new OwnershipFilter(paid, null).IsAllowed,
            LockedMods = new HashSet<string> { "c.warbond-port" }
        };
        HashSet<uint> Served()
        {
            var held = JsonSerializer.SerializeToElement(new { items = Array.Empty<object>() });
            return [.. JsonNode.Parse(items.Apply("/Progression/inventory", held).GetRawText())!["items"]!.AsArray().Select(i => (uint)Whole(i!["mixId"]))];
        }
        var served = Served();
        check(!served.Contains(KnightCopy), "A copy of DLC gear was served to a player who does not own it.");
        check(served.Contains(LiberatorCopy) && served.Contains(StrikeCopy), "A copy of free content was withheld.");
        check(!served.Contains(LockedCopy), "A locked premium mod's item was served.");
        items.OwnsBase = new OwnershipFilter(paid, [new JsonObject { ["mixId"] = Knight, ["amount"] = 1, ["isDiscovered"] = true }]).IsAllowed;
        check(Served().Contains(KnightCopy), "A copy of DLC gear was withheld from its owner.");
    }

    // A hand-edited profile may list an item twice; an import must still work and keep one entry per item it touches.
    static void DuplicateItems(string root, Ids ids, Action<bool, string> check)
    {
        var (profileDir, _) = Install(root, "duplicates", ids);
        using (ProfileStore.Open(profileDir, "starter")) { }
        var file = Path.Combine(profileDir, ProfileStore.FileName);
        var profile = JsonNode.Parse(File.ReadAllText(file))!.AsObject();
        foreach (var amount in new[] { 1, 7 })
            ((JsonArray)profile["inventory"]!["items"]!).Add((JsonNode?)new JsonObject { ["mixId"] = VanillaArmoryItem, ["amount"] = amount, ["isDiscovered"] = true });
        File.WriteAllText(file, profile.ToJsonString());
        using var store = ProfileStore.Open(profileDir);
        store.Import(Snapshot(profileDir), new AccountImportSettings(), PaidContentSet.Load());
        var entries = store.Current()["inventory"]!["items"]!.AsArray().OfType<JsonObject>().Where(i => Whole(i["mixId"]) == VanillaArmoryItem).ToList();
        check(entries.Count == 2 && Whole(entries[0]["amount"]) == 2, "An import over a duplicated item failed or changed the wrong entry.");
    }

    static Ids FindIds(PaidContentSet paid)
    {
        using var data = LocalBackend.LoadResource("progression-data.json");
        var details = data.RootElement.GetProperty("seasonPassDetails");
        IEnumerable<uint> Items(string pass) => details.GetProperty(pass).GetProperty("seasonPassPages").EnumerateArray()
            .SelectMany(page => page.GetProperty("seasonPassItems").EnumerateArray()).Select(item => item.GetProperty("itemMixId").GetUInt32());
        var token = details.GetProperty("610512218").GetProperty("premiumTokenMixIds")[0].GetUInt32();
        var owned = Items("610512218").First(i => paid.TryGetWarbondTokens(i, out var tokens) && tokens.Contains(token));
        var other = Items("19991972").First(i => paid.TryGetWarbondTokens(i, out var tokens) && !tokens.Contains(token));
        var store = paid.MixIds.Except(paid.WarbondTokens).Except(paid.DlcItems).First();
        var mobilize = Items("1929468580").First(i => !paid.TryGetWarbondTokens(i, out _));
        return new Ids(token, owned, other, store, mobilize);
    }

    static void PremiumRules(PaidContentSet paid, Ids ids, Action<bool, string> check)
    {
        check(paid.MixIds.Count > 100 && paid.IsPaid(ids.OwnedToken) && paid.IsPaid(ids.StoreItem), "Premium tokens or Superstore items are not paid.");
        check(!paid.IsPaid(MobilizeToken), "The free warbond's token counts as paid.");
        // Store fronts: the Superstore (Super Credits) is paid; the requisition terminal is not, nor the stratagems every
        // account starts with (Orbital Precision Strike, MG-43), which the terminal lists.
        using (var data = LocalBackend.LoadResource("progression-data.json"))
            foreach (var front in data.RootElement.GetProperty("storeFront").EnumerateArray())
            {
                var items = front.GetProperty("sections").EnumerateArray().SelectMany(s => s.GetProperty("items").EnumerateArray()).Select(i => i.GetProperty("mixId").GetUInt32()).ToList();
                var id = front.GetProperty("id32").GetUInt32();
                // every item, the weapons whose mastery packages require them included (a requirement is not a reward)
                if (id == 2776696735) check(items.All(paid.IsPaid), "Superstore items are not paid: " + items.Count(i => !paid.IsPaid(i)));
                if (id == 124284010) check(!items.Any(paid.IsPaid), "A requisition terminal item counts as paid.");
            }
        check(!paid.IsPaid(1497469599) && !paid.IsPaid(3787370254), "The starting stratagems count as paid.");
        check(DlcGear.All(paid.IsPaid) && DlcGear.All(paid.DlcItems.Contains), "Steam DLC gear is not paid.");
        check(!PromotionalArmour.Any(paid.IsPaid), "Promotional armour that is no Steam DLC counts as paid.");
        check(!paid.TryGetWarbondTokens(ids.MobilizeItem, out _), "A free warbond item needs a token.");
        check(new[] { SuperCreditPrice, 0x6C3C83C1u, SuperCreditWallet, 0x865AF2FDu }.All(PaidContentSet.IsSuperCredits), "A Super Credits item is not recognised.");
        check(paid.Currencies.Contains(Medals) && paid.Currencies.Contains(Requisition) && !paid.Currencies.Contains(SuperCreditWallet), "Currencies are wrong.");

        // No snapshot: every paid item and every premium warbond's item is withheld, the free warbond is not.
        using var unlockAll = ProfileStore.Open(null, "unlock-all");
        unlockAll.Ownership = new OwnershipFilter(paid, null);
        var served = Served(unlockAll);
        check(!served.Any(paid.IsPaid), "A paid item was served without a snapshot.");
        check(!served.Contains(ids.OwnedWarbondItem) && !served.Contains(ids.OtherWarbondItem), "A premium warbond's item was served without its token.");
        check(served.Contains(MobilizeToken) && served.Contains(ids.MobilizeItem) && served.Count > 1000, "The free warbond or ordinary items were withheld.");
        check(!served.Overlaps(DlcGear), "Steam DLC gear was served without a snapshot.");
    }

    static void SuperCredits(string root, Action<bool, string> check)
    {
        var folder = Path.Combine(root, "super-credits", "profiles", "player");
        using (ProfileStore.Open(folder, "starter")) { }
        var file = Path.Combine(folder, ProfileStore.FileName);
        var profile = JsonNode.Parse(File.ReadAllText(file))!.AsObject();
        foreach (var id in new[] { SuperCreditPrice, SuperCreditWallet })
            ((JsonArray)profile["inventory"]!["items"]!).Add((JsonNode?)new JsonObject { ["mixId"] = id, ["amount"] = 5000, ["isDiscovered"] = true });
        File.WriteAllText(file, profile.ToJsonString());

        using var store = ProfileStore.Open(folder);
        check(!Served(store).Any(PaidContentSet.IsSuperCredits), "Super Credits added to profile.json were served.");
        store.Ownership = new OwnershipFilter(PaidContentSet.Load(), null);
        check(!Served(store).Any(PaidContentSet.IsSuperCredits), "Super Credits were served with ownership rules.");

        store.ApplyMissionRewards(0, new Dictionary<uint, long> { [SuperCreditWallet] = 100, [CommonSamples] = 3 });
        using var mission = JsonDocument.Parse("{\"operationId\":1,\"missionIndex\":0,\"missionSuccess\":true,\"difficultyType\":4,\"players\":[{\"lootedItems\":[" +
            "{\"mixId\":" + SuperCreditWallet + ",\"amount\":10},{\"mixId\":" + CommonSamples + ",\"amount\":2}]}]}");
        var (reply, _) = new MissionService(store).Handle("/Operation/Mission/end", mission.RootElement);
        var rewarded = ((JsonArray)reply["lootedItemRewards"]!).Select(i => (uint)Whole(i!["mixId"])).ToList();
        check(rewarded.Contains(CommonSamples) && !rewarded.Any(PaidContentSet.IsSuperCredits), "A mission granted Super Credits found on the map.");
        var stored = Items(store.Current());
        check(stored[CommonSamples] >= 5 && stored[SuperCreditWallet] == 5000, "A mission reward added Super Credits to the profile.");
    }

    static void Seals(string root, PaidContentSet paid, Ids ids, Action<bool, string> check)
    {
        var (_, snapshot) = Install(root, "seals", ids);
        var owned = OwnershipFilter.ForSnapshot(paid, snapshot, Player, out var problem);
        check(problem == null && owned.OwnedPaidCount == 3, "A sealed snapshot did not unlock its premium items: " + problem);
        using (var starter = ProfileStore.Open(null, "starter"))
        {
            starter.Ownership = owned;
            var served = Served(starter);
            check(served.Contains(ids.OwnedToken) && served.Contains(ids.StoreItem) && served.Contains(Knight), "Owned premium items were not served to a profile lacking them.");
            check(!served.Overlaps(DlcGear.Where(id => id != Knight)), "DLC gear the snapshot lacks was served.");
            check(!served.Any(PaidContentSet.IsSuperCredits), "The snapshot's Super Credits were served.");
        }
        Refused(paid, snapshot, Sibling, "another Steam account", check);
        Refused(paid, snapshot, null, "not signed in", check);

        var inventory = Path.Combine(snapshot, InventorySync.InventoryFile);
        var original = File.ReadAllBytes(inventory);
        File.WriteAllText(inventory, File.ReadAllText(inventory).Replace("\"items\":[", "\"items\":[{\"mixId\":" + ids.OtherWarbondItem + ",\"amount\":1,\"isDiscovered\":true},", StringComparison.Ordinal));
        Refused(paid, snapshot, Player, "changed", check);
        File.WriteAllBytes(inventory, original);
        check(OwnershipFilter.ForSnapshot(paid, snapshot, Player, out _).OwnedPaidCount == 3, "The restored snapshot no longer verifies.");

        var seal = Path.Combine(snapshot, SnapshotSeal.FileName);
        var sealText = File.ReadAllText(seal);
        File.WriteAllText(seal, sealText.Replace(Player.ToString(), Sibling.ToString(), StringComparison.Ordinal));
        Refused(paid, snapshot, Sibling, "changed", check);
        File.WriteAllText(seal, sealText);

        // Another installation (its own key) does not accept this one's snapshot.
        var (_, elsewhere) = Install(root, "seals-elsewhere", ids);
        foreach (var name in new[] { InventorySync.InventoryFile, InventorySync.ProgressionFile, SnapshotSeal.FileName })
            File.Copy(Path.Combine(snapshot, name), Path.Combine(elsewhere, name), overwrite: true);
        Refused(paid, elsewhere, Player, "another PC", check);
        check(!OwnershipFilter.ForSnapshot(paid, Path.Combine(root, "none", "account-snapshot", "p"), Player, out problem).HasRealInventory && problem == null,
            "A profile without a snapshot reported a problem or ownership.");
    }

    static void Refused(PaidContentSet paid, string snapshot, ulong? account, string reason, Action<bool, string> check)
    {
        var filter = OwnershipFilter.ForSnapshot(paid, snapshot, account, out var problem);
        check(filter.OwnedPaidCount == 0 && problem != null && problem.Contains(reason, StringComparison.Ordinal),
            $"A snapshot was not refused for '{reason}': {problem ?? "accepted"}.");
    }

    static void Imports(string root, Ids ids, Action<bool, string> check)
    {
        var (profileDir, _) = Install(root, "imports", ids);
        long beforeVanillaXp;
        using (var store = ProfileStore.Open(profileDir, "starter"))
        {
            store.ApplyMissionRewards(0, new Dictionary<uint, long> { [LocalArmoryItem] = 1, [Medals] = 5, [Requisition] = 20000, [CommonSamples] = 30 });
            beforeVanillaXp = Experience(store.Current());
            var shipBefore = store.Current()["progression"]!["shipName"]?.ToJsonString();
            store.Import(Snapshot(profileDir), new AccountImportSettings(), PaidContentSet.Load());
            var merged = store.Current();
            var items = Items(merged);
            var progression = merged["progression"]!.AsObject();
            check(Experience(merged) == Math.Max(beforeVanillaXp, 485592), "Level did not keep the higher XP.");
            check(items.ContainsKey(LocalArmoryItem) && items.ContainsKey(VanillaArmoryItem) && items.ContainsKey(ids.OwnedWarbondItem), "Items were not merged.");
            check(items[Medals] == 119 && items[Requisition] == 50000 && items[CommonSamples] == 30, "Currencies did not keep the higher amounts.");
            check(progression["purchasedSeasonPassItems"]!.AsArray().Count == 2 && progression["packageProgress"]!.AsArray().Any(p => Whole(p!["id32"]) == 2373460877),
                "Warbond unlocks or packages were not merged.");
            check(progression["shipName"]?.ToJsonString() == shipBefore && !progression.ContainsKey("location") && Whole(progression["onboardingFlags"]) != 16350331,
                "Ship or tutorial imported though left out by default.");
            check(!items.Keys.Any(PaidContentSet.IsSuperCredits) && !items.ContainsKey(ids.OwnedToken) && !items.ContainsKey(ids.StoreItem) && !items.ContainsKey(Knight),
                "Super Credits or premium items were written into the profile.");
        }
        check(ProfileBackups.List(profileDir) is [{ Reason: "import" }], "The first import did not back the profile up.");
        ImportsByCommand(profileDir, beforeVanillaXp, check);
    }

    static void ImportsByCommand(string profileDir, long firstXp, Action<bool, string> check)
    {
        var settings = new AccountImportSettings();
        settings.Set("level", true, ImportRule.Vanilla);
        settings.Set("armory", true, ImportRule.Vanilla);
        settings.Set("ship", true, ImportRule.Vanilla);
        settings.Set("onboarding", true, ImportRule.Vanilla);
        settings.Save(AccountImportSettings.PathIn(profileDir));
        var file = Path.Combine(profileDir, ProfileStore.FileName);
        var beforeSecond = File.ReadAllText(file);

        check(Command(AccountCommands.Import, profileDir, Sibling, out var error) == 1 && error.Contains("another Steam account", StringComparison.Ordinal) &&
            File.ReadAllText(file) == beforeSecond, "Another account's import was not refused, or changed the profile.");
        check(Command(AccountCommands.Preview, profileDir, Player, out var preview) == 0, "The preview failed.");
        using (var review = JsonDocument.Parse(preview))
            check(review.RootElement.GetProperty("categories").GetProperty("level").GetProperty("vanilla").GetString() == "485,592 XP" &&
                review.RootElement.GetProperty("premium").GetString() == "1 warbond token, 1 Superstore item and 1 DLC item", "The preview is wrong: " + preview);

        check(Command(AccountCommands.Import, profileDir, Player, out error) == 0, "The import command failed: " + error);
        var imported = ProfileStore.Load(file);
        var progression = imported["progression"]!.AsObject();
        check(Experience(imported) == 485592 && !Items(imported).ContainsKey(LocalArmoryItem) && Items(imported).ContainsKey(VanillaArmoryItem),
            "Vanilla rules did not replace level or armory items.");
        check(progression["shipName"]!.ToJsonString() == "[9,16]" && Whole(progression["location"]!["planetIndex"]) == 171 &&
            Whole(progression["onboardingFlags"]) == 16350331, "Ship or tutorial progress was not imported.");

        var backups = ProfileBackups.List(profileDir);
        check(backups.Count == 2 && File.ReadAllText(backups[0].Path) == beforeSecond, "The second import's backup is not the profile it replaced.");
        check(AccountCommands.Restore(profileDir, backups[0].Path, TextWriter.Null, TextWriter.Null) == 0 && File.ReadAllText(file) == beforeSecond &&
            Items(ProfileStore.Load(file)).ContainsKey(LocalArmoryItem) && Experience(ProfileStore.Load(file)) == Math.Max(firstXp, 485592),
            "Restore did not put the backup back.");
        check(ProfileBackups.List(profileDir) is [{ Reason: "restore" }, ..] && ProfileBackups.List(profileDir).Count == 3, "Restore did not back up the profile it replaced.");
    }

    // Warbond unlocks: as on the vanilla account, every item of each warbond the account owns, or none. Ownership is the
    // snapshot's: the free warbond and the one whose token it holds; another warbond's items are never unlocked.
    static void WarbondChoices(string root, PaidContentSet paid, Ids ids, Action<bool, string> check)
    {
        var owned = paid.OwnedWarbonds(new HashSet<uint> { ids.OwnedToken }).ToList();
        check(owned.Count == 2 && owned.Any(w => w.Tokens.Length == 0) && owned.Any(w => w.Tokens.Contains(ids.OwnedToken)),
            "Owned warbonds are not the free one and the one whose token the snapshot holds.");
        check(paid.Warbonds.Count == 25 && paid.Warbonds.All(w => w.Items.Length > 0) && paid.Warbonds.Sum(w => w.Items.Length) == 585,
            "Not every warbond's page items are listed.");

        // Each choice imports into its own fresh starter profile.
        JsonObject Imported(ImportRule rule)
        {
            var (dir, _) = Install(root, "warbond-" + rule, ids);
            using var store = ProfileStore.Open(dir, "starter");
            var settings = new AccountImportSettings();
            settings.Set("warbondItems", false, rule);
            store.Import(Snapshot(dir), settings, paid);
            return store.Current();
        }
        static HashSet<long> Unlocked(JsonObject profile) =>
            [.. (profile["progression"]!["purchasedSeasonPassItems"] as JsonArray ?? []).Select(Whole)];

        var asVanilla = Imported(ImportRule.Merge);
        check(Unlocked(asVanilla).IsSupersetOf([871, 872]) && Items(asVanilla).ContainsKey(ids.OwnedWarbondItem),
            "As in vanilla did not take the account's warbond unlocks (a category without a checkbox ignored include: false).");

        var every = Imported(ImportRule.All);
        var expected = owned.SelectMany(w => w.Items).Select(i => i.ItemId).ToHashSet();
        check(Unlocked(every).IsSupersetOf(expected) && Unlocked(every).IsSupersetOf([871, 872]), "Every item did not unlock each owned warbond's items.");
        var items = Items(every);
        check(owned.SelectMany(w => w.Items).All(i => items.ContainsKey(i.MixId)) && !items.ContainsKey(ids.OtherWarbondItem),
            "Every item did not add the owned warbonds' items, or added another warbond's.");
        check(!items.Keys.Any(PaidContentSet.IsSuperCredits) && !items.ContainsKey(ids.OwnedToken) && !items.Keys.Any(paid.IsPaid),
            "Every item added Super Credits or a paid item.");

        var none = Imported(ImportRule.None);
        var (fresh, _) = Install(root, "warbond-starter", ids);
        JsonObject starter;
        using (var store = ProfileStore.Open(fresh, "starter")) starter = store.Current();
        check(Unlocked(none).SetEquals(Unlocked(starter)) && !Items(none).ContainsKey(ids.OwnedWarbondItem), "None imported warbond unlocks.");

        check(Command(AccountCommands.Preview, fresh, Player, out var preview) == 0, "The preview failed.");
        using var review = JsonDocument.Parse(preview);
        check(review.RootElement.GetProperty("warbonds").GetString() == $"2 warbonds you own hold {expected.Count} unlocks", "The preview's warbonds line is wrong: " + preview);
    }

    static int Command(Func<string, string?, ulong?, TextWriter, TextWriter, int> command, string profileDir, ulong account, out string text)
    {
        using var output = new StringWriter();
        using var error = new StringWriter();
        var code = command(profileDir, null, account, output, error);
        text = code == 0 ? output.ToString() : error.ToString();
        return code;
    }

    // An installation folder with a profile folder and a sealed snapshot for Player: a token, a Superstore item, DLC gear, a premium warbond
    // item, an armory item, currencies, Super Credits, and the progression of the 2026-10-24 capture's shape.
    static (string ProfileDir, string SnapshotDir) Install(string root, string name, Ids ids)
    {
        var profileDir = Path.Combine(root, name, "profiles", "player");
        var snapshot = InventorySync.SnapshotDirectoryFromProfileDir(profileDir)!;
        Directory.CreateDirectory(profileDir);
        Directory.CreateDirectory(snapshot);
        string Item(uint id, long amount) => "{\"mixId\":" + id + ",\"amount\":" + amount + ",\"isDiscovered\":true}";
        File.WriteAllText(Path.Combine(snapshot, InventorySync.InventoryFile), "{\"items\":[" + string.Join(",", Item(ids.OwnedToken, 1), Item(ids.StoreItem, 1), Item(Knight, 1),
            Item(ids.OwnedWarbondItem, 1), Item(VanillaArmoryItem, 2), Item(Medals, 119), Item(Requisition, 50000), Item(SuperCreditWallet, 180), Item(SuperCreditPrice, 180)) +
            "],\"loadouts\":[]}");
        File.WriteAllText(Path.Combine(snapshot, InventorySync.ProgressionFile), """
            {"experience":485592,"selectedProgressionPackage":131213678,"activeSeasonPass":1929468580,"onboardingState":128,"onboardingFlags":16350331,
             "packageProgress":[{"id32":2373460877,"level":10,"progress":476,"sortOrder":0,"claimedLevels":[]}],"assignments":[],
             "purchasedSeasonPassItems":[871,872],"shipName":[9,16],"location":{"warId":801,"planetIndex":171,"territoryIndex":11},"achievements":[{"id":1}]}
            """);
        SnapshotSeal.Write(snapshot, InventorySync.SealKeyFor(snapshot), Player, DateTime.UtcNow);
        return (profileDir, snapshot);
    }

    static AccountSnapshot Snapshot(string profileDir) =>
        AccountSnapshot.Open(InventorySync.SnapshotDirectoryFromProfileDir(profileDir)!, Player, out var problem) ?? throw new InvalidDataException(problem);

    static HashSet<uint> Served(ProfileStore store)
    {
        if (!store.TryGetResponse("/Progression/inventory", out var inventory)) throw new InvalidDataException("No inventory served.");
        return [.. inventory.GetProperty("items").EnumerateArray().Select(item => item.GetProperty("mixId").GetUInt32())];
    }

    static Dictionary<uint, long> Items(JsonObject profile) =>
        profile["inventory"]!["items"]!.AsArray().OfType<JsonObject>().ToDictionary(i => (uint)Whole(i["mixId"]), i => Whole(i["amount"]));

    static long Experience(JsonObject profile) => Whole(profile["progression"]!["experience"]);

    // Profile numbers may be stored as any integer type (ProfileStore), so read them from their JSON text.
    static long Whole(JsonNode? node) => long.Parse(node!.ToJsonString(), System.Globalization.CultureInfo.InvariantCulture);
}
