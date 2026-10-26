using System.Text.Json;
using HellForge.LocalBackend;

// The all-planets war (OpenWar, backend --war open): every planet but Super Earth's home world an enemy's with one
// liberation campaign, Super Earth's planets given to the nearest front, the frozen war's defenses gone; served in place
// of the frozen war and of the live war.
static class OpenWarTests
{
    public static async Task Run(string root, Action<bool, string> check)
    {
        using var embedded = LocalBackend.LoadResource("war-data.json");
        var info = embedded.RootElement.GetProperty("warInfo");
        var frozen = embedded.RootElement.GetProperty("status");
        var open = OpenWar.Status(info, frozen);

        var planets = info.GetProperty("planetInfos").GetArrayLength();
        var owner = open.GetProperty("planetStatus").EnumerateArray().ToDictionary(p => p.GetProperty("index").GetInt32(), p => p.GetProperty("owner").GetInt32());
        var campaigns = open.GetProperty("campaigns").EnumerateArray().ToList();
        check(campaigns.Count == planets - 1, $"Every planet but Super Earth has a campaign: {campaigns.Count} of {planets - 1}.");
        check(owner[0] == 1 && campaigns.All(c => c.GetProperty("planetIndex").GetInt32() != 0), "Super Earth stays Super Earth's, with no campaign.");
        foreach (var war in new[] { open, OpenWar.Status(info, frozen, seed: 12345), OpenWar.Status(info, frozen, dealSubfactions: false, dealSeaf: false) })
            check(war.GetProperty("planetActiveEffects").EnumerateArray().All(e => e.GetProperty("index").GetInt32() != 0) &&
                  war.GetProperty("planetRegions").EnumerateArray().All(r => r.GetProperty("planetIndex").GetInt32() != 0),
                "Super Earth has no faction: no effects (subfactions, flags, SEAF) and no region rows, dealt or not.");
        // Meridia, Ivis, Moradesh, Calypso, Penta, Angel's Venture: their no-missions modifiers go, so their campaigns offer
        // missions; each an enemy's, with its campaign
        var blocked = new (int Planet, int Effect)[] { (64, 1229), (51, 1252), (85, 1252), (97, 1418), (115, 1352), (127, 1241) };
        bool Has(JsonElement war, int planet, int effect) => war.GetProperty("planetActiveEffects").EnumerateArray()
            .Any(e => e.GetProperty("index").GetInt32() == planet && e.GetProperty("galacticEffectId").GetInt32() == effect);
        foreach (var (planet, effect) in blocked)
            check(Has(frozen, planet, effect) && !Has(open, planet, effect) && owner[planet] is 2 or 3 or 4 &&
                  campaigns.Any(c => c.GetProperty("planetIndex").GetInt32() == planet),
                $"Planet {planet} loses effect {effect}, which blocks its missions, and is an enemy's ({owner[planet]}) with a campaign.");
        check(Has(open, 97, 1190), "Calypso keeps its other effect (1190).");
        check(campaigns.All(c => c.GetProperty("type").GetInt32() == 0 && c.GetProperty("race").GetInt32() == 1 && owner[c.GetProperty("planetIndex").GetInt32()] is 2 or 3 or 4),
            "Each campaign is a liberation (type 0, race 1) of an enemy-held planet.");
        check(campaigns.Select(c => c.GetProperty("id").GetInt32()).Distinct().Count() == campaigns.Count, "Campaign ids are unique.");
        check(open.GetProperty("planetEvents").GetArrayLength() == 0 && open.GetProperty("planetAttacks").GetArrayLength() == 0 &&
            open.GetProperty("jointOperations").GetArrayLength() == 0, "The frozen war's defenses, attacks and joint operations are gone.");
        // one enemy per sector of the galaxy map; a sector the frozen war's enemy holds stays that enemy's
        var frozenOwner = frozen.GetProperty("planetStatus").EnumerateArray().ToDictionary(p => p.GetProperty("index").GetInt32(), p => p.GetProperty("owner").GetInt32());
        var sectors = OpenWar.Sectors();
        var mixed = owner.Where(o => o.Key != 0).GroupBy(o => sectors[o.Key]).Where(g => g.Select(o => o.Value).Distinct().Count() > 1).Select(g => g.Key).ToList();
        check(mixed.Count == 0, "Each sector is one enemy's: " + string.Join(", ", mixed));
        check(frozenOwner.Where(o => o.Value != 1).GroupBy(o => sectors[o.Key]).Where(g => g.Select(o => o.Value).Distinct().Count() == 1)
            .All(g => owner[g.First().Key] == g.First().Value), "A sector one enemy holds in the frozen war stays its.");
        check(owner[196] == 3 && owner[241] == 3 && owner[153] == 3, $"Malevelon Creek, Ubanea and Draupnir are the Automatons', not {owner[196]}, {owner[241]}, {owner[153]}.");
        check(owner[97] == 4 && owner[34] == 2 && owner[169] == 2, "Calypso the Illuminate's; Hellmire and Estanu the Terminids'.");
        var shares = owner.Where(o => o.Key != 0).GroupBy(o => o.Value).ToDictionary(g => g.Key, g => g.Count());
        check(shares.Count == 3 && shares.Values.All(n => n >= 60), "Each enemy holds a large share of the galaxy: " + string.Join(", ", shares.Select(s => s.Key + "=" + s.Value)));
        check(OpenWar.Status(info, frozen).GetRawText() == open.GetRawText(), "The same on every machine.");
        // subfactions: the effect definitions are served, a planet holds only its enemy's, every Illuminate planet one
        var subfactions = OpenWar.Subfactions();
        var definitions = LocalBackend.LoadStartupFixtures()["/WarSeason/GalacticWarEffects"];
        var flags = OpenWar.Flags();
        check(definitions.GetArrayLength() == 24 && definitions.EnumerateArray().All(e => e.GetProperty("effectType").GetInt32() == 40 ?
            subfactions.ContainsKey(e.GetProperty("id").GetInt32()) : e.GetProperty("effectType").GetInt32() == 71 && flags.Values.Contains(e.GetProperty("id").GetInt32())),
            "The 13 subfaction effects and 11 flags are served (not an empty list).");
        var onPlanet = open.GetProperty("planetActiveEffects").EnumerateArray()
            .Select(e => (Index: e.GetProperty("index").GetInt32(), Id: e.GetProperty("galacticEffectId").GetInt32()))
            .Where(e => subfactions.TryGetValue(e.Id, out var f) && f != 0).ToList();
        check(onPlanet.All(e => owner[e.Index] == subfactions[e.Id]), "No planet keeps another enemy's subfaction.");
        var illuminate = owner.Where(o => o.Value == 4).Select(o => o.Key).ToList();
        check(illuminate.All(i => onPlanet.Any(e => e.Index == i)), "Every Illuminate planet has an Illuminate subfaction.");
        // dealt planet by planet (the default): half of each enemy's planets without one (every Illuminate planet with one),
        // neighbours in a sector unlike each other
        var disabled = info.GetProperty("planetInfos").EnumerateArray().Where(p => p.TryGetProperty("disabled", out var d) && d.ValueKind == JsonValueKind.True)
            .Select(p => p.GetProperty("index").GetInt32()).ToHashSet();
        // key planets (Cyberstan, Oshaune) keep the frozen war's subfactions, so they are not dealt
        var keyPlanets = new[] { 260, 212 };
        foreach (var faction in new[] { 2, 3 })
        {
            var held = owner.Where(o => o.Value == faction && !disabled.Contains(o.Key) && !keyPlanets.Contains(o.Key)).Select(o => o.Key).ToList();
            var with = held.Count(i => onPlanet.Any(e => e.Index == i));
            check(with == held.Count - held.Count / 2, $"Faction {faction}: half of its {held.Count} planets have a subfaction ({with}).");
        }
        foreach (var war in new[] { open, OpenWar.Status(info, frozen, seed: 12345), OpenWar.Status(info, frozen, seed: 999) })
        {
            var at = war.GetProperty("planetActiveEffects").EnumerateArray()
                .Select(e => (Index: e.GetProperty("index").GetInt32(), Id: e.GetProperty("galacticEffectId").GetInt32())).ToHashSet();
            List<int> Subfactions(int planet) => [.. at.Where(e => e.Index == planet && subfactions.TryGetValue(e.Id, out var f) && f != 0).Select(e => e.Id).Order()];
            check(owner[260] == 3 && Subfactions(260).SequenceEqual([1360]) && at.Contains((260, 1361)),
                "Cyberstan keeps its Cyborgs (1360) and their flag, whatever the seed: " + string.Join(",", Subfactions(260)));
            check(owner[212] == 2 && Subfactions(212).SequenceEqual([1303, 1306, 1307]) && new[] { 1310, 1308, 1309 }.All(f => at.Contains((212, f))),
                "Oshaune keeps the Rupture Strain, Dragonroach and Hive Lord, with their flags, whatever the seed: " + string.Join(",", Subfactions(212)));
        }
        check(sectors.Values.Distinct().Any(sector => onPlanet.Where(e => sectors.GetValueOrDefault(e.Index) == sector).Select(e => e.Id).Distinct().Count() >= 2),
            "Subfactions go planet by planet: a sector holds more than one.");
        // not dealt: the frozen war's own stay (Predator Strain on 171), and its SEAF allies
        var kept = OpenWar.Status(info, frozen, dealSubfactions: false, dealSeaf: false);
        List<(int Index, int Id)> Effects(JsonElement war, Func<int, bool> which) => war.GetProperty("planetActiveEffects").EnumerateArray()
            .Select(e => (e.GetProperty("index").GetInt32(), e.GetProperty("galacticEffectId").GetInt32())).Where(e => which(e.Item2)).OrderBy(e => e).ToList();
        bool Seaf(int id) => subfactions.TryGetValue(id, out var f) && f == 0;
        check(owner[171] == 2 && Effects(kept, id => id == 1243).Contains((171, 1243)), "Not dealt: a frozen-war subfaction of the planet's enemy stays (Predator Strain on 171).");
        check(Effects(kept, id => subfactions.TryGetValue(id, out var f) && f != 0).All(e => owner[e.Item1] == subfactions[e.Item2]) &&
              illuminate.All(i => Effects(kept, id => subfactions.TryGetValue(id, out var f) && f == 4).Any(e => e.Item1 == i)),
            "Not dealt: no planet keeps another enemy's subfaction, and every Illuminate planet still has one.");
        check(Effects(kept, Seaf).SequenceEqual(Effects(frozen, Seaf)) && Effects(kept, Seaf).Count > 0, "Not dealt: the frozen war's SEAF allies stay.");
        // SEAF allies dealt (the default): about one enemy planet in eight, not the frozen war's
        var seaf = Effects(open, Seaf);
        var enemyPlanets = owner.Count(o => o.Value != 1);
        check(seaf.Count > enemyPlanets / 16 && seaf.Count < enemyPlanets / 4 && !seaf.SequenceEqual(Effects(frozen, Seaf)),
            $"SEAF allies dealt across the galaxy: {seaf.Count} of {enemyPlanets} planets.");
        var all = open.GetProperty("planetActiveEffects").EnumerateArray()
            .Select(e => (Index: e.GetProperty("index").GetInt32(), Id: e.GetProperty("galacticEffectId").GetInt32())).ToList();
        // (the SEAF's, 1401, is any enemy's: all, not onPlanet)
        check(all.Where(e => flags.ContainsKey(e.Id)).All(e => all.Count(x => x == (e.Index, flags[e.Id])) == 1) &&
              all.Where(e => flags.Values.Contains(e.Id)).All(e => all.Any(x => x.Index == e.Index && flags.TryGetValue(x.Id, out var f) && f == e.Id)),
            "Each subfaction on a planet has its flag there once, and no flag is without its subfaction.");
        // another seed deals another layout, within each planet's enemy's own; the same seed, the same war; 0 the default
        List<(int Index, int Id)> Dealt(JsonElement war) => war.GetProperty("planetActiveEffects").EnumerateArray()
            .Select(e => (e.GetProperty("index").GetInt32(), e.GetProperty("galacticEffectId").GetInt32()))
            .Where(e => subfactions.TryGetValue(e.Item2, out var f) && f != 0).OrderBy(e => e).ToList();
        var seeded = OpenWar.Status(info, frozen, seed: 12345);
        var reseeded = OpenWar.Status(info, frozen, seed: 999);
        check(OpenWar.Status(info, frozen, seed: 0).GetRawText() == open.GetRawText(), "Seed 0 is the default layout.");
        check(OpenWar.Status(info, frozen, seed: 12345).GetRawText() == seeded.GetRawText() && !Dealt(seeded).SequenceEqual(Dealt(open)) &&
              !Dealt(seeded).SequenceEqual(Dealt(reseeded)), "A seed deals another layout, the same each time.");
        check(Dealt(seeded).All(e => owner[e.Index] == subfactions[e.Id]) && illuminate.All(i => Dealt(seeded).Any(e => e.Index == i)),
            "A seeded layout keeps each planet to its enemy's subfactions, and every Illuminate planet has one.");
        // Cyberstan (the Automatons' home world): its 8 Megafactory regions are served available, at full health, with its
        // campaign; the frozen war's other region rows stay
        var regionInfos = info.GetProperty("planetRegions").EnumerateArray().Where(r => r.GetProperty("planetIndex").GetInt32() == 260)
            .ToDictionary(r => r.GetProperty("regionIndex").GetInt32(), r => r.GetProperty("maxHealth").GetInt64());
        var cyberstan = open.GetProperty("planetRegions").EnumerateArray().Where(r => r.GetProperty("planetIndex").GetInt32() == 260).ToList();
        check(owner[260] == 3 && regionInfos.Count == 8 && cyberstan.Count == 8 && cyberstan.All(r => r.GetProperty("owner").GetInt32() == 3 &&
              r.GetProperty("isAvailable").GetBoolean() && r.GetProperty("health").GetInt64() == regionInfos[r.GetProperty("regionIndex").GetInt32()] &&
              r.EnumerateObject().Count() == 8) && campaigns.Any(c => c.GetProperty("planetIndex").GetInt32() == 260),
            "Cyberstan's 8 regions are available, the Automatons', at full health, with its campaign.");
        check(open.GetProperty("planetRegions").GetArrayLength() == frozen.GetProperty("planetRegions").GetArrayLength() + 8,
            "The other planets' region rows stay.");
        foreach (var faction in new[] { 2, 3 })
            check(onPlanet.Where(e => owner[e.Index] == faction).Select(e => e.Id).Distinct().Count() >= 2,
                $"Faction {faction}: several subfactions across its planets.");

        // served by the backend in place of the frozen war
        await using var app = LocalBackend.Create(0, Path.Combine(root, "open-war-backend"), LoginMode.LocalSession, true, openWar: true);
        await app.StartAsync();
        try
        {
            using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { BaseAddress = new Uri(app.Urls.Single()) };
            using var response = await client.GetAsync("/WarSeason/801/Status");
            using var served = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            check(served.RootElement.GetProperty("campaigns").GetArrayLength() == planets - 1, "The backend serves the all-planets war.");
        }
        finally { await app.StopAsync(); }
    }
}
