namespace HellForge.Core;

// One gameplay mod as friends compare it (ModCatalog.Modlist): its id, name, version and a hash of its part of the
// modlist digest (its version and files).
public sealed record ModFingerprint(string Id, string Name, string Version, string Hash);

// What makes two modlists differ, mod by mod, for a refused join: the host's backend compares its gameplay mods with the
// joiner's and both games say which differ (docs/mods.md, "Playing with friends"). On the wire a modlist is a line per
// mod, "id \t name \t version \t hash".
public static class ModlistDiff
{
    const int MaxMods = 512;

    public static string Encode(IEnumerable<ModFingerprint> mods) =>
        string.Join('\n', mods.Select(m => string.Join('\t', Clean(m.Id), Clean(m.Name), Clean(m.Version), Clean(m.Hash))));

    // A friend's modlist; malformed lines are skipped.
    public static List<ModFingerprint> Decode(string text) =>
        [.. text.Split('\n').Select(line => line.Split('\t')).Where(f => f.Length == 4 && f[0].Length > 0).Take(MaxMods)
            .Select(f => new ModFingerprint(f[0], f[1].Length > 0 ? f[1] : f[0], f[2], f[3]))];

    // A line for each mod that differs, as one player reads it: mine are this player's mods, theirs the other's.
    // asHost: this player is the host and the other a friend who tried to join; else this player is that friend.
    public static List<string> Describe(IReadOnlyList<ModFingerprint> mine, IReadOnlyList<ModFingerprint> theirs, bool asHost)
    {
        var their = theirs.GroupBy(m => m.Id, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);
        var own = mine.GroupBy(m => m.Id, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);
        var lines = new List<(string Name, string Line)>();
        foreach (var m in own.Values)
            if (Difference(m, their.GetValueOrDefault(m.Id), asHost) is { } line) lines.Add((m.Name, line));
        foreach (var t in their.Values.Where(t => !own.ContainsKey(t.Id)))
            lines.Add((t.Name, asHost ? $"They have {Label(t)}, which you don't" : $"Missing: {Label(t)} (the host has it)"));
        return [.. lines.OrderBy(l => l.Name, StringComparer.OrdinalIgnoreCase).Select(l => l.Line)];
    }

    // The line for one of this player's mods against the other's copy of it (null: they have none), or null when the two
    // are the same.
    static string? Difference(ModFingerprint m, ModFingerprint? t, bool asHost)
    {
        if (t == null) return asHost ? $"They don't have {Label(m)}" : $"Not on the host: {Label(m)}";
        if (t.Version != m.Version)
            return asHost ? $"{m.Name}: they have {t.Version}, you have {m.Version}" : $"{m.Name}: you have {m.Version}, the host has {t.Version}";
        if (t.Hash != m.Hash) return asHost ? $"{Label(m)}: their files differ from yours" : $"{Label(m)}: your files differ from the host's";
        return null;
    }

    static string Label(ModFingerprint mod) => mod.Name + " " + mod.Version;

    static string Clean(string text) => string.Concat(text.Where(c => !char.IsControl(c)).Take(80));
}
