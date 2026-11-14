namespace HellForge.Core;

// Several mods in one call: HellForge.Cli's mod-remove and mod-switch with --ids, and mod-add with --file given more than
// once (docs/external-launchers.md). The mods folder is read once for the whole list, not once per mod, so a launcher
// sending 30 mods waits for one read. Every id is checked before anything changes: one unknown or refused id refuses
// the whole list, and nothing is touched.
public static class ModBatch
{
    public sealed record Outcome(string Id, string? Path, string? Error);

    // The player's own mods; the ones HellForge ships come back by themselves, so they are switched off instead.
    public static bool Removable(ModManifest mod) => mod.Id != ModCatalog.LocalStatsId && BundledMods.ShippedFiles(mod.Folder) == null;

    // "a, b,c" as ids, in order; an empty list or an id named twice is refused.
    public static List<string> Ids(string list)
    {
        var ids = list.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).ToList();
        if (ids.Count == 0) throw new ArgumentException("--ids names no mod.");
        if (ids.Distinct(StringComparer.Ordinal).Count() != ids.Count) throw new ArgumentException("--ids lists a mod twice.");
        return ids;
    }

    // The mods the ids name, in the order given. Every missing id is named in one refusal.
    public static List<ModManifest> Find(IReadOnlyList<ModManifest> mods, IReadOnlyList<string> ids, string modsFolder)
    {
        var byId = mods.ToDictionary(m => m.Id, StringComparer.Ordinal);
        var missing = ids.Where(id => !byId.ContainsKey(id)).ToList();
        if (missing.Count > 0) throw new ArgumentException($"No mod with the id {string.Join(", ", missing)} in {modsFolder}.");
        return ids.Select(id => byId[id]).ToList();
    }

    // Deletes each mod's zip or folder. Refused whole when one is missing or ships with HellForge; a file that can't be
    // deleted (open in another program) is that mod's error, and the others still go.
    public static List<Outcome> Remove(InstallLayout install, IReadOnlyList<string> ids)
    {
        var chosen = Find(ModCatalog.Load(install.Mods).Mods, ids, install.Mods);
        var kept = chosen.Where(m => !Removable(m)).Select(m => m.Id).ToList();
        if (kept.Count > 0)
            throw new InvalidDataException($"{string.Join(", ", kept)} ships with HellForge: switch it off instead (mod-switch). Nothing was removed.");
        var outcomes = new List<Outcome>();
        foreach (var mod in chosen)
        {
            var path = mod.Archive != null ? System.IO.Path.Combine(install.Mods, mod.Archive) : mod.Folder;
            try { ModInstaller.Remove(install, mod); outcomes.Add(new(mod.Id, path, null)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                outcomes.Add(new(mod.Id, path, error.Message));
            }
        }
        return outcomes;
    }

    // Switches every listed mod on or off with one write of hellforge-mods.json: all of them change, or none.
    public static List<string> Switch(InstallLayout install, IReadOnlyList<string> ids, bool enabled)
    {
        Find(ModCatalog.Load(install.Mods).Mods, ids, install.Mods);
        if (!enabled && ids.Contains(ModCatalog.LocalStatsId))
            throw new InvalidDataException("Local Stats is always on: it holds the in-game editor's saved changes (reset them in the editor). Nothing was switched.");
        ModSettings.SetEnabled(install.Mods, ids, enabled);
        return [.. ids];
    }

    // Adds every chosen file as the Mods page's Add mods... does. A file that isn't a mod refuses them all before any is
    // copied; a copy that fails is that mod's error, and the others still go in.
    public static List<(ModInstaller.Candidate Mod, string? Installed, string? Error)> Add(InstallLayout install, IReadOnlyList<string> files)
    {
        var (candidates, problems) = ModInstaller.Inspect(install, files);
        if (problems.Count > 0) throw new InvalidDataException(string.Join("; ", problems) + ". Nothing was added.");
        var added = new List<(ModInstaller.Candidate, string?, string?)>();
        foreach (var mod in candidates)
        {
            try { added.Add((mod, ModInstaller.Add(install, mod), null)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                added.Add((mod, null, error.Message));
            }
        }
        return added;
    }
}
