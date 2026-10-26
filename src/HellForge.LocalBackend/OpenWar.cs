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
// Predator Strain, Jet Brigade, the Illuminate's...), each with its flag on the war table (type 71). Dealt (the launcher's
// "Deal subfactions", backend --war-subfactions, on by default), every enemy planet gets them anew, planet by planet,
// from the seed (the launcher's, backend --war-seed): half of each enemy's planets get none, the others one of that
// enemy's subfactions. Not dealt, a planet keeps the frozen war's subfactions of its new enemy and loses another enemy's.
// Key planets keep theirs either way and are not dealt any: the enemy home worlds (Cyberstan's Cyborgs) and Oshaune, the
// Terminid hive world (Rupture Strain, Dragonroach and Hive Lord).
// Either way an Illuminate planet always has one: every Illuminate squad needs an Illuminate tag (HD2Mods
// research/subfactions). SEAF allies (an effect of any enemy) are dealt the same way (--war-seaf): about one enemy planet
// in eight; not dealt, the frozen war's stay. Stable hashes of the planet and the seed, so the same on every machine of a
// lobby (each builds its missions' tags itself). In co-op the host's war decides (whether a joiner with another seed loads
// the host's subfaction enemies is to be tested).
//
// An enemy's home world (warInfo homeWorlds: Cyberstan, the Automatons') has no operations of its own from its campaign:
// the game strips its map's drop points (game.dll 0x12D88FB), and its operations come from its regions instead (the
// Megafactories: operation type 13, "LIBERATE CYBERSTAN", difficulties 4 to 10; the region generator 0x11E44D0). So each of
// its warInfo regions gets a status row: its enemy's, at full health (flags 1 with health 0 reads as taken) and available
// (HD2Mods research/cyberstan). Not tested in game.
//
// Six planets keep a mission modifier (type 71, its settings' byte +0x4c bit 2) with which the campaign builder offers
// no missions on the planet (0x11E3D5A): Meridia 1229, Ivis and Moradesh 1252, Calypso 1418, Penta 1352, Angel's
// Venture 1241 (1228 and 1230 are of the kind too, on no planet). They go, so every campaign has missions.
// Super Earth's home world stays Super Earth's, with no faction: no campaign, no enemy, no effects (none dealt, none kept).
public static class OpenWar
{
    public const string Mode = "open";
    // Campaign ids: past the war's own (about 51000 in war-data.json), one per planet.
    public const int CampaignIdBase = 900000;
    const int SuperEarth = 1, Illuminate = 4;
    // The effects that keep a planet's campaign from offering missions, which go.
    static readonly HashSet<int> NoMissions = [1228, 1229, 1230, 1241, 1252, 1352, 1418];
    // Key planets besides the enemy home worlds, which keep their frozen-war subfactions: Oshaune, the Terminid hive world.
    static readonly HashSet<int> KeyPlanets = [212];

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

    // The frozen war's planets: where each lies, its full health, which are disabled, and Super Earth's home world(s),
    // never an enemy's.
    sealed record Planets(Dictionary<int, (double X, double Y)> Position, Dictionary<int, long> MaxHealth, HashSet<int> Disabled, HashSet<int> Home);

    static Planets ReadPlanets(JsonElement warInfo)
    {
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
        return new Planets(position, maxHealth, disabled, HomeWorlds(warInfo, superEarth: true));
    }

    // The planets of Super Earth's home worlds (superEarth), or of the enemies'.
    static HashSet<int> HomeWorlds(JsonElement warInfo, bool superEarth)
    {
        var home = new HashSet<int>();
        if (warInfo.TryGetProperty("homeWorlds", out var homeWorlds))
            foreach (var h in homeWorlds.EnumerateArray())
                if ((h.GetProperty("race").GetInt32() == SuperEarth) == superEarth)
                    foreach (var i in h.GetProperty("planetIndices").EnumerateArray()) home.Add(i.GetInt32());
        return home;
    }

    public static JsonElement Status(JsonElement warInfo, JsonElement status, IReadOnlyDictionary<int, string>? sectors = null, uint seed = 0,
        bool dealSubfactions = true, bool dealSeaf = true)
    {
        sectors ??= Sectors();
        var planets = ReadPlanets(warInfo);
        var position = planets.Position;

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
        var open = owner.Keys.Where(i => !planets.Home.Contains(i) && !planets.Disabled.Contains(i)).ToList();
        var sectorEnemy = open.Where(sectors.ContainsKey).GroupBy(i => sectors[i]).ToDictionary(g => g.Key, g =>
        {
            var held = g.Where(i => owner[i] != SuperEarth).Select(i => owner[i]).ToList();
            return held.Count > 0 ? Most(held) : Most(g.Select(Front));
        });
        int EnemyOf(int index) => sectors.TryGetValue(index, out var sector) && sectorEnemy.TryGetValue(sector, out var enemy) ? enemy : owner[index] != SuperEarth ? owner[index] : Front(index);

        var changed = Liberate(node, planetStatus, planets, owner, EnemyOf);
        if (node["planetRegions"] is JsonArray regions)
            foreach (var r in regions)
                if (r?["planetIndex"] is { } at && changed.TryGetValue((int)at, out var enemy)) r["owner"] = enemy;
        // subfactions and SEAF allies: dealt planet by planet from the seed, or the frozen war's (its enemy's) kept; an
        // Illuminate planet always has one; each with its flag (the frozen war's flags go, and come back with them)
        var now = planetStatus.ToDictionary(p => (int)p!["index"]!, p => (int)p!["owner"]!);
        var deal = new Dealing(Subfactions(), now, planets, HomeWorlds(warInfo, superEarth: false), seed, dealSubfactions, dealSeaf);
        var flags = Flags();
        var effects = new JsonArray();
        var has = KeepEffects(node, deal, flags.Values.ToHashSet(), effects);
        DealEffects(effects, deal, has);
        AddFlags(effects, flags);
        node["planetActiveEffects"] = effects;
        EnemyHomeRegions(node, warInfo, now);
        using var document = JsonDocument.Parse(node.ToJsonString());
        return document.RootElement.Clone();
    }

    // Every open planet goes to its enemy and gets one liberation campaign; the frozen war's defenses, attacks and joint
    // operations go. Returns the planets given to another enemy, with it.
    static Dictionary<int, int> Liberate(JsonObject node, JsonArray planetStatus, Planets planets, Dictionary<int, int> owner, Func<int, int> enemyOf)
    {
        var changed = new Dictionary<int, int>();
        var campaigns = new JsonArray();
        foreach (var p in planetStatus.OrderBy(p => (int)p!["index"]!))
        {
            var index = (int)p!["index"]!;
            if (planets.Home.Contains(index) || planets.Disabled.Contains(index)) continue;
            var enemy = enemyOf(index);
            if (enemy != owner[index])
            {
                p["owner"] = enemy;
                // a planet Super Earth held starts unliberated; one another enemy held keeps its progress
                if (owner[index] == SuperEarth && planets.MaxHealth.TryGetValue(index, out var full)) p["health"] = full;
                changed[index] = enemy;
            }
            campaigns.Add((JsonNode)new JsonObject { ["id"] = CampaignIdBase + index, ["planetIndex"] = index, ["type"] = 0, ["count"] = 1, ["race"] = SuperEarth });
        }
        node["campaigns"] = campaigns;
        node["planetEvents"] = new JsonArray();
        node["planetAttacks"] = new JsonArray();
        node["jointOperations"] = new JsonArray();
        return changed;
    }

    // What the subfactions and SEAF allies are dealt from: the subfaction effects (by id, with their enemy; 0 any), each
    // planet's enemy now, the frozen war's planets, the enemy home worlds, the seed, and whether each kind is dealt.
    sealed record Dealing(IReadOnlyDictionary<int, int> Subfactions, Dictionary<int, int> Now, Planets Planets, HashSet<int> EnemyHomes, uint Seed,
        bool DealSubfactions, bool DealSeaf)
    {
        // key planets keep the frozen war's subfactions of their enemy, dealt or not: the enemy home worlds and KeyPlanets
        public bool Key(int index) => EnemyHomes.Contains(index) || KeyPlanets.Contains(index);
        public uint Roll(int index, string what) => StableHash(index.ToString(System.Globalization.CultureInfo.InvariantCulture) + "/" + what + "/" +
            Seed.ToString(System.Globalization.CultureInfo.InvariantCulture));
    }

    static void Add(JsonArray effects, int index, int id) => effects.Add((JsonNode)new JsonObject { ["index"] = index, ["galacticEffectId"] = id });

    // The frozen war's effects that stay, added to effects: all but the flags (they come back with their subfactions), the
    // effects that keep a campaign from offering missions, Super Earth's home world's, another enemy's subfactions and
    // those dealt anew. Returns the planets that keep a subfaction of their enemy's.
    static HashSet<int> KeepEffects(JsonObject node, Dealing deal, HashSet<int> flagIds, JsonArray effects)
    {
        var has = new HashSet<int>();
        foreach (var e in node["planetActiveEffects"] as JsonArray ?? [])
        {
            int index = (int)e!["index"]!, id = (int)e["galacticEffectId"]!;
            if (!Stays(deal, flagIds, index, id, out var subfaction)) continue;
            if (subfaction) has.Add(index);
            Add(effects, index, id);
        }
        return has;
    }

    // Whether a frozen-war effect on a planet stays (KeepEffects); subfaction: it is a subfaction of the planet's enemy.
    static bool Stays(Dealing deal, HashSet<int> flagIds, int index, int id, out bool subfaction)
    {
        subfaction = false;
        if (flagIds.Contains(id) || NoMissions.Contains(id) || deal.Planets.Home.Contains(index)) return false;
        if (!deal.Subfactions.TryGetValue(id, out var of)) return true;
        if (!deal.Now.TryGetValue(index, out var holder) || of != 0 && holder != of) return false; /* another enemy's */
        if (of == 0 ? deal.DealSeaf : deal.DealSubfactions && !deal.Key(index)) return false; /* dealt anew */
        subfaction = of != 0;
        return true;
    }

    // Each enemy's planets without one of its subfactions: dealt, half of them (by the seed's order) go without and the
    // others get one; not dealt, only an Illuminate planet without one gets one. Then the SEAF allies, when dealt: about
    // one enemy planet in eight.
    static void DealEffects(JsonArray effects, Dealing deal, HashSet<int> has)
    {
        var now = deal.Now;
        var held = now.Where(o => o.Value != SuperEarth && !deal.Planets.Home.Contains(o.Key) && !deal.Planets.Disabled.Contains(o.Key))
            .Select(o => o.Key).OrderBy(i => i).ToList();
        foreach (var enemy in held.Select(i => now[i]).Distinct().Order())
        {
            var choices = deal.Subfactions.Where(s => s.Value == enemy).Select(s => s.Key).OrderBy(id => id).ToList();
            if (choices.Count == 0) continue;
            var theirs = held.Where(i => now[i] == enemy && !has.Contains(i)).OrderBy(i => deal.Roll(i, "order")).ThenBy(i => i).ToList();
            var without = enemy == Illuminate ? 0 : !deal.DealSubfactions ? theirs.Count : theirs.Count / 2;
            foreach (var index in theirs.Skip(without)) Add(effects, index, choices[(int)(deal.Roll(index, "subfaction") % (uint)choices.Count)]);
        }
        if (!deal.DealSeaf) return;
        foreach (var seaf in deal.Subfactions.Where(s => s.Value == 0).Select(s => s.Key).Order())
            foreach (var index in held.Where(i => deal.Roll(i, "seaf/" + seaf.ToString(System.Globalization.CultureInfo.InvariantCulture)) % 8 == 0)) Add(effects, index, seaf);
    }

    // Each subfaction's flag (type 71) beside it.
    static void AddFlags(JsonArray effects, IReadOnlyDictionary<int, int> flags)
    {
        foreach (var e in effects.ToList())
            if (flags.TryGetValue((int)e!["galacticEffectId"]!, out var flag)) Add(effects, (int)e["index"]!, flag);
    }

    // an enemy home world's regions: its operations (Cyberstan's Megafactories)
    static void EnemyHomeRegions(JsonObject node, JsonElement warInfo, Dictionary<int, int> now)
    {
        var enemyHomes = HomeWorlds(warInfo, superEarth: false);
        if (enemyHomes.Count == 0 || !warInfo.TryGetProperty("planetRegions", out var regionInfos)) return;
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
}
