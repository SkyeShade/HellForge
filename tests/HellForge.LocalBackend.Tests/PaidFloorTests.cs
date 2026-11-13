using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;

// The compiled paid item list (HellForge.Core PaidItems) against the suite's catalogue: it is what that catalogue's rules
// give, so an honest player's classification is unchanged; and a catalogue edited every way a player could still leaves
// every paid and premium warbond item where it was.
static class PaidFloorTests
{
    const uint Owned = 610512218, Other = 19991972;

    public static void Run(string root, Action<bool, string> check)
    {
        var floor = PaidItems.Current;
        // the list's build is the one HellForge supports and pins the catalogue to: a list left from another build would
        // name the wrong ids and order
        check(floor.Build == SupportedBuild.Current.BuildId && floor.Build == PinnedBuild(),
            $"data/paid-items.json is for build {floor.Build}, not the supported {SupportedBuild.Current.BuildId} (catalogue pinned to {PinnedBuild()}): run paid-list on that build's catalogue.");
        var plain = PaidContentSet.FromCatalogue();
        var build = PaidContentSet.CatalogueBuild();
        if (build == floor.Build) SameAsCatalogue(floor, plain, check);
        else Console.WriteLine($"NOTE paid floor: the suite's catalogue is build {build}, the list {floor.Build}: equality not checked");
        var merged = PaidContentSet.Load();
        check(merged.MixIds.SetEquals(plain.MixIds.Union(floor.Paid)) && Tokens(merged, Owned).SequenceEqual(floor.Warbonds.Single(w => w.Id == Owned).Tokens),
            "The paid set from an unedited catalogue is not the list's.");
        Edited(root, plain, check);
    }

    // data/catalogue-pinned.json's build, read from the repository above the tests.
    static string? PinnedBuild()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "data", GameCatalog.PinnedFile)))
                return (string?)JsonNode.Parse(File.ReadAllText(Path.Combine(folder.FullName, "data", GameCatalog.PinnedFile)))?["build"];
        return null;
    }

    // The list holds exactly what the rules give for its build, so the [premium] count and every armory stay as they were.
    static void SameAsCatalogue(PaidItems floor, PaidContentSet plain, Action<bool, string> check)
    {
        var written = plain.ToPaidItems(floor.Build);
        check(PaidItems.Parse(written.ToJson()).ToJson() == floor.ToJson(),
            "data/paid-items.json is not what the catalogue's rules give: run HellForge.LocalBackend paid-list on a catalogue of build " + floor.Build + ".");
        check(floor.Paid.Count > 100 && floor.Gated.Count > 100 && floor.Parents.Count > 100 && floor.Warbonds.Count > 10, "The paid item list is nearly empty.");
        check(PaidItems.Parse(floor.ToJson()).ToJson() == floor.ToJson(), "The paid item list does not read back as written.");
    }

    // A catalogue copy edited each way that used to free paid items, read through the backend's rules.
    static void Edited(string root, PaidContentSet plain, Action<bool, string> check)
    {
        var suite = LocalBackend.CatalogueDirectory!;
        var folder = Directory.CreateDirectory(Path.Combine(root, "paid-floor-edited")).FullName;
        var progression = JsonNode.Parse(File.ReadAllText(Path.Combine(suite, GameCatalog.ProgressionDataFile)))!.AsObject();
        var catalog = JsonNode.Parse(File.ReadAllText(Path.Combine(suite, GameCatalog.ItemCatalogFile)))!.AsObject();
        var ownedToken = Tokens(plain, Owned)[0];
        var premiumItem = Items(plain, Other).First(i => plain.TryGetWarbondTokens(i, out var tokens) && !tokens.Contains(ownedToken) && !plain.IsPaid(i));
        var store = plain.MixIds.Except(plain.WarbondTokens).Except(plain.DlcItems).First();
        var (child, parent) = plain.ToPaidItems("").Parents.First(entry => plain.TryGetWarbondTokens(entry.Value, out var tokens) && !tokens.Contains(ownedToken));
        var dlc = plain.DlcItems.First(i => !PremiumContent.SteamDlc.Values.Any(packages => packages.Contains(i)));
        var third = plain.Warbonds.First(w => w.Tokens.Length > 0 && w.Id is not Owned and not Other).Id;
        Edit(progression, catalog, (ownedToken, premiumItem, store, child, dlc), third);
        File.WriteAllText(Path.Combine(folder, GameCatalog.ProgressionDataFile), progression.ToJsonString());
        File.WriteAllText(Path.Combine(folder, GameCatalog.ItemCatalogFile), catalog.ToJsonString());
        try
        {
            LocalBackend.CatalogueDirectory = folder;
            var edited = PaidContentSet.FromCatalogue();
            check(!edited.IsPaid(store) && !edited.TryGetWarbondTokens(premiumItem, out _) && !edited.Warbonds.Any(w => w.Id == third),
                "The edited catalogue does not open anything on its own, so the checks below prove nothing.");
            var paid = PaidContentSet.Load();
            var filter = new OwnershipFilter(paid, [new JsonObject { ["mixId"] = ownedToken, ["amount"] = 1 }]);
            check(paid.IsPaid(store) && !filter.IsAllowed(store), "A Super Credits item without its price was served.");
            check(!filter.IsAllowed(premiumItem), "A premium warbond item was served through an edited warbond.");
            check(!filter.IsAllowed(Tokens(plain, Other)[0]) && !filter.Owns(Tokens(plain, Other)[0]), "A premium warbond was owned through a token added to it.");
            check(!filter.IsAllowed(child) && paid.TryGetParent(child, out var kept) && kept == parent, "A premium item's child was served without its parent.");
            check(paid.IsPaid(dlc) && !filter.IsAllowed(dlc), "Steam DLC gear was served after its package was emptied.");
            check(paid.Warbonds.Single(w => w.Id == third).Tokens.SequenceEqual(Tokens(plain, third)) && !paid.OwnedWarbonds(new HashSet<uint> { ownedToken }).Any(w => w.Id == third) &&
                Items(plain, third).Where(i => plain.TryGetWarbondTokens(i, out var tokens) && !tokens.Contains(ownedToken)).All(i => !filter.IsAllowed(i)),
                "A premium warbond re-keyed as Helldivers Mobilize lost its tokens.");
            var premium = PremiumContent.Load(Path.Combine(folder, GameCatalog.ProgressionDataFile), [ownedToken]);
            var knight = PremiumContent.Load(Path.Combine(folder, GameCatalog.ProgressionDataFile), [2161995568]);
            check(!premium.Owns("warbond:" + Other) && !premium.Owns("warbond:" + third) && premium.Owns("warbond:" + Owned) && knight.Owns("dlc:super-citizen-edition"),
                "A mod's premium list followed an edited warbond or an emptied DLC package.");
        }
        finally { LocalBackend.CatalogueDirectory = suite; }
    }

    // Each edit: Super Credits prices gone, the other premium warbond's tokens joined by an owned one, its item also on the
    // owned warbond's pages and on Mobilize's, a third premium warbond re-keyed as Mobilize, every paid item a package
    // reward, the child's parentId gone, and the DLC packages emptied.
    static void Edit(JsonObject progression, JsonObject catalog, (uint OwnedToken, uint PremiumItem, uint Store, uint Child, uint Dlc) ids, uint third)
    {
        var (ownedToken, premiumItem, store, child, dlc) = ids;
        foreach (var item in catalog["items"]!.AsArray().OfType<JsonObject>())
        {
            item["buyPrice"] = new JsonArray();
            if ((uint?)item["mixId"] == child) item.Remove("parentId");
        }
        var details = progression["seasonPassDetails"]!.AsObject();
        var other = details[Other.ToString()]!.AsObject();
        other["premiumTokenMixIds"]!.AsArray().Add(ownedToken);
        var page = new JsonObject { ["seasonPassItemId"] = 999001, ["itemMixId"] = premiumItem, ["medalCost"] = 1 };
        foreach (var pass in new[] { details[Owned.ToString()]!, details[PremiumContent.FreeWarbond.ToString()]! })
            pass["seasonPassPages"]![0]!["seasonPassItems"]!.AsArray().Add(page.DeepClone());
        var rekeyed = details[third.ToString()]!.AsObject();
        details.Remove(third.ToString());
        rekeyed["id32"] = PremiumContent.FreeWarbond;
        rekeyed["premiumTokenMixIds"] = new JsonArray();
        details["copy-" + third] = rekeyed;
        var rewards = new JsonArray([.. new[] { store, premiumItem, dlc }.Select(id => (JsonNode)new JsonObject { ["type"] = 1, ["id32"] = id, ["amount"] = 1 })]);
        progression["progressionPackages"]!.AsArray().Add(new JsonObject { ["id32"] = 999002, ["levels"] = new JsonArray(new JsonObject { ["level"] = 1, ["rewards"] = rewards }) });
        foreach (var package in progression["itemPackages"]!.AsArray().OfType<JsonObject>()) package["items"] = new JsonArray();
    }

    static uint[] Tokens(PaidContentSet paid, uint warbond) => paid.Warbonds.Single(w => w.Id == warbond).Tokens;

    static IEnumerable<uint> Items(PaidContentSet paid, uint warbond) => paid.Warbonds.Single(w => w.Id == warbond).Items.Select(i => i.MixId);
}
