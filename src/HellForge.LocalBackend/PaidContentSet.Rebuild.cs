using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The game decides what is held from the catalogue reply's structure: the first entry of a category with an itemId
// (0x136FC20 for a stratagem's stable id, boosters, the weapon screens), then its or its parent's state. The parent is the
// root before it in the reply (0x136EC00), whatever parentId says. So the reserved families of a served catalogue are
// rebuilt from the compiled list, in its order and with its ids: moving a child, or adding an entry under a reserved
// itemId, links nothing.
public sealed partial class PaidContentSet
{
    // The compiled entries the reply is rebuilt with (PaidItems.Entries); none without a compiled list.
    public IReadOnlyList<PaidItems.Entry> Entries { get; private init; } = [];

    HashSet<ulong>? reservedKeys;
    List<List<PaidItems.Entry>>? families;

    // An itemId a stratagem entry (category 10 or 11) of the rebuilt families has: no mod's stratagem may take it.
    public bool IsReservedStratagemId(uint itemId) => (reservedKeys ??= Keys(Entries)).Contains(Key(10, itemId)!.Value);

    // What a first-match lookup finds an entry by: its category and itemId, stratagem items and children together (both
    // name a stratagem's ids). None for a weapon part: paint itemIds are shared by every weapon that offers the paint, and
    // the game finds one with its weapon's itemId too (0x18E0140), so a part is never taken for another weapon's.
    static ulong? Key(uint category, uint itemId) => category == 2 ? null : (ulong)(category is 11 ? 10 : category) << 32 | itemId;

    static HashSet<ulong> Keys(IEnumerable<PaidItems.Entry> entries) =>
        [.. entries.Select(e => Key((uint)e.Category, e.ItemId)).OfType<ulong>()];

    // The catalogue reply with every entry of a rebuilt family, and every other entry a first match could take for one
    // (Key), taken out, as is any entry not of the game's shape. Each family is put back at the end with the compiled ids,
    // then the entries the reply linked to it by parentId (a mod's pattern on a paid weapon, a copy of a paid skin), so
    // their run is still its. A family whose root the reply lacks stays out, with them, as they would link elsewhere.
    public JsonArray Rebuilt(JsonArray reply)
    {
        if (Entries.Count == 0) return reply;
        var (kept, fields, linked) = Sorted(reply);
        foreach (var family in families ??= Families(Entries))
        {
            if (!fields.ContainsKey(family[0].MixId)) continue;
            foreach (var e in family.Where(e => fields.ContainsKey(e.MixId))) kept.Add(Compiled(fields[e.MixId], e));
            foreach (var entry in family.SelectMany(e => linked.GetValueOrDefault(e.MixId) ?? [])) kept.Add(entry.DeepClone());
        }
        return kept;
    }

    // The reply's entries sorted for Rebuilt: those kept in place, the served fields of each rebuilt family entry, and the
    // entries linked to one by parentId, in served order.
    (JsonArray Kept, Dictionary<uint, JsonObject> Fields, Dictionary<uint, List<JsonObject>> Linked) Sorted(JsonArray reply)
    {
        var rebuilt = Entries.Select(e => e.MixId).ToHashSet();
        var keys = reservedKeys ??= Keys(Entries);
        var sorted = (Kept: new JsonArray(), Fields: new Dictionary<uint, JsonObject>(), Linked: new Dictionary<uint, List<JsonObject>>());
        foreach (var entry in reply.OfType<JsonObject>())
            if (Ids(entry) is not (var mixId, var itemId, var category)) continue;
            else if (rebuilt.Contains(mixId)) sorted.Fields.TryAdd(mixId, entry);
            else if (Key(category, itemId) is { } key && keys.Contains(key)) continue;
            else if (Number(entry, "parentId") is { } parent && rebuilt.Contains(parent)) Linked(sorted.Linked, parent).Add(entry);
            else sorted.Kept.Add(entry.DeepClone());
        return sorted;
    }

    static List<JsonObject> Linked(Dictionary<uint, List<JsonObject>> linked, uint parent) =>
        linked.TryGetValue(parent, out var list) ? list : linked[parent] = [];

    // The compiled entries as runs: a root, then its children.
    static List<List<PaidItems.Entry>> Families(IReadOnlyList<PaidItems.Entry> entries)
    {
        var runs = new List<List<PaidItems.Entry>>();
        foreach (var e in entries)
            if (e.Parent == 0 || runs.Count == 0) runs.Add([e]);
            else runs[^1].Add(e);
        return runs;
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
    // with all its children, and every family with an entry a first match could take for one of theirs (Key), until no
    // more join. The game links by position, so a catalogue where a family is not one run (a grandchild, a child away
    // from its root) is refused, as is a paid weapon part: its paint's itemId is shared too widely to rebuild.
    List<PaidItems.Entry> CompiledEntries()
    {
        using var catalog = LocalBackend.LoadResource("item-catalog.json");
        var all = catalog.RootElement.GetProperty("items").EnumerateArray().Select(item => new PaidItems.Entry(item.GetProperty("mixId").GetUInt32(),
            item.GetProperty("itemId").GetUInt32(), item.GetProperty("progressionCategory").GetInt32(),
            item.TryGetProperty("parentId", out var parent) && parent.TryGetUInt32(out var id) ? id : 0)).ToList();
        var roots = Roots(all);
        var chosen = all.Where(e => IsReserved(e.MixId)).Select(e => e.Parent == 0 ? e.MixId : e.Parent).ToHashSet();
        for (var grown = true; grown;)
        {
            var keys = Keys(all.Where((e, i) => chosen.Contains(roots[i])));
            grown = false;
            for (var i = 0; i < all.Count; i++)
                if (Key((uint)all[i].Category, all[i].ItemId) is { } key && keys.Contains(key)) grown |= chosen.Add(roots[i]);
        }
        return [.. all.Where((e, i) => chosen.Contains(roots[i]))];
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
