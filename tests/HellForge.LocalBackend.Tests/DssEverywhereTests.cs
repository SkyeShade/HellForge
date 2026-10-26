using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using HellForge.LocalBackend;

// DSS effects on every planet (DssEverywhere, backend --dss-everywhere <keys>): the picked effects, and with "current" the
// ones the DSS has now, join every planet of the war status once, the station stays put, and the backend serves them with
// the frozen war and the all-planets war, and only when asked.
static class DssEverywhereTests
{
    public static async Task Run(string root, Action<bool, string> check)
    {
        using var embedded = LocalBackend.LoadResource("war-data.json");
        var frozen = embedded.RootElement.GetProperty("status");
        var planets = frozen.GetProperty("planetStatus").EnumerateArray().Select(p => p.GetProperty("index").GetInt32()).ToList();
        var station = frozen.GetProperty("spaceStations")[0];
        var stationEffects = station.GetProperty("activeEffectIds").EnumerateArray().Select(e => e.GetInt32()).ToList();
        var all = DssEffects.Keys;
        var expected = DssEffects.Ids(all).Union(stationEffects).ToList();

        static List<(int Planet, int Effect)> Active(JsonElement status) => status.GetProperty("planetActiveEffects").EnumerateArray()
            .Select(e => (e.GetProperty("index").GetInt32(), e.GetProperty("galacticEffectId").GetInt32())).ToList();
        var status = DssEverywhere.Status(frozen, all);
        var active = Active(status);
        check(planets.Count > 200 && planets.All(p => expected.All(e => active.Contains((p, e)))), "Every planet has every DSS effect.");
        check(new[] { 1425, 1210, 1237, 1214, 1238, 1261, 1262, 1384, 1385 }.All(e => active.Contains((planets[^1], e))),
            "Eagle Storm, Orbital Blockade, Heavy Ordnance Distribution, Operational Support, Exosuit Reserves.");
        check(active.Distinct().Count() == active.Count, "No planet has an effect twice.");
        var before = Active(frozen);
        check(before.All(active.Contains), "Planets keep their own effects.");
        check(active.Count == before.Count + planets.Count * expected.Count - before.Count(b => expected.Contains(b.Effect)), $"Only DSS effects are added ({active.Count}).");
        check(status.GetProperty("spaceStations").GetRawText() == frozen.GetProperty("spaceStations").GetRawText() &&
            status.GetProperty("campaigns").GetRawText() == frozen.GetProperty("campaigns").GetRawText(), "The station and the rest of the war are unchanged.");
        check(DssEverywhere.Status(status, all).GetRawText() == status.GetRawText(), "Applying twice changes nothing more.");

        // only the picked ones; none picked, no change
        var picked = Active(DssEverywhere.Status(frozen, ["exosuit-reserves-patriot"]));
        check(planets.All(p => picked.Contains((p, 1261))) && !picked.Contains((planets[^1], 1262)) && !picked.Contains((planets[^1], 1425)),
            "Only the picked effects join.");
        check(DssEverywhere.Status(frozen, []).GetRawText() == frozen.GetRawText(), "None picked, no change.");
        var withoutCurrent = Active(DssEverywhere.Status(frozen, all.Where(k => k != DssEffects.Current).ToList()));
        check(withoutCurrent.Count == active.Count, "The frozen DSS's effects are all in the list.");

        // an effect the DSS has now that the list does not know joins too
        var node = JsonNode.Parse(frozen.GetRawText())!.AsObject();
        node["spaceStations"]![0]!["activeEffectIds"]!.AsArray().Add(4242);
        using (var withNew = JsonDocument.Parse(node.ToJsonString()))
        {
            check(planets.All(p => Active(DssEverywhere.Status(withNew.RootElement, [DssEffects.Current])).Contains((p, 4242))), "The DSS's current effects join every planet.");
            check(!Active(DssEverywhere.Status(withNew.RootElement, ["eagle-storm"])).Contains((planets[0], 4242)), "Not unless picked.");
        }
        var none = JsonNode.Parse(frozen.GetRawText())!.AsObject();
        none.Remove("spaceStations");
        none.Remove("planetActiveEffects");
        using (var bare = JsonDocument.Parse(none.ToJsonString()))
            check(Active(DssEverywhere.Status(bare.RootElement, all)).Count == planets.Count * DssEffects.Ids(all).Count, "Without a station, the list alone.");

        // the game's definitions of every DSS effect (and of the frozen station's own), each once, kept when already listed
        static List<int> Defined(JsonElement list) => list.EnumerateArray().Select(d => d.GetProperty("id").GetInt32()).ToList();
        using (var empty = JsonDocument.Parse("[]"))
        {
            var defined = Defined(DssEverywhere.Definitions(empty.RootElement));
            check(expected.All(defined.Contains) && defined.Distinct().Count() == defined.Count, $"Every DSS effect has its definition ({defined.Count}).");
            check(Defined(DssEverywhere.Definitions(DssEverywhere.Definitions(empty.RootElement))).Count == defined.Count, "Adding the definitions twice changes nothing more.");
        }
        using (var own = JsonDocument.Parse("""[{"id": 1425, "effectType": 0}]"""))
        {
            var merged = DssEverywhere.Definitions(own.RootElement);
            check(merged.EnumerateArray().Count(d => d.GetProperty("id").GetInt32() == 1425) == 1 && merged[0].GetProperty("effectType").GetInt32() == 0,
                "A definition the list has already stays as it is.");
        }

        // served by the backend, with the frozen war and the all-planets war; not without the option
        foreach (var (name, openWar, on) in new[] { ("frozen", false, true), ("open", true, true), ("off", false, false) })
        {
            await using var app = LocalBackend.Create(0, Path.Combine(root, "dss-everywhere-" + name), LoginMode.LocalSession, true, openWar: openWar, dssEverywhere: on ? all : null);
            await app.StartAsync();
            try
            {
                using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()) };
                using var response = await client.GetAsync("/WarSeason/801/Status");
                using var served = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                var servedActive = Active(served.RootElement).ToHashSet();
                var everywhere = planets.All(p => expected.All(e => servedActive.Contains((p, e))));
                check(everywhere == on, $"The backend serves every DSS effect on every planet only when asked ({name}).");
                using var definitions = JsonDocument.Parse(await client.GetStringAsync(DssEverywhere.DefinitionsPath));
                var servedDefinitions = Defined(definitions.RootElement);
                check(on ? expected.All(servedDefinitions.Contains) : servedDefinitions.Count == 0, $"The backend serves the DSS effects' definitions only when asked ({name}).");
                if (openWar) check(served.RootElement.GetProperty("campaigns").GetArrayLength() == planets.Count - 1, "Still the all-planets war.");
            }
            finally { await app.StopAsync(); }
        }
        var refused = false;
        try { LocalBackend.Create(0, Path.Combine(root, "dss-everywhere-refused"), LoginMode.LocalSession, false, dssEverywhere: all); }
        catch (ArgumentException) { refused = true; }
        check(refused, "Every DSS effect everywhere needs the startup fixtures.");
    }
}
