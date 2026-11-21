using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;
using ProfileStore = HellForge.LocalBackend.ProfileStore;

// Weapon mastery: missions pay XP into the reported package, a mod weapon is a whole copy with its own level, items and
// purchases, saved apart from vanilla data. Report and reply shapes are the captured and parser-checked ones (MissionService).
static class MasteryTests
{
    const uint Liberator = 0xAA035309, Gold = 0x32895C2A, Second = 0x32895C2B, Package = 3391943729, Requisition = 0xD7150EEC;
    const uint Attachment2 = 2309820280; // the Liberator's level 2 unlock, 3,000 requisition
    const string Me = "76561198325117057", Friend = "76561199130940682";
    const uint FriendPackage = 1240353314;

    sealed class Rig : IDisposable
    {
        public readonly ProfileStore Store;
        public readonly RegisteredItems Items;
        public readonly MissionService Missions;
        public readonly Purchases Shop;
        public readonly Dictionary<string, JsonElement> Fixtures = LocalBackend.LoadStartupFixtures();
        public readonly string File;

        public Rig(string root, string name, string? profileDir = null, params (uint Weapon, string Mod)[] weapons)
        {
            File = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "mastery-" + name)).FullName, "registered-items.json");
            Register(weapons);
            Store = ProfileStore.Open(profileDir, "starter");
            Items = new RegisteredItems(File)
            {
                Source = path => Fixtures.TryGetValue(path, out var body) ? body : null, ModProgress = Store.ModProgress
            };
            Missions = new MissionService(Store) { Registered = Items, LocalAccount = ulong.Parse(Me) };
            Shop = new Purchases(Store, _ => null) { Registered = Items };
        }

        public void Register(params (uint Weapon, string Mod)[] weapons) =>
            System.IO.File.WriteAllText(File, "{\"format\":1,\"items\":[" +
                string.Join(",", weapons.Select(w => $$"""{"mixId":{{w.Weapon}},"copyOf":{{Liberator}},"mod":"{{w.Mod}}"}""")) + "]}");

        public JsonObject End(string mission, uint package, int difficulty = 4)
        {
            using var body = JsonDocument.Parse($$"""
                {"type":1,"difficultyType":{{difficulty}},"missionId":"{{mission}}","missionSuccess":true,"operationId":2,"missionIndex":0,
                 "objectives":[{"id":1,"succeeded":true}],"heavyLootedItems":[],
                 "participants":[
                   {"extracted":false,"loadout":[1,1],"lootedItems":[],"platformAccountId":"{{Friend}}","progressedPackages":[{"id32":{{FriendPackage}},"factor":1}]},
                   {"extracted":true,"loadout":[1,1],"lootedItems":[],"platformAccountId":"{{Me}}","progressedPackages":[{"id32":{{package}},"factor":1}]}]}
                """);
            return (JsonObject)Missions.Handle("/Operation/Mission/end", body.RootElement).Reply;
        }

        public JsonElement Progression()
        {
            Store.TryGetResponse("/Progression", out var body);
            return Items.Apply("/Progression", body);
        }

        public HashSet<uint> Inventory()
        {
            Store.TryGetResponse("/Progression/inventory", out var body);
            return [.. Items.Apply("/Progression/inventory", body).GetProperty("items").EnumerateArray().Select(i => i.GetProperty("mixId").GetUInt32())];
        }

        public int Status(string path, object body) => Shop.Handle(path, JsonSerializer.SerializeToElement(body)).Status;

        public void Dispose() => Store.Dispose();
    }

    static (int Level, long Progress)? Level(JsonElement progression, uint package)
    {
        foreach (var entry in progression.GetProperty("packageProgress").EnumerateArray())
            if (entry.GetProperty("id32").GetUInt32() == package) return (entry.GetProperty("level").GetInt32(), entry.GetProperty("progress").GetInt64());
        return null;
    }

    public static void Run(string root, Action<bool, string> check)
    {
        Earning(root, check);
        Copies(root, check);
        Buying(root, check);
        Storage(root, check);
        Launcher(root, check);
        Caching(root, check);
    }

    static void Earning(string root, Action<bool, string> check)
    {
        using var rig = new Rig(root, "earning");
        var xp = MissionService.Rewards.Experience(4, true, 1);
        check(xp == 450, "This test's XP: " + xp);
        check(Level(rig.Progression(), Package) == null, "A starter's Liberator has a level before its first mission.");
        // Level 1 costs no XP (requirementType 3), level 2 costs 400, so 450 XP ends at level 2 with 50 towards level 3.
        var reply = rig.End("m1", Package);
        var updates = reply["progressionPackageUpdates"]!.AsArray();
        check(updates.Count == 1, "One update per reported package: " + updates.ToJsonString());
        var update = updates[0]!.AsObject();
        check((string?)update["platformAccountId"] == Me && (uint)update["id32"]! == Package && (int)update["level"]! == 2 && (long)update["progress"]! == 50,
            "The update is not the weapon's state after the mission: " + update.ToJsonString());
        // The parser takes exactly these fields: a rewards that is not an object, or a numeric account, refuses the whole reply.
        check(update.Select(p => p.Key).Order().SequenceEqual(["id32", "level", "platformAccountId", "progress"]) &&
            update["platformAccountId"]!.GetValueKind() == JsonValueKind.String, "The update has a field the client's parser may refuse: " + update.ToJsonString());
        check(Level(rig.Progression(), Package) == (2, 50), "The weapon's level was not saved.");
        check(Level(rig.Progression(), FriendPackage) == null, "A squadmate's weapon earned on this profile.");
        var defaults = WeaponMastery.Definition(Package)!.Value.GetProperty("levels")[0].GetProperty("rewards").EnumerateArray().Select(r => r.GetProperty("id32").GetUInt32()).ToList();
        check(defaults.Count == 7 && defaults.All(rig.Inventory().Contains), "Level 1's default attachments were not given.");

        var retried = rig.End("m1", Package);
        check((long)retried["experience"]! == 0 && retried["progressionPackageUpdates"]!.AsArray().Count == 0 && Level(rig.Progression(), Package) == (2, 50),
            "A retried Mission/end paid the weapon again.");
        rig.End("m2", Package);
        check(Level(rig.Progression(), Package) == (3, 100), "XP does not carry between missions: " + Level(rig.Progression(), Package));

        // Without the account known, the first participant, as a host's own report lists it.
        using var unknown = JsonDocument.Parse($$"""{"missionId":"m3","participants":[{"platformAccountId":"{{Friend}}","progressedPackages":[{"id32":{{Package}},"factor":1}]}]}""");
        var noAccount = new MissionService(rig.Store) { Registered = rig.Items };
        check(((JsonObject)noAccount.Handle("/Operation/Mission/end", unknown.RootElement).Reply)["progressionPackageUpdates"]!.AsArray().Count == 1, "A one-player report was not paid.");

        // Only weapons' packages earn: a ship module, and ids the game does not have, are ignored.
        check(rig.End("m4", 131213678)["progressionPackageUpdates"]!.AsArray().Count == 0 && Level(rig.Progression(), 131213678) == null, "A ship module earned XP.");
        check(rig.End("m5", 12345)["progressionPackageUpdates"]!.AsArray().Count == 0, "An unknown package earned XP.");

        // A report without participants, as before.
        using var plain = JsonDocument.Parse("""{"missionId":"m6","difficultyType":1}""");
        check(((JsonObject)rig.Missions.Handle("/Operation/Mission/end", plain.RootElement).Reply)["progressionPackageUpdates"]!.AsArray().Count == 0, "A report with no players has updates.");

        // The last level: nothing past it, and no progress towards a level that does not exist.
        var track = WeaponMastery.Vanilla(Package)!;
        var last = default(WeaponMastery.Earned);
        rig.Store.TryChange((progression, inventory) => { last = WeaponMastery.Earn(track, progression, inventory, 10_000_000); return true; });
        check(last is { Level: 25, Progress: 0 }, "The last level: " + last);
        var total = track.Definition.GetProperty("levels").EnumerateArray().Skip(1).Sum(l => l.GetProperty("requirement").GetInt64());
        rig.Store.TryChange((progression, inventory) => { progression["packageProgress"] = new JsonArray(); last = WeaponMastery.Earn(track, progression, inventory, total - 1); return true; });
        check(last is { Level: 24 } && last.Progress == 5399, "Thresholds are not per level: " + last);

        // The served data and the embedded data the levels walk agree.
        var served = rig.Fixtures["/Progression/ProgressionPackages"].EnumerateArray().First(p => p.GetProperty("id32").GetUInt32() == Package);
        check(served.GetProperty("levels").EnumerateArray().Select(l => l.GetProperty("requirement").GetInt64())
            .SequenceEqual(track.Definition.GetProperty("levels").EnumerateArray().Select(l => l.GetProperty("requirement").GetInt64())), "The client and the backend read different thresholds.");
    }

    static void Copies(string root, Action<bool, string> check)
    {
        using var rig = new Rig(root, "copies", null, (Gold, "a"), (Second, "a"));
        var own = RegisteredItems.PackageFor(Package, Liberator, Gold);
        var second = RegisteredItems.PackageFor(Package, Liberator, Second);
        rig.End("base", Package);
        rig.End("base2", Package);
        check(Level(rig.Progression(), own) == null, "A new copy took its base's level.");
        var held = rig.Inventory();
        var baseChild = WeaponMastery.Definition(Package)!.Value.GetProperty("levels")[0].GetProperty("rewards")[0].GetProperty("id32").GetUInt32();
        check(held.Contains(Gold) && !held.Contains(RegisteredItems.Rehome(baseChild, Liberator, Gold)), "A new copy holds what its base earned.");

        var reply = rig.End("copy", own)["progressionPackageUpdates"]!.AsArray()[0]!;
        check((uint)reply["id32"]! == own && (int)reply["level"]! == 2, "A copy's update: " + reply.ToJsonString());
        var progression = rig.Progression();
        check(Level(progression, own) == (2, 50) && Level(progression, Package) == (3, 100) && Level(progression, second) == null, "Levels are not each weapon's own.");
        held = rig.Inventory();
        check(held.Contains(RegisteredItems.Rehome(baseChild, Liberator, Gold)) && !held.Contains(RegisteredItems.Rehome(baseChild, Liberator, Second)), "A copy's defaults are not its own.");

        // Nothing of it in vanilla data, and the section does not reach the client.
        var saved = rig.Store.Current();
        check(!saved["progression"]!["packageProgress"]!.AsArray().Any(e => ModPackages.Whole(e!["id32"]) == own) &&
            !saved["inventory"]!["items"]!.AsArray().Any(i => ModPackages.Whole(i!["mixId"]) == RegisteredItems.Rehome(baseChild, Liberator, Gold)) &&
            ModPackages.Find(saved["progression"]!.AsObject(), own) != null, "A copy's progress is not apart from vanilla data.");
        check(!progression.TryGetProperty(ModPackages.Section, out _) && rig.Store.TryGetResponse("/Progression", out var raw) && !raw.TryGetProperty(ModPackages.Section, out _),
            "The client was served modPackageProgress.");
        check(progression.GetProperty("packageProgress").EnumerateArray().First(e => e.GetProperty("id32").GetUInt32() == own).EnumerateObject().Select(p => p.Name).Order()
            .SequenceEqual(["claimedLevels", "id32", "level", "progress", "sortOrder"]), "A served entry is not what /Progression's parser wants.");

        // A locked mod earns nothing, and holds nothing.
        rig.Items.LockedMods = new HashSet<string> { "a" };
        check(rig.End("locked", own)["progressionPackageUpdates"]!.AsArray().Count == 0 && !rig.Inventory().Contains(Gold), "A locked mod weapon earned or was held.");
    }

    static void Buying(string root, Action<bool, string> check)
    {
        const string Unlock = "/Progression/ProgressionPackage/AcquireUnlockableItem", Level_ = "/Progression/ProgressionPackage/AcquireLevel";
        using var rig = new Rig(root, "buying", null, (Gold, "a"), (Second, "b"));
        var own = RegisteredItems.PackageFor(Package, Liberator, Gold);
        var attachment = RegisteredItems.Rehome(Attachment2, Liberator, Gold);
        object Offer(long package, long item) => new { packageId32 = package, targetLevel = 2, itemMixId = item, expectedCost = Array.Empty<object>() };
        long Money() => ProfileStore.Held(rig.Store.Current()["inventory"]!["items"]!.AsArray(), Requisition);
        rig.Store.ApplyMissionRewards(0, new Dictionary<uint, long> { [Requisition] = 20000 });
        var money = Money();

        check(rig.Status(Unlock, Offer(own, attachment)) == 400 && Money() == money, "An attachment was sold below the copy's level.");
        rig.End("base", Package);
        check(rig.Status(Unlock, Offer(own, attachment)) == 400, "The base's level opened the copy's attachment.");
        rig.End("copy", own);
        money = Money();
        check(rig.Status(Unlock, Offer(own, Attachment2)) == 400, "A copy sold its base's child.");
        check(rig.Status(Unlock, Offer(own, attachment)) == 200 && Money() == money - 3000, "A copy's attachment is not sold at its base's price.");
        check(rig.Inventory().Contains(attachment) && !rig.Inventory().Contains(Attachment2), "The purchase did not give the copy's own child alone.");
        check(ModPackages.Find(rig.Store.Current()["progression"]!.AsObject(), own)![ModPackages.HeldItems]!.AsArray().Any(i => ModPackages.Whole(i) == attachment) &&
            !rig.Store.Current()["inventory"]!["items"]!.AsArray().Any(i => ModPackages.Whole(i!["mixId"]) == attachment), "A bought child is not saved with the copy.");
        check(rig.Status(Unlock, Offer(own, attachment)) == 400 && Money() == money - 3000, "An attachment was sold twice.");
        check(rig.Status(Unlock, Offer(Package, Attachment2)) == 200 && Money() == money - 6000, "The vanilla purchase changed.");

        // A level on a copy gives the copy's rewards.
        var second = RegisteredItems.PackageFor(Package, Liberator, Second);
        var defaultItem = WeaponMastery.Definition(Package)!.Value.GetProperty("levels")[0].GetProperty("rewards")[0].GetProperty("id32").GetUInt32();
        check(rig.Status(Level_, new { id32 = second, level = 1, expectedCost = Array.Empty<object>() }) == 200 && Level(rig.Progression(), second)?.Level == 1 &&
            rig.Inventory().Contains(RegisteredItems.Rehome(defaultItem, Liberator, Second)), "AcquireLevel does not work on a copy.");

        // Premium: a locked mod's and a not-owned base's copies sell nothing.
        rig.Items.LockedMods = new HashSet<string> { "a" };
        check(rig.Status(Level_, new { id32 = own, level = 3, expectedCost = Array.Empty<object>() }) == 400, "A locked mod weapon took a level.");
        rig.Items.LockedMods = new HashSet<string>();
        rig.Items.OwnsBase = _ => false;
        check(rig.Status(Level_, new { id32 = own, level = 3, expectedCost = Array.Empty<object>() }) == 400, "A copy of a base the player does not own took a level.");
        rig.Items.OwnsBase = null;
        check(rig.Status(Unlock, Offer(RegisteredItems.PackageFor(Package, Liberator, 0x12345678), 1)) == 400, "An unknown copy's package sold something.");
    }

    // Removing a mod leaves vanilla data as it was, and adding it back restores the level; all of it across a restart.
    static void Storage(string root, Action<bool, string> check)
    {
        var folder = Path.Combine(root, "mastery-profile");
        var own = RegisteredItems.PackageFor(Package, Liberator, Gold);
        string vanilla;
        {
            using var rig = new Rig(root, "storage", folder, (Gold, "a"));
            rig.End("m", own);
            rig.Register();
            check(Level(rig.Progression(), own) == null && !rig.Inventory().Contains(Gold), "A removed mod's weapon is still served.");
            rig.Store.TryGetResponse("/Progression", out var raw);
            vanilla = raw.GetRawText();
            check(!vanilla.Contains(ModPackages.Section, StringComparison.Ordinal), "A removed mod's progress reached the client.");
            rig.Register((Gold, "a"));
            check(Level(rig.Progression(), own) == (2, 50), "The level did not come back with the mod.");
        }
        {
            using var rig = new Rig(root, "storage", folder, (Gold, "a"));
            check(Level(rig.Progression(), own) == (2, 50) && rig.Inventory().Contains(RegisteredItems.Rehome(
                WeaponMastery.Definition(Package)!.Value.GetProperty("levels")[0].GetProperty("rewards")[0].GetProperty("id32").GetUInt32(), Liberator, Gold)),
                "A copy's progress was not saved to the profile.");
        }
        var file = Path.Combine(folder, ProfileStore.FileName);
        var profile = JsonNode.Parse(File.ReadAllText(file))!.AsObject();
        profile["progression"]![ModPackages.Section] = 5;
        File.WriteAllText(file, profile.ToJsonString());
        var refused = false;
        try { ProfileStore.Load(file); } catch (InvalidDataException) { refused = true; }
        check(refused, "A malformed modPackageProgress passed validation.");
    }

    // The launcher's edit (WeaponLevels.Save calls ModPackages for a mod weapon).
    static void Launcher(string root, Action<bool, string> check)
    {
        using var rig = new Rig(root, "launcher", null, (Gold, "a"));
        var own = RegisteredItems.PackageFor(Package, Liberator, Gold);
        rig.Store.TryChange((progression, _) => { ModPackages.SetLevel(progression, own, 2); return true; });
        var attachment = RegisteredItems.Rehome(Attachment2, Liberator, Gold);
        check(Level(rig.Progression(), own) == (2, 0) && rig.Inventory().Contains(attachment), "A level set on the launcher does not give the copy's items.");
        check(rig.Status("/Progression/ProgressionPackage/AcquireUnlockableItem", new { packageId32 = own, targetLevel = 2, itemMixId = attachment, expectedCost = Array.Empty<object>() }) == 400,
            "An item the launcher granted was sold.");
        var levels = ModPackages.Levels(JsonSerializer.SerializeToElement(rig.Store.Current()["progression"]));
        check(levels.GetValueOrDefault(own) == 2, "The launcher does not read a mod weapon's level.");
        rig.Store.TryChange((progression, _) => { ModPackages.SetLevel(progression, own, 0); return true; });
        check(Level(rig.Progression(), own) == null && rig.Inventory().Contains(attachment), "Level 0 took items away, or stayed served.");
        rig.Store.TryChange((progression, _) => { ModPackages.SetLevel(progression, 77, 0); return true; });
        check(rig.Store.Current()["progression"]![ModPackages.Section]!.AsArray().Count == 1, "Level 0 made an entry that holds nothing.");
        var listed = Path.Combine(Path.GetDirectoryName(rig.File)!, "listed.json");
        File.WriteAllText(listed, $$"""{"items":[{"mixId":{{Gold}},"copyOf":{{Liberator}}},{"kind":"stratagem","mixId":5,"stable":6,"copyOf":7},{"mixId":0,"copyOf":1}]}""");
        check(ModPackages.RegisteredWeapons(listed).SequenceEqual([(Gold, Liberator)]) && ModPackages.RegisteredWeapons(listed + ".none").Count == 0, "The launcher's list of mod weapons is wrong.");
    }

    // The file and the long bodies are worked out once per version of the file.
    static void Caching(string root, Action<bool, string> check)
    {
        using var rig = new Rig(root, "caching", null, (Gold, "a"));
        var first = rig.Items.Load();
        check(ReferenceEquals(first, rig.Items.Load()), "The file was parsed again unchanged.");
        var catalogue = rig.Fixtures["/Progression/items"];
        var served = rig.Items.Apply("/Progression/items", catalogue);
        var watch = System.Diagnostics.Stopwatch.StartNew();
        for (var i = 0; i < 20; i++) rig.Items.Apply("/Progression/items", catalogue);
        check(watch.Elapsed.TotalSeconds < 2 && served.GetArrayLength() > catalogue.GetArrayLength(), "The catalogue is not kept: " + watch.Elapsed);
        rig.Register((Gold, "a"), (Second, "a"));
        check(!ReferenceEquals(first, rig.Items.Load()) && rig.Items.Load().Count == 2 &&
            rig.Items.Apply("/Progression/items", catalogue).GetArrayLength() > served.GetArrayLength(), "A weapon registered during the session was not seen.");
    }
}
