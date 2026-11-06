using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using ModWarbonds = HellForge.Core.ModWarbonds;
using PremiumContent = HellForge.Core.PremiumContent;
using HellForge.LocalBackend;

// Warbonds mods add (HellForge.Core ModWarbonds, LocalBackend ModWarbondService): the spec's checks and ids, what the backend
// serves beside the game's warbonds, and buying their items in the warbond's own currency.
static class ModWarbondsTests
{
    const uint Liberator = 0xAA035309, Gold = 0x32895C2A, Requisition = 0xD7150EEC, SuperSamples = 0xDAC0E9DB;

    public static async Task Run(string root, Action<bool, string> check)
    {
        Spec(check);
        var folder = Path.Combine(root, "mod-warbonds");
        Directory.CreateDirectory(folder);
        // Read from each loaded mod's folder: a mod without the file adds nothing, an unreadable one says so.
        HellForge.Core.ModManifest Mod(string id, string? text)
        {
            var modFolder = Directory.CreateDirectory(Path.Combine(folder, "mods", id)).FullName;
            if (text != null) File.WriteAllText(Path.Combine(modFolder, ModWarbonds.FileName), text);
            return new HellForge.Core.ModManifest(id, id + " name", "1", "", true, modFolder, [], [], []);
        }
        var (loaded, loadProblems) = ModWarbonds.Load([
            Mod("plain", null),
            Mod("broken", "{\"format\":1,"),
            Mod("good", "{\"format\":1,\"warbonds\":[{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}")]);
        check(loaded.Count == 1 && loaded[0].Mod == "good" && loadProblems.Count == 1 && loadProblems[0].StartsWith("broken name: ", StringComparison.Ordinal),
            "Warbonds files are not read from the mods' folders as they should be: " + string.Join(" | ", loadProblems));
        // Two free items of Helldivers Mobilize (anyone may hold them) and the gold Liberator a code mod registers.
        using var progression = LocalBackend.LoadResource("progression-data.json");
        var mobilize = progression.RootElement.GetProperty("seasonPassDetails").GetProperty(PremiumContent.FreeWarbond.ToString())
            .GetProperty("seasonPassPages")[0].GetProperty("seasonPassItems").EnumerateArray().Select(i => i.GetProperty("itemMixId").GetUInt32()).ToList();
        var registeredFile = Path.Combine(folder, "registered-items.json");
        File.WriteAllText(registeredFile, $$"""{"format":1,"items":[{"mixId":{{Gold}},"copyOf":{{Liberator}},"mod":"test","unit":"test/gold"}]}""");
        var spec = JsonDocument.Parse($$"""
            {"format":1,"warbonds":[
              {"key":"arsenal","name":"Test Arsenal","currency":"requisition","pages":[
                {"requirement":0,"items":[{"weapon":"test/gold","cost":1000,"position":[0,0],"size":[1,2]},{"mixId":{{mobilize[0]}},"cost":500}]},
                {"requirement":1500,"items":[{"mixId":{{mobilize[1]}},"cost":200}]}]},
              {"key":"dear","name":"Dear","currency":"super-samples","pages":[{"items":[{"mixId":{{mobilize[2]}},"cost":100}]}]},
              {"key":"missing","name":"Missing","pages":[{"items":[{"weapon":"test/nothing","cost":5}]}]},
              {"key":"capped","name":"Capped","pages":[{"items":[{"mixId":{{mobilize[0]}},"cost":251}]}]}]}
            """);
        var problems = new List<string>();
        var warbonds = ModWarbonds.Parse("test", "Test", spec.RootElement, problems);
        check(warbonds.Count == 4 && problems.Count == 0, "A valid warbonds file was refused: " + string.Join(" | ", problems));
        var warbondsFile = Path.Combine(folder, "mod-warbonds.json");
        ModWarbonds.Write(warbondsFile, warbonds);
        var read = ModWarbonds.Read(warbondsFile);
        check(read.Count == 4 && read[0].Id32 == warbonds[0].Id32 && read[0].Pages[0].Items[0].Sells == warbonds[0].Pages[0].Items[0].Sells &&
            read[0].Pages[1].Requirement == 1500 && read[0].Pages[0].Items[1].X == 1, "The session's warbonds file does not read back as written.");
        check(ModWarbonds.Read(Path.Combine(folder, "none.json")).Count == 0, "A session without a warbonds file serves warbonds.");

        var fixtures = LocalBackend.LoadStartupFixtures();
        var arsenal = warbonds[0];
        var goldId = arsenal.Pages[0].Items[0].Id;
        var freeId = arsenal.Pages[0].Items[1].Id;
        var pageTwoId = arsenal.Pages[1].Items[0].Id;

        // Served over HTTP to a starter profile with 5,000 requisition.
        using var store = ProfileStore.Open(null, "starter");
        store.TryChange((_, inventory) =>
        {
            ProfileStore.AddItem(inventory, Requisition, 5000 - ProfileStore.Held(inventory, Requisition));
            ProfileStore.AddItem(inventory, SuperSamples, -ProfileStore.Held(inventory, SuperSamples));
            return true;
        });
        var registered = new RegisteredItems(registeredFile);
        var service = new ModWarbondService(read);
        var logged = new List<string>();
        service.Log = logged.Add;
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "mod-warbonds-backend"), LoginMode.LocalSession)
            { StartupFixtures = true, Profile = store, RegisteredItems = registered, ModWarbonds = service });
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false })
                { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
            async Task<JsonDocument> Get(string path)
            {
                using var response = await client.GetAsync(path);
                check(response.StatusCode == HttpStatusCode.OK, path + " was not served: " + response.StatusCode);
                return JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            }
            async Task<(HttpStatusCode Status, string Body)> Buy(uint itemId)
            {
                using var response = await client.PostAsync("/SeasonPass/item/buy", new StringContent(
                    $$"""{"expectedCost":[{"mixId":{{Requisition}},"amount":1}],"seasonPassItemId":{{itemId}},"warId":801}""", Encoding.UTF8, "application/json"));
                return (response.StatusCode, await response.Content.ReadAsStringAsync());
            }
            Dictionary<uint, long> Held(JsonDocument inventory) =>
                inventory.RootElement.GetProperty("items").EnumerateArray().ToDictionary(i => i.GetProperty("mixId").GetUInt32(), i => i.GetProperty("amount").GetInt64());

            using (var list = await Get("/SeasonPass"))
            {
                var entries = list.RootElement.EnumerateArray().ToList();
                var gameCount = fixtures["/SeasonPass"].GetArrayLength();
                var entry = entries.SingleOrDefault(e => e.GetProperty("id32").GetUInt32() == arsenal.Id32);
                check(entries.Count == gameCount + 2 && entry.ValueKind == JsonValueKind.Object && entries[gameCount].GetProperty("id32").GetUInt32() == arsenal.Id32,
                    "The mods' warbonds are not listed after the game's (a warbond selling nothing this session is not listed).");
                check(entry.GetProperty("medalsConsumableMixId").GetUInt32() == Requisition && entry.GetProperty("medalsSpentMixId").GetUInt32() == arsenal.Spent &&
                    entry.GetProperty("totalCost").GetInt64() == 1700 && entry.GetProperty("pageRequirements").EnumerateArray().Select(r => r.GetInt64()).SequenceEqual([0L, 1500L]) &&
                    entry.GetProperty("premiumTokenMixIds").GetArrayLength() == 0, "A mod warbond's list entry is wrong: " + entry.GetRawText());
            }
            check(logged.Any(l => l.Contains("\"missing\"") && l.Contains("test/nothing")), "A warbond whose items are not registered was not reported: " + string.Join(" | ", logged));
            check(logged.Any(l => l.Contains("\"capped\"") && l.Contains("at most 250")), "A price above what a player may hold (251 of 250 medals) was not left out: " + string.Join(" | ", logged));
            using (var detail = await Get("/SeasonPass/" + arsenal.Id32))
            {
                var pages = detail.RootElement.GetProperty("seasonPassPages");
                var gold = pages[0].GetProperty("seasonPassItems")[0];
                check(detail.RootElement.GetProperty("medalsConsumableMixId").GetUInt32() == Requisition && detail.RootElement.GetProperty("layoutType").GetInt32() == 1 &&
                    detail.RootElement.GetProperty("layoutSize").GetProperty("x").GetInt32() == 5 && pages.GetArrayLength() == 2 &&
                    pages[1].GetProperty("totalMedalsRequired").GetInt64() == 1500 && gold.GetProperty("seasonPassItemId").GetUInt32() == goldId &&
                    gold.GetProperty("itemMixId").GetUInt32() == Gold && gold.GetProperty("medalCost").GetInt64() == 1000 &&
                    gold.GetProperty("gridSize").GetProperty("y").GetInt32() == 2, "A mod warbond's pages are wrong: " + detail.RootElement.GetRawText());
            }
            using (var items = await Get("/Progression/items"))
                check(items.RootElement.EnumerateArray().Any(i => i.GetProperty("mixId").GetUInt32() == arsenal.Spent && i.GetProperty("isConsumable").GetBoolean()),
                    "The catalogue lacks a mod warbond's spent counter.");
            Dictionary<uint, long> before;
            using (var inventory = await Get("/Progression/inventory")) before = Held(inventory);
            check(!before.ContainsKey(Gold), "A mod weapon sold on a warbond is held before it is bought.");

            check((await Buy(pageTwoId)).Status == HttpStatusCode.BadRequest, "An item of a page not open yet was sold.");
            check((await Buy(warbonds[1].Pages[0].Items[0].Id)).Status == HttpStatusCode.BadRequest, "An item was sold without its currency (super samples).");
            var bought = await Buy(goldId);
            check(bought.Status == HttpStatusCode.OK && bought.Body.Contains(Gold.ToString()) && bought.Body.Contains(arsenal.Spent.ToString()) &&
                bought.Body.Contains("packageProgress"), "A mod weapon was not sold for requisition: " + bought.Body);
            Dictionary<uint, long> after;
            using (var inventory = await Get("/Progression/inventory")) after = Held(inventory);
            check(after[Requisition] == before[Requisition] - 1000 && after.GetValueOrDefault(arsenal.Spent) == 1000 && after.ContainsKey(Gold),
                "The purchase did not take 1,000 requisition, count it as spent and give the weapon.");
            using (var saved = await Get("/Progression"))
                check(saved.RootElement.GetProperty("purchasedSeasonPassItems").EnumerateArray().Any(i => i.GetInt64() == goldId), "The bought item is not saved as bought.");
            check(store.TryChange((_, inventory) => { check(ProfileStore.Held(inventory, Gold) == 0, "A mod's weapon was saved in the profile."); return false; }) == false,
                "The profile check did not run.");
            check((await Buy(goldId)).Status == HttpStatusCode.BadRequest, "A mod warbond's item was sold twice.");
            check((await Buy(freeId)).Status == HttpStatusCode.OK, "A catalogue item on a mod warbond was not sold.");
            check((await Buy(pageTwoId)).Status == HttpStatusCode.OK, "The second page did not open once 1,500 requisition were spent.");
            using (var inventory = await Get("/Progression/inventory"))
                check(Held(inventory)[Requisition] == before[Requisition] - 1700 && Held(inventory)[arsenal.Spent] == 1700, "The three purchases did not add up.");
        }
        finally { await app.StopAsync(); }

        // Unlock-all has bought everything: its ids are served bought and the spent counters full, nothing saved.
        var unlockAll = new ModWarbondService(read) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Registered = new RegisteredItems(registeredFile) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null }, UnlockAll = true, Log = _ => { } };
        unlockAll.Registered!.Withheld = unlockAll.Withheld;
        var progressionBody = JsonSerializer.SerializeToElement(new { purchasedSeasonPassItems = new[] { 1 } });
        var servedIds = unlockAll.Apply("/Progression", progressionBody).GetProperty("purchasedSeasonPassItems").EnumerateArray().Select(i => i.GetInt64()).ToHashSet();
        check(servedIds.Contains(1) && servedIds.Contains(goldId) && servedIds.Contains(pageTwoId), "Unlock-all does not have every mod warbond item bought.");
        var unlockInventory = unlockAll.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = Array.Empty<object>() }));
        check(unlockInventory.GetProperty("items").EnumerateArray().Any(i => i.GetProperty("mixId").GetUInt32() == arsenal.Spent && i.GetProperty("amount").GetInt64() == 1700),
            "Unlock-all's spent counter does not open every page.");
        check(unlockAll.Withheld().Count == 0 && unlockAll.Registered.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = Array.Empty<object>() }))
            .GetProperty("items").EnumerateArray().Any(i => i.GetProperty("mixId").GetUInt32() == Gold), "Unlock-all does not hold a mod weapon sold on a warbond.");

        // Premium content is not sold to a player who does not own it, and the game's list keeps 32: past it, a warbond waits.
        var steeled = progression.RootElement.GetProperty("seasonPassDetails").GetProperty("610512218").GetProperty("seasonPassPages")[0]
            .GetProperty("seasonPassItems")[0].GetProperty("itemMixId").GetUInt32();
        var many = Enumerable.Range(0, 9).Select(i => new ModWarbonds.Warbond("test", "w" + i, "W", ModWarbonds.Hash("w" + i), ModWarbonds.Hash("s" + i), "medals", 0x4C19589F,
            [new ModWarbonds.Page(0, [new ModWarbonds.Item(ModWarbonds.ItemId("test", "w" + i, "a"), new ModWarbonds.Ref("item", i == 0 ? steeled : mobilize[0], "", ""), 5, 0, 0, 1, 1, false)])])).ToList();
        var lines = new List<string>();
        var capped = new ModWarbondService(many) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, IsAllowed = id => id != steeled, Log = lines.Add };
        var room = ModWarbondService.ListCap - fixtures["/SeasonPass"].GetArrayLength();
        check(capped.Resolve().Count == Math.Min(room, 8) && lines.Any(l => l.Contains("\"w0\"") && l.Contains("premium")) &&
            (room >= 8 || lines.Any(l => l.Contains("lists 32"))), "Premium content or the 32-warbond list was not respected: " + string.Join(" | ", lines));
        check(capped.Apply("/SeasonPass", fixtures["/SeasonPass"]).GetArrayLength() == ModWarbondService.ListCap || room >= 8, "The list grew past 32 warbonds.");
    }

    // The spec's checks: what the game holds (15 pages, 10 items, the 5 x 3 grid), prices, reachable pages and stable ids.
    static void Spec(Action<bool, string> check)
    {
        List<string> Problems(string warbond)
        {
            var problems = new List<string>();
            using var json = JsonDocument.Parse("{\"format\":1,\"warbonds\":[" + warbond + "]}");
            ModWarbonds.Parse("mod", "Mod", json.RootElement, problems);
            return problems;
        }
        string Items(int count, string extra = "") => string.Join(",", Enumerable.Range(1, count).Select(i => $"{{\"mixId\":{i},\"cost\":1{extra}}}"));
        foreach (var (warbond, why) in new[]
        {
            ($"{{\"key\":\"a\",\"name\":\"A\",\"pages\":[{{\"items\":[{Items(11)}]}}]}}", "11 items on a page"),
            ($"{{\"key\":\"a\",\"name\":\"A\",\"pages\":[{string.Join(",", Enumerable.Repeat($"{{\"items\":[{Items(1)}]}}", 16))}]}}", "16 pages"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"position\":[4,0],\"size\":[2,1]}]}]}", "an item past the grid's edge"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"position\":[0,0],\"size\":[2,2]},{\"mixId\":2,\"cost\":1,\"position\":[1,1]}]}]}", "overlapping items"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"size\":[5,3]},{\"mixId\":2,\"cost\":1}]}]}", "a full grid"),
            ("{\"key\":\"a\",\"name\":\"A\",\"currency\":\"super credits\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "an unknown currency"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":0}]}]}", "a free item"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"requirement\":5,\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a first page that is not open"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":3}]},{\"requirement\":4,\"items\":[{\"mixId\":2,\"cost\":1}]}]}", "a page that can never open"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1},{\"mixId\":1,\"cost\":2}]}]}", "the same item twice without keys"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"weapon\":\"x\",\"cost\":1}]}]}", "an item naming two things"),
            ("{\"key\":\"A B\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a key with capitals and a space"),
            ("{\"key\":\"a\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "no name"),
        })
            check(Problems(warbond).Count == 1, "A warbond with " + why + " was accepted.");
        check(Problems("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]},{\"key\":\"a\",\"name\":\"B\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}").Count == 1,
            "Two warbonds with one key were accepted.");

        // Ten items fill the grid row by row around a placed one; ids are the same every time, above the game's.
        var problems = new List<string>();
        using var fullKeyed = JsonDocument.Parse("{\"format\":1,\"warbonds\":[{\"key\":\"full\",\"name\":\"Full\",\"pages\":[{\"items\":[" +
            "{\"mixId\":100,\"cost\":1,\"position\":[0,0],\"size\":[1,2]}," + Items(9) + "]}]}]}");
        var parsed = ModWarbonds.Parse("mod", "Mod", fullKeyed.RootElement, problems);
        check(problems.Count == 0 && parsed.Count == 1, "A full page of 10 items was refused: " + string.Join(" | ", problems));
        var cells = parsed[0].Pages[0].Items;
        check(cells[1].X == 1 && cells[1].Y == 0 && cells[4].X == 4 && cells[5].X == 1 && cells[5].Y == 1 && cells[9].X == 0 && cells[9].Y == 2,
            "Items without a position do not fill the free cells row by row.");
        var again = ModWarbonds.Parse("mod", "Mod", fullKeyed.RootElement, []);
        check(again[0].Id32 == parsed[0].Id32 && again[0].Spent == parsed[0].Spent && again[0].Pages[0].Items.Select(i => i.Id).SequenceEqual(cells.Select(i => i.Id)),
            "A warbond's ids are not the same every time.");
        check(cells.All(i => i.Id >= ModWarbonds.FirstItemId && i.Id < ModWarbonds.FirstItemId + ModWarbonds.ItemIdSpan) && cells.Select(i => i.Id).Distinct().Count() == 10,
            "Item ids are not unique or not above the game's.");
        check(ModWarbonds.Parse("other", "Other", fullKeyed.RootElement, [])[0].Id32 != parsed[0].Id32, "Two mods' warbonds with one key share an id.");
    }
}
