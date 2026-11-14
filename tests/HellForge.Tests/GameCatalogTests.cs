using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

// The game catalogues from the player's own Sync Inventory (GameCatalog): cut to the pinned build's ids in the API's order,
// installed beside the installation, and required before a session starts. Synthetic catalogues only.
static class GameCatalogTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The game catalogues keep only the pinned build's entries, in the API's order, with the API's extra keys and the pinned warbond pages", () =>
        {
            var folder = Captured(Path.Combine(root, "catalogue-assemble"));
            var pinned = Pinned(Path.Combine(root, "catalogue-assemble"));
            var assembled = GameCatalog.Assemble(folder, pinned);
            var items = assembled.ItemCatalog["items"]!.AsArray();
            check(items.Count == 2 && (uint)items[0]!["mixId"]! == 20 && (uint)items[1]!["mixId"]! == 10,
                "The items are not the two pinned ones in the API's order: " + items.ToJsonString());
            check(items[0]!["buyPrice"]![0]!["flags"] != null, "The API's extra key on a price was dropped.");
            var data = assembled.ProgressionData;
            check(data["seasonPass"]!.AsArray().Select(e => (uint)e!["id32"]!).SequenceEqual([1929468580u]), "A warbond the pinned build does not know was kept.");
            check(data["seasonPass"]![0]!["itemIds"] != null, "The warbond list lost the API's extra key.");
            check(data["progressionPackages"]!.AsArray().Count == 1 && data["itemPackages"]!.AsArray().Count == 1 && data["levelSpec"]!.AsArray().Count == 2,
                "The packages or the level table were not cut or kept as the pinned file says.");
            check(data["seasonPassDetails"]!["1929468580"] != null && data["storeFront"]!.AsArray().Count == 1,
                "The pinned warbond pages or store did not come from the pinned file.");
            check((string?)data["build"] == "25480438", "The assembled catalogue does not name the pinned build.");
        });

        test("A captured catalogue missing a file, not a list, or sharing nothing with the pinned build is refused", () =>
        {
            var folder = Captured(Path.Combine(root, "catalogue-refusals"));
            var pinned = Pinned(Path.Combine(root, "catalogue-refusals"));
            check(GameCatalog.CheckCaptured(folder) == null, "A complete capture was refused.");
            File.WriteAllText(Path.Combine(folder, "levelspec.json"), "{}");
            check(GameCatalog.CheckCaptured(folder) == "levelspec.json is not a list of entries", "An object was taken for a list.");
            File.Delete(Path.Combine(folder, "levelspec.json"));
            check(GameCatalog.CheckCaptured(folder) == "levelspec.json was not captured", "A missing catalogue was not named.");
            var other = Captured(Path.Combine(root, "catalogue-other-game"));
            File.WriteAllText(Path.Combine(other, "items.json"), """[{"mixId":7,"itemId":7}]""");
            check(Throws(() => GameCatalog.Assemble(other, pinned)), "A catalogue sharing no items with the pinned build was assembled.");
        });

        test("A session needs the installation's catalogues: none before a sync, assembled once one is installed, and again when the pinned file is newer", () =>
        {
            var install = Directory.CreateDirectory(Path.Combine(root, "catalogue-install")).FullName;
            var app = Directory.CreateDirectory(Path.Combine(install, "app")).FullName;
            File.Copy(Pinned(Path.Combine(root, "catalogue-install-source")), Path.Combine(app, GameCatalog.PinnedFile));
            check(GameCatalog.Ensure(install) == GameCatalog.SyncFirst, "An installation without a sync was ready.");
            // What the launcher asks before PLAY starts anything: the same answer, read only.
            check(GameCatalog.Missing(install) && !Directory.Exists(GameCatalog.Directory(install)),
                "An installation without a sync was not reported as missing its catalogue, or the check wrote something.");
            GameCatalog.Install(install, Captured(Path.Combine(root, "catalogue-install-capture")), DateTime.UtcNow);
            check(!GameCatalog.Missing(install), "A synced installation was reported as missing its catalogue.");
            check(GameCatalog.Ensure(install) == null && File.Exists(GameCatalog.ProgressionData(install)) && File.Exists(GameCatalog.ItemCatalog(install)),
                "An installed catalogue was not assembled.");
            check(!GameCatalog.Missing(install), "An assembled catalogue was reported as missing.");
            check(File.Exists(Path.Combine(GameCatalog.CapturedDirectory(install), GameCatalog.CapturedStampFile)), "The capture's time was not kept.");
            // An update with new pinned ids reassembles: a pinned file newer than the assembled one.
            var pinned = JsonNode.Parse(File.ReadAllText(Path.Combine(app, GameCatalog.PinnedFile)))!.AsObject();
            pinned["items"]!.AsArray().Add(new JsonArray(30, 30));
            File.WriteAllText(Path.Combine(app, GameCatalog.PinnedFile), pinned.ToJsonString());
            File.SetLastWriteTimeUtc(Path.Combine(app, GameCatalog.PinnedFile), DateTime.UtcNow.AddMinutes(1));
            check(GameCatalog.Ensure(install) == null, "A newer pinned file was not taken.");
            using (var catalog = JsonDocument.Parse(File.ReadAllBytes(GameCatalog.ItemCatalog(install))))
                check(catalog.RootElement.GetProperty("items").GetArrayLength() == 3, "A backported id did not reach the assembled items.");
            // An installation keeps its last good catalogue when a later capture is broken.
            var broken = Captured(Path.Combine(root, "catalogue-install-broken"));
            File.Delete(Path.Combine(broken, "SeasonPass.json"));
            check(Throws(() => GameCatalog.Install(install, broken, DateTime.UtcNow)) && GameCatalog.Ensure(install) == null,
                "A broken capture replaced a good catalogue.");
        });
    }

    static bool Throws(Action action)
    {
        try { action(); return false; }
        catch (InvalidDataException) { return true; }
    }

    // What the game's five catalogue reads might answer: three items, one new to the pinned build, in the API's order.
    static string Captured(string folder)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "items.json"), """
            [{"mixId":20,"itemId":20,"buyPrice":[{"mixId":3608481516,"amount":5,"flags":0}],"progressionCategory":2},
             {"mixId":99,"itemId":99,"progressionCategory":4},
             {"mixId":10,"itemId":10,"progressionCategory":0},
             {"mixId":30,"itemId":30,"progressionCategory":3}]
            """);
        File.WriteAllText(Path.Combine(folder, "ItemPackages.json"), """[{"itemMixId32":5,"items":[]},{"itemMixId32":6,"items":[]}]""");
        File.WriteAllText(Path.Combine(folder, "ProgressionPackages.json"), """[{"id32":7,"levels":[]},{"id32":8,"levels":[]}]""");
        File.WriteAllText(Path.Combine(folder, "levelspec.json"), """[{"level":1,"requiredExperience":1},{"level":2,"requiredExperience":100}]""");
        File.WriteAllText(Path.Combine(folder, "SeasonPass.json"), """[{"id32":1929468580,"itemIds":[1,2]},{"id32":2512818458,"itemIds":[3]}]""");
        return folder;
    }

    static string Pinned(string folder)
    {
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, GameCatalog.PinnedFile);
        File.WriteAllText(path, """
            {"format":1,"build":"25480438","items":[[10,10],[20,20]],"itemPackages":[5],"progressionPackages":[7],"seasonPass":[1929468580],
             "seasonPassDetails":{"1929468580":{"id32":1929468580,"pages":[]}},"storeFront":[{"id":1}]}
            """);
        return path;
    }
}
