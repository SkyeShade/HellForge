namespace HellForge.Core;

// The Democracy Space Station's effects a session can put on every planet (Settings > Galactic war, "DSS effects on
// every planet"; HellForge.Cli --dss-everywhere and --dss-effects; backend --dss-everywhere <keys>). Each is picked by its
// key and is one or more galactic effect ids (names from the community's list, github.com/helldivers-2/json
// effects/planetEffects.json): a tactical action has the mission's effect, its map marker and the war's.
public static class DssEffects
{
    public sealed record Effect(string Key, string Name, string Description, IReadOnlyList<int> Ids);

    // The effects the DSS gives at the moment (the war status's spaceStations activeEffectIds), whatever they are.
    public const string Current = "current";

    public static readonly IReadOnlyList<Effect> All =
    [
        new("eagle-storm", "Eagle Storm", "Eagle Strafing Runs during missions; enemy progress in defense campaigns is slowed.", [1209, 1212, 1216]),
        new("orbital-blockade", "Orbital Blockade", "The Hellpod Space Optimization booster is active in every mission; no defense campaigns start.", [1210, 1213, 1215]),
        new("planetary-bombardment", "Planetary Bombardment", "Nonstop orbital bombardments; liberation progress is accelerated by 150%.", [1211, 1214]),
        new("operational-support", "Operational Support", "A slight boost to liberation progress.", [1238]),
        new("exosuit-reserves-patriot", "Exosuit Reserves: EXO-45 Patriot", "-35% cooldown on the EXO-45 Patriot Exosuit.", [1261]),
        new("exosuit-reserves-emancipator", "Exosuit Reserves: EXO-49 Emancipator", "-35% cooldown on the EXO-49 Emancipator Exosuit.", [1262]),
        // the DSS's in the frozen war and the live war of 26 October 2026; not named in the community's list yet
        new("effect-1384", "Effect 1384", "A DSS effect not yet named in the community's list.", [1384]),
        new("effect-1385", "Effect 1385", "A DSS effect not yet named in the community's list.", [1385]),
        new(Current, "Whatever the DSS gives now", "The effects the DSS has at the moment, including any not listed here.", []),
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
