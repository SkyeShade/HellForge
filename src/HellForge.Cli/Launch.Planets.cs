using HellForge.Core;

// The launcher's Planets page for other launchers and scripts (docs/planet-modifiers.md).
static partial class Launch
{
    // planets: the installation's planet modifiers (docs/planet-modifiers.md), as the launcher's Planets page keeps them.
    // With --planet, that planet's choices change first: --faction (a faction's key, or war), --subfactions (keys, war or
    // none), --enemies (group=surge|none|normal,...) and --reset yes; --reset-all yes clears every planet. Every value is
    // checked before anything is written. --catalogue yes lists the keys. A running session reads the file within a minute.
    public static object PlanetsCommand(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var file = PlanetModifiers.PathFor(paths.HostRoot);
        var catalog = PlanetModifiers.Catalog;
        var plan = PlanetPlan.Read(file, out var problems);
        var changing = new[] { "--faction", "--subfactions", "--enemies", "--reset" }.Where(options.ContainsKey).ToList();
        if (changing.Count > 0 && !options.ContainsKey("--planet")) throw new ArgumentException(string.Join(", ", changing) + " needs --planet.");
        if (options.TryGetValue("--reset-all", out var resetAll))
        {
            if (resetAll != "yes") throw new ArgumentException("--reset-all takes yes.");
            if (options.ContainsKey("--planet")) throw new ArgumentException("Give --planet or --reset-all, not both.");
            plan = PlanetPlan.Empty;
            plan.Write(file);
        }
        if (options.TryGetValue("--planet", out var which))
        {
            var planet = catalog.Find(which) ?? throw new ArgumentException($"No planet {which}: give its index or its name.");
            var choice = Changed(plan.For(planet.Index) ?? new PlanetOverride(planet.Index, null, null, new Dictionary<string, string>()), planet, options, changing.Count);
            plan = choice.IsEmpty ? plan.Without(planet.Index) : plan.With(choice);
            plan.Write(file);
            problems = [];
        }
        var (views, source, active) = PlanetWarView.Read(paths, plan);
        return new
        {
            file,
            war = source,
            // whether the next session plays with them: only the Custom planets war does
            active,
            problems,
            sessionRunning = SessionRunning(),
            planets = plan.Planets.Select(p => new
            {
                p.Index, name = catalog.Planet(p.Index)?.Name, faction = p.Faction ?? "war", subfactions = (object?)p.Subfactions ?? "war", enemies = p.Enemies,
                served = views.GetValueOrDefault(p.Index) is { } v ? new
                {
                    owner = catalog.FactionOf(v.Owner)?.Key ?? "super-earth", enemy = catalog.FactionOf(v.Enemy)?.Key, v.Playable, v.Subfactions, v.Surges, left = v.Left,
                } : null,
            }),
            catalogue = options.GetValueOrDefault("--catalogue") == "yes" ? new
            {
                factions = catalog.Factions.Select(f => f.Key),
                subfactions = catalog.Subfactions.Select(s => new { s.Key, s.Name, faction = catalog.FactionOf(s.Faction)?.Key ?? "any" }),
                enemies = catalog.Enemies.Select(e => new { e.Key, e.Name, faction = catalog.FactionOf(e.Faction)?.Key }),
            } : null,
        };
    }

    // A planet's choices with --reset, --faction, --subfactions and --enemies applied, each checked.
    static PlanetOverride Changed(PlanetOverride choice, PlanetInfo planet, Dictionary<string, string> options, int changing)
    {
        var catalog = PlanetModifiers.Catalog;
        if (options.TryGetValue("--reset", out var reset))
        {
            if (reset != "yes") throw new ArgumentException("--reset takes yes.");
            if (changing > 1) throw new ArgumentException("--reset takes no other change.");
            choice = choice with { Faction = null, Subfactions = null, Enemies = new Dictionary<string, string>() };
        }
        if (options.TryGetValue("--faction", out var faction)) choice = choice with { Faction = Faction(faction, planet) };
        if (options.TryGetValue("--subfactions", out var subfactions)) choice = choice with { Subfactions = Subfactions(subfactions) };
        if (options.TryGetValue("--enemies", out var enemies)) choice = choice with { Enemies = Enemies(enemies, choice.Enemies) };
        return choice;
    }

    // --faction: a faction's key, or war (null). Super Earth's home world keeps its owner: the faction attacks it.
    static string? Faction(string text, PlanetInfo planet)
    {
        if (text == "war") return null;
        var catalog = PlanetModifiers.Catalog;
        if (catalog.Faction(text) == null) throw new ArgumentException($"--faction takes {string.Join(", ", catalog.Factions.Select(f => f.Key))} or war.");
        if (planet.Home && !planet.SuperEarthHome) throw new ArgumentException($"{planet.Name} is a home world and keeps its owner.");
        return text;
    }

    // --subfactions: keys, war (null) or none (empty).
    static List<string>? Subfactions(string text)
    {
        if (text == "war") return null;
        if (text == "none") return [];
        var catalog = PlanetModifiers.Catalog;
        return [.. text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(key => catalog.Subfaction(key) != null ? key
            : throw new ArgumentException($"No subfaction {key}; there are: {string.Join(", ", catalog.Subfactions.Select(s => s.Key))}."))];
    }

    // --enemies group=surge|none|normal,... over the planet's groups as they are.
    static Dictionary<string, string> Enemies(string text, IReadOnlyDictionary<string, string> now)
    {
        var catalog = PlanetModifiers.Catalog;
        var set = new Dictionary<string, string>(now);
        foreach (var pair in text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var parts = pair.Split('=', 2, StringSplitOptions.TrimEntries);
            if (parts.Length != 2 || catalog.Enemy(parts[0]) == null)
                throw new ArgumentException($"--enemies takes group=surge|none|normal; the groups are: {string.Join(", ", catalog.Enemies.Select(e => e.Key))}.");
            if (parts[1] is PlanetOverride.Surge or PlanetOverride.None) set[parts[0]] = parts[1];
            else if (parts[1] == "normal") set.Remove(parts[0]);
            else throw new ArgumentException($"{parts[0]} takes surge, none or normal.");
        }
        return set;
    }
}
