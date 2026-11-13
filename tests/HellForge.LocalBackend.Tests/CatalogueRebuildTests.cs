using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;

// The served catalogue's paid families rebuilt from the compiled list (PaidContentSet.Rebuilt). Each reply is read as
// the game reads it: a child links to the root before it when its parentId is that root's, else to the entry before it
// (0x136EC00); a stratagem is held when the first category-10 entry with its stable id, or that entry's parent, is held
// (0x136FC20). Moving a paid child under a free root, or adding an entry under a paid stable id, then holds nothing.
static class CatalogueRebuildTests
{
    const uint SharedStable = 2265180087, SharedChild = 1247082912;

    public static void Run(string root, Action<bool, string> check)
    {
        var paid = PaidContentSet.Load();
        var filter = new OwnershipFilter(paid, null);
        var stock = (JsonArray)JsonNode.Parse(LocalBackend.LoadStartupFixtures()["/Progression/items"].GetRawText())!;
        JsonArray Served(JsonArray reply) => (JsonArray)JsonNode.Parse(filter.FilterCatalogueResponse(JsonSerializer.SerializeToElement(reply)).GetRawText())!;
        // a paid stratagem's category-10 child, and a free stratagem item a player holds
        var premium = paid.Entries.First(e => e.Category == 10 && e.Parent != 0 && e.ItemId != SharedStable && paid.IsReserved(e.MixId));
        var free = stock.OfType<JsonObject>().First(e => (int)e["progressionCategory"]! == 11 && e["parentId"] == null && !paid.IsReserved((uint)e["mixId"]!));
        var freeId = (uint)free["mixId"]!;
        var forged = EngineHash.Id32("test/forged-stable");
        HashSet<uint> held = [.. stock.OfType<JsonObject>().Select(e => (uint)e["mixId"]!).Where(id => !paid.IsReserved(id)), forged];

        var moved = Clone(stock);
        var child = moved.OfType<JsonObject>().First(e => (uint)e["mixId"]! == premium.MixId);
        moved.Remove(child);
        moved.Insert(moved.IndexOf(moved.OfType<JsonObject>().First(e => (uint)e["mixId"]! == freeId)) + 1, child);
        check(Holds(moved, held, premium.ItemId), "A paid child moved under a free root is not held as the game reads it: the check below proves nothing.");
        check(!Holds(Served(moved), held, premium.ItemId), "A paid stratagem was held through its child moved under a free root.");

        var added = Clone(stock);
        added.Insert(added.IndexOf(added.OfType<JsonObject>().First(e => (uint)e["mixId"]! == premium.MixId)),
            new JsonObject { ["itemId"] = premium.ItemId, ["mixId"] = forged, ["progressionCategory"] = 10, ["parentId"] = freeId });
        check(Holds(added, held, premium.ItemId) && !Holds(Served(added), held, premium.ItemId), "A paid stratagem was held through an entry added under its stable id.");

        Honest(stock, paid.Rebuilt(Clone(stock)), (paid, held, premium.Parent), check);
        var rootless = Clone(stock);
        rootless.Remove(rootless.OfType<JsonObject>().First(e => (uint)e["mixId"]! == premium.Parent));
        check(!Served(rootless).OfType<JsonObject>().Any(e => (uint?)e["parentId"] == premium.Parent), "A family was served without its root.");
        Patterns(stock, paid, Served, check);
        var booster = paid.Entries.First(e => e.Category == 5 && paid.IsReserved(e.MixId));
        var boosted = Clone(stock);
        boosted.Insert(0, new JsonObject { ["itemId"] = booster.ItemId, ["mixId"] = forged, ["progressionCategory"] = 5 });
        check(First(Served(boosted), 5, booster.ItemId) == booster.MixId, "An entry added under a paid booster's itemId is found before the booster.");
        Generator(root, stock, paid, check);
        Registered(root, stock, (paid, premium.ItemId), check);
    }

    // An unedited catalogue: the same entries, each family in the compiled order, the shared stable id's paid child still
    // first, and every stratagem held exactly as before, with or without a paid one owned.
    static void Honest(JsonArray stock, JsonArray served, (PaidContentSet Paid, HashSet<uint> Held, uint Owned) held, Action<bool, string> check)
    {
        var (paid, free, owned) = held;
        static string Key(JsonNode? e) => e!.ToJsonString();
        check(stock.Select(Key).Order().SequenceEqual(served.Select(Key).Order()), "An unedited catalogue's entries changed in the rebuild.");
        var order = served.OfType<JsonObject>().Select(e => (uint)e["mixId"]!).Where(paid.Entries.Select(x => x.MixId).ToHashSet().Contains);
        check(order.SequenceEqual(paid.Entries.Select(e => e.MixId)), "The rebuilt families are not in the compiled order.");
        check(served.OfType<JsonObject>().First(e => (int)e["progressionCategory"]! == 10 && (uint)e["itemId"]! == SharedStable)["mixId"]!.GetValue<uint>() == SharedChild,
            "The shared category-10 itemId no longer finds the paid child first.");
        var stables = stock.OfType<JsonObject>().Where(e => (int)e["progressionCategory"]! == 10).Select(e => (uint)e["itemId"]!).Distinct().ToList();
        var keys = stock.OfType<JsonObject>().Select(e => ((int)e["progressionCategory"]!, (uint)e["itemId"]!)).Where(k => k.Item1 != 2).Distinct();
        check(keys.All(k => First(stock, k.Item1, k.Item2) == First(served, k.Item1, k.Item2)), "The rebuild changed which entry a lookup by itemId finds first.");
        foreach (var inventory in new[] { free, [.. free, owned] })
            check(stables.All(s => Holds(stock, inventory, s) == Holds(served, inventory, s)), "The rebuild changed which stratagems an unedited catalogue holds.");
    }

    // paid-list refuses a catalogue it cannot rebuild by position: a child away from its root, or a paid weapon part.
    static void Generator(string root, JsonArray stock, PaidContentSet paid, Action<bool, string> check)
    {
        var suite = LocalBackend.CatalogueDirectory!;
        var weapon = stock.OfType<JsonObject>().First(e => (int)e["progressionCategory"]! == 1 && e["parentId"] == null && stock.OfType<JsonObject>().Any(c => (uint?)c["parentId"] == (uint)e["mixId"]!));
        var part = stock.OfType<JsonObject>().First(e => paid.IsPaid((uint)e["mixId"]!) && e["parentId"] == null && !stock.OfType<JsonObject>().Any(c => (uint?)c["parentId"] == (uint)e["mixId"]!));
        var astray = stock.OfType<JsonObject>().First(e => (uint?)e["parentId"] == (uint)weapon["mixId"]!);
        foreach (var (edit, why) in new (Action<JsonArray>, string)[]
                 {
                     (items => Move(items, astray, () => 0), "does not follow its root"),
                     (items =>
                     {
                         var moved = Move(items, part, () => items.IndexOf(Find(items, weapon)) + 1);
                         moved["progressionCategory"] = 2;
                         moved["parentId"] = weapon["mixId"]!.DeepClone();
                     }, "paid weapon part"),
                 })
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "rebuild-generator", why.Replace(' ', '-'))).FullName;
            var catalog = JsonNode.Parse(File.ReadAllText(Path.Combine(suite, GameCatalog.ItemCatalogFile)))!.AsObject();
            edit(catalog["items"]!.AsArray());
            File.WriteAllText(Path.Combine(folder, GameCatalog.ItemCatalogFile), catalog.ToJsonString());
            File.Copy(Path.Combine(suite, GameCatalog.ProgressionDataFile), Path.Combine(folder, GameCatalog.ProgressionDataFile), true);
            string? refused = null;
            try
            {
                LocalBackend.CatalogueDirectory = folder;
                PaidContentSet.FromCatalogue().ToPaidItems("");
            }
            catch (InvalidDataException e) { refused = e.Message; }
            finally { LocalBackend.CatalogueDirectory = suite; }
            check(refused?.Contains(why, StringComparison.Ordinal) == true, $"paid-list wrote a list from a catalogue whose entry {why}: {refused}");
        }
    }

    // A mod stratagem under a paid stratagem's stable id is refused although the installation's catalogue left it out.
    static void Registered(string root, JsonArray stock, (PaidContentSet Paid, uint Stable) premium, Action<bool, string> check)
    {
        var (paid, stable) = premium;
        var edited = JsonSerializer.SerializeToElement(new JsonArray([.. stock.OfType<JsonObject>().Where(e => (uint)e["itemId"]! != stable).Select(e => e.DeepClone())]));
        var file = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "rebuild-registered")).FullName, "registered-items.json");
        var mixId = EngineHash.Id32("evil/s#item");
        File.WriteAllText(file, $$"""{"format":1,"items":[{"kind":"stratagem","mixId":{{mixId}},"stable":{{stable}},"copyOf":{{stable}},"mod":"test.evil","name":"evil/s"}]}""");
        var log = new List<string>();
        var fixtures = LocalBackend.LoadStartupFixtures();
        var items = new RegisteredItems(file) { Source = path => path == "/Progression/items" ? edited : fixtures.TryGetValue(path, out var body) ? body : null, Log = log.Add, Reserved = paid.IsReserved, ReservedStratagemId = paid.IsReservedStratagemId };
        check(items.Served().Count == 0 && log.Any(l => l.Contains("stable id", StringComparison.Ordinal)), "A mod stratagem took a paid stratagem's stable id: " + string.Join(" | ", log));
    }

    // A mod's entries placed by position inside a paid family (RegisteredItems.AddPatterns): a weapon pattern's child at
    // the end of a paid weapon's run, a unit pattern's copy of a paid child skin right after that skin. Served, each must
    // still link to the entry its parentId names, or it is held through whatever free root comes before it.
    static void Patterns(JsonArray stock, PaidContentSet paid, Func<JsonArray, JsonArray> served, Action<bool, string> check)
    {
        var weapon = paid.Entries.First(e => e.Category == 1 && e.Parent == 0 && paid.IsReserved(e.MixId) && paid.Entries.Any(c => c.Parent == e.MixId));
        var skin = paid.Entries.First(e => e.Category == 4 && e.Parent != 0 && paid.IsReserved(e.MixId));
        uint pattern = EngineHash.Id32("test/pattern#item"), child = EngineHash.Id32("test/pattern#weapon"), unit = EngineHash.Id32("test/pattern#unit");
        var reply = Clone(stock);
        var end = reply.Select((e, i) => (e, i)).Last(x => (uint?)x.e!["parentId"] == weapon.MixId).i;
        var made = reply[end]!.DeepClone().AsObject();
        (made["itemId"], made["mixId"]) = (pattern, child);
        reply.Insert(end + 1, made);
        var copy = Find(reply, new JsonObject { ["mixId"] = skin.MixId }).DeepClone().AsObject();
        (copy["itemId"], copy["mixId"]) = (pattern, unit);
        reply.Insert(reply.IndexOf(Find(reply, new JsonObject { ["mixId"] = skin.MixId })) + 1, copy);
        check(Linked(reply, child) == weapon.MixId && Linked(reply, unit) == skin.Parent, "The placed pattern entries do not link as the game reads them: the check below proves nothing.");
        var rebuilt = served(reply);
        check(Linked(rebuilt, child) == weapon.MixId, "A mod's pattern on a paid weapon links to another weapon once rebuilt.");
        check(Linked(rebuilt, unit) == skin.Parent, "A mod's copy of a paid skin links to another root once rebuilt.");
    }

    // The mixId of the entry the game links this one to, or null when it is a root or not served.
    static uint? Linked(JsonArray reply, uint mixId)
    {
        var entries = reply.OfType<JsonObject>().ToList();
        var i = entries.FindIndex(e => (uint)e["mixId"]! == mixId);
        return i < 0 || Parents(entries)[i] is not (>= 0 and var parent) ? null : (uint)entries[parent]["mixId"]!;
    }

    // The mixId of the first entry of a category with this itemId, as the game's lookups find it.
    static uint? First(JsonArray reply, int category, uint itemId) => (uint?)reply.OfType<JsonObject>()
        .FirstOrDefault(e => (int)e["progressionCategory"]! == category && (uint)e["itemId"]! == itemId)?["mixId"];

    // Each entry's parent as the game links it (0x136EC00): the run's root when its parentId names that, else the entry
    // before it; -1 for a root.
    static int[] Parents(List<JsonObject> entries)
    {
        var parents = new int[entries.Count];
        var runRoot = -1;
        for (var i = 0; i < entries.Count; i++)
        {
            var parentId = (uint?)entries[i]["parentId"] ?? 0;
            parents[i] = parentId == 0 ? -1 : runRoot >= 0 && (uint)entries[runRoot]["mixId"]! == parentId ? runRoot : i - 1;
            if (parentId == 0) runRoot = i;
        }
        return parents;
    }

    // Whether the game holds the stratagem with this stable id, from the reply's order and the held mixIds.
    static bool Holds(JsonArray reply, IReadOnlySet<uint> held, uint stable)
    {
        var entries = reply.OfType<JsonObject>().ToList();
        var parents = Parents(entries);
        var i = entries.FindIndex(e => (int)e["progressionCategory"]! == 10 && (uint)e["itemId"]! == stable);
        return i >= 0 && (held.Contains((uint)entries[i]["mixId"]!) || parents[i] >= 0 && held.Contains((uint)entries[parents[i]]["mixId"]!));
    }

    static JsonArray Clone(JsonArray items) => (JsonArray)items.DeepClone();

    static JsonObject Find(JsonArray items, JsonObject like) => items.OfType<JsonObject>().First(e => (uint)e["mixId"]! == (uint)like["mixId"]!);

    // The entry like this one, moved to the index at() gives once it is out of the list; the moved entry.
    static JsonObject Move(JsonArray items, JsonObject like, Func<int> at)
    {
        var entry = Find(items, like);
        items.Remove(entry);
        items.Insert(Math.Min(at(), items.Count), entry);
        return entry;
    }
}
