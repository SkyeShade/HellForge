using System.Collections.Concurrent;
using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;
using HellForge.LocalBackend.Multiplayer;

// Planet modifiers (HellForge.Core PlanetModifiers, PlanetOverrides, PlanetShare): the frozen war with a planet's enemy,
// subfactions and enemy groups changed as the file says; the catalogue's effect definitions; the file read again as it
// changes; a host's served to its joiner while it is there, and the joiner's own back when it leaves.
static class PlanetModifiersTests
{
    const ulong HostId = 0x110000100000041, JoinerId = 0x110000100000042;

    public static async Task Run(string root, Action<bool, string> check)
    {
        Transform(check);
        await Served(root, check);
        Shared(check);
    }

    static void Transform(Action<bool, string> check)
    {
        var catalog = PlanetModifiers.Catalog;
        check(catalog.Planets.Count == 273 && catalog.Planet(196)?.Name == "Malevelon Creek", "Planets: 273, by index, named (196 Malevelon Creek).");
        check(catalog.Planets.All(p => p.Preview != 0) && catalog.Planet(0)?.Preview == 0x4FC004E4038C7635, "Every planet names its picture (Super Earth's 4FC004E4038C7635).");
        var hulks = catalog.Enemy("hulks")!;
        check(hulks.Faction == 3 && hulks.Families.Contains(0x30D5739Cu) && hulks.Card == 1357, "Hulks: the Automatons', with the live war's Hulk family and HULK SURGE card.");
        var definitions = PlanetPlan.Effects(JsonNode.Parse("[{\"id\":1202,\"effectType\":0},{\"id\":5}]")!).AsArray();
        var ids = definitions.Select(e => (int)e!["id"]!).ToList();
        check(ids.Count == ids.Distinct().Count() && ids.Contains(5) && definitions.First(e => (int)e!["id"]! == 1202)!["effectType"]!.GetValue<int>() == 40,
            "Effect definitions: the catalogue's join the war's, replacing the same id, none twice.");
        var surge = definitions.First(e => (int)e!["id"]! == hulks.Surge[0])!;
        check(surge["effectType"]!.GetValue<int>() == 72 && surge["values"]![0]!.GetValue<uint>() == hulks.Families[0] && surge["values"]![1]!.GetValue<uint>() == 1000,
            "A surge is a type 72 spawn weight of x10 on its family.");
        var ban = definitions.First(e => (int)e!["id"]! == hulks.None[0])!;
        check(ban["values"]![1]!.GetValue<uint>() == unchecked((uint)-100), "None is a spawn weight of -100 (left out).");

        using var war = LocalBackend.LoadResource("war-data.json");
        var info = war.RootElement.GetProperty("warInfo");
        var frozen = war.RootElement.GetProperty("status").GetRawText();
        var plan = PlanetPlan.Parse("""
            { "planets": [
              // a Super Earth planet given to the Automatons, with the Jet Brigade and a Hulk surge (and Chargers, not theirs)
              { "index": 1, "faction": "automatons", "subfactions": ["jet-brigade"], "enemies": { "hulks": "surge", "chargers": "surge", "tanks": "none" } },
              // Menkent, the Jet Brigade's: given to the Terminids, it loses them
              { "index": 203, "faction": "terminids" },
              // a defense against the Automatons: now a liberation of the Terminids
              { "index": 246, "faction": "terminids", "subfactions": ["predator-strain"] },
              // Calypso: its no-missions modifier goes; an Illuminate planet with no subfaction gets one
              { "index": 97, "faction": "illuminate", "subfactions": [] },
              // the Illuminate's own planet: its subfactions replaced by none at all (but one Illuminate tag stays)
              { "index": 7, "subfactions": [] },
              { "index": 0, "faction": "terminids" },
              { "index": 260, "faction": "terminids" },
              { "index": 2, "faction": "bugs", "enemies": { "nothing": "surge", "hulks": "lots" } },
            ] }
            """, out var problems);
        check(problems.Any(p => p.Contains("Super Earth is a home world")) && problems.Any(p => p.Contains("is a home world")) && problems.Any(p => p.Contains("no faction \"bugs\"")) &&
            problems.Any(p => p.Contains("no enemy group \"nothing\"")) && problems.Any(p => p.Contains("hulks takes")), "Parse: home worlds keep their owners, unknown keys are named: " + string.Join("; ", problems));
        check(plan.For(0) == null && plan.For(260) == null && plan.For(2) == null, "Entries left with nothing to do are dropped.");
        var log = new List<string>();
        var served = plan.Status(JsonNode.Parse(frozen)!, info, log.Add).AsObject();
        JsonObject Row(int index) => served["planetStatus"]!.AsArray().OfType<JsonObject>().First(p => (int)p["index"]! == index);
        HashSet<int> Effects(int index) => [.. served["planetActiveEffects"]!.AsArray().OfType<JsonObject>().Where(e => (int)e["index"]! == index).Select(e => (int)e["galacticEffectId"]!)];
        List<JsonObject> Campaigns(int index) => [.. served["campaigns"]!.AsArray().OfType<JsonObject>().Where(c => (int)c["planetIndex"]! == index)];
        var maxHealth = info.GetProperty("planetInfos").EnumerateArray().First(p => p.GetProperty("index").GetInt32() == 1).GetProperty("maxHealth").GetInt64();

        check((int)Row(1)["owner"]! == 3 && (long)Row(1)["health"]! == maxHealth, "Planet 1 is the Automatons', unliberated.");
        check(Campaigns(1) is [var c1] && (int)c1["id"]! == PlanetModifiers.CampaignIdBase + 1 && (int)c1["type"]! == 0 && (int)c1["race"]! == 1, "Planet 1 has one liberation campaign.");
        var on1 = Effects(1);
        check(on1.IsSupersetOf([1202, 1203, 1357, .. hulks.Surge, .. catalog.Enemy("tanks")!.None]) && !on1.Overlaps(catalog.Enemy("chargers")!.Surge),
            "Planet 1: Jet Brigade with its flag, the Hulk surge with its card, no tanks; the Terminids' Chargers left out.");
        check(log.Any(l => l.Contains("chargers are not its enemy's")), "Chargers on an Automaton planet are logged.");
        check((int)Row(203)["owner"]! == 2 && !Effects(203).Overlaps([1202, 1203]), "Menkent is the Terminids', without the Jet Brigade.");
        check(!served["planetEvents"]!.AsArray().OfType<JsonObject>().Any(e => (int)e["planetIndex"]! == 246) &&
            !served["jointOperations"]!.AsArray().OfType<JsonObject>().Any(j => (int)j["planetIndex"]! == 246) &&
            Campaigns(246) is [var c246] && (int)c246["type"]! == 0 && (int)Row(246)["owner"]! == 2 && Effects(246).IsSupersetOf([1243, 1245]),
            "The defense of 246 is gone: a Terminid liberation with the Predator Strain.");
        check((int)Row(97)["owner"]! == 4 && !Effects(97).Contains(1418) && Effects(97).Contains(1190) && Effects(97).Contains(1413) && Campaigns(97).Count == 1,
            "Calypso: the Illuminate's, its no-missions modifier gone, its other effect kept, Invasion Fleet so its squads spawn.");
        var ownCampaign = JsonNode.Parse(frozen)!["campaigns"]!.AsArray().OfType<JsonObject>().Where(c => (int)c["planetIndex"]! == 7).Select(c => (int)c["id"]!).Single();
        check(Campaigns(7) is [var c7] && (int)c7["id"]! == ownCampaign && Effects(7).SetEquals([1413]),
            "Planet 7 keeps its campaign; with no subfactions it has only the Illuminate tag its squads need.");
        check(served.ToJsonString() == plan.Status(JsonNode.Parse(frozen)!, info).ToJsonString(), "The same on every machine.");
        check(PlanetPlan.Empty.Status(JsonNode.Parse(frozen)!, info).ToJsonString() == JsonNode.Parse(frozen)!.ToJsonString(), "No modifiers: the war as it is.");

        // more effects than a planet holds
        var many = new PlanetPlan([new PlanetOverride(196, "automatons", ["jet-brigade", "incineration-corps", "cyborgs", "heavy-seaf-presence"],
            catalog.Enemies.Where(e => e.Faction == 3).ToDictionary(e => e.Key, _ => PlanetOverride.Surge))]);
        var capped = many.Status(JsonNode.Parse(frozen)!, info).AsObject();
        check(capped["planetActiveEffects"]!.AsArray().OfType<JsonObject>().Count(e => (int)e["index"]! == 196) == PlanetModifiers.MaxEffectsPerPlanet,
            $"At most {PlanetModifiers.MaxEffectsPerPlanet} effects a planet.");
        var again = PlanetPlan.Parse(many.ToJson(), out var none);
        check(none.Count == 0 && again.ToJson() == many.ToJson(), "The file round trips.");
    }

    static async Task Served(string root, Action<bool, string> check)
    {
        var file = Path.Combine(root, "planet-modifiers", PlanetModifiers.FileName);
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var log = new ConcurrentQueue<string>();
        var planets = new PlanetOverrides(file, log.Enqueue);
        await using var app = LocalBackend.Create(new BackendSettings(0, Path.Combine(root, "planet-modifiers", "evidence"), LoginMode.LocalSession)
            { StartupFixtures = true, Planets = planets });
        await app.StartAsync();
        using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()), Timeout = TimeSpan.FromSeconds(10) };
        async Task<JsonElement> Get(string path)
        {
            using var response = await client.GetAsync(path);
            var text = await response.Content.ReadAsStringAsync();
            check(response.StatusCode == HttpStatusCode.OK, $"GET {path} answered {(int)response.StatusCode}");
            using var json = JsonDocument.Parse(text);
            return json.RootElement.Clone();
        }
        int Owner(JsonElement status, int index) => status.GetProperty("planetStatus").EnumerateArray().First(p => p.GetProperty("index").GetInt32() == index).GetProperty("owner").GetInt32();

        var effects = await Get("/WarSeason/GalacticWarEffects");
        check(effects.EnumerateArray().Any(e => e.GetProperty("id").GetInt32() == PlanetModifiers.Catalog.Enemy("hulks")!.Surge[0]),
            "The catalogue's effects are served with no modifiers set.");
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 1, "No file: planet 1 is Super Earth's.");
        new PlanetPlan([new PlanetOverride(1, "automatons", null, new Dictionary<string, string>())]).Write(file);
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 3, "With the file: planet 1 is the Automatons'.");
        await File.WriteAllTextAsync(file, "{ not json");
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 3 && log.Any(l => l.Contains("the last good one stays")), "A broken edit keeps the last good file.");
        new PlanetPlan([new PlanetOverride(1, "terminids", null, new Dictionary<string, string>())]).Write(file);
        File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddSeconds(5));
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 2, "An edit is read again: planet 1 is the Terminids'.");
        planets.SetHost("Host", new PlanetPlan([new PlanetOverride(1, "illuminate", null, new Dictionary<string, string>())]).ToJson());
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 4, "Joined: the host's planet modifiers are served.");
        planets.ClearHost();
        check(Owner(await Get("/WarSeason/801/Status"), 1) == 2, "The host gone: this machine's own come back.");
    }

    sealed class Machine : IDisposable
    {
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly IPeerTransport Link;
        public readonly LobbyService Lobby;
        public readonly PlanetOverrides Planets;
        public readonly PlanetShare Share;

        public Machine(InMemoryNetwork network, ulong id, string? file, ConcurrentQueue<string> log)
        {
            Link = network.Join(id);
            Lobby = new LobbyService(Link, Inbox.Add, _ => { }, "mods");
            Planets = new PlanetOverrides(file, line => log.Enqueue($"{id:X}: {line}"));
            Share = new PlanetShare(Link, Lobby, Planets, _ => "Host", line => log.Enqueue($"{id:X}: {line}"));
            Link.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type == "war.planets") Share.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
        }

        public void Dispose() { Share.DisposeAsync().AsTask().Wait(); Link.Dispose(); }
    }

    static bool Eventually(Func<bool> test)
    {
        for (var deadline = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < deadline; Thread.Sleep(50)) if (test()) return true;
        return test();
    }

    static void Shared(Action<bool, string> check)
    {
        var folder = Directory.CreateTempSubdirectory("hf-planets-");
        try
        {
            var hostFile = Path.Combine(folder.FullName, "host.json");
            var joinerFile = Path.Combine(folder.FullName, "joiner.json");
            new PlanetPlan([new PlanetOverride(1, "automatons", ["jet-brigade"], new Dictionary<string, string> { ["hulks"] = PlanetOverride.Surge })]).Write(hostFile);
            new PlanetPlan([new PlanetOverride(5, "terminids", null, new Dictionary<string, string>())]).Write(joinerFile);
            var network = new InMemoryNetwork();
            var log = new ConcurrentQueue<string>();
            using var host = new Machine(network, HostId, hostFile, log);
            using var joiner = new Machine(network, JoinerId, joinerFile, log);
            check(joiner.Planets.Current.For(5) != null, "Alone, the joiner plays with its own planet modifiers.");

            host.Lobby.FromLocal(MultiplayerTests.Create("c1", HostId));
            host.Inbox.TryTake(out var created, TimeSpan.FromSeconds(5));
            joiner.Lobby.FromLocal(MultiplayerTests.Join("j1", JoinerId, created!.Text(4)));
            check(Eventually(() => joiner.Planets.Current.For(1)?.Faction == "automatons" && joiner.Planets.Current.For(5) == null),
                "Joined, the joiner plays with the host's: " + string.Join(" | ", log));
            new PlanetPlan([new PlanetOverride(1, "illuminate", null, new Dictionary<string, string>())]).Write(hostFile);
            File.SetLastWriteTimeUtc(hostFile, DateTime.UtcNow.AddSeconds(5));
            check(Eventually(() => joiner.Planets.Current.For(1)?.Faction == "illuminate"), "The host's change reaches the joiner.");
            joiner.Lobby.FromLocal(new Frame("lobby.leave").Add("l1").Add(created.Text(3)).Add(LobbyService.EntityId(JoinerId)));
            check(Eventually(() => joiner.Planets.Current.For(5) != null && joiner.Planets.Current.For(1) == null), "Gone from the lobby, the joiner's own come back.");
        }
        finally { folder.Delete(recursive: true); }
    }
}
