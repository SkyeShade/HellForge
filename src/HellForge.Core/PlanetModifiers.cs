using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Planet modifiers (docs/planet-modifiers.md, "What each choice does"): each planet's enemy, subfactions and enemy groups
// that surge or are left out, from the installation's planet-modifiers.json, served with whichever war is played. An
// enemy makes the planet a liberation (campaign CampaignIdBase + index), or on Super Earth's home world, which keeps its
// owner, the Battle for Super Earth against that enemy (SuperEarthBattle); subfactions are type 40 effects with their type
// 71 flags; a surge or "none" is a type 72 spawn weight per enemy family. Every Illuminate squad needs an Illuminate tag,
// so such a planet always keeps one. The catalogue's definitions are always served: the game asks for them only every 15
// minutes, so a machine that joins a lobby later must have them from its start (it asks for Status every minute).
public static class PlanetModifiers
{
    public const string FileName = "planet-modifiers.json";
    // Past the war's campaigns and the all-planets war's (900000 + index).
    public const int CampaignIdBase = 910000;
    public const int MaxEffectsPerPlanet = 32;
    public const int SuperEarth = 1, Illuminate = 4;
    // What a file may hold, so a host's cannot make a joiner's backend do much work.
    public const int MaxPlanets = 300, MaxSubfactions = 16, MaxEnemies = 64, MaxFileBytes = 256 * 1024;
    // The effects with which a campaign offers no missions (OpenWar: Meridia, Calypso, Penta...).
    public static readonly IReadOnlySet<int> NoMissions = new HashSet<int> { 1228, 1229, 1230, 1241, 1252, 1352, 1418 };
    // The subfaction an Illuminate planet gets when it would have none.
    public const string IlluminateDefault = "invasion-fleet";

    static readonly Lazy<PlanetCatalog> catalog = new(PlanetCatalog.Load);
    public static PlanetCatalog Catalog => catalog.Value;

    public static string PathFor(string hostRoot) => Path.Combine(hostRoot, FileName);
}

// Preview and Hologram: the hashes of the planet's picture (GameData.PlanetPictures) and galaxy map material
// (GameData.PlanetIcons) in the game data, 0 none.
public sealed record PlanetInfo(int Index, string Name, string Sector, string Biome, double X, double Y, long MaxHealth, int Owner, bool Home, IReadOnlyList<int> Links,
    ulong Preview, ulong Hologram)
{
    // Super Earth's home world: it keeps its owner, and an enemy given to it attacks its mega cities (SuperEarthBattle).
    public bool SuperEarthHome => Home && Owner == PlanetModifiers.SuperEarth;
}
public sealed record PlanetFaction(string Key, int Race, string Name);
public sealed record PlanetSubfaction(string Key, string Name, int Faction, int Effect, int? Flag);
public sealed record PlanetEnemyGroup(string Key, string Name, int Faction, IReadOnlyList<uint> Families, IReadOnlyList<int> Surge, IReadOnlyList<int> None, int? Card);

// The planets (data/planets.json) and what they can be given (data/planet-modifiers.json). Planets are the galaxy map's:
// the file's "unused" ones (placeholders of the war data on no sector of the map, at Super Earth's spot: 264 NEW
// INSIGHT, 265 WAYWARD) are in Unused instead, so no map, planet list or planet modifier offers them.
public sealed class PlanetCatalog
{
    public required IReadOnlyList<PlanetInfo> Planets { get; init; }
    public required IReadOnlySet<int> Unused { get; init; }
    public required IReadOnlyList<PlanetFaction> Factions { get; init; }
    public required IReadOnlyList<PlanetSubfaction> Subfactions { get; init; }
    public required IReadOnlyList<PlanetEnemyGroup> Enemies { get; init; }
    // The effect definitions, as /WarSeason/GalacticWarEffects serves them.
    public required string EffectsJson { get; init; }

    public PlanetInfo? Planet(int index) => Planets.FirstOrDefault(p => p.Index == index);
    public PlanetFaction? Faction(string? key) => Factions.FirstOrDefault(f => f.Key == key);
    public PlanetFaction? FactionOf(int race) => Factions.FirstOrDefault(f => f.Race == race);
    public PlanetSubfaction? Subfaction(string key) => Subfactions.FirstOrDefault(s => s.Key == key);
    public PlanetEnemyGroup? Enemy(string key) => Enemies.FirstOrDefault(e => e.Key == key);

    // A planet by its index or name ("203", "Menkent", any case), or null.
    public PlanetInfo? Find(string text)
    {
        text = text.Trim();
        if (int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var index)) return Planet(index);
        return Planets.FirstOrDefault(p => string.Equals(p.Name, text, StringComparison.OrdinalIgnoreCase));
    }

    internal static PlanetCatalog Load()
    {
        using var planets = Resource("HellForge.Core.planets.json");
        using var modifiers = Resource("HellForge.Core.planet-modifiers.json");
        var root = modifiers.RootElement;
        static IReadOnlyList<int> Ints(JsonElement e, string name) => e.TryGetProperty(name, out var a) ? [.. a.EnumerateArray().Select(i => i.GetInt32())] : [];
        static int? Maybe(JsonElement e, string name) => e.TryGetProperty(name, out var v) ? v.GetInt32() : null;
        static ulong Hex(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && ulong.TryParse(v.GetString(), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var hash) ? hash : 0;
        static bool IsUnused(JsonElement p) => p.TryGetProperty("unused", out var u) && u.ValueKind == JsonValueKind.True;
        var all = planets.RootElement.GetProperty("planets").EnumerateArray().ToList();
        return new PlanetCatalog
        {
            Unused = all.Where(IsUnused).Select(p => p.GetProperty("index").GetInt32()).ToHashSet(),
            Planets = [.. all.Where(p => !IsUnused(p)).Select(p => new PlanetInfo(p.GetProperty("index").GetInt32(),
                p.GetProperty("name").GetString()!, p.GetProperty("sector").GetString()!, p.GetProperty("biome").GetString()!, p.GetProperty("x").GetDouble(),
                p.GetProperty("y").GetDouble(), p.GetProperty("maxHealth").GetInt64(), p.GetProperty("owner").GetInt32(), p.GetProperty("home").GetBoolean(),
                Ints(p, "links"), Hex(p, "preview"), Hex(p, "hologram")))],
            Factions = [.. root.GetProperty("factions").EnumerateArray().Select(f => new PlanetFaction(f.GetProperty("key").GetString()!, f.GetProperty("race").GetInt32(),
                f.GetProperty("name").GetString()!))],
            Subfactions = [.. root.GetProperty("subfactions").EnumerateArray().Select(s => new PlanetSubfaction(s.GetProperty("key").GetString()!,
                s.GetProperty("name").GetString()!, s.GetProperty("faction").GetInt32(), s.GetProperty("effect").GetInt32(), Maybe(s, "flag")))],
            Enemies = [.. root.GetProperty("enemies").EnumerateArray().Select(g => new PlanetEnemyGroup(g.GetProperty("key").GetString()!,
                g.GetProperty("name").GetString()!, g.GetProperty("faction").GetInt32(), [.. g.GetProperty("families").EnumerateArray().Select(f => f.GetUInt32())],
                Ints(g, "surge"), Ints(g, "none"), Maybe(g, "card")))],
            EffectsJson = root.GetProperty("effects").GetRawText(),
        };
    }

    static JsonDocument Resource(string name)
    {
        using var stream = typeof(PlanetCatalog).Assembly.GetManifestResourceStream(name) ?? throw new InvalidOperationException("Missing resource " + name);
        return JsonDocument.Parse(stream);
    }
}

// One planet's choices. Faction: a PlanetFaction key, null the war's. Subfactions: keys, null the war's, empty none.
// Enemies: a group's key and "surge" or "none" (a group left out plays as the war has it).
public sealed record PlanetOverride(int Index, string? Faction, IReadOnlyList<string>? Subfactions, IReadOnlyDictionary<string, string> Enemies)
{
    public const string Surge = "surge", None = "none";
    public bool IsEmpty => Faction == null && Subfactions == null && Enemies.Count == 0;
}

// The planet modifiers of one file (planet-modifiers.json):
//   {"planets": [{"index": 203, "faction": "automatons", "subfactions": ["jet-brigade"], "enemies": {"hulks": "surge"}}]}
// Read leniently: an entry or key the catalogue does not know is left out, with a problem saying which.
public sealed class PlanetPlan
{
    public static readonly PlanetPlan Empty = new([]);
    public IReadOnlyList<PlanetOverride> Planets { get; }

    public PlanetPlan(IEnumerable<PlanetOverride> planets) =>
        Planets = [.. planets.Where(p => !p.IsEmpty).GroupBy(p => p.Index).Select(g => g.Last()).OrderBy(p => p.Index)];

    public PlanetOverride? For(int index) => Planets.FirstOrDefault(p => p.Index == index);

    public PlanetPlan With(PlanetOverride planet) => new(Planets.Where(p => p.Index != planet.Index).Append(planet));
    public PlanetPlan Without(int index) => new(Planets.Where(p => p.Index != index));

    // The file, or Empty when there is none; a file that does not parse throws (JsonException).
    public static PlanetPlan Read(string path, out IReadOnlyList<string> problems)
    {
        problems = [];
        if (!File.Exists(path)) return Empty;
        if (new FileInfo(path).Length > PlanetModifiers.MaxFileBytes) throw new JsonException($"the file is over {PlanetModifiers.MaxFileBytes / 1024} KB");
        return Parse(File.ReadAllText(path), out problems);
    }

    public static PlanetPlan Parse(string json, out IReadOnlyList<string> problems)
    {
        var found = new List<string>();
        problems = found;
        if (string.IsNullOrWhiteSpace(json)) return Empty;
        if (json.Length > PlanetModifiers.MaxFileBytes) throw new JsonException($"the file is over {PlanetModifiers.MaxFileBytes / 1024} KB");
        using var document = JsonDocument.Parse(json, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new JsonException("the file is not a JSON object");
        if (!root.TryGetProperty("planets", out var list)) return Empty;
        if (list.ValueKind != JsonValueKind.Array) throw new JsonException("\"planets\" is not a list");
        var planets = new List<PlanetOverride>();
        foreach (var entry in list.EnumerateArray())
        {
            if (planets.Count == PlanetModifiers.MaxPlanets) { found.Add($"more than {PlanetModifiers.MaxPlanets} planets: the rest are left out"); break; }
            if (Entry(entry, found) is { } planet) planets.Add(planet);
        }
        return new PlanetPlan(planets);
    }

    // One entry of the file, or null when it is left out (found says why).
    static PlanetOverride? Entry(JsonElement entry, List<string> found)
    {
        if (entry.ValueKind != JsonValueKind.Object || !entry.TryGetProperty("index", out var at) || !at.TryGetInt32(out var index))
        {
            found.Add("an entry without a planet index is left out");
            return null;
        }
        if (PlanetModifiers.Catalog.Planet(index) is not { } planet) { found.Add($"planet {index} is not in the galaxy: left out"); return null; }
        return new PlanetOverride(index, EntryFaction(entry, planet, found), EntrySubfactions(entry, planet, found), EntryEnemies(entry, planet, found));
    }

    static string? EntryFaction(JsonElement entry, PlanetInfo planet, List<string> found)
    {
        if (!entry.TryGetProperty("faction", out var f) || f.ValueKind != JsonValueKind.String || f.GetString() is not { Length: > 0 } key || key == "war") return null;
        if (PlanetModifiers.Catalog.Faction(key) == null) found.Add($"{planet.Name}: no faction \"{key}\"");
        else if (planet.Home && !planet.SuperEarthHome) found.Add($"{planet.Name} is a home world and keeps its owner");
        else return key;
        return null;
    }

    static List<string>? EntrySubfactions(JsonElement entry, PlanetInfo planet, List<string> found)
    {
        if (!entry.TryGetProperty("subfactions", out var list) || list.ValueKind != JsonValueKind.Array) return null;
        var subfactions = new List<string>();
        foreach (var item in list.EnumerateArray().Take(PlanetModifiers.MaxSubfactions))
            if (item.ValueKind != JsonValueKind.String || item.GetString() is not { } key || PlanetModifiers.Catalog.Subfaction(key) == null)
                found.Add($"{planet.Name}: no subfaction \"{item}\"");
            else if (!subfactions.Contains(key)) subfactions.Add(key);
        return subfactions;
    }

    static Dictionary<string, string> EntryEnemies(JsonElement entry, PlanetInfo planet, List<string> found)
    {
        var enemies = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!entry.TryGetProperty("enemies", out var groups) || groups.ValueKind != JsonValueKind.Object) return enemies;
        foreach (var group in groups.EnumerateObject().Take(PlanetModifiers.MaxEnemies))
        {
            var mode = group.Value.ValueKind == JsonValueKind.String ? group.Value.GetString() : null;
            if (PlanetModifiers.Catalog.Enemy(group.Name) == null) found.Add($"{planet.Name}: no enemy group \"{group.Name}\"");
            else if (mode is PlanetOverride.Surge or PlanetOverride.None) enemies[group.Name] = mode;
            else if (mode is not ("normal" or "war")) found.Add($"{planet.Name}: {group.Name} takes \"surge\", \"none\" or \"normal\"");
        }
        return enemies;
    }

    public string ToJson()
    {
        var planets = new JsonArray();
        foreach (var p in Planets)
        {
            var entry = new JsonObject { ["index"] = p.Index };
            if (PlanetModifiers.Catalog.Planet(p.Index) is { } info) entry["name"] = info.Name; /* for people reading the file; not read */
            if (p.Faction != null) entry["faction"] = p.Faction;
            if (p.Subfactions != null) entry["subfactions"] = new JsonArray([.. p.Subfactions.Select(s => (JsonNode?)s)]);
            if (p.Enemies.Count > 0)
            {
                var enemies = new JsonObject();
                foreach (var (key, mode) in p.Enemies.OrderBy(e => e.Key, StringComparer.Ordinal)) enemies[key] = mode;
                entry["enemies"] = enemies;
            }
            planets.Add((JsonNode)entry);
        }
        return new JsonObject { ["planets"] = planets }.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n";
    }

    // Written whole, beside it first, so a reader never sees half a file.
    public void Write(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var partial = path + ".partial";
        File.WriteAllText(partial, ToJson());
        File.Move(partial, path, overwrite: true);
    }

    // The war's effect definitions with the catalogue's (an id already there is replaced by the catalogue's).
    public static JsonNode Effects(JsonNode effects)
    {
        var catalog = JsonNode.Parse(PlanetModifiers.Catalog.EffectsJson)!.AsArray();
        var ids = catalog.Select(e => (int)e!["id"]!).ToHashSet();
        var merged = new JsonArray();
        foreach (var e in effects as JsonArray ?? [])
            if (e?["id"] is not { } id || id.GetValueKind() != JsonValueKind.Number || !ids.Contains((int)id)) merged.Add(e?.DeepClone());
        foreach (var e in catalog.ToList()) { catalog.Remove(e); merged.Add(e); }
        return merged;
    }

    // The war's Status with these planets' choices (see PlanetModifiers). warInfo gives a planet's full health (the
    // catalogue's frozen-war value without it). log: what could not be done.
    public JsonNode Status(JsonNode status, JsonElement? warInfo = null, Action<string>? log = null)
    {
        if (Planets.Count == 0 || status is not JsonObject node || node["planetStatus"] is not JsonArray planetStatus) return status;
        var fullHealth = FullHealth(warInfo);
        var rows = planetStatus.OfType<JsonObject>().Where(p => Int(p["index"]) != null).GroupBy(p => Int(p["index"])!.Value).ToDictionary(g => g.Key, g => g.Last());
        var effects = node["planetActiveEffects"] as JsonArray ?? (JsonArray)(node["planetActiveEffects"] = new JsonArray());
        foreach (var o in Planets)
        {
            if (!rows.TryGetValue(o.Index, out var row)) { log?.Invoke($"Planet {o.Index} is not in this war: its modifiers wait."); continue; }
            var name = PlanetModifiers.Catalog.Planet(o.Index)?.Name ?? o.Index.ToString(CultureInfo.InvariantCulture);
            var changed = Owner(node, row, o, fullHealth, warInfo);
            var owner = Int(row["owner"]) ?? SuperEarth;
            // whom the planet's missions fight: its owner, or the attacker of Super Earth's planet
            var enemy = owner != SuperEarth ? owner : Attacker(node, o.Index) ?? 0;
            if (o.Subfactions != null || changed)
                Remove(effects, e => Int(e["index"]) == o.Index && Int(e["galacticEffectId"]) is { } id && SubfactionOf.TryGetValue(id, out var of) &&
                    (o.Subfactions != null || of != 0 && of != enemy));
            AddEffects(effects, o.Index, name, Additions(o, enemy, name, log), enemy, log);
        }
        return node;
    }

    // The planet given to its chosen enemy, as a liberation (Super Earth's home world: attacked by it, SuperEarthBattle);
    // true when its owner (or attacker) changed.
    static bool Owner(JsonObject node, JsonObject row, PlanetOverride o, Dictionary<int, long> fullHealth, JsonElement? warInfo)
    {
        if (o.Faction == null || PlanetModifiers.Catalog.Faction(o.Faction) is not { } faction) return false;
        bool changed;
        if (PlanetModifiers.Catalog.Planet(o.Index) is { SuperEarthHome: true })
        {
            changed = Attacker(node, o.Index) != faction.Race;
            SuperEarthBattle.Serve(node, o.Index, faction.Race, PlanetModifiers.CampaignIdBase + o.Index, warInfo);
        }
        else
        {
            changed = Reown(node, row, o.Index, faction.Race, fullHealth);
            Liberation(node, o.Index, rebuild: changed);
        }
        if (node["planetActiveEffects"] is JsonArray effects)
            Remove(effects, e => Int(e["index"]) == o.Index && Int(e["galacticEffectId"]) is { } id && PlanetModifiers.NoMissions.Contains(id));
        return changed;
    }

    // The planet and its regions given to the race (one Super Earth held starts unliberated); true when its owner changed.
    static bool Reown(JsonObject node, JsonObject row, int index, int race, Dictionary<int, long> fullHealth)
    {
        var owner = Int(row["owner"]) ?? SuperEarth;
        if (owner == race) return false;
        if (owner == SuperEarth) row["health"] = fullHealth.TryGetValue(index, out var full) ? full : PlanetModifiers.Catalog.Planet(index)?.MaxHealth ?? 1000000;
        row["owner"] = race;
        foreach (var region in (node["planetRegions"] as JsonArray ?? []).OfType<JsonObject>().Where(r => Int(r["planetIndex"]) == index))
            region["owner"] = race;
        return true;
    }

    // The effects the planet's choices add: its enemy's subfactions with their flags, and its enemy's groups.
    static List<int> Additions(PlanetOverride o, int enemy, string name, Action<string>? log)
    {
        var catalog = PlanetModifiers.Catalog;
        var adding = new List<int>();
        foreach (var key in o.Subfactions ?? [])
            if (catalog.Subfaction(key) is { } s && (s.Faction == 0 || s.Faction == enemy))
                adding.AddRange(s.Flag is { } flag ? [s.Effect, flag] : [s.Effect]);
            else log?.Invoke($"{name}: {key} is not a subfaction of its enemy; left out.");
        foreach (var (key, mode) in o.Enemies)
            if (catalog.Enemy(key) is { } group && group.Faction == enemy)
                adding.AddRange(mode != PlanetOverride.Surge ? group.None : group.Card is { } card ? [.. group.Surge, card] : group.Surge);
            else log?.Invoke($"{name}: {key} are not its enemy's; left out.");
        return adding;
    }

    // The additions the planet does not have yet, within its 32, and an Illuminate tag when an Illuminate planet has none.
    static void AddEffects(JsonArray effects, int index, string name, List<int> adding, int enemy, Action<string>? log)
    {
        var has = effects.OfType<JsonObject>().Where(e => Int(e["index"]) == index).Select(e => Int(e["galacticEffectId"]) ?? 0).ToHashSet();
        if (enemy == Illuminate && !has.Concat(adding).Any(id => SubfactionOf.TryGetValue(id, out var of) && of == Illuminate) &&
            PlanetModifiers.Catalog.Subfaction(PlanetModifiers.IlluminateDefault) is { } fallback)
            adding.Add(fallback.Effect);
        var room = PlanetModifiers.MaxEffectsPerPlanet - has.Count;
        foreach (var id in adding.Distinct().Where(id => !has.Contains(id)))
        {
            if (room-- <= 0) { log?.Invoke($"{name}: more than {PlanetModifiers.MaxEffectsPerPlanet} effects; the rest are left out."); break; }
            effects.Add((JsonNode)new JsonObject { ["index"] = index, ["galacticEffectId"] = id });
        }
    }

    // Each subfaction effect and flag, with its enemy (0 any).
    static readonly Lazy<Dictionary<int, int>> subfactionOf = new(() =>
    {
        var of = new Dictionary<int, int>();
        foreach (var s in PlanetModifiers.Catalog.Subfactions)
        {
            of[s.Effect] = s.Faction;
            if (s.Flag is { } flag) of[flag] = s.Faction;
        }
        return of;
    });
    static Dictionary<int, int> SubfactionOf => subfactionOf.Value;

    // One liberation campaign on the planet (type 0, race 1): kept when it is the planet's only one and nothing else runs
    // there, else made anew, its defense, attacks and joint operations gone.
    static void Liberation(JsonObject node, int index, bool rebuild)
    {
        var campaigns = node["campaigns"] as JsonArray ?? (JsonArray)(node["campaigns"] = new JsonArray());
        var mine = campaigns.OfType<JsonObject>().Where(c => Int(c["planetIndex"]) == index).ToList();
        var eventsHere = (node["planetEvents"] as JsonArray)?.OfType<JsonObject>().Any(e => Int(e["planetIndex"]) == index) == true;
        if (!rebuild && !eventsHere && mine.Count == 1 && Int(mine[0]["type"]) == 0) return;
        SuperEarthBattle.ClearPlanet(node, index, campaigns: true);
        campaigns.Add((JsonNode)new JsonObject { ["id"] = PlanetModifiers.CampaignIdBase + index, ["planetIndex"] = index, ["type"] = 0, ["count"] = 1, ["race"] = SuperEarth });
    }

    // The enemy attacking Super Earth's planet: an event's (a defense), else a campaign's (the Battle for Super Earth's), as
    // the game takes it (SuperEarthBattle); or null.
    static int? Attacker(JsonObject node, int index) =>
        new[] { "planetEvents", "campaigns" }.SelectMany(list => (node[list] as JsonArray ?? []).OfType<JsonObject>())
            .Where(e => Int(e["planetIndex"]) == index).Select(e => Int(e["race"])).FirstOrDefault(r => r is > SuperEarth);

    static Dictionary<int, long> FullHealth(JsonElement? warInfo)
    {
        var full = new Dictionary<int, long>();
        if (warInfo is { ValueKind: JsonValueKind.Object } info && info.TryGetProperty("planetInfos", out var planets) && planets.ValueKind == JsonValueKind.Array)
            foreach (var p in planets.EnumerateArray())
                if (p.TryGetProperty("index", out var i) && i.TryGetInt32(out var index) && p.TryGetProperty("maxHealth", out var h) && h.TryGetInt64(out var health))
                    full[index] = health;
        return full;
    }

    static void Remove(JsonArray list, Func<JsonObject, bool> match)
    {
        foreach (var item in list.OfType<JsonObject>().Where(match).ToList()) list.Remove(item);
    }

    static int? Int(JsonNode? value) => value is JsonValue v && v.GetValueKind() == JsonValueKind.Number && v.TryGetValue<int>(out var i) ? i : null;

    const int SuperEarth = PlanetModifiers.SuperEarth, Illuminate = PlanetModifiers.Illuminate;
}
