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

        // The tests below are for #392's review points.
        test("A level typed for --all is plain digits, read the same whatever the PC's language", () =>
        {
            var before = System.Globalization.CultureInfo.CurrentCulture;
            try
            {
                foreach (var culture in new[] { "en-US", "de-DE", "ar-SA", "fa-IR", "th-TH" })
                {
                    System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo(culture);
                    check(WeaponLevels.ParseLevel("0") == 0 && WeaponLevels.ParseLevel("25") == 25, "A plain level was not read under " + culture);
                    foreach (var typed in new[] { "-1", "+5", " 5", "5 ", "1.000", "1,000", "5.0", "٥", "۵", "five", "", "99999999999" })
                    {
                        var refused = false;
                        try { WeaponLevels.ParseLevel(typed); }
                        catch (ArgumentException) { refused = true; }
                        check(refused, $"\"{typed}\" was taken as a level under {culture}.");
                    }
                }
            }
            finally { System.Globalization.CultureInfo.CurrentCulture = before; }
        });

        test("Each mod of a MOD SYNC offer names the answers that apply to it: trust and open, as the card's buttons", () =>
        {
            static ModSyncOffer.Mod Mod(string action, string? url = null) => new("test.mod", "Mod", "1.0.0", "code", action, 10, url);
            static ModSyncOffer In(string state) => new(3, state, "Host", 10, 0, "", []);
            const string page = "https://example.com/mod";
            check(In("offer").ModChoices(Mod("trust", page)).SequenceEqual(["trust", "open"]), "A mod to trust with a page, in an offer.");
            check(In("blocked").ModChoices(Mod("trust")).SequenceEqual(["trust"]), "A mod to trust without a page, in a blocked offer.");
            check(In("downloading").ModChoices(Mod("trust", page)).SequenceEqual(["open"]), "TRUST was offered during a download.");
            check(In("ready").ModChoices(Mod("trust")).Count == 0 && In("failed").ModChoices(Mod("trust")).Count == 0, "TRUST was offered on a ready or failed offer.");
            check(In("offer").ModChoices(Mod("manual", page)).SequenceEqual(["open"]), "A mod to install by hand with a page.");
            check(In("offer").ModChoices(Mod("manual")).Count == 0, "GET IT was offered for a mod with no page.");
            foreach (var action in new[] { "have", "download", "blocked", "update", "off" })
                check(In("offer").ModChoices(Mod(action, page)).Count == 0, "An answer was offered for a mod with the action " + action);
        });

        test("mod-sync-offer reads whether the game shows the prompt from the session's own record, not from the mods folder", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "mod-sync-run-editor")).FullName;
            var mods = Directory.CreateDirectory(Path.Combine(folder, "mods")).FullName;
            // A mods folder that cannot be loaded as mods at all: nothing in it is read but the settings file.
            Directory.CreateDirectory(Path.Combine(mods, "broken"));
            File.WriteAllText(Path.Combine(mods, "broken", "hellforge-mod.json"), "{ not json");
            static SessionMod Ran(string id) => new(id, "1.0.0", "folder", new string('a', 64), "", true, false);
            string Run(string name, params string[] ids)
            {
                var run = Directory.CreateDirectory(Path.Combine(folder, name)).FullName;
                new SessionMods([.. ids.Select(Ran)], [], "").Write(Path.Combine(run, SessionMods.FileName));
                return run;
            }
            var withEditor = Run("with-editor", "test.rifle", ModSyncRun.EditorId);
            var without = Run("without-editor", "test.rifle");
            var notStaged = Directory.CreateDirectory(Path.Combine(folder, "not-staged")).FullName;
            var unreadable = Directory.CreateDirectory(Path.Combine(folder, "unreadable")).FullName;
            File.WriteAllText(Path.Combine(unreadable, SessionMods.FileName), "{");

            check(ModSyncRun.InGamePrompt(withEditor, mods) && !ModSyncRun.InGamePrompt(without, mods), "The session's record was not read.");
            check(ModSyncRun.InGamePrompt(notStaged, mods) && ModSyncRun.InGamePrompt(unreadable, mods) && ModSyncRun.InGamePrompt(null, mods),
                "With no record, the editor's switch (on) was not used.");
            // The player switches the editor off during the session: the running session still has it, the next one will not.
            ModSettings.SetEnabled(mods, ModSyncRun.EditorId, false);
            check(ModSyncRun.InGamePrompt(withEditor, mods), "The mods folder as it is now changed a running session's answer.");
            check(!ModSyncRun.InGamePrompt(without, mods), "A session without the editor shows the prompt.");
            check(!ModSyncRun.InGamePrompt(notStaged, mods) && !ModSyncRun.InGamePrompt(unreadable, mods) && !ModSyncRun.InGamePrompt(null, mods),
                "With no record, the editor's switch (off) was not used.");
            // And back on, while the session that started without it still runs.
            ModSettings.SetEnabled(mods, ModSyncRun.EditorId, true);
            check(!ModSyncRun.InGamePrompt(without, mods), "Switching the editor on changed a running session's answer.");
        });

        test("A MOD SYNC offer waits only in the run that is running: not in one that ended or whose game is gone", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "mod-sync-run-waiting")).FullName;
            string Run(string name, string? result, string state = "offer")
            {
                var run = Directory.CreateDirectory(Path.Combine(folder, name)).FullName;
                Directory.CreateDirectory(Path.Combine(run, "backend"));
                if (result != null) File.WriteAllText(Path.Combine(run, "result.json"), result);
                File.WriteAllText(Path.Combine(run, "backend", ModSyncOffer.FileName), new JsonObject
                {
                    ["version"] = 3, ["state"] = state, ["host"] = "Host", ["bytes"] = 10, ["done"] = 0, ["message"] = "Host plays with a mod.",
                    ["mods"] = new JsonArray(new JsonObject { ["id"] = "test.code", ["name"] = "Code", ["version"] = "1.0.0", ["class"] = "code", ["action"] = "trust", ["bytes"] = 10 })
                }.ToJsonString());
                return run;
            }
            static bool Alive(int pid) => pid == 4242;
            static bool Gone(int pid) => false;

            var playing = Run("playing", """{"status": "playing", "gamePid": 4242}""");
            check(ModSyncRun.Waiting(playing, true, Alive)?.Version == 3 && ModSyncRun.Waiting(playing, true)?.Version == 3, "The running session's offer does not wait.");
            check(ModSyncRun.Waiting(playing, false, Alive) == null, "An offer waits with no session running.");
            // The review's case: another installation's session runs, and this one's last run left its offer behind.
            foreach (var status in new[] { "observed", "failed", "cleanup_failed" })
                check(ModSyncRun.Waiting(Run("ended-" + status, $$"""{"status": "{{status}}", "gamePid": 4242}"""), true, Alive) == null, "An offer waits in a run that ended: " + status);
            check(ModSyncRun.Waiting(playing, true, Gone) == null, "An offer waits in a run whose game is gone (a crash keeps its last status).");
            check(ModSyncRun.Waiting(playing, true, null) != null, "Where the game's process cannot be asked about, the status alone decides.");
            check(ModSyncRun.Waiting(Run("no-game-yet", """{"status": "preparing"}"""), true, Gone) != null, "A run with no game yet was read as gone.");
            check(ModSyncRun.Waiting(Run("no-result", null), true, Gone) != null && ModSyncRun.Waiting(Run("half-written", "{"), true, Gone) != null,
                "A run whose result.json is not there yet, or is being written, was read as ended.");
            check(ModSyncRun.Waiting(Run("closed", """{"status": "playing"}""", "none"), true) == null && ModSyncRun.Waiting(Run("empty", """{"status": "playing"}""", ""), true) == null,
                "A closed offer waits.");
            check(ModSyncRun.Waiting(null, true) == null && ModSyncRun.Waiting(Path.Combine(folder, "missing"), true) == null, "An offer waits without a run.");
        });

        test("An answer to a MOD SYNC offer is checked before it is written, and refused while the game shows the prompt", () =>
        {
            var mod = new ModSyncOffer.Mod("test.code", "Code Mod", "1.0.0", "code", "trust", 10);
            var offer = new ModSyncOffer(3, "offer", "Host", 10, 0, "", [mod]);
            check(ModSyncRun.CheckAnswer(offer, false, "accept", "3", null) == 3 && ModSyncRun.CheckAnswer(offer, false, "trust", "3", "test.code") == 3 &&
                ModSyncRun.CheckAnswer(offer, false, "block", "3", "test.code") == 3, "A good answer was refused.");

            void Refused<T>(string why, ModSyncOffer? to, bool inGame, string answer, string? version, string? modId) where T : Exception
            {
                var refused = false;
                try { ModSyncRun.CheckAnswer(to, inGame, answer, version, modId); }
                catch (T) { refused = true; }
                check(refused, "Not refused: " + why);
            }
            Refused<InvalidDataException>("the game shows the prompt itself", offer, true, "accept", "3", null);
            Refused<InvalidDataException>("trust while the game shows the prompt itself", offer, true, "trust", "3", "test.code");
            Refused<InvalidDataException>("no offer waits", null, false, "accept", "3", null);
            Refused<InvalidDataException>("an older offer's version", offer, false, "accept", "2", null);
            Refused<ArgumentException>("no version", offer, false, "accept", null, null);
            foreach (var typed in new[] { "+3", " 3", "3.0", "-3", "٣", "three" })
                Refused<ArgumentException>("the version " + typed, offer, false, "accept", typed, null);
            Refused<ArgumentException>("an unknown answer", offer, false, "yes", "3", null);
            Refused<ArgumentException>("trust without a mod", offer, false, "trust", "3", null);
            Refused<ArgumentException>("accept with a mod", offer, false, "accept", "3", "test.code");
            Refused<ArgumentException>("a mod id that is none", offer, false, "trust", "3", "bad id");
            Refused<ArgumentException>("a mod the offer does not have", offer, false, "trust", "3", "test.other");
        });
    }
}
