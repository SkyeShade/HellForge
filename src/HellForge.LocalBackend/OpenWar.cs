using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The all-planets war (the launcher's "All planets open"; backend --war open): the frozen war (war-data.json) with every
// planet but Super Earth's home world open for missions. A liberation campaign is an enemy-held planet with a campaign
// of race 1 (Super Earth attacking), type 0 (as war-data.json's own liberations are); so every planet is given to an
// enemy, the ones Super Earth held starting at 0% liberated, and every planet gets one liberation campaign.
//
// Which enemy: one per sector of the galaxy map (planet-sectors.json: war-data.json's own sector numbers are not the
// map's sectors), since the map paints a sector by its planets and a sector of two factions looked wrong (an Illuminate
// Severin around an Automaton Malevelon Creek). A sector where the frozen war has enemy planets goes to the enemy holding
// most of them; any other to the enemy whose front lies the same way from Super Earth for most of its planets (the
// enemy-held planet nearest in direction, not in distance: by distance the front nearest the centre took half the
// galaxy). The galaxy is cut into the fronts' wedges along sector borders, about a third each, the same on every machine.
// The frozen war's defenses, attacks and joint operations go: they name campaigns and events that no longer exist.
// warinfo is unchanged.
//
// Subfactions (galactic-war-effects.json): a planet's type-40 effects add the enemy tags its missions spawn from (the
// Predator Strain, Jet Brigade, the Illuminate's...). A planet keeps the frozen war's subfaction effects of its new enemy
// and loses another enemy's; each sector then gets one of its enemy's subfactions or none, picked by a stable hash of the
// sector's name (the same on every machine of a lobby, which each build their missions' tags themselves). An Illuminate
// sector always gets one: every Illuminate squad needs an Illuminate tag (HD2Mods research/subfactions). A subfaction
// seed (the launcher's, backend --war-seed) other than 0 deals every sector's anew from that seed, the frozen war's
// included: a player who dislikes the layout rolls another. In co-op the host's war decides (whether a joiner with another
// seed loads the host's subfaction enemies is to be tested: each game builds its missions' tags from its own war).
//
// An enemy's home world (warInfo homeWorlds: Cyberstan, the Automatons') has no operations of its own from its campaign:
// the game strips its map's drop points (game.dll 0x12D88FB), and its operations come from its regions instead (the
// Megafactories: operation type 13, "LIBERATE CYBERSTAN", difficulties 4 to 10; the region generator 0x11E44D0). So each of
// its warInfo regions gets a status row: its enemy's, at full health (flags 1 with health 0 reads as taken) and available
// (HD2Mods research/cyberstan). Not tested in game.
public static class OpenWar
{
    public const string Mode = "open";
    // Campaign ids: past the war's own (about 51000 in war-data.json), one per planet.
    public const int CampaignIdBase = 900000;
    const int SuperEarth = 1, Illuminate = 4;

    // The subfaction effects by id, with the enemy each belongs to (0: any enemy).
    public static IReadOnlyDictionary<int, int> Subfactions()
    {
        using var data = LocalBackend.LoadResource("galactic-war-effects.json");
        return data.RootElement.GetProperty("subfactions").EnumerateArray().ToDictionary(s => s.GetProperty("id").GetInt32(), s => s.GetProperty("faction").GetInt32());
    }

    // Each subfaction's flag effect (type 71), which goes with it on a planet: by the subfaction's id.
    public static IReadOnlyDictionary<int, int> Flags()
    {
        using var data = LocalBackend.LoadResource("galactic-war-effects.json");
        return data.RootElement.GetProperty("subfactions").EnumerateArray().Where(s => s.TryGetProperty("flag", out _))
            .ToDictionary(s => s.GetProperty("id").GetInt32(), s => s.GetProperty("flag").GetInt32());
    }

    // FNV-1a of a sector's name: string.GetHashCode differs per process.
    static uint StableHash(string text)
    {
        var hash = 2166136261u;
        foreach (var c in text) { hash ^= c; hash *= 16777619u; }
        return hash;
    }

    // The galaxy map's sector of each planet (planet-sectors.json), by planet index.
    public static IReadOnlyDictionary<int, string> Sectors()
    {
        using var data = LocalBackend.LoadResource("planet-sectors.json");
        return data.RootElement.GetProperty("sectors").EnumerateObject().ToDictionary(s => int.Parse(s.Name, System.Globalization.CultureInfo.InvariantCulture), s => s.Value.GetString()!);
    }

    public static JsonElement Status(JsonElement warInfo, JsonElement status, IReadOnlyDictionary<int, string>? sectors = null, uint seed = 0)
    {
        sectors ??= Sectors();
        var planets = warInfo.GetProperty("planetInfos");
        var position = new Dictionary<int, (double X, double Y)>();
        var maxHealth = new Dictionary<int, long>();
        var disabled = new HashSet<int>();
        foreach (var p in planets.EnumerateArray())
        {
            var index = p.GetProperty("index").GetInt32();
            var at = p.GetProperty("position");
            position[index] = (at.GetProperty("x").GetDouble(), at.GetProperty("y").GetDouble());
            maxHealth[index] = p.GetProperty("maxHealth").GetInt64();
            if (p.TryGetProperty("disabled", out var off) && off.ValueKind == JsonValueKind.True) disabled.Add(index);
        }
        var home = new HashSet<int>(); // Super Earth's home world(s): never an enemy's
        if (warInfo.TryGetProperty("homeWorlds", out var homeWorlds))
            foreach (var h in homeWorlds.EnumerateArray())
                if (h.GetProperty("race").GetInt32() == SuperEarth)
                    foreach (var i in h.GetProperty("planetIndices").EnumerateArray()) home.Add(i.GetInt32());

        var node = JsonNode.Parse(status.GetRawText())!.AsObject();
        var planetStatus = node["planetStatus"]!.AsArray();
        var owner = new Dictionary<int, int>();
        foreach (var p in planetStatus) owner[(int)p!["index"]!] = (int)p["owner"]!;
        var enemies = owner.Where(o => o.Value != SuperEarth && position.ContainsKey(o.Key)).OrderBy(o => o.Key).ToList();
        double Angle(int index) { var (x, y) = position[index]; return Math.Atan2(y, x); }
        // the enemy whose held planet lies nearest in direction from Super Earth
        int Front(int index)
        {
            if (enemies.Count == 0 || !position.ContainsKey(index)) return 2;
            var at = Angle(index);
            var best = enemies[0];
            var bestTurn = double.MaxValue;
            foreach (var e in enemies)
            {
                var turn = Math.Abs(Angle(e.Key) - at) % (2 * Math.PI);
                turn = Math.Min(turn, 2 * Math.PI - turn);
                if (turn < bestTurn) { bestTurn = turn; best = e; }
            }
            return best.Value;
        }
        static int Most(IEnumerable<int> factions) => factions.GroupBy(f => f).OrderByDescending(g => g.Count()).ThenBy(g => g.Key).First().Key;
        // one enemy per sector: the frozen war's there, else the fronts' (Super Earth's home world votes for none)
        var open = owner.Keys.Where(i => !home.Contains(i) && !disabled.Contains(i)).ToList();
        var sectorEnemy = open.Where(sectors.ContainsKey).GroupBy(i => sectors[i]).ToDictionary(g => g.Key, g =>
        {
            var held = g.Where(i => owner[i] != SuperEarth).Select(i => owner[i]).ToList();
            return held.Count > 0 ? Most(held) : Most(g.Select(Front));
        });
        int EnemyOf(int index) => sectors.TryGetValue(index, out var sector) && sectorEnemy.TryGetValue(sector, out var enemy) ? enemy : owner[index] != SuperEarth ? owner[index] : Front(index);

        var changed = new Dictionary<int, int>();
        var campaigns = new JsonArray();
        foreach (var p in planetStatus.OrderBy(p => (int)p!["index"]!))
        {
            var index = (int)p!["index"]!;
            if (home.Contains(index) || disabled.Contains(index)) continue;
            var enemy = EnemyOf(index);
            if (enemy != owner[index])
            {
                p["owner"] = enemy;
                // a planet Super Earth held starts unliberated; one another enemy held keeps its progress
                if (owner[index] == SuperEarth && maxHealth.TryGetValue(index, out var full)) p["health"] = full;
                changed[index] = enemy;
            }
            campaigns.Add((JsonNode)new JsonObject { ["id"] = CampaignIdBase + index, ["planetIndex"] = index, ["type"] = 0, ["count"] = 1, ["race"] = SuperEarth });
        }
        node["campaigns"] = campaigns;
        node["planetEvents"] = new JsonArray();
        node["planetAttacks"] = new JsonArray();
        node["jointOperations"] = new JsonArray();
        if (node["planetRegions"] is JsonArray regions)
            foreach (var r in regions)
                if (r?["planetIndex"] is { } at && changed.TryGetValue((int)at, out var enemy)) r["owner"] = enemy;
        // subfactions: the new enemy's own, else one per sector (or none), always one on an Illuminate planet; each with
        // its flag (the frozen war's flags go, and come back with the subfactions kept or dealt)
        var subfactions = Subfactions();
        var flags = Flags();
        var flagIds = flags.Values.ToHashSet();
        var now = planetStatus.ToDictionary(p => (int)p!["index"]!, p => (int)p!["owner"]!);
        var effects = new JsonArray();
        var has = new HashSet<int>();
        foreach (var e in node["planetActiveEffects"] as JsonArray ?? [])
        {
            int index = (int)e!["index"]!, id = (int)e["galacticEffectId"]!;
            if (flagIds.Contains(id)) continue;
            if (subfactions.TryGetValue(id, out var of) && of != 0)
            {
                if (seed != 0 || !now.TryGetValue(index, out var holder) || holder != of) continue; /* another enemy's, or dealt anew */
                has.Add(index);
            }
            effects.Add((JsonNode)new JsonObject { ["index"] = index, ["galacticEffectId"] = id });
        }
        foreach (var (index, holder) in now.OrderBy(o => o.Key))
        {
            if (holder == SuperEarth || has.Contains(index) || home.Contains(index) || disabled.Contains(index)) continue;
            var choices = subfactions.Where(s => s.Value == holder).Select(s => s.Key).OrderBy(id => id).ToList();
            if (choices.Count == 0) continue;
            var key = sectors.TryGetValue(index, out var sector) ? sector : "planet " + index;
            var pick = (int)(StableHash(key + "/" + holder + (seed != 0 ? "/" + seed.ToString(System.Globalization.CultureInfo.InvariantCulture) : "")) %
                (uint)(choices.Count + (holder == Illuminate ? 0 : 1)));
            if (pick < choices.Count) effects.Add((JsonNode)new JsonObject { ["index"] = index, ["galacticEffectId"] = choices[pick] });
        }
        foreach (var e in effects.ToList())
            if (flags.TryGetValue((int)e!["galacticEffectId"]!, out var flag))
                effects.Add((JsonNode)new JsonObject { ["index"] = (int)e["index"]!, ["galacticEffectId"] = flag });
        node["planetActiveEffects"] = effects;
        // an enemy home world's regions: its operations (Cyberstan's Megafactories)
        var enemyHomes = new HashSet<int>();
        if (warInfo.TryGetProperty("homeWorlds", out var worlds))
            foreach (var h in worlds.EnumerateArray())
                if (h.GetProperty("race").GetInt32() != SuperEarth)
                    foreach (var i in h.GetProperty("planetIndices").EnumerateArray()) enemyHomes.Add(i.GetInt32());
        if (enemyHomes.Count > 0 && warInfo.TryGetProperty("planetRegions", out var regionInfos))
        {
            var rows = node["planetRegions"] as JsonArray ?? [];
            var kept = new JsonArray();
            foreach (var r in rows.ToList())
                if (r?["planetIndex"] is not { } at || !enemyHomes.Contains((int)at)) { rows.Remove(r); kept.Add(r); }
            foreach (var region in regionInfos.EnumerateArray())
            {
                var index = region.GetProperty("planetIndex").GetInt32();
                if (!enemyHomes.Contains(index) || !now.TryGetValue(index, out var holder) || holder == SuperEarth) continue;
                var health = region.GetProperty("maxHealth").GetInt64();
                kept.Add((JsonNode)new JsonObject
                {
                    ["planetIndex"] = index, ["regionIndex"] = region.GetProperty("regionIndex").GetInt32(), ["owner"] = holder,
                    ["health"] = health, ["regerPerSecond"] = (float)(health / 360000.0), ["availabilityFactor"] = 1,
                    ["isAvailable"] = true, ["players"] = 0,
                });
            }
            node["planetRegions"] = kept;
        }
        using var document = JsonDocument.Parse(node.ToJsonString());
        return document.RootElement.Clone();
    }
}
