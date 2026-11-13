using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The game decides a stratagem is held from the catalogue reply's structure (0x136FC20): the first category-10 entry
// whose itemId is its stable id, then that entry's or its parent's state. The parent is the root before it in the reply
// (0x136EC00), whatever parentId says. So the reserved families of a served catalogue are rebuilt from the compiled list,
// in its order and with its ids: moving a child, or adding an entry under a reserved stable id, links nothing.
public sealed partial class PaidContentSet
{
    // The compiled entries the reply is rebuilt with (PaidItems.Entries); none without a compiled list.
    public IReadOnlyList<PaidItems.Entry> Entries { get; private init; } = [];

    HashSet<uint>? stratagemItemIds;

    // An itemId a stratagem entry (category 10 or 11) of the rebuilt families has: no mod's stratagem or other entry
    // there may take it.
    public bool IsReservedStratagemId(uint itemId) => (stratagemItemIds ??= StratagemItemIds(Entries)).Contains(itemId);

    static HashSet<uint> StratagemItemIds(IEnumerable<PaidItems.Entry> entries) => [.. entries.Where(e => e.Category is 10 or 11).Select(e => e.ItemId)];

    // The catalogue reply with every entry of a rebuilt family, and every stratagem entry using one of their itemIds,
    // taken out (and any entry that is not one of the game's shape), then each family put back at the end: its root, then its children, with the compiled ids. A family
    // whose root the reply lacks stays out, as its children would link to whatever came before them.
    public JsonArray Rebuilt(JsonArray reply)
    {
        if (Entries.Count == 0) return reply;
        var rebuilt = Entries.Select(e => e.MixId).ToHashSet();
        var stratagemIds = stratagemItemIds ??= StratagemItemIds(Entries);
        var fields = new Dictionary<uint, JsonObject>();
        var kept = new JsonArray();
        foreach (var entry in reply.OfType<JsonObject>())
            if (Ids(entry) is not (var mixId, var itemId, var category)) continue;
            else if (rebuilt.Contains(mixId)) fields.TryAdd(mixId, entry);
            else if (category is not (10 or 11) || !stratagemIds.Contains(itemId)) kept.Add(entry.DeepClone());
        var rootServed = false;
        foreach (var e in Entries)
        {
            if (e.Parent == 0) rootServed = fields.ContainsKey(e.MixId);
            if (rootServed && fields.TryGetValue(e.MixId, out var source)) kept.Add(Compiled(source, e));
        }
        return kept;
    }

    // An entry's served fields under the compiled ids, category and parent.
    static JsonNode Compiled(JsonObject source, PaidItems.Entry e)
    {
        var copy = source.DeepClone().AsObject();
        copy["mixId"] = e.MixId;
        copy["itemId"] = e.ItemId;
        copy["progressionCategory"] = e.Category;
        if (e.Parent == 0) copy.Remove("parentId");
        else copy["parentId"] = e.Parent;
        return copy;
    }

    // An entry's ids and category, or null (left out) when one is not a plain number, as every entry of the game's is.
    static (uint MixId, uint ItemId, uint Category)? Ids(JsonObject entry) =>
        Number(entry, "mixId") is { } mixId && Number(entry, "itemId") is { } itemId && Number(entry, "progressionCategory") is { } category
            ? (mixId, itemId, category) : null;

    static uint? Number(JsonObject entry, string name) =>
        entry[name] is JsonValue value && value.TryGetValue<long>(out var number) && number is >= 0 and <= uint.MaxValue ? (uint)number : null;

    // The families paid-list compiles, from a catalogue of the supported build in its order: each reserved entry's root
    // with all its children, and every stratagem family sharing an itemId with one. The game links by position, so a
    // catalogue where a family is not one run (a grandchild, a child away from its root) is refused, as is a paid weapon
    // part: their itemIds are shared too widely for the rebuild to cover.
    List<PaidItems.Entry> CompiledEntries()
    {
        using var catalog = LocalBackend.LoadResource("item-catalog.json");
        var all = catalog.RootElement.GetProperty("items").EnumerateArray().Select(item => new PaidItems.Entry(item.GetProperty("mixId").GetUInt32(),
            item.GetProperty("itemId").GetUInt32(), item.GetProperty("progressionCategory").GetInt32(),
            item.TryGetProperty("parentId", out var parent) && parent.TryGetUInt32(out var id) ? id : 0)).ToList();
        var roots = Roots(all);
        // until no stratagem entry outside the families shares an itemId with one inside: the reply drops all such entries
        var families = all.Where(e => IsReserved(e.MixId)).Select(e => e.Parent == 0 ? e.MixId : e.Parent).ToHashSet();
        for (var grown = true; grown;)
        {
            var stratagemIds = StratagemItemIds(all.Where((e, i) => families.Contains(roots[i])));
            grown = false;
            for (var i = 0; i < all.Count; i++)
                if (all[i].Category is 10 or 11 && stratagemIds.Contains(all[i].ItemId)) grown |= families.Add(roots[i]);
        }
        return [.. all.Where((e, i) => families.Contains(roots[i]))];
    }

    // Each entry's root, in order; a catalogue the game would link otherwise than its parentIds say is refused.
    List<uint> Roots(List<PaidItems.Entry> all)
    {
        var roots = new List<uint>();
        foreach (var e in all)
        {
            if (e.Parent != 0 && (roots.Count == 0 || roots[^1] != e.Parent)) throw new InvalidDataException($"Catalogue entry {e.MixId} does not follow its root {e.Parent}.");
            if (Gates(e.MixId) && e.Category == 2 && e.Parent != 0) throw new InvalidDataException($"Catalogue entry {e.MixId} is a paid weapon part.");
            roots.Add(e.Parent == 0 ? e.MixId : e.Parent);
        }
        return roots;
    }
}
