using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using ModWarbonds = HellForge.Core.ModWarbonds;
using HellForge.LocalBackend;

// A code mod's currency spend (/hellforge/spend): taken and saved when the balance covers it, refused whole when it does
// not, and nothing a request sends can add currency, spend Super Credits or take an item that is no currency.
static class CurrencySpendTests
{
    static readonly uint Requisition = ModWarbonds.Currencies["requisition"];
    static readonly uint Medals = ModWarbonds.Currencies["medals"];

    public static async Task Run(string root, Action<bool, string> check)
    {
        var folder = Path.Combine(root, "currency-spend");
        using (var profile = ProfileStore.Open(folder, "starter"))
        {
            profile.TryChange((_, inventory) =>
            {
                ProfileStore.AddItem(inventory, Requisition, 5000 - ProfileStore.Held(inventory, Requisition));
                ProfileStore.AddItem(inventory, Medals, 10 - ProfileStore.Held(inventory, Medals));
                return true;
            });
            await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "currency-spend-evidence"), LoginMode.LocalSession)
                { StartupFixtures = true, Profile = profile });
            await app.StartAsync();
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };

            async Task<(HttpStatusCode Status, JsonNode? Body)> Spend(string body)
            {
                using var response = await client.PostAsync(CurrencySpend.Path, new StringContent(body, Encoding.UTF8, "application/json"));
                var text = await response.Content.ReadAsStringAsync();
                return (response.StatusCode, response.IsSuccessStatusCode ? JsonNode.Parse(text) : null);
            }
            string Body(uint currency, long amount, string mod = "yui.kit", string reason = "kit level 2") =>
                new JsonObject { ["mod"] = mod, ["currency"] = currency, ["amount"] = amount, ["reason"] = reason }.ToJsonString();
            long Held(uint mixId) => ProfileStore.Held(profile.Current()["inventory"]!["items"]!.AsArray(), mixId);

            var spent = await Spend(Body(Requisition, 1500));
            check(spent.Status == HttpStatusCode.OK && (string?)spent.Body?["result"] == "spent" && (long?)spent.Body?["balance"] == 3500 &&
                Held(Requisition) == 3500, "A spend the balance covers was not taken, or its new balance is wrong.");

            var over = await Spend(Body(Medals, 11));
            check(over.Status == HttpStatusCode.OK && (string?)over.Body?["result"] == "too_little" && (long?)over.Body?["balance"] == 10 &&
                Held(Medals) == 10, "A spend over the balance took something.");

            foreach (var (body, what) in new[]
                     {
                         (Body(Requisition, -100), "a negative amount"), (Body(Requisition, 0), "nothing"),
                         (Body(Requisition, CurrencySpend.MaxAmount + 1), "more than the cap"),
                         (Body(PaidContentSet.SuperCreditPrice, 1), "Super Credits"), (Body(0x7001, 1), "an item that is no currency"),
                         (Body(Requisition, 1, mod: ""), "no mod"), ("{\"currency\":" + Requisition + ",\"amount\":1}", "no mod or reason")
                     })
                check((await Spend(body)).Status == HttpStatusCode.BadRequest, "A spend of " + what + " was not refused.");
            check(Held(Requisition) == 3500, "A refused spend changed the balance.");
        }
        using (var reopened = ProfileStore.Open(folder))
            check(ProfileStore.Held(reopened.Current()["inventory"]!["items"]!.AsArray(), Requisition) == 3500, "A spend was not saved with the profile.");
        using var empty = JsonDocument.Parse("{}");
        check(CurrencySpend.Handle(null, empty.RootElement).Status == 409, "A spend without a profile was not refused as unavailable.");
    }
}
