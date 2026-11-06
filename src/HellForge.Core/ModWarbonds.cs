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
        // How the game shows it (the mod loader's entry in the game's warbond table): its look (Variants), the line above
        // its name (the mod's name by default), and the game warbond whose art it shows (0: the game's placeholder).
        public int Variant { get; init; }
        public string Season { get; init; } = "";
        public uint ArtFrom { get; init; }
        // Its own picture instead ("art": {"image": ...}): the PNG, and the texture HellForge builds from it, or HellForge's
        // default card without "art" (ModWarbondArt; 0 until built, and then the game's placeholder).
        public string ArtImage { get; init; } = "";
        public ulong ArtTexture { get; init; }
        // Its price in Forge Credits, bought once with the game's warbond button (0: open to everyone), and its token: the
        // item that owns it (a premium or legendary look needs one too, held by everyone when the warbond is open).
        public long UnlockCost { get; init; }
        public uint Token { get; init; }
        public bool HasToken => Variant != 0 || UnlockCost > 0;
        // The Helldiver and background shown beside its card (null: the game's default showcase).
        public Showcase? Showcase { get; init; }
    }

    // "showcase": the Acquisition Center's Helldiver beside a warbond (native warbond_showcase.c). From: the game warbond
    // whose background and Helldiver it starts from (0: the game's default); Pose: the game warbond whose pose it takes (0:
    // From's); Armor, Helmet, Cape: catalogue items it wears instead (0: From's, or the player's own cape); Weapon: the
    // entity it holds (0: From's).
    public sealed record Showcase(uint From, uint Pose, uint Armor, uint Helmet, uint Cape, ulong Weapon);

    // A warbond's look in the game: the card's style, tag and token kind (the game's table +0x48).
    public static IReadOnlyList<string> Variants { get; } = ["free", "premium", "legendary"];

    // A page: what must have been spent on the warbond to open it, and its items.
    public sealed record Page(long Requirement, IReadOnlyList<Item> Items);

    // An item on a page: its seasonPassItemId, what it sells, its price, its cell in the 5 x 3 grid, and its own currency
    // when it is not its warbond's (CurrencyMixId 0: the warbond's).
    public sealed record Item(uint Id, Ref Sells, long Cost, int X, int Y, int Width, int Height, bool Repeatable)
    {
        public string Currency { get; init; } = "";
        public uint CurrencyMixId { get; init; }
    }

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
                warbonds.AddRange(Parse(mod.Id, mod.Name, json.RootElement, problems, mod.Folder));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
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
            uint[] own = [warbond.Id32, warbond.Spent, warbond.Token];
            if (own.Distinct().Count() != 3 || own.Any(ids.Contains) || items.Distinct().Count() != items.Count || items.Any(itemIds.Contains))
            {
                problems.Add($"{warbond.Mod}: warbond \"{warbond.Key}\" has an id another warbond has; rename its key.");
                continue;
            }
            ids.UnionWith(own);
            itemIds.UnionWith(items);
            unique.Add(warbond);
        }
        return (unique, problems);
    }

    // One mod's file: its warbonds that pass, with a problem for each that does not.
    public static List<Warbond> Parse(string modId, string modName, JsonElement root, List<string> problems, string? folder = null)
    {
        List<Warbond> warbonds = [];
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number ||
            !format.TryGetInt32(out var version) || version != Format)
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
                var warbond = ParseWarbond(modId, modName, entry, folder);
                if (!keys.Add(warbond.Key)) throw new InvalidDataException($"the key \"{warbond.Key}\" is used twice");
                warbonds.Add(warbond);
            }
            /* a value of the wrong JSON type (a string where a number goes), or a path the system refuses */
            catch (Exception error) when (error is InvalidDataException or InvalidOperationException or ArgumentException or NotSupportedException)
            {
                var key = entry.ValueKind == JsonValueKind.Object && entry.TryGetProperty("key", out var k) && k.ValueKind == JsonValueKind.String ? $"\"{k.GetString()}\"" : "#" + index;
                problems.Add($"{modName}: warbond {key} is not added: {error.Message}.");
            }
        }
        return warbonds;
    }

    // "art": {"from": <a game warbond's id32>}: that warbond's pictures (the ids in mappings/warbond-art.json, or Helldivers
    // Mobilize's); or {"image": "<a PNG in the mod's folder>"}: the mod's own, ArtWidth x ArtHeight as the game's are.
    static (uint From, string Image) Art(JsonElement entry, string? folder)
    {
        if (!entry.TryGetProperty("art", out var art)) return (0, "");
        const string Shape = "\"art\" is {\"from\": <the id of a game warbond>} or {\"image\": \"<a PNG in your mod's folder>\"}";
        if (art.ValueKind != JsonValueKind.Object || art.EnumerateObject().Count() != 1) throw new InvalidDataException(Shape);
        if (art.TryGetProperty("image", out var image))
            return (0, ArtImage(image.ValueKind == JsonValueKind.String ? image.GetString()! : "", folder) ?? throw new InvalidDataException(Shape));
        if (!art.TryGetProperty("from", out var from) || !from.TryGetUInt32(out var id) || !IsGameWarbond(id))
            throw new InvalidDataException(Shape);
        return (id, "");
    }

    public const int ArtWidth = 2048, ArtHeight = 1024;

    // The image's full path: a PNG inside the mod's folder of ArtWidth x ArtHeight (read from its header); null when the
    // value is not a relative path.
    static string? ArtImage(string relative, string? folder)
    {
        if (relative.Length == 0 || Path.IsPathRooted(relative) || folder == null) return null;
        var root = Path.GetFullPath(folder);
        var path = Path.GetFullPath(Path.Combine(root, relative));
        if (!path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.Ordinal)) return null;
        var header = new byte[24];
        try
        {
            using var file = File.OpenRead(path);
            file.ReadExactly(header);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            throw new InvalidDataException($"its art \"{relative}\" could not be read ({error.Message})");
        }
        if (!header.AsSpan(0, 8).SequenceEqual(GameData.IconImage.PngSignature) ||
            BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(16)) != ArtWidth || BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(20)) != ArtHeight)
            throw new InvalidDataException($"its art \"{relative}\" is not a {ArtWidth} x {ArtHeight} PNG");
        return path;
    }

    // The name of the texture built from a warbond's image: the first eight bytes of a SHA-256, never 0.
    public static ulong ArtTextureName(string modId, string key)
    {
        var name = BinaryPrimitives.ReadUInt64LittleEndian(SHA256.HashData(Encoding.UTF8.GetBytes("warbond-art/" + modId + "/" + key)));
        return name == 0 ? 1ul : name;
    }

    // Whether the id is a game warbond's id32: the ids in mappings/warbond-art.json, or Helldivers Mobilize's.
    static bool IsGameWarbond(uint id) => id == PremiumContent.FreeWarbond || GameData.WarbondLocks.Mapping().Any(w => w.Id == id);

    static readonly string[] ShowcaseFields = ["from", "pose", "armor", "helmet", "cape", "weapon"];

    // "showcase": {"from": <a game warbond>, "pose": <a game warbond>, "armor"/"helmet"/"cape": <a catalogue item's mixId>,
    // "weapon": "<an entity id, hex>"}, each optional.
    static Showcase? ParseShowcase(JsonElement entry)
    {
        if (!entry.TryGetProperty("showcase", out var show)) return null;
        if (show.ValueKind != JsonValueKind.Object) throw new InvalidDataException("\"showcase\" is an object");
        foreach (var field in show.EnumerateObject())
            if (!ShowcaseFields.Contains(field.Name)) throw new InvalidDataException($"\"showcase\" has no \"{field.Name}\": it takes {string.Join(", ", ShowcaseFields)}");
        uint Id(string name, bool warbond)
        {
            if (!show.TryGetProperty(name, out var value)) return 0;
            if (!value.TryGetUInt32(out var id) || id == 0 || (warbond && !IsGameWarbond(id)))
                throw new InvalidDataException(warbond ? $"the showcase's \"{name}\" is the id of a game warbond" : $"the showcase's \"{name}\" is an item's mixId");
            return id;
        }
        ulong weapon = 0;
        if (show.TryGetProperty("weapon", out var w) && (w.ValueKind != JsonValueKind.String || w.GetString()!.Length is 0 or > 16 ||
            !ulong.TryParse(w.GetString(), System.Globalization.NumberStyles.AllowHexSpecifier, System.Globalization.CultureInfo.InvariantCulture, out weapon) || weapon == 0))
            throw new InvalidDataException("the showcase's \"weapon\" is an entity id: 1 to 16 hex digits, as in arsenal.tsv");
        return new Showcase(Id("from", true), Id("pose", true), Id("armor", false), Id("helmet", false), Id("cape", false), weapon);
    }

    // "unlock": {"currency": "forge-credits", "cost": <n>}: the warbond is bought once before its items (the game's warbond
    // button takes the Super Credits price, which HellForge pays in Forge Credits).
    static long UnlockCost(JsonElement entry)
    {
        if (!entry.TryGetProperty("unlock", out var unlock)) return 0;
        if (unlock.ValueKind != JsonValueKind.Object || Text(unlock, "currency") != "forge-credits" || !unlock.TryGetProperty("cost", out var cost) ||
            !cost.TryGetInt64(out var amount) || amount is < 1 or > MaxCost)
            throw new InvalidDataException($"\"unlock\" is {{\"currency\": \"forge-credits\", \"cost\": 1 to {MaxCost}}}");
        return amount;
    }

    // The line above its name (the mod's name by default) and its variant.
    static (string Season, int Variant) Look(JsonElement entry, string modName)
    {
        var season = entry.TryGetProperty("season", out _) ? Text(entry, "season").Trim() : modName;
        if (season.Length is 0 or > 32 || season.Any(char.IsControl)) throw new InvalidDataException("\"season\" is 1 to 32 characters on one line");
        var variant = entry.TryGetProperty("variant", out _) ? Variants.ToList().IndexOf(Text(entry, "variant")) : 0;
        if (variant < 0) throw new InvalidDataException($"\"variant\" is one of {string.Join(", ", Variants)}");
        return (season, variant);
    }

    static Warbond ParseWarbond(string modId, string modName, JsonElement entry, string? folder)
    {
        if (entry.ValueKind != JsonValueKind.Object) throw new InvalidDataException("it is not an object");
        var key = Text(entry, "key");
        if (key.Length is 0 or > 64 || !key.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_' or '.'))
            throw new InvalidDataException("\"key\" is 1 to 64 of a-z, 0-9, '-', '_' and '.'");
        var name = Text(entry, "name").Trim();
        if (name.Length is 0 or > 64 || name.Any(char.IsControl)) throw new InvalidDataException("\"name\" is 1 to 64 characters on one line");
        var (season, variant) = Look(entry, modName);
        var (artFrom, artImage) = Art(entry, folder);
        var unlockCost = UnlockCost(entry);
        var showcase = ParseShowcase(entry);
        var currency = entry.TryGetProperty("currency", out var c) ? c.ValueKind == JsonValueKind.String ? c.GetString()! : "" : "medals";
        if (!Currencies.TryGetValue(currency, out var currencyMixId))
            throw new InvalidDataException($"\"currency\" is one of {string.Join(", ", Currencies.Keys)}");
        if (!entry.TryGetProperty("pages", out var pagesJson) || pagesJson.ValueKind != JsonValueKind.Array || pagesJson.GetArrayLength() is 0 or > MaxPages)
            throw new InvalidDataException($"\"pages\" holds 1 to {MaxPages} pages");
        List<Page> pages = [];
        var itemKeys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var pageJson in pagesJson.EnumerateArray()) pages.Add(ParsePage(modId, key, pageJson, pages, itemKeys));
        return new Warbond(modId, key, name, Hash("warbond/" + modId + "/" + key), Hash("warbond-spent/" + modId + "/" + key), currency, currencyMixId, pages)
            { Variant = variant, Season = season, ArtFrom = artFrom, ArtImage = artImage, UnlockCost = unlockCost, Token = Hash("warbond-token/" + modId + "/" + key),
                Showcase = showcase };
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
        return new Page(requirement, [.. parsed.Select((p, i) => new Item(ItemId(modId, warbondKey, p.Key), p.Sells, p.Cost, cells[i].X, cells[i].Y, p.W, p.H, p.Repeatable)
            { Currency = p.Currency, CurrencyMixId = p.Currency.Length > 0 ? Currencies[p.Currency] : 0 })]);
    }

    // An item as its file gives it, before its cell is known.
    sealed record ParsedItem(Ref Sells, string Key, long Cost, int W, int H, bool Repeatable, (int X, int Y)? Position, string At, string Currency);

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
        var currency = json.TryGetProperty("currency", out var c) ? c.ValueKind == JsonValueKind.String ? c.GetString()! : "?" : "";
        if (currency.Length > 0 && !Currencies.ContainsKey(currency)) throw new InvalidDataException($"{at}'s \"currency\" is one of {string.Join(", ", Currencies.Keys)}");
        var repeatable = Flag(json, "repeatable", at);
        if (repeatable && sells.Kind != "item") throw new InvalidDataException($"{at} is a {sells.Kind}: only a game item (a consumable) can be \"repeatable\"");
        return new ParsedItem(sells, key, cost, w, h, repeatable, position, at, currency);
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
            if (json.TryGetProperty("mod", out _)) throw new InvalidDataException(at + "'s \"mod\" goes with \"weapon\" or \"stratagem\" only");
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

    // The mod loader's plan lines: one per warbond, for its entry in the game's warbond table (native warbonds.c):
    // warbond, id32, variant, currency mixId, its text key, name, season, the game warbond whose art it shows, and the
    // texture built from its own image (hex; 0 for none).
    public static IEnumerable<string> PlanLines(IEnumerable<Warbond> warbonds) =>
        warbonds.SelectMany(w => (IEnumerable<string>)[WarbondLine(w), .. ShowcaseLines(w), .. ItemLines(w)]);

    // "warbond_showcase", id32, from, pose, armor, helmet, cape, weapon (hex): after its warbond's line, when it has one.
    static IEnumerable<string> ShowcaseLines(Warbond w)
    {
        if (w.Showcase is not { } s) yield break;
        var c = System.Globalization.CultureInfo.InvariantCulture;
        yield return string.Join('\t', "warbond_showcase", w.Id32.ToString(c), s.From.ToString(c), s.Pose.ToString(c), s.Armor.ToString(c),
            s.Helmet.ToString(c), s.Cape.ToString(c), s.Weapon.ToString("x16", c));
    }

    // "warbond_item", seasonPassItemId, currency mixId: every item with the currency it costs (its own, else its warbond's;
    // the game's MVVM buy path assumes medals for all).
    static IEnumerable<string> ItemLines(Warbond w) => w.Pages.SelectMany(p => p.Items).Select(i => string.Join('\t', "warbond_item",
        i.Id.ToString(System.Globalization.CultureInfo.InvariantCulture), (i.CurrencyMixId != 0 ? i.CurrencyMixId : w.CurrencyMixId).ToString(System.Globalization.CultureInfo.InvariantCulture)));

    static string WarbondLine(Warbond w) => string.Join('\t',
        "warbond", w.Id32.ToString(System.Globalization.CultureInfo.InvariantCulture), w.Variant.ToString(System.Globalization.CultureInfo.InvariantCulture),
        w.CurrencyMixId.ToString(System.Globalization.CultureInfo.InvariantCulture), "warbond/" + w.Mod + "/" + w.Key, w.Name, w.Season,
        w.ArtFrom.ToString(System.Globalization.CultureInfo.InvariantCulture), w.ArtTexture.ToString("x16", System.Globalization.CultureInfo.InvariantCulture));

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
                ["currency"] = w.Currency, ["currencyMixId"] = w.CurrencyMixId, ["variant"] = w.Variant, ["season"] = w.Season, ["artFrom"] = w.ArtFrom,
                ["unlockCost"] = w.UnlockCost, ["token"] = w.Token,
                ["pages"] = new JsonArray([.. w.Pages.Select(p => (JsonNode)new JsonObject
                {
                    ["requirement"] = p.Requirement,
                    ["items"] = new JsonArray([.. p.Items.Select(i => (JsonNode)new JsonObject
                    {
                        ["id"] = i.Id, ["kind"] = i.Sells.Kind, ["mixId"] = i.Sells.MixId, ["name"] = i.Sells.Name, ["mod"] = i.Sells.Mod,
                        ["cost"] = i.Cost, ["currency"] = i.Currency, ["currencyMixId"] = i.CurrencyMixId, ["x"] = i.X, ["y"] = i.Y, ["width"] = i.Width, ["height"] = i.Height, ["repeatable"] = i.Repeatable,
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
                    i.GetProperty("width").GetInt32(), i.GetProperty("height").GetInt32(), i.GetProperty("repeatable").GetBoolean())
                    { Currency = i.GetProperty("currency").GetString()!, CurrencyMixId = i.GetProperty("currencyMixId").GetUInt32() })]))])
            {
                Variant = w.GetProperty("variant").GetInt32(), Season = w.GetProperty("season").GetString()!, ArtFrom = w.GetProperty("artFrom").GetUInt32(),
                UnlockCost = w.GetProperty("unlockCost").GetInt64(), Token = w.GetProperty("token").GetUInt32(),
            })];
    }
}
