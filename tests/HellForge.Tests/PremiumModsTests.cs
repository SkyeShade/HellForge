using HellForge.Core;

// Mods built on premium content (PremiumContent, PremiumMods, the manifest's "premium"): the entries a manifest may name,
// what a synced account owns, and what a session does with a mod the player does not own. Synthetic files only.
static class PremiumModsTests
{
    const uint PremiumWarbond = 610512218, PremiumToken = 2021502022, Knight = 2161995568;
    const ulong Account = 76561198000000001;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A manifest's premium entries are warbond:<id>, a known dlc:<name> or item:<mixId>, in one spelling", () =>
        {
            check(PremiumContent.Normalize("warbond:0610512218") == "warbond:610512218" && PremiumContent.Normalize("dlc:super-citizen-edition") == "dlc:super-citizen-edition" &&
                PremiumContent.Normalize("item:2161995568") == "item:2161995568", "Valid entries were not taken.");
            foreach (var bad in new[] { "dlc:gold-eagle", "item:0", "warbond:-1", "warbond:", "super-citizen-edition", "token:5", "item:1 " })
                reject(() => PremiumContent.Normalize(bad));

            var folder = ModFolder(root, "premium-manifest", """["dlc:super-citizen-edition", "warbond:610512218", "dlc:super-citizen-edition"]""");
            check(ModCatalog.Parse(folder, EntityProfile.Current).Premium.SequenceEqual(["dlc:super-citizen-edition", "warbond:610512218"]),
                "The manifest's premium list was not read, or kept a repeated entry.");
            reject(() => ModCatalog.Parse(ModFolder(root, "premium-typo", """["dlc:super-citizen"]"""), EntityProfile.Current));
            reject(() => ModCatalog.Parse(ModFolder(root, "premium-shape", "\"dlc:super-citizen-edition\""), EntityProfile.Current));
            check(ModCatalog.Parse(ModFolder(root, "premium-none", null), EntityProfile.Current).Premium.Count == 0, "A mod without a premium list declares some.");
        });

        test("A synced account owns a warbond by its token, the free warbond always, a DLC by any of its gear, an item by holding it", () =>
        {
            var data = ProgressionData(root);
            var none = PremiumContent.Load(data, []);
            check(none.Owns("warbond:" + PremiumContent.FreeWarbond) && !none.Owns("warbond:" + PremiumWarbond) && !none.Owns("dlc:super-citizen-edition") &&
                !none.Owns("item:" + Knight) && !none.Owns("warbond:42"), "Without items only the free warbond is owned.");
            var owner = PremiumContent.Load(data, [PremiumToken, Knight]);
            check(owner.Owns("warbond:" + PremiumWarbond) && owner.Owns("dlc:super-citizen-edition") && owner.Owns("item:" + Knight) &&
                !owner.Owns("dlc:tr-40-gold-eagle"), "Held tokens or DLC gear are not owned.");
            check(owner.Missing(["dlc:super-citizen-edition", "dlc:tr-40-gold-eagle"]).SequenceEqual(["dlc:tr-40-gold-eagle"]), "Missing lists the wrong entries.");
            check(!PremiumContent.Nothing.Owns("warbond:" + PremiumContent.FreeWarbond), "Nothing owns the free warbond.");
        });

        test("Ownership comes only from a snapshot sealed on this PC for the Steam account signed in", () =>
        {
            var data = ProgressionData(root);
            var snapshot = Directory.CreateDirectory(Path.Combine(root, "premium-install", InventorySync.SnapshotFolder, "player")).FullName;
            File.WriteAllText(Path.Combine(snapshot, InventorySync.InventoryFile), "{\"items\":[{\"mixId\":" + Knight + ",\"amount\":1,\"isDiscovered\":true}]}");
            check(!PremiumContent.ForSnapshot(data, snapshot, Account).Owns("item:" + Knight), "An unsealed snapshot gave ownership.");
            SnapshotSeal.Write(snapshot, InventorySync.SealKeyFor(snapshot), Account, DateTime.UtcNow);
            check(PremiumContent.ForSnapshot(data, snapshot, Account).Owns("dlc:super-citizen-edition"), "A sealed snapshot gave no ownership.");
            check(!PremiumContent.ForSnapshot(data, snapshot, Account + 1).Owns("item:" + Knight) && !PremiumContent.ForSnapshot(data, snapshot, null).Owns("item:" + Knight),
                "Another Steam account, or none, owned the snapshot's items.");
            check(!PremiumContent.ForSnapshot(data, null, Account).Owns("item:" + Knight), "A profile without a snapshot owned something.");
        });

        test("A session keeps a gameplay mod the player does not own but locks its items, and leaves such a cosmetic mod out", () =>
        {
            var data = ProgressionData(root);
            ModManifest Mod(string id, bool gameplay, params string[] premium) => new(id, id, "1", "", gameplay, Path.Combine(root, id), [], [], []) { Premium = premium };
            var mods = new[] { Mod("free", true), Mod("knight-variant", true, "dlc:super-citizen-edition"), Mod("knight-look", false, "dlc:super-citizen-edition"),
                Mod("warbond-port", true, "warbond:" + PremiumWarbond) };
            var none = PremiumMods.Apply(mods, PremiumContent.Load(data, []));
            check(none.Loaded.Select(m => m.Id).SequenceEqual(["free", "knight-variant", "warbond-port"]) && none.Locked.SequenceEqual(["knight-variant", "warbond-port"]),
                "Without the content: a gameplay mod was dropped or not locked, or a cosmetic one was loaded.");
            check(none.Problems.Count == 3 && none.Problems.Any(p => p.Contains("Super Citizen Edition", StringComparison.Ordinal) && p.Contains("not loaded", StringComparison.Ordinal)),
                "The player is not told what is missing.");
            var owner = PremiumMods.Apply(mods, PremiumContent.Load(data, [Knight, PremiumToken]));
            check(owner.Loaded.Count == 4 && owner.Locked.Count == 0 && owner.Problems.Count == 0, "An owner's premium mods were locked.");

            var install = new InstallLayout(Path.Combine(root, "premium-session"));
            var plain = PremiumMods.ForSession(install, null, Account, [mods[0]]);
            check(plain.Loaded.Count == 1 && plain.Problems.Count == 0, "A session without premium mods needed progression data.");
            var withoutData = PremiumMods.ForSession(install, null, Account, mods);
            check(withoutData.Locked.Count == 2 && withoutData.Problems[0].Contains("missing", StringComparison.Ordinal), "Missing progression data unlocked premium mods.");
        });

        test("Every option the session runner passes mods-stage is one the command line accepts", () =>
        {
            var source = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../src"));
            var cli = File.ReadAllText(Path.Combine(source, "HellForge.Cli", "Program.cs"));
            var accepted = System.Text.RegularExpressions.Regex.Match(cli, "\"mods-stage\" => \\[(.*?)\\]").Groups[1].Value;
            var session = File.ReadAllText(Path.Combine(source, "HellForge.Session", "PlaySession.cs"));
            var stage = session[session.IndexOf("string[] stage = [\"mods-stage\"", StringComparison.Ordinal)..session.IndexOf("HellForgeCli(stage)", StringComparison.Ordinal)];
            var premium = session[session.IndexOf("string[] PremiumStageArguments(", StringComparison.Ordinal)..];
            premium = premium[..premium.IndexOf(';')];
            var passed = System.Text.RegularExpressions.Regex.Matches(stage + premium, "\"(--[a-z][a-z-]+)\"").Select(m => m.Groups[1].Value).Distinct().ToList();
            check(passed.Contains("--premium-snapshot") && passed.Contains("--modset"), "The session runner's mods-stage options were not found.");
            check(passed.All(option => accepted.Contains('"' + option + '"', StringComparison.Ordinal)),
                "mods-stage refuses an option the session passes: " + string.Join(", ", passed.Where(o => !accepted.Contains('"' + o + '"', StringComparison.Ordinal))));
        });

        test("The locked mods file reads back what was written; a file that cannot be read is refused", () =>
        {
            var file = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "premium-locked")).FullName, "premium-locked.json");
            PremiumMods.WriteLocked(file, ["a.knight", "b.port"]);
            check(PremiumMods.ReadLocked(file).SetEquals(["a.knight", "b.port"]) && PremiumMods.ReadLocked(null).Count == 0, "The locked mods did not round-trip.");
            File.WriteAllText(file, "{\"mods\":");
            reject(() => PremiumMods.ReadLocked(file));
            File.WriteAllText(file, "{\"mods\":[1]}");
            reject(() => PremiumMods.ReadLocked(file));
        });
    }

    // A data mod (one patch file) with the given premium list (JSON), or none.
    static string ModFolder(string root, string name, string? premium)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, name, "data")).Parent!.FullName;
        File.WriteAllText(Path.Combine(folder, "data", "0123456789abcdef.patch_0"), "synthetic");
        File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName),
            "{\"format\":1,\"id\":\"test." + name + "\",\"name\":\"" + name + "\",\"version\":\"1\"" + (premium == null ? "" : ",\"premium\":" + premium) + "}");
        return folder;
    }

    // The free warbond, one premium warbond with its token, and the Super Citizen Edition's package.
    static string ProgressionData(string root)
    {
        var path = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "premium-data")).FullName, "progression-data.json");
        File.WriteAllText(path, $$$"""
            {"seasonPassDetails":{"{{{PremiumContent.FreeWarbond}}}":{"id32":{{{PremiumContent.FreeWarbond}}},"premiumTokenMixIds":[2032304616]},
              "{{{PremiumWarbond}}}":{"id32":{{{PremiumWarbond}}},"premiumTokenMixIds":[{{{PremiumToken}}},1093811959]}},
             "itemPackages":[{"itemMixId32":1542124658,"items":[{"mixId":{{{Knight}}},"amount":1},{"mixId":1296068557,"amount":1}]}]}
            """);
        return path;
    }
}
