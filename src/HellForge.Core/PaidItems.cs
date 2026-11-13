using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The supported build's paid items as numbers only (data/paid-items.json): the premium warbonds with their tokens and pages,
// the items each premium warbond gates, every paid item (premium tokens, Super Credits items, Steam DLC gear), the parents
// of their children, the Steam DLC packages' contents, the paid stratagems' stable ids and the catalogue entries the backend rebuilds. It is compiled in because the installation's catalogue is the
// player's own file: what it says about these ids is never taken over this list (HellForge.LocalBackend PaidContentSet).
// HellForge.LocalBackend paid-list writes it from a catalogue of the supported build.
public sealed record PaidItems(string Build, IReadOnlySet<uint> Paid, IReadOnlySet<uint> Dlc, IReadOnlyList<PaidItems.Warbond> Warbonds,
    IReadOnlyDictionary<uint, uint[]> Gated, IReadOnlyDictionary<uint, uint> Parents, IReadOnlyDictionary<uint, uint[]> DlcPackages,
    IReadOnlyList<PaidItems.Entry> Entries, IReadOnlySet<uint> Stratagems)
{
    public const int Format = 2;

    // A catalogue entry as the game links it: its ids, category and parent (0: a root), in the catalogue's order. The
    // game links a child to the root before it, not by parentId (0x136EC00), so these are served in this order.
    public sealed record Entry(uint MixId, uint ItemId, int Category, uint Parent);

    // Stratagems: the itemIds of the paid category-10 entries, the stable ids StratagemInfo rows carry. The mod loader
    // takes them from a header sdk-headers generates from this file, so a level's mission stratagem is none of them.

    // A warbond: its tokens in the game's order (the client buys the first), none for Helldivers Mobilize; its medal items
    // (total, consumable, spent); and its pages' items.
    public sealed record Warbond(uint Id, uint[] Tokens, uint[] Medals, (long ItemId, uint MixId)[] Items);

    static readonly Lazy<PaidItems> embedded = new(() =>
    {
        using var stream = typeof(PaidItems).Assembly.GetManifestResourceStream("HellForge.Core.paid-items.json")
            ?? throw new InvalidDataException("The paid item list is not embedded.");
        using var reader = new StreamReader(stream);
        return Parse(reader.ReadToEnd());
    });

    public static PaidItems Current => embedded.Value;

    // Every premium warbond token.
    public IEnumerable<uint> Tokens => Warbonds.SelectMany(warbond => warbond.Tokens);

    public bool KnowsWarbond(uint id) => Warbonds.Any(warbond => warbond.Id == id);

    public static PaidItems Parse(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The paid item list is not an object.");
        if ((int?)root["format"] != Format) throw new InvalidDataException("The paid item list is not format " + Format + ".");
        var warbonds = (root["warbonds"] as JsonArray ?? []).OfType<JsonObject>().Select(w => new Warbond((uint)w["id32"]!, Ids(w["tokens"]), Ids(w["medals"]),
            [.. (w["items"] as JsonArray ?? []).OfType<JsonArray>().Select(pair => ((long)pair[0]!, (uint)pair[1]!))])).ToList();
        return new PaidItems((string?)root["build"] ?? "", Ids(root["paid"]).ToHashSet(), Ids(root["dlc"]).ToHashSet(), warbonds,
            Map(root["gated"], Ids), Map(root["parents"], node => (uint)node!), Map(root["dlcPackages"], Ids),
            [.. (root["entries"] as JsonArray ?? []).OfType<JsonArray>().Select(e => new Entry((uint)e[0]!, (uint)e[1]!, (int)e[2]!, (uint)e[3]!))],
            Ids(root["stratagems"]).ToHashSet());
    }

    // Sorted, one entry a line, so a regenerated file differs only where the build did.
    public string ToJson()
    {
        static string List(IEnumerable<uint> ids) => "[" + string.Join(",", ids.Order()) + "]";
        static string Keyed<T>(IReadOnlyDictionary<uint, T> map, Func<T, string> value) =>
            "{\n" + string.Join(",\n", map.OrderBy(e => e.Key).Select(e => "    \"" + e.Key + "\": " + value(e.Value))) + "\n  }";
        var warbonds = Warbonds.Select(w => "    {\"id32\":" + w.Id + ",\"tokens\":[" + string.Join(",", w.Tokens) + "],\"medals\":[" + string.Join(",", w.Medals) +
            "],\"items\":[" + string.Join(",", w.Items.Select(i => "[" + i.ItemId + "," + i.MixId + "]")) + "]}");
        var fields = new[]
        {
            "\"format\": " + Format,
            "\"build\": " + JsonValue.Create(Build).ToJsonString(),
            "\"note\": \"Ids only, from a catalogue of this build (HellForge.LocalBackend paid-list). Never edit by hand.\"",
            "\"paid\": " + List(Paid),
            "\"dlc\": " + List(Dlc),
            "\"warbonds\": [\n" + string.Join(",\n", warbonds) + "\n  ]",
            "\"gated\": " + Keyed(Gated, List),
            "\"parents\": " + Keyed(Parents, parent => parent.ToString(CultureInfo.InvariantCulture)),
            "\"dlcPackages\": " + Keyed(DlcPackages, List),
            "\"stratagems\": " + List(Stratagems),
            "\"entries\": [\n" + string.Join(",\n", Entries.Select(e => $"    [{e.MixId},{e.ItemId},{e.Category},{e.Parent}]")) + "\n  ]"
        };
        return "{\n  " + string.Join(",\n  ", fields) + "\n}\n";
    }

    public const string StratagemHeaderFile = "native/mod-loader/paid_stratagems.h";

    // The mod loader's copy of Stratagems (sdk-headers writes it, and its check fails when it differs from this file).
    public string StratagemHeader()
    {
        var ids = Stratagems.Order().Select(id => "0x" + id.ToString("X8", CultureInfo.InvariantCulture) + "u").ToList();
        var rows = ids.Chunk(8).Select(row => "    " + string.Join(", ", row) + ",");
        return $"/* The paid stratagems' stable ids for game build {Build} (data/paid-items.json \"stratagems\"): a level's mission\n" +
            " * stratagem may not be one (difficulty_missions.c GrantRefused).\n" +
            " * Generated by `hellforge sdk-headers` from data/paid-items.json; do not edit. */\n" +
            $"#define PAID_STRATAGEM_COUNT {ids.Count}u\n" +
            "static const uint32_t kPaidStratagems[PAID_STRATAGEM_COUNT + 1] = {\n" + string.Join("\n", rows) + (ids.Count > 0 ? "\n" : "") + "    0};\n";
    }

    static uint[] Ids(JsonNode? node) => [.. (node as JsonArray ?? []).Select(id => (uint)id!)];

    static Dictionary<uint, T> Map<T>(JsonNode? node, Func<JsonNode?, T> value) =>
        (node as JsonObject ?? []).ToDictionary(entry => uint.Parse(entry.Key, NumberStyles.None, CultureInfo.InvariantCulture), entry => value(entry.Value));
}
