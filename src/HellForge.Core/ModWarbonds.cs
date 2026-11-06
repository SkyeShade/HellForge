using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Warbonds a mod adds (docs/modding/warbonds.md): hellforge-warbonds.json beside its manifest. mods-stage checks every
// loaded mod's file and writes the session's warbonds (--mod-warbonds) for the backend (LocalBackend ModWarbondService).
// The limits are game.dll 25480438's: 15 pages (0x103E03A); 10 items a page, drawn on a 5 x 3 grid (0x196D81E, cursor
// 0x1974880); one price an item, in the warbond's currency (checked 0x196D2E4, charged 0x196BB0F); and one flat list of
// bought ids (0x196E192), so a mod's item ids sit above the game's 1..989. Every id comes from the mod's id and the
// warbond's key: the same in every session and on every machine.
public static class ModWarbonds
{
    public const string FileName = "hellforge-warbonds.json";
    public const int Format = 1, MaxPages = 15, MaxItemsPerPage = 10, GridWidth = 5, GridHeight = 3;
    public const long MaxCost = 1_000_000;
    // A mod item's id is in [FirstItemId, FirstItemId + ItemIdSpan): above the game's own, within a signed 32-bit number.
    public const uint FirstItemId = 0x10000000, ItemIdSpan = 0x70000000;

    // The currencies a warbond's items may cost, by name: medals, requisition, the samples and Forge Credits (the wallet
    // HellForge's pickups pay into).
    public static IReadOnlyDictionary<string, uint> Currencies { get; } = new Dictionary<string, uint>(StringComparer.Ordinal)
    {
        ["medals"] = 0x4C19589F,
        ["requisition"] = 0xD7150EEC,
        ["common-samples"] = 0xEDF6EAF5,
        ["rare-samples"] = 0xB1ED1C41,
        ["super-samples"] = 0xDAC0E9DB,
        ["forge-credits"] = 0x6C3C83C1,
    };

    // A warbond, its ids derived (Id32: the warbond; Spent: the item counting what was spent on it, which opens its pages).
    public sealed record Warbond(string Mod, string Key, string Name, uint Id32, uint Spent, string Currency, uint CurrencyMixId,
        IReadOnlyList<Page> Pages)
    {
        public long TotalCost => Pages.Sum(page => page.Items.Sum(item => item.Cost));
    }

    // A page: what must have been spent on the warbond to open it, and its items.
    public sealed record Page(long Requirement, IReadOnlyList<Item> Items);

    // An item on a page: its seasonPassItemId, what it sells, its price, its cell in the 5 x 3 grid.
    public sealed record Item(uint Id, Ref Sells, long Cost, int X, int Y, int Width, int Height, bool Repeatable);

    // What an item sells: a catalogue item by its mixId (Kind "item"), or a weapon or stratagem a code mod registers this
    // session, by its name and that mod's id, resolved by the backend from registered-items.json.
    public sealed record Ref(string Kind, uint MixId, string Name, string Mod)
    {
        public override string ToString() => Kind == "item" ? "item " + MixId : $"{Kind} {Name} ({Mod})";
    }

    // The warbonds of the mods in load order, and the problems: a warbond with a mistake is left out whole and its
    // mod's other warbonds still load.
    public static (List<Warbond> Warbonds, List<string> Problems) Load(IEnumerable<ModManifest> mods)
    {
        List<Warbond> warbonds = [];
        List<string> problems = [];
        foreach (var mod in mods)
        {
            var file = Path.Combine(mod.Folder, FileName);
            if (!File.Exists(file)) continue;
            try
            {
                using var json = JsonDocument.Parse(File.ReadAllText(file), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                warbonds.AddRange(Parse(mod.Id, mod.Name, json.RootElement, problems));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
            {
                problems.Add($"{mod.Name}: {FileName} could not be read ({error.Message}); its warbonds are not added.");
            }
        }
        // Two mods' warbonds or items with the same id (a hash collision) would be one in the game: the later is left out.
        var ids = new HashSet<uint>();
        var itemIds = new HashSet<uint>();
        List<Warbond> unique = [];
        foreach (var warbond in warbonds)
        {
            var items = warbond.Pages.SelectMany(page => page.Items).Select(item => item.Id).ToList();
            if (!ids.Add(warbond.Id32) || !ids.Add(warbond.Spent) || items.Any(itemIds.Contains))
            {
                problems.Add($"{warbond.Mod}: warbond \"{warbond.Key}\" has an id another warbond has; rename its key.");
                continue;
            }
            itemIds.UnionWith(items);
            unique.Add(warbond);
        }
        return (unique, problems);
    }

    // One mod's file: its warbonds that pass, with a problem for each that does not.
    public static List<Warbond> Parse(string modId, string modName, JsonElement root, List<string> problems)
    {
        List<Warbond> warbonds = [];
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("format", out var format) || !format.TryGetInt32(out var version) || version != Format)
        {
            problems.Add($"{modName}: {FileName} needs \"format\": {Format}; its warbonds are not added.");
            return warbonds;
        }
        if (!root.TryGetProperty("warbonds", out var list) || list.ValueKind != JsonValueKind.Array)
        {
            problems.Add($"{modName}: {FileName} has no \"warbonds\" list.");
            return warbonds;
        }
        var keys = new HashSet<string>(StringComparer.Ordinal);
        var index = 0;
        foreach (var entry in list.EnumerateArray())
        {
            index++;
            try
            {
                var warbond = ParseWarbond(modId, entry);
                if (!keys.Add(warbond.Key)) throw new InvalidDataException($"the key \"{warbond.Key}\" is used twice");
                warbonds.Add(warbond);
            }
            catch (InvalidDataException error)
            {
                var key = entry.ValueKind == JsonValueKind.Object && entry.TryGetProperty("key", out var k) && k.ValueKind == JsonValueKind.String ? $"\"{k.GetString()}\"" : "#" + index;
                problems.Add($"{modName}: warbond {key} is not added: {error.Message}.");
            }
        }
        return warbonds;
    }

    static Warbond ParseWarbond(string modId, JsonElement entry)
    {
        if (entry.ValueKind != JsonValueKind.Object) throw new InvalidDataException("it is not an object");
        var key = Text(entry, "key");
        if (key.Length is 0 or > 64 || !key.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_' or '.'))
            throw new InvalidDataException("\"key\" is 1 to 64 of a-z, 0-9, '-', '_' and '.'");
        var name = Text(entry, "name").Trim();
        if (name.Length is 0 or > 64) throw new InvalidDataException("\"name\" is 1 to 64 characters");
        var currency = entry.TryGetProperty("currency", out var c) ? c.ValueKind == JsonValueKind.String ? c.GetString()! : "" : "medals";
        if (!Currencies.TryGetValue(currency, out var currencyMixId))
            throw new InvalidDataException($"\"currency\" is one of {string.Join(", ", Currencies.Keys)}");
        if (!entry.TryGetProperty("pages", out var pagesJson) || pagesJson.ValueKind != JsonValueKind.Array || pagesJson.GetArrayLength() is 0 or > MaxPages)
            throw new InvalidDataException($"\"pages\" holds 1 to {MaxPages} pages");
        List<Page> pages = [];
        var itemKeys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var pageJson in pagesJson.EnumerateArray()) pages.Add(ParsePage(modId, key, pageJson, pages, itemKeys));
        return new Warbond(modId, key, name, Hash("warbond/" + modId + "/" + key), Hash("warbond-spent/" + modId + "/" + key), currency, currencyMixId, pages);
    }

    // A page: its requirement opens it no sooner than the page before it, and no later than the pages before it cost.
    static Page ParsePage(string modId, string warbondKey, JsonElement pageJson, List<Page> before, HashSet<string> itemKeys)
    {
        var at = $"page {before.Count + 1}";
        if (pageJson.ValueKind != JsonValueKind.Object) throw new InvalidDataException(at + " is not an object");
        var requirement = pageJson.TryGetProperty("requirement", out var r) ? Whole(r, at + " \"requirement\"") : 0;
        var spentBefore = before.Sum(page => page.Items.Sum(item => item.Cost));
        if (before.Count == 0 && requirement != 0) throw new InvalidDataException("page 1's \"requirement\" is 0: it is open from the start");
        if (before.Count > 0 && requirement < before[^1].Requirement) throw new InvalidDataException(at + "'s \"requirement\" is below the page before it");
        if (requirement > spentBefore)
            throw new InvalidDataException($"{at} needs {requirement} spent, but the pages before it cost {spentBefore} in all, so it could never open");
        if (!pageJson.TryGetProperty("items", out var itemsJson) || itemsJson.ValueKind != JsonValueKind.Array || itemsJson.GetArrayLength() is 0 or > MaxItemsPerPage)
            throw new InvalidDataException($"{at} holds 1 to {MaxItemsPerPage} items");
        var parsed = itemsJson.EnumerateArray().Select((json, i) => ParseItem(modId, json, $"{at} item {i + 1}", itemKeys)).ToList();
        // Items with a position take their cells first; the others fill the first free cells, row by row.
        var grid = new bool[GridWidth, GridHeight];
        var cells = parsed.Select(item => item.Position is { } position ? Place(grid, item, position) : (X: -1, Y: -1)).ToArray();
        for (var i = 0; i < parsed.Count; i++)
            if (cells[i].X < 0) cells[i] = Fill(grid, parsed[i]);
        return new Page(requirement, [.. parsed.Select((p, i) => new Item(ItemId(modId, warbondKey, p.Key), p.Sells, p.Cost, cells[i].X, cells[i].Y, p.W, p.H, p.Repeatable))]);
    }

    // An item as its file gives it, before its cell is known.
    sealed record ParsedItem(Ref Sells, string Key, long Cost, int W, int H, bool Repeatable, (int X, int Y)? Position, string At);

    static ParsedItem ParseItem(string modId, JsonElement json, string at, HashSet<string> itemKeys)
    {
        if (json.ValueKind != JsonValueKind.Object) throw new InvalidDataException(at + " is not an object");
        var sells = ParseRef(modId, json, at);
        var key = ItemKey(json, sells, at);
        if (!itemKeys.Add(key)) throw new InvalidDataException($"{at} sells what another item of the warbond sells; give one of them its own \"key\"");
        if (!json.TryGetProperty("cost", out var costJson)) throw new InvalidDataException(at + " has no \"cost\"");
        var cost = Whole(costJson, at + " \"cost\"");
        if (cost is < 1 or > MaxCost) throw new InvalidDataException($"{at}'s \"cost\" is 1 to {MaxCost}");
        var (w, h) = json.TryGetProperty("size", out var size) ? Pair(size, at + " \"size\"") : (1, 1);
        if (w < 1 || h < 1 || w > GridWidth || h > GridHeight) throw new InvalidDataException($"{at}'s \"size\" fits the {GridWidth} x {GridHeight} grid");
        (int, int)? position = json.TryGetProperty("position", out var p) ? Pair(p, at + " \"position\"") : null;
        return new ParsedItem(sells, key, cost, w, h, Flag(json, "repeatable", at), position, at);
    }

    // An item's key within its warbond: its own, or what it sells.
    static string ItemKey(JsonElement json, Ref sells, string at)
    {
        if (!json.TryGetProperty("key", out var key))
            return sells.Kind + ":" + (sells.Kind == "item" ? sells.MixId.ToString(System.Globalization.CultureInfo.InvariantCulture) : sells.Mod + "/" + sells.Name);
        return key.ValueKind == JsonValueKind.String && key.GetString()!.Length is > 0 and <= 64 ? key.GetString()!
            : throw new InvalidDataException(at + "'s \"key\" is 1 to 64 characters");
    }

    static (int X, int Y) Place(bool[,] grid, ParsedItem item, (int X, int Y) at)
    {
        if (at.X < 0 || at.Y < 0 || at.X + item.W > GridWidth || at.Y + item.H > GridHeight)
            throw new InvalidDataException($"{item.At} does not fit in the {GridWidth} x {GridHeight} grid at {at.X}, {at.Y}");
        return Take(grid, at.X, at.Y, item.W, item.H) ? at : throw new InvalidDataException($"{item.At} overlaps another item at {at.X}, {at.Y}");
    }

    static (int X, int Y) Fill(bool[,] grid, ParsedItem item)
    {
        for (var y = 0; y + item.H <= GridHeight; y++)
            for (var x = 0; x + item.W <= GridWidth; x++)
                if (Take(grid, x, y, item.W, item.H)) return (x, y);
        throw new InvalidDataException($"{item.At} has no room left in the {GridWidth} x {GridHeight} grid");
    }

    static Ref ParseRef(string modId, JsonElement json, string at)
    {
        var named = new[] { "mixId", "weapon", "stratagem" }.Where(name => json.TryGetProperty(name, out _)).ToList();
        if (named.Count != 1) throw new InvalidDataException(at + " names exactly one of \"mixId\", \"weapon\" or \"stratagem\"");
        var mod = json.TryGetProperty("mod", out var m) ? m.ValueKind == JsonValueKind.String && m.GetString()!.Length > 0 ? m.GetString()!
            : throw new InvalidDataException(at + "'s \"mod\" is a mod id") : modId;
        if (named[0] == "mixId")
        {
            var mixId = json.GetProperty("mixId");
            if (!mixId.TryGetUInt32(out var id) || id == 0) throw new InvalidDataException(at + "'s \"mixId\" is a catalogue item's id");
            return new Ref("item", id, "", "");
        }
        var name = json.GetProperty(named[0]);
        if (name.ValueKind != JsonValueKind.String || name.GetString()!.Length is 0 or > 191)
            throw new InvalidDataException($"{at}'s \"{named[0]}\" is the name the mod registers it by");
        return new Ref(named[0], 0, name.GetString()!, mod);
    }

    static bool Take(bool[,] grid, int x, int y, int w, int h)
    {
        for (var i = x; i < x + w; i++)
            for (var j = y; j < y + h; j++)
                if (grid[i, j]) return false;
        for (var i = x; i < x + w; i++)
            for (var j = y; j < y + h; j++)
                grid[i, j] = true;
        return true;
    }

    static string Text(JsonElement entry, string name) =>
        entry.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()! : "";

    static bool Flag(JsonElement entry, string name, string at) =>
        entry.TryGetProperty(name, out var value) && (value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean() : throw new InvalidDataException($"{at}'s \"{name}\" is true or false"));

    static long Whole(JsonElement value, string what) =>
        value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var whole) && whole >= 0 ? whole : throw new InvalidDataException(what + " is a whole number, 0 or more");

    static (int, int) Pair(JsonElement value, string what) =>
        value.ValueKind == JsonValueKind.Array && value.GetArrayLength() == 2 && value[0].TryGetInt32(out var a) && value[1].TryGetInt32(out var b)
            ? (a, b) : throw new InvalidDataException(what + " is [x, y]");

    // A stable 32-bit id: the first four bytes of the text's SHA-256, never 0.
    public static uint Hash(string text)
    {
        var id = BinaryPrimitives.ReadUInt32LittleEndian(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
        return id == 0 ? 1u : id;
    }

    public static uint ItemId(string modId, string warbondKey, string itemKey) =>
        FirstItemId + Hash("warbond-item/" + modId + "/" + warbondKey + "/" + itemKey) % ItemIdSpan;

    // The session's warbonds (mods-stage --mod-warbonds), for the backend.
    public static void Write(string path, IReadOnlyList<Warbond> warbonds)
    {
        var list = new JsonArray();
        foreach (var w in warbonds)
            list.Add((JsonNode)new JsonObject
            {
                ["mod"] = w.Mod, ["key"] = w.Key, ["name"] = w.Name, ["id32"] = w.Id32, ["spent"] = w.Spent,
                ["currency"] = w.Currency, ["currencyMixId"] = w.CurrencyMixId,
                ["pages"] = new JsonArray([.. w.Pages.Select(p => (JsonNode)new JsonObject
                {
                    ["requirement"] = p.Requirement,
                    ["items"] = new JsonArray([.. p.Items.Select(i => (JsonNode)new JsonObject
                    {
                        ["id"] = i.Id, ["kind"] = i.Sells.Kind, ["mixId"] = i.Sells.MixId, ["name"] = i.Sells.Name, ["mod"] = i.Sells.Mod,
                        ["cost"] = i.Cost, ["x"] = i.X, ["y"] = i.Y, ["width"] = i.Width, ["height"] = i.Height, ["repeatable"] = i.Repeatable,
                    })]),
                })]),
            });
        var text = new JsonObject { ["format"] = Format, ["warbonds"] = list }.ToJsonString();
        File.WriteAllText(path, text);
    }

    // The warbonds in a session file; none without one.
    public static List<Warbond> Read(string? path)
    {
        if (path == null || !File.Exists(path)) return [];
        using var json = JsonDocument.Parse(File.ReadAllText(path));
        if (json.RootElement.GetProperty("format").GetInt32() != Format) throw new InvalidDataException(path + " is not a format " + Format + " warbonds file.");
        return [.. json.RootElement.GetProperty("warbonds").EnumerateArray().Select(w => new Warbond(
            w.GetProperty("mod").GetString()!, w.GetProperty("key").GetString()!, w.GetProperty("name").GetString()!,
            w.GetProperty("id32").GetUInt32(), w.GetProperty("spent").GetUInt32(), w.GetProperty("currency").GetString()!, w.GetProperty("currencyMixId").GetUInt32(),
            [.. w.GetProperty("pages").EnumerateArray().Select(p => new Page(p.GetProperty("requirement").GetInt64(),
                [.. p.GetProperty("items").EnumerateArray().Select(i => new Item(i.GetProperty("id").GetUInt32(),
                    new Ref(i.GetProperty("kind").GetString()!, i.GetProperty("mixId").GetUInt32(), i.GetProperty("name").GetString()!, i.GetProperty("mod").GetString()!),
                    i.GetProperty("cost").GetInt64(), i.GetProperty("x").GetInt32(), i.GetProperty("y").GetInt32(),
                    i.GetProperty("width").GetInt32(), i.GetProperty("height").GetInt32(), i.GetProperty("repeatable").GetBoolean()))]))]))];
    }
}
