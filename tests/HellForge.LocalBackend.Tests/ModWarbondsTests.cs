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
        ArtImages(check, folder);
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
        // A mod of warbonds alone is a mod: its manifest loads (it changes something).
        var warbondsOnly = Directory.CreateDirectory(Path.Combine(folder, "catalog", "only")).FullName;
        File.WriteAllText(Path.Combine(warbondsOnly, "hellforge-mod.json"), "{\"format\":1,\"id\":\"test.only\",\"name\":\"Only\",\"version\":\"1.0.0\",\"gameplay\":false}");
        File.WriteAllText(Path.Combine(warbondsOnly, ModWarbonds.FileName), "{\"format\":1,\"warbonds\":[]}");
        var (catalog, catalogProblems) = HellForge.Core.ModCatalog.Load(Path.Combine(folder, "catalog"));
        check(catalog.Any(m => m.Id == "test.only") && catalogProblems.Count == 0, "A mod of warbonds alone was refused: " + string.Join(" | ", catalogProblems));
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
                var gameLast = fixtures["/SeasonPass"][gameCount - 1].GetProperty("id32").GetUInt32();
                check(entries.Count == gameCount + 2 && entry.ValueKind == JsonValueKind.Object && entries[gameCount].GetProperty("id32").GetUInt32() == arsenal.Id32 &&
                    entries[gameCount - 1].GetProperty("id32").GetUInt32() == gameLast,
                    "The mods' warbonds are not listed after all of the game's (a warbond selling nothing this session is not listed).");
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
            var goldItem = registered.Load().Single(i => i.MixId == Gold);
            check(registered.IsLeftOut(goldItem) && !registered.IsLocked(goldItem), "A mod weapon not bought yet is not left out for purchases and earning.");

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
            check(!registered.IsLeftOut(goldItem), "A bought mod weapon is still left out for purchases and earning.");
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

        // Premium content is not sold to a player who does not own it, and the list keeps ListCap (the room the mod loader
        // makes): past it, a warbond waits.
        var steeled = progression.RootElement.GetProperty("seasonPassDetails").GetProperty("610512218").GetProperty("seasonPassPages")[0]
            .GetProperty("seasonPassItems")[0].GetProperty("itemMixId").GetUInt32();
        ModWarbonds.Warbond Simple(int i, int pages, int perPage, uint first) => new("test", "w" + i, "W", ModWarbonds.Hash("w" + i), ModWarbonds.Hash("s" + i), "medals", 0x4C19589F,
            [.. Enumerable.Range(0, pages).Select(p => new ModWarbonds.Page(0, [.. Enumerable.Range(0, perPage).Select(k => new ModWarbonds.Item(
                ModWarbonds.ItemId("test", "w" + i, p + "/" + k), new ModWarbonds.Ref("item", i == 0 ? first : mobilize[0], "", ""), 5, k % 5, k / 5, 1, 1, false))]))]);
        // The table names TableRoom mods' warbonds, in order: one left out (w0, premium) still takes its row.
        var many = Enumerable.Range(0, ModWarbondService.TableRoom + 6).Select(i => Simple(i, 1, 1, steeled)).ToList();
        var lines = new List<string>();
        var capped = new ModWarbondService(many) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, IsAllowed = id => id != steeled, Log = lines.Add };
        check(ModWarbondService.TableRoom < ModWarbondService.ListCap - fixtures["/SeasonPass"].GetArrayLength() &&
            capped.Resolve().Count == ModWarbondService.TableRoom - 1 && lines.Any(l => l.Contains("\"w0\"") && l.Contains("premium")) &&
            lines.Any(l => l.Contains($"names {ModWarbondService.TableRoom}") && l.Contains($"number {ModWarbondService.TableRoom + 1}")),
            "Premium content or the warbond table's room was not respected: " + string.Join(" | ", lines));
        check(capped.Apply("/SeasonPass", fixtures["/SeasonPass"]).GetArrayLength() == fixtures["/SeasonPass"].GetArrayLength() + ModWarbondService.TableRoom - 1,
            "The list did not hold the served warbonds alone.");
        // A game list near the cap leaves the mods less room than the table.
        using var longList = JsonDocument.Parse("[" + string.Join(",", Enumerable.Range(1, 110).Select(i => $"{{\"id32\":{i}}}")) + "]");
        lines.Clear();
        var crowded = new ModWarbondService(many) { Source = path => path == "/SeasonPass" ? longList.RootElement : capped.Source!(path), IsAllowed = capped.IsAllowed, Log = lines.Add };
        check(crowded.Resolve().Count == ModWarbondService.ListCap - 110 && lines.Any(l => l.Contains($"lists {ModWarbondService.ListCap}")),
            "The warbond list's room was not respected: " + string.Join(" | ", lines));
        // The game holds 5,280 bought items: mods' warbonds sell ItemRoom in all, so unlock-all's list fits.
        var full = Enumerable.Range(1, ModWarbondService.ItemRoom / 150 + 1).Select(i => Simple(i, 15, 10, 0)).ToList();
        lines.Clear();
        var heavy = new ModWarbondService(full) { Source = capped.Source, Log = lines.Add }.Resolve();
        check(heavy.Count == ModWarbondService.ItemRoom / 150 && heavy.Sum(w => w.Sells.Count) <= ModWarbondService.ItemRoom &&
            lines.Any(l => l.Contains($"holds {ModWarbondService.ItemRoom} items")), "Mods' warbonds sold more items than a profile holds: " + string.Join(" | ", lines));
        Tokens(check, fixtures, mobilize);
        ItemCurrencies(check, fixtures, mobilize);
        LeftOut(check, fixtures, mobilize, steeled);
    }

    // Items left out for the session: a later page's requirement stays within what the pages before it sell, and paid
    // content is never sold, owned or not.
    static void LeftOut(Action<bool, string> check, IReadOnlyDictionary<string, JsonElement> fixtures, List<uint> mobilize, uint steeled)
    {
        var spec = JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"gaps","name":"Gaps","currency":"requisition","pages":[
              {"items":[{"weapon":"other/gone","mod":"other","cost":5000},{"mixId":{{mobilize[0]}},"cost":5000},{"mixId":{{steeled}},"cost":10}]},
              {"requirement":10000,"items":[{"mixId":{{mobilize[1]}},"cost":10}]}]}]}
            """);
        var gaps = ModWarbonds.Parse("test", "Test", spec.RootElement, [])[0];
        var service = new ModWarbondService([gaps]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, IsPaid = id => id == steeled, Log = _ => { } };
        var served = service.Resolve().Single();
        var requirements = served.ListEntry["pageRequirements"]!.AsArray().Select(r => (long)r!).ToList();
        check(requirements.SequenceEqual([0L, 5000L]) && (long)served.Detail["seasonPassPages"]![1]!["totalMedalsRequired"]! == 5000,
            "A page's requirement was not kept within what the pages before it sell: " + string.Join(",", requirements));
        check(!served.Sells.Values.Contains(steeled), "Paid content was sold on a mod's warbond.");
        // An item held once that the player has already (from another warbond, or bought on another mod's) is left out;
        // one bought on this warbond stays, claimed.
        var two = ModWarbonds.Parse("test", "Test", JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"one","name":"One","pages":[{"items":[{"mixId":{{mobilize[0]}},"cost":5},{"mixId":{{mobilize[1]}},"cost":5}]}]},
              {"key":"two","name":"Two","pages":[{"items":[{"mixId":{{mobilize[1]}},"cost":5},{"mixId":{{mobilize[2]}},"cost":5}]}]}]}
            """).RootElement, []);
        var oneBought = two[0].Pages[0].Items[1].Id;
        var ownedLog = new List<string>();
        var owned = new ModWarbondService(two)
        {
            Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = ownedLog.Add,
            Holds = () => new HashSet<uint> { mobilize[0], mobilize[1] }, Purchased = () => new HashSet<long> { oneBought },
        }.Resolve();
        check(owned.Count == 2 && owned[0].Sells.Keys.SequenceEqual([(long)oneBought]) && owned[1].Sells.Values.SequenceEqual([mobilize[2]]) &&
            ownedLog.Count(l => l.Contains("has it already")) == 2,
            "Items the player has already were sold again, or one bought on its own warbond was left out: " + string.Join(" | ", ownedLog));
        // Unlock-all has everything bought, so its warbonds keep every item though the profile holds them all.
        var allOwned = new ModWarbondService(two)
        {
            Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = _ => { }, UnlockAll = true,
            Holds = () => new HashSet<uint> { mobilize[0], mobilize[1], mobilize[2] },
        }.Resolve();
        check(allOwned.Count == 2 && allOwned.Sum(w => w.Sells.Count) == 4, "Unlock-all left out items its profile holds.");
        // At the buy: an item gained since its page was served is refused; never a consumable, never on unlock-all.
        var buyCheck = new ModWarbondService(two) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = _ => { } };
        var none = new HashSet<long>();
        check(buyCheck.HasAlready(two[1].Pages[0].Items[1].Id, mobilize[2], false, none, m => m == mobilize[2]) &&
            buyCheck.HasAlready(two[1].Pages[0].Items[0].Id, mobilize[1], false, new HashSet<long> { oneBought }, _ => false) &&
            !buyCheck.HasAlready(two[1].Pages[0].Items[1].Id, mobilize[2], true, none, _ => true) &&
            !buyCheck.HasAlready(two[1].Pages[0].Items[1].Id, mobilize[2], false, none, _ => false),
            "The buy did not refuse an item the player has gained, or refused one they have not.");
    }

    // An item's own "currency": priced, capped and charged in it, not counted as spent on its warbond (whose spent line
    // names the warbond's currency), and named to the mod loader (warbond_item) so the game checks and shows it.
    static void ItemCurrencies(Action<bool, string> check, IReadOnlyDictionary<string, JsonElement> fixtures, List<uint> mobilize)
    {
        var spec = JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"mixed","name":"Mixed","pages":[{"items":[
              {"mixId":{{mobilize[0]}},"cost":5},{"mixId":{{mobilize[1]}},"cost":400,"currency":"requisition"},
              {"mixId":{{mobilize[2]}},"cost":5,"currency":"medals"},{"mixId":{{mobilize[3]}},"cost":60000,"currency":"requisition"}]}]}]}
            """);
        var problems = new List<string>();
        var mixed = ModWarbonds.Parse("test", "Test", spec.RootElement, problems)[0];
        var items = mixed.Pages[0].Items;
        check(problems.Count == 0 && items[1].CurrencyMixId == Requisition && items[0].CurrencyMixId == 0, "An item's own currency was not taken: " + string.Join(" | ", problems));
        var lines = ModWarbonds.PlanLines([mixed]).ToList();
        check(lines.Count == 5 && lines[1] == $"warbond_item\t{items[0].Id}\t1276729503\t0" && lines[2] == $"warbond_item\t{items[1].Id}\t{Requisition}\t0" &&
            lines[3].EndsWith("1276729503\t0", StringComparison.Ordinal) && lines[4].EndsWith($"{Requisition}\t0", StringComparison.Ordinal),
            "The plan does not name every item's currency: " + string.Join(" | ", lines));
        // A consumable of the catalogue (an item the player holds many of) sells again; an armor marked repeatable is left out.
        var consumable = fixtures["/Progression/items"].EnumerateArray().First(i => i.TryGetProperty("isConsumable", out var c) && c.ValueKind == JsonValueKind.True &&
            i.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out var id) && !PaidContentSet.IsSuperCredits(id)).GetProperty("mixId").GetUInt32();
        var repeatable = ModWarbonds.Parse("test", "Test", JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"again","name":"Again","pages":[{"items":[{"mixId":{{consumable}},"cost":5,"repeatable":true}]}]}]}
            """).RootElement, problems)[0];
        var heldOnce = ModWarbonds.Parse("test", "Test", JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"armor","name":"Armor","pages":[{"items":[{"mixId":{{mobilize[0]}},"cost":5,"repeatable":true}]}]}]}
            """).RootElement, problems)[0];
        var heldLog = new List<string>();
        var heldService = new ModWarbondService([heldOnce]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = heldLog.Add };
        check(heldService.Resolve().Count == 0 && heldLog.Any(l => l.Contains("only a consumable")), "A repeatable item that is not a consumable was sold: " + string.Join(" | ", heldLog));
        check(ModWarbonds.PlanLines([repeatable]).Last().EndsWith("\t1", StringComparison.Ordinal), "The plan does not name a repeatable item.");
        var again = new ModWarbondService([repeatable]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, UnlockAll = true, Log = _ => { } };
        var allBought = again.Apply("/Progression", JsonSerializer.SerializeToElement(new { purchasedSeasonPassItems = new[] { 1 } }))
            .GetProperty("purchasedSeasonPassItems").EnumerateArray().Select(i => i.GetInt64()).ToList();
        check(again.Resolve().Count == 1 && allBought.SequenceEqual([1L]), "Unlock-all bought a repeatable item for good: " + string.Join(",", allBought));
        check(Problems("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"currency\":\"credits\"}]}]}").Count == 1, "An item with an unknown currency was accepted.");
        var logged = new List<string>();
        var service = new ModWarbondService([mixed]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = logged.Add };
        var served = service.Resolve().Single();
        check(served.Sells.Count == 3 && logged.Any(l => l.Contains("60000 requisition") && l.Contains("50000")), "An item's price was not capped by its own currency: " + string.Join(" | ", logged));
        var catalogue = service.Apply("/Progression/items", fixtures["/Progression/items"]).EnumerateArray()
            .Where(i => i.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out _)).GroupBy(i => i.GetProperty("mixId").GetUInt32()).ToDictionary(g => g.Key, g => g.First());
        using var store = ProfileStore.Open(null, "starter");
        store.TryChange((_, inventory) => ProfileStore.AddItem(inventory, Requisition, 1000 - ProfileStore.Held(inventory, Requisition)) >= 0 &
            ProfileStore.AddItem(inventory, 0x4C19589F, 100 - ProfileStore.Held(inventory, 0x4C19589F)) >= 0);
        var purchases = new Purchases(store, id => catalogue.TryGetValue(id, out var item) ? item : null, service);
        using var wrong = JsonDocument.Parse("{\"seasonPassItemId\":" + items[1].Id + ",\"warId\":801,\"expectedCost\":[{\"mixId\":1276729503,\"amount\":400}]}");
        check(purchases.Handle("/SeasonPass/item/buy", wrong.RootElement).Item1 != 200, "An item sent in its warbond's currency, not its own, was sold.");
        using var body = JsonDocument.Parse("{\"seasonPassItemId\":" + items[1].Id + ",\"warId\":801,\"expectedCost\":[{\"mixId\":" + Requisition + ",\"amount\":400}]}");
        var (status, reply, _) = purchases.Handle("/SeasonPass/item/buy", body.RootElement);
        check(status == 200, "An item in its own currency was not sold: " + reply?.ToJsonString());
        check(reply?["items"]?.AsArray().All(i => (uint?)i!["mixId"] != mixed.Spent) == true, "The reply counts an item in another currency as spent: " + reply?.ToJsonString());
        check(served.TotalCost == 10, "The warbond's whole cost counts prices in other currencies: " + served.TotalCost);
        check(store.TryChange((_, inventory) =>
        {
            check(ProfileStore.Held(inventory, Requisition) == 600 && ProfileStore.Held(inventory, 0x4C19589F) == 100 && ProfileStore.Held(inventory, mixed.Spent) == 0,
                "An item in its own currency did not take 400 requisition (and no medals), or counted it as spent in medals.");
            return false;
        }) == false, "The profile check did not run.");
        // A repeatable item, bought again and again, never counts as spent: the warbond is finished (held spent == totalCost)
        // only when its other items are bought.
        var sold = new ModWarbondService([repeatable]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, Log = _ => { } };
        check(sold.Resolve().Single().TotalCost == 0, "A repeatable item counts towards its warbond's whole cost.");
        var again2 = new Purchases(store, id => catalogue.TryGetValue(id, out var item) ? item : null, sold);
        using var buyAgain = JsonDocument.Parse("{\"seasonPassItemId\":" + repeatable.Pages[0].Items[0].Id + ",\"warId\":801,\"expectedCost\":[]}");
        var first = again2.Handle("/SeasonPass/item/buy", buyAgain.RootElement);
        var second = again2.Handle("/SeasonPass/item/buy", buyAgain.RootElement);
        check(first.Item1 == 200 && second.Item1 == 200 && second.Item2?["items"]?.AsArray().All(i => (uint?)i!["mixId"] != repeatable.Spent) == true,
            "A repeatable item did not sell twice without counting as spent: " + second.Item2?.ToJsonString());
        check(store.TryChange((_, inventory) =>
        {
            check(ProfileStore.Held(inventory, 0x4C19589F) == 90 && ProfileStore.Held(inventory, repeatable.Spent) == 0,
                "Two buys of a repeatable item did not take 10 medals, or counted them as spent.");
            return false;
        }) == false, "The profile check did not run.");
        // Unlock-all opens every page, a later page's repeatable item included, though the saved counter stays 0.
        var later = ModWarbonds.Parse("test", "Test", JsonDocument.Parse($$"""
            {"format":1,"warbonds":[{"key":"later","name":"Later","currency":"requisition","pages":[{"items":[{"mixId":{{mobilize[1]}},"cost":1000}]},
              {"requirement":1000,"items":[{"mixId":{{consumable}},"cost":5,"repeatable":true}]}]}]}
            """).RootElement, problems)[0];
        var all = new ModWarbondService([later]) { Source = path => fixtures.TryGetValue(path, out var body) ? body : null, UnlockAll = true, Log = _ => { } };
        all.Resolve();
        using var buyLater = JsonDocument.Parse("{\"seasonPassItemId\":" + later.Pages[1].Items[0].Id + ",\"warId\":801,\"expectedCost\":[]}");
        check(new Purchases(store, id => catalogue.TryGetValue(id, out var item) ? item : null, all).Handle("/SeasonPass/item/buy", buyLater.RootElement).Item1 == 200,
            "Unlock-all did not sell a later page's repeatable item.");
    }

    // A premium look or an unlock price gives a warbond a token (the game's ownership, game.dll 0x136E470): served held when the
    // warbond is open, sold for its price in Forge Credits through /Progression/buy/v2 when it is not.
    static void Tokens(Action<bool, string> check, IReadOnlyDictionary<string, JsonElement> fixtures, List<uint> mobilize)
    {
        var spec = JsonDocument.Parse($$"""
            {"format":1,"warbonds":[
              {"key":"look","name":"Look","variant":"premium","pages":[{"items":[{"mixId":{{mobilize[0]}},"cost":5}]}]},
              {"key":"paid","name":"Paid","unlock":{"currency":"forge-credits","cost":300},"pages":[{"items":[{"mixId":{{mobilize[1]}},"cost":5}]}]}]}
            """);
        var problems = new List<string>();
        var parsed = ModWarbonds.Parse("test", "Test", spec.RootElement, problems);
        check(parsed.Count == 2 && problems.Count == 0 && parsed[0].HasToken && parsed[0].UnlockCost == 0 && parsed[1].HasToken && parsed[1].UnlockCost == 300 &&
            parsed[0].Token != 0 && parsed[0].Token != parsed[1].Token, "Warbond tokens were not given: " + string.Join(" | ", problems));
        var (look, paid) = (parsed[0], parsed[1]);
        JsonElement? Fixture(string path) => fixtures.TryGetValue(path, out var body) ? body : null;
        var service = new ModWarbondService(parsed) { Source = Fixture, Log = _ => { } };
        var list = service.Apply("/SeasonPass", fixtures["/SeasonPass"]).EnumerateArray().ToList();
        uint[] Tokens(uint id32) => list.Single(e => e.GetProperty("id32").GetUInt32() == id32).GetProperty("premiumTokenMixIds").EnumerateArray().Select(t => t.GetUInt32()).ToArray();
        check(Tokens(look.Id32).SequenceEqual([look.Token]) && Tokens(paid.Id32).SequenceEqual([paid.Token]) &&
            service.Detail(paid.Id32.ToString())?.GetProperty("premiumTokenMixIds")[0].GetUInt32() == paid.Token, "A warbond's token is not in its list entry and pages.");
        var catalogue = service.Apply("/Progression/items", fixtures["/Progression/items"]).EnumerateArray()
            .Where(i => i.TryGetProperty("mixId", out var m) && m.TryGetUInt32(out _)).GroupBy(i => i.GetProperty("mixId").GetUInt32()).ToDictionary(g => g.Key, g => g.First());
        long Price(uint token) => catalogue[token].GetProperty("buyPrice").EnumerateArray().Single(p => p.GetProperty("mixId").GetUInt32() == PaidContentSet.SuperCreditPrice).GetProperty("amount").GetInt64();
        check(catalogue.ContainsKey(look.Token) && catalogue.ContainsKey(paid.Token) && Price(paid.Token) == 300 && Price(look.Token) == 1,
            "A warbond's token is not in the catalogue at its price.");
        var held = service.Apply("/Progression/inventory", JsonSerializer.SerializeToElement(new { items = Array.Empty<object>() })).GetProperty("items").EnumerateArray()
            .Select(i => i.GetProperty("mixId").GetUInt32()).ToHashSet();
        check(held.Contains(look.Token) && !held.Contains(paid.Token), "An open warbond's token is not held, or a priced one's is before it is bought.");

        // Bought: refused while the wallet is short, then once for 300 Forge Credits, then never again; its items sell after.
        using var store = ProfileStore.Open(null, "starter");
        store.TryChange((_, inventory) => ProfileStore.AddItem(inventory, 0x4C19589F, 100 - ProfileStore.Held(inventory, 0x4C19589F)) >= 0);
        var purchases = new Purchases(store, id => catalogue.TryGetValue(id, out var item) ? item : null, service);
        (int Status, string Reply) Post(string path, string body)
        {
            using var json = JsonDocument.Parse(body);
            var (status, reply, _) = purchases.Handle(path, json.RootElement);
            return (status, reply?.ToJsonString() ?? "");
        }
        string Unlock() => "{\"mixId\":" + paid.Token + ",\"amount\":1,\"warId\":801,\"expectedCost\":[{\"mixId\":" + PaidContentSet.SuperCreditPrice + ",\"amount\":300}]}";
        string BuyItem(ModWarbonds.Warbond w) => "{\"seasonPassItemId\":" + w.Pages[0].Items[0].Id + ",\"warId\":801,\"expectedCost\":[]}";
        check(Post("/SeasonPass/item/buy", BuyItem(paid)).Status != 200, "A priced warbond's item was sold before the warbond was bought.");
        check(Post("/SeasonPass/item/buy", BuyItem(look)).Status == 200, "An open premium-look warbond's item was not sold.");
        store.TryChange((_, inventory) => ProfileStore.AddItem(inventory, PaidContentSet.ForgeCreditsWallet, 299) >= 0);
        check(Post("/Progression/buy/v2", Unlock()).Status != 200, "A warbond was sold for less than its price.");
        store.TryChange((_, inventory) => ProfileStore.AddItem(inventory, PaidContentSet.ForgeCreditsWallet, 1) >= 0);
        var bought = Post("/Progression/buy/v2", Unlock());
        check(bought.Status == 200 && bought.Reply.Contains(paid.Token.ToString()) && bought.Reply.Contains(PaidContentSet.ForgeCreditsWallet.ToString()),
            "A warbond was not sold for 300 Forge Credits: " + bought.Reply);
        check(store.TryChange((_, inventory) =>
        {
            check(ProfileStore.Held(inventory, paid.Token) == 1 && ProfileStore.Held(inventory, PaidContentSet.ForgeCreditsWallet) == 0, "The bought warbond is not saved, or not paid.");
            return false;
        }) == false, "The profile check did not run.");
        check(Post("/Progression/buy/v2", Unlock()).Status != 200, "A warbond was sold twice.");
        check(Post("/SeasonPass/item/buy", BuyItem(paid)).Status == 200, "A bought warbond's item was not sold.");
        check(new ModWarbondService(parsed) { Source = Fixture, UnlockAll = true, Log = _ => { } }.Owns(paid.Id32, _ => false), "Unlock-all does not own a priced warbond.");
    }

    static List<string> Problems(string warbond)
    {
        var problems = new List<string>();
        using var json = JsonDocument.Parse("{\"format\":1,\"warbonds\":[" + warbond + "]}");
        ModWarbonds.Parse("mod", "Mod", json.RootElement, problems);
        return problems;
    }

    // "art": {"image": ...}: a 2048 x 1024 PNG inside the mod's folder, named for the texture HellForge builds from it.
    static void ArtImages(Action<bool, string> check, string root)
    {
        var mod = Directory.CreateDirectory(Path.Combine(root, "art-mod")).FullName;
        static byte[] Header(int width, int height)
        {
            var png = new byte[33];
            HellForge.Core.GameData.IconImage.PngSignature.CopyTo(png);
            png[11] = 13; "IHDR"u8.CopyTo(png.AsSpan(12));
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(png.AsSpan(16), (uint)width);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(png.AsSpan(20), (uint)height);
            return png;
        }
        File.WriteAllBytes(Path.Combine(mod, "card.png"), Header(2048, 1024));
        File.WriteAllBytes(Path.Combine(mod, "small.png"), Header(1024, 512));
        File.WriteAllBytes(Path.Combine(root, "outside.png"), Header(2048, 1024));
        List<string> Parse(string art, out List<ModWarbonds.Warbond> warbonds)
        {
            var problems = new List<string>();
            using var json = JsonDocument.Parse("{\"format\":1,\"warbonds\":[{\"key\":\"a\",\"name\":\"A\",\"art\":" + art + ",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}");
            warbonds = ModWarbonds.Parse("mod", "Mod", json.RootElement, problems, mod);
            return problems;
        }
        var problems = Parse("{\"image\":\"card.png\"}", out var good);
        check(problems.Count == 0 && good[0].ArtImage == Path.Combine(mod, "card.png") && good[0].ArtFrom == 0 && good[0].ArtTexture == 0,
            "A warbond's own art was not taken: " + string.Join(" | ", problems));
        check(ModWarbonds.ArtTextureName("mod", "a") == ModWarbonds.ArtTextureName("mod", "a") && ModWarbonds.ArtTextureName("mod", "a") != ModWarbonds.ArtTextureName("mod", "b"),
            "Art texture names are not stable and unique.");
        check(ModWarbonds.PlanLines([good[0] with { ArtTexture = 0xDA2176742E3F1FB2 }]).First().Split('\t')[8] == "da2176742e3f1fb2", "A warbond's built art is not in its plan line.");
        foreach (var (art, why) in new[] { ("{\"image\":\"small.png\"}", "art of the wrong size"), ("{\"image\":\"../outside.png\"}", "art outside its mod's folder"),
            ("{\"image\":\"none.png\"}", "art that is not there"), ("{\"image\":\"card.png\",\"from\":610512218}", "two kinds of art"),
            ("{\"image\":\"a\\u0000b.png\"}", "a path the system refuses") })
            check(Parse(art, out _).Count == 1, "A warbond with " + why + " was accepted.");
    }

    // The spec's checks: what the game holds (15 pages, 10 items, the 5 x 3 grid), prices, reachable pages and stable ids.
    static void Spec(Action<bool, string> check)
    {
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
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":3,\"currency\":\"requisition\"}]},{\"requirement\":2,\"items\":[{\"mixId\":2,\"cost\":1}]}]}",
                "a page opened only by prices in another currency"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1},{\"mixId\":1,\"cost\":2}]}]}", "the same item twice without keys"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"weapon\":\"x\",\"cost\":1}]}]}", "an item naming two things"),
            ("{\"key\":\"A B\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a key with capitals and a space"),
            ("{\"key\":\"a\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "no name"),
            /* a value of the wrong JSON type is reported like any mistake, never thrown out of the session */
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":\"5\",\"cost\":1}]}]}", "a mixId as a string"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"position\":[\"0\",0]}]}]}", "a position as strings"),
            ("{\"key\":\"a\",\"name\":\"A\",\"unlock\":{\"currency\":\"forge-credits\",\"cost\":\"5\"},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "an unlock cost as a string"),
            ("{\"key\":\"a\",\"name\":\"A\",\"art\":{\"from\":\"x\"},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "art from a string"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"weapon\":\"x\",\"cost\":1,\"repeatable\":true}]}]}", "a repeatable weapon"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"mod\":\"x\",\"cost\":1}]}]}", "a mod named on a game item"),
            /* a misspelt field is a mistake, never ignored */
            ("{\"key\":\"a\",\"name\":\"A\",\"curency\":\"requisition\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a misspelt warbond field"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":3}]},{\"requirment\":3,\"items\":[{\"mixId\":2,\"cost\":1}]}]}", "a misspelt page field"),
            ("{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1,\"repeatble\":true}]}]}", "a misspelt item field"),
            ("{\"key\":\"a\",\"name\":\"A\",\"unlock\":{\"currency\":\"forge-credits\",\"cost\":5,\"cots\":5},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a misspelt unlock field"),
            ("{\"key\":\"a\",\"name\":\"A\",\"art\":{\"form\":610512218},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}", "a misspelt art field"),
        })
            check(Problems(warbond).Count == 1, "A warbond with " + why + " was accepted.");
        var formatProblems = new List<string>();
        using (var wrongFormat = JsonDocument.Parse("{\"format\":\"1\",\"warbonds\":[]}")) ModWarbonds.Parse("mod", "Mod", wrongFormat.RootElement, formatProblems);
        check(formatProblems.Count == 1, "A format given as a string was not reported.");
        formatProblems.Clear();
        using (var extra = JsonDocument.Parse("{\"format\":1,\"warbond\":[],\"warbonds\":[]}")) ModWarbonds.Parse("mod", "Mod", extra.RootElement, formatProblems);
        check(formatProblems.Count == 1 && formatProblems[0].Contains("\"warbond\""), "A misspelt field at the file's top was not reported.");
        // Without "season", a mod name longer than a season takes is cut, never a reason to refuse the warbond.
        var longProblems = new List<string>();
        var longName = ModWarbonds.Parse("test", "Vanilla Plus Megapack Warbond Collection", JsonDocument.Parse(
            "{\"format\":1,\"warbonds\":[{\"key\":\"a\",\"name\":\"A\",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}").RootElement, longProblems);
        check(longProblems.Count == 0 && longName.Count == 1 && longName[0].Season == "Vanilla Plus Megapack Warbond Co",
            "A long mod name refused a warbond without a season: " + string.Join(" | ", longProblems));
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

        // The look: the variant, the line above the name (the mod's name by default) and a game warbond's art, in the plan.
        check(parsed[0].Variant == 0 && parsed[0].Season == "Mod" && parsed[0].ArtFrom == 0, "A warbond's look does not default to free, its mod's name and no art.");
        using var look = JsonDocument.Parse("{\"format\":1,\"warbonds\":[{\"key\":\"l\",\"name\":\"Look\",\"variant\":\"legendary\",\"season\":\"Act II\"," +
            "\"art\":{\"from\":610512218},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}");
        var styled = ModWarbonds.Parse("mod", "Mod", look.RootElement, problems)[0];
        var line = ModWarbonds.PlanLines([styled]).First().Split('\t');
        check(line.Length == 9 && line[8] == "0000000000000000" && line[0] == "warbond" && line[1] == styled.Id32.ToString() && line[2] == "2" && line[3] == "1276729503" &&
            line[4] == "warbond/mod/l" && line[5] == "Look" && line[6] == "Act II" && line[7] == "610512218", "A warbond's plan line is wrong: " + string.Join("|", line));
        foreach (var (field, why) in new[] { ("\"variant\":\"gold\"", "an unknown variant"), ("\"art\":{\"from\":12345}", "art from no game warbond"),
            ("\"season\":\"\"", "an empty season line") })
            check(Problems("{\"key\":\"a\",\"name\":\"A\"," + field + ",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}").Count == 1, "A warbond with " + why + " was accepted.");

        // The plain card of a warbond without art: one opaque 2048 x 1024 image, under a name of its own.
        var plainCard = HellForge.Core.GameData.ModWarbondArt.DefaultImage();
        check(plainCard.Length == ModWarbonds.ArtWidth * ModWarbonds.ArtHeight * 4 && plainCard.Where((_, i) => i % 4 == 3).All(a => a == 255) &&
            HellForge.Core.GameData.ModWarbondArt.DefaultTexture != 0, "The default warbond card is not an opaque 2048 x 1024 image with a name.");

        // The showcase: none by default (no plan line); its fields in a line of their own after the warbond's.
        check(parsed[0].Showcase == null && ModWarbonds.PlanLines([parsed[0]]).All(l => !l.StartsWith("warbond_showcase", StringComparison.Ordinal)),
            "A warbond without a showcase has one.");
        using var shown = JsonDocument.Parse("{\"format\":1,\"warbonds\":[{\"key\":\"s\",\"name\":\"Shown\",\"showcase\":{\"from\":610512218," +
            "\"pose\":1929468580,\"armor\":11,\"helmet\":12,\"cape\":13,\"weapon\":\"27EE1ED8F6FB6356\"},\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}");
        var showcased = ModWarbonds.Parse("mod", "Mod", shown.RootElement, problems)[0];
        check(showcased.Showcase == new ModWarbonds.Showcase(610512218, 1929468580, 11, 12, 13, 0x27EE1ED8F6FB6356), "A warbond's showcase is not read as written.");
        var showLines = ModWarbonds.PlanLines([showcased]).ToList();
        check(showLines.Count == 3 && showLines[0].StartsWith("warbond\t", StringComparison.Ordinal) &&
            showLines[1] == string.Join('\t', "warbond_showcase", showcased.Id32.ToString(), "610512218", "1929468580", "11", "12", "13", "27ee1ed8f6fb6356"),
            "A warbond's showcase line is wrong: " + string.Join(" | ", showLines));
        using var partial = JsonDocument.Parse("{\"format\":1,\"warbonds\":[{\"key\":\"p\",\"name\":\"P\",\"showcase\":{\"helmet\":7}," +
            "\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}]}");
        check(ModWarbonds.Parse("mod", "Mod", partial.RootElement, problems)[0].Showcase == new ModWarbonds.Showcase(0, 0, 0, 7, 0, 0),
            "A showcase's missing fields are not the game's (0).");
        foreach (var (field, why) in new[] { ("\"showcase\":{\"from\":12345}", "a showcase from no game warbond"),
            ("\"showcase\":{\"pose\":0}", "a pose of no warbond"), ("\"showcase\":{\"armor\":\"x\"}", "an armor that is no mixId"),
            ("\"showcase\":{\"weapon\":\"zz\"}", "a weapon that is not hex"), ("\"showcase\":{\"weapon\":\"0\"}", "a weapon of 0"),
            ("\"showcase\":{\"weapon\":\"12345678901234567\"}", "a weapon past 16 digits"), ("\"showcase\":{\"boots\":1}", "an unknown showcase field"),
            ("\"showcase\":[]", "a showcase that is no object") })
            check(Problems("{\"key\":\"a\",\"name\":\"A\"," + field + ",\"pages\":[{\"items\":[{\"mixId\":1,\"cost\":1}]}]}").Count == 1, "A warbond with " + why + " was accepted.");
    }
}
