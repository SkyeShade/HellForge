namespace HellForge.Core;

// The Democracy Space Station's effects a session can put on every planet (Settings > Galactic war, "DSS effects on
// every planet"; HellForge.Cli --dss-everywhere and --dss-effects; backend --dss-everywhere <keys>). Each is picked by its
// key and is one or more galactic effect ids, as the official station gives them for Steam build 25480438 (its tactical
// actions' effectIds and its own activeEffectIds): a tactical action has the mission's effect, its map marker and the
// war's. The game knows what an id does only from its definition, which the backend serves with them (dss-effects.json).
public static class DssEffects
{
    public sealed record Effect(string Key, string Name, string Description, IReadOnlyList<int> Ids);

    // The effects the DSS gives at the moment (the war status's spaceStations activeEffectIds), whatever they are.
    public const string Current = "current";

    public static readonly IReadOnlyList<Effect> All =
    [
        // the three tactical actions
        new("eagle-storm", "Eagle Storm", "Eagle Gas Strikes during missions; enemy progress in defense campaigns is slowed. Its map marker on every planet.", [1425, 1212, 1216]),
        new("orbital-blockade", "Orbital Blockade", "The Hellpod Space Optimization booster is active in every mission; no defense campaigns start. Its map marker on every planet.", [1210, 1213, 1215]),
        new("heavy-ordnance-distribution", "Heavy Ordnance Distribution", "The Orbital 380mm HE Barrage stratagem in every loadout; liberation progress is accelerated by 150%.", [1237, 1214]),
        // the station's own effects (its activeEffectIds; the exosuits are the item mixIds in the effects' values)
        new("operational-support", "Operational Support", "A slight boost to liberation progress.", [1238]),
        new("exosuit-reserves-patriot", "Exosuit Reserves: EXO-45 Patriot", "-35% cooldown on the EXO-45 Patriot Exosuit.", [1261]),
        new("exosuit-reserves-emancipator", "Exosuit Reserves: EXO-49 Emancipator", "-35% cooldown on the EXO-49 Emancipator Exosuit.", [1262]),
        new("exosuit-reserves-lumberer", "Exosuit Reserves: EXO-51 Lumberer", "-35% cooldown on the EXO-51 Lumberer Exosuit.", [1384]),
        new("exosuit-reserves-breakthrough", "Exosuit Reserves: EXO-55 Breakthrough", "-35% cooldown on the EXO-55 Breakthrough Exosuit.", [1385]),
        new(Current, "Whatever the DSS gives now", "The effects the DSS has at the moment, including any not listed here (those work only if the game knows them).", []),
    ];

    public static IReadOnlyList<string> Keys { get; } = All.Select(e => e.Key).ToList();

    // A comma-separated list of keys, in the list's order and without repeats; "all" is every key and "" none.
    public static List<string> Parse(string text)
    {
        var given = text.Trim() == "all" ? [.. Keys]
            : text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        foreach (var key in given)
            if (!Keys.Contains(key)) throw new ArgumentException($"Unknown DSS effect \"{key}\"; the effects are all, or some of: {string.Join(", ", Keys)}.");
        return Keys.Where(given.Contains).ToList();
    }

    public static string Text(IEnumerable<string> keys) => string.Join(",", keys);

    // The galactic effect ids of the picked effects (Current adds none: the backend reads those from the war status).
    public static List<int> Ids(IEnumerable<string> keys) => All.Where(e => keys.Contains(e.Key)).SelectMany(e => e.Ids).Distinct().ToList();
}
