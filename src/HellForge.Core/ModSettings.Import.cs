namespace HellForge.Core;

// Each HellForge version keeps its own mod list (KeptVersions): a new one starts empty after an update, and the Mods page
// can bring a kept version's list over.
public static partial class ModSettings
{
    // A new version's own list after an update: the given mods off, no order, pack choices or trust; the sharing and
    // lobby switches stay, as the rest of the player's settings do.
    public static void StartFresh(string modsFolder, IEnumerable<string> off) => Change(modsFolder, state =>
    {
        state.Disabled.Clear();
        state.Disabled.UnionWith(off);
        state.Order.Clear();
        state.Choices.Clear();
        state.Blocked.Clear();
        state.Trusted.Clear();
        state.ImportDone = false;
    });

    // Another version's list brought over for the mods it knew (ids): on or off as there, its order first, its pack choices,
    // blocked and trusted mods added. Mods it never knew keep their state here.
    public static void Import(string modsFolder, State from, IReadOnlyCollection<string> ids) => Change(modsFolder, state =>
    {
        foreach (var id in ids)
            if (from.Disabled.Contains(id)) state.Disabled.Add(id);
            else state.Disabled.Remove(id);
        var order = from.Order.Concat(state.Order).Distinct(StringComparer.Ordinal).ToList();
        state.Order.Clear();
        state.Order.AddRange(order);
        foreach (var (id, choice) in from.Choices) state.Choices[id] = choice;
        state.Blocked.UnionWith(from.Blocked);
        foreach (var mod in from.Trusted.Where(t => !state.Trusts(t.Id, t.Hash, sandboxed: true))) state.Trusted.Add(mod);
        state.ImportDone = true;
    });

    public static void SetImportDone(string modsFolder) => Change(modsFolder, state => state.ImportDone = true);
}
