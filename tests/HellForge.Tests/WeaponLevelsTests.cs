using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

// Weapon levels (WeaponLevels), as the profile page's card and HellForge.Cli's weapon-levels read and set them, and the
// MOD SYNC offer's answers and TRUST warning (ModSyncOffer), as the Play page card and mod-sync-offer show them.
static class WeaponLevelsTests
{
    const uint Package = 3391943729, Weapon = 0xAA035309, Default = 0x32895C2A, Unlock = 2309820280;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Weapon levels load from the progression packages, save with their items, and typed levels are checked", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "weapon-levels")).FullName;
            var progression = Path.Combine(folder, "progression-data.json");
            File.WriteAllText(progression, new JsonObject
            {
                ["progressionPackages"] = new JsonArray(
                    new JsonObject
                    {
                        ["id32"] = Package, ["type"] = 2, ["requiredItems"] = new JsonArray(Weapon),
                        ["levels"] = new JsonArray(
                            new JsonObject { ["level"] = 1, ["rewards"] = new JsonArray(new JsonObject { ["type"] = 1, ["id32"] = Default }), ["unlockableItems"] = new JsonArray() },
                            new JsonObject { ["level"] = 2, ["rewards"] = new JsonArray(), ["unlockableItems"] = new JsonArray(new JsonObject { ["mixId"] = Unlock }) })
                    },
                    new JsonObject { ["id32"] = 1, ["type"] = 1, ["requiredItems"] = new JsonArray(), ["levels"] = new JsonArray() })
            }.ToJsonString());
            var weapons = WeaponLevels.Load(progression, Path.Combine(folder, "no-runs"));
            check(weapons.Count == 1 && weapons[0].Package == Package && weapons[0].Weapon == Weapon && weapons[0].MaxLevel == 2 && !weapons[0].Mod,
                "Loaded: " + string.Join(", ", weapons));

            var profile = Path.Combine(folder, "profile.json");
            File.WriteAllText(profile, """{"progression":{"packageProgress":[]},"inventory":{"items":[]}}""");
            check(WeaponLevels.Save(progression, profile, WeaponLevels.Parse($"{Package}=2", weapons)) == null, "Saving failed.");
            using (var saved = JsonDocument.Parse(File.ReadAllText(profile)))
            {
                check(WeaponLevels.Levels(saved.RootElement).GetValueOrDefault(Package) == 2, "The level was not saved.");
                var items = saved.RootElement.GetProperty("inventory").GetProperty("items").EnumerateArray().Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet();
                check(items.SetEquals([Default, Unlock]), "Levels 1 and 2 did not give their items: " + string.Join(", ", items));
            }
            check(WeaponLevels.Save(progression, profile, WeaponLevels.All(0, weapons)) == null, "Setting all to 0 failed.");
            using (var cleared = JsonDocument.Parse(File.ReadAllText(profile)))
                check(!WeaponLevels.Levels(cleared.RootElement).ContainsKey(Package), "Level 0 kept a progress entry.");
            check(WeaponLevels.All(9, weapons)[Package] == 2, "SET ALL went past the weapon's top level.");

            foreach (var typed in new[] { $"{Package}=3", $"{Package}=-1", "1=1", $"{Package}=1,{Package}=2", "", $"{Package}:1" })
            {
                var refused = false;
                try { WeaponLevels.Parse(typed, weapons); }
                catch (ArgumentException) { refused = true; }
                check(refused, $"\"{typed}\" was taken.");
            }
        });

        test("A MOD SYNC offer names the answers its buttons send, and its TRUST warning says when the host is a stranger", () =>
        {
            var mod = new ModSyncOffer.Mod("test.code", "Code Mod", "1.0.0", "code", "trust", 10);
            ModSyncOffer Offer(string state, long bytes, bool stranger = false) => new(3, state, "Host", bytes, 0, "", [mod], stranger);
            check(Offer("offer", 0).Choices.SequenceEqual(["restart", "decline"]), "An offer with nothing to download.");
            check(Offer("offer", 10).Choices.SequenceEqual(["accept", "decline"]), "An offer to download.");
            check(Offer("downloading", 10).Choices.SequenceEqual(["decline"]), "A download.");
            check(Offer("ready", 10).Choices.SequenceEqual(["restart", "decline"]), "A finished download.");
            check(Offer("failed", 10).Choices.SequenceEqual(["decline"]), "A failed offer.");
            check(Offer("offer", 10).TrustWarning(mod).Contains("Only trust it if you trust Host", StringComparison.Ordinal), "A friend's warning.");
            check(Offer("offer", 10, stranger: true).TrustWarning(mod).Contains("Host is not your Steam friend", StringComparison.Ordinal), "A stranger's warning.");
            check((Offer("offer", 10) with { Host = "" }).TrustNote.Contains("the one the host has", StringComparison.Ordinal), "A note without a host name.");
        });
    }
}
