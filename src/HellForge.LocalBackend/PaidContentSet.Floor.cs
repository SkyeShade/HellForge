using System.Text.Json;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The compiled list (HellForge.Core PaidItems) over the installation's catalogue, which is the player's own file. For
// every warbond, gated item and child the list knows, its answer replaces the catalogue's, never joins it: a token added
// to a premium warbond, a premium item put on an owned or a free warbond's pages, or a removed parentId opens nothing.
// The catalogue only adds what the list does not know (another build's ids, as in tests).
public sealed partial class PaidContentSet
{
    public PaidContentSet WithFloor(PaidItems floor)
    {
        var floorTokens = floor.Tokens.ToHashSet();
        // A warbond the list does not know keeps no token the list does: it cannot become another way to own one.
        var extras = Warbonds.Where(w => !floor.KnowsWarbond(w.Id))
            .Select(w => w with { Tokens = [.. w.Tokens.Where(t => !floorTokens.Contains(t))] }).ToList();
        var gated = floor.Gated.ToDictionary(entry => entry.Key, entry => entry.Value);
        foreach (var (item, tokens) in warbondItemTokens) gated.TryAdd(item, tokens);
        var parents = new Dictionary<uint, uint>(Parents);
        foreach (var (child, parent) in floor.Parents) parents[child] = parent;
        var warbondTokens = floorTokens.Concat(extras.SelectMany(w => w.Tokens)).ToHashSet();
        return new PaidContentSet(warbondTokens, [.. DlcItems, .. floor.Dlc], [.. MixIds, .. floor.Paid, .. warbondTokens], gated,
            [.. WarbondItems, .. floor.Warbonds.SelectMany(w => w.Items.Select(item => item.MixId))], [.. Currencies, .. floor.Warbonds.SelectMany(w => w.Medals)])
            { Warbonds = [.. floor.Warbonds.Select(w => new Warbond(w.Id, w.Tokens, w.Items)), .. extras], Parents = parents, Entries = floor.Entries };
    }

    // The list for build, from this set as FromCatalogue made it: the final answers, so nothing in a catalogue (an
    // earned list, a price) can take an id out of it later. Parents: the children of every paid or gated item. Entries:
    // the families the served catalogue is rebuilt with (PaidContentSet.Rebuild.cs).
    public PaidItems ToPaidItems(string build)
    {
        using var data = LocalBackend.LoadResource("progression-data.json");
        var medals = data.RootElement.GetProperty("seasonPassDetails").EnumerateObject().Select(entry => entry.Value)
            .ToDictionary(pass => pass.GetProperty("id32").GetUInt32(), pass => new[] { "medalsTotalMixId", "medalsConsumableMixId", "medalsSpentMixId" }
                .Select(name => pass.GetProperty(name).GetUInt32()).ToArray());
        var packages = data.RootElement.GetProperty("itemPackages").EnumerateArray()
            .Where(package => SteamDlcPackages.Contains(package.GetProperty("itemMixId32").GetUInt32()))
            .ToDictionary(package => package.GetProperty("itemMixId32").GetUInt32(),
                package => package.GetProperty("items").EnumerateArray().Select(item => item.GetProperty("mixId").GetUInt32()).ToArray());
        var gatedOrPaid = MixIds.Concat(warbondItemTokens.Keys).ToHashSet();
        return new PaidItems(build, MixIds, DlcItems,
            [.. Warbonds.Select(w => new PaidItems.Warbond(w.Id, w.Tokens, medals[w.Id], w.Items))],
            warbondItemTokens, Parents.Where(entry => gatedOrPaid.Contains(entry.Value)).ToDictionary(), packages, CompiledEntries());
    }

    // The build a catalogue folder was assembled for (GameCatalog writes it), for paid-list.
    public static string CatalogueBuild()
    {
        using var data = LocalBackend.LoadResource("progression-data.json");
        return data.RootElement.TryGetProperty("build", out var build) && build.ValueKind == JsonValueKind.String ? build.GetString()! : "";
    }
}
