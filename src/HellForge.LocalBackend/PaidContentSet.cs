using System.Text.Json;

namespace HellForge.LocalBackend;

// What the game sells for real money, and its currencies, from the embedded progression-data.json and item-catalog.json:
// - paid items (MixIds): each premium warbond's tokens, every item priced in Super Credits (the Superstore's, also those
//   not in the current rotation) and the gear of the Steam DLCs, unless a warbond page offers it or a progression package
//   rewards it. A player has them only by buying them in the normal game, so the backend serves them only from a sealed
//   account snapshot (OwnershipFilter). The requisition terminal's and the samples store's items are not paid, though
//   they are store fronts.
// - warbond items: bought with medals on a warbond's pages. A premium warbond's items need one of its tokens; Helldivers
//   Mobilize is free, and an item it also offers needs none.
// - Super Credits: a premium currency. The price item and the three wallets that make up its balance are never stored,
//   served, granted or imported (KB hellforge-vanilla-progression).
// - currencies: medals, requisition and samples, which the import treats as numbers.
public sealed class PaidContentSet
{
    const uint SuperCreditPrice = 0xCF875032;
    static readonly HashSet<uint> SuperCreditIds = [SuperCreditPrice, 0x6C3C83C1, 0xCBFFC645, 0x865AF2FD];
    // Requisition, common, rare and super samples, and the generic medal reward
    static readonly uint[] FixedCurrencies = [0xD7150EEC, 0xEDF6EAF5, 0xB1ED1C41, 0xDAC0E9DB, 0x3584C850];
    // The item packages the Steam DLCs with gear grant (HellForge.Core PremiumContent.SteamDlc, which mods name too). Ownership
    // comes from the player's sealed inventory, never from Steam's DLC list. The premium warbond DLCs are tokens, paid already.
    static readonly uint[] SteamDlcPackages = [.. HellForge.Core.PremiumContent.SteamDlc.Values.SelectMany(packages => packages)];

    // One warbond: its id, the tokens that own it (none for Helldivers Mobilize, which is free), and its pages' items
    // (seasonPassItemId, itemMixId). Two warbonds' pages are all repeatable items; the account's purchases of them are
    // recorded like any other, so they are listed too.
    public sealed record Warbond(uint Id, uint[] Tokens, (long ItemId, uint MixId)[] Items);

    readonly Dictionary<uint, uint[]> warbondItemTokens;

    public IReadOnlyList<Warbond> Warbonds { get; private init; } = [];

    // Premium warbond tokens, items sold for Super Credits and Steam DLC gear.
    public HashSet<uint> MixIds { get; }
    // The premium warbond tokens alone.
    public HashSet<uint> WarbondTokens { get; }
    // The Steam DLC gear alone.
    public HashSet<uint> DlcItems { get; }
    // Every item on any warbond's pages, free or premium.
    public HashSet<uint> WarbondItems { get; }
    public HashSet<uint> Currencies { get; }

    PaidContentSet(HashSet<uint> warbondTokens, HashSet<uint> dlcItems, HashSet<uint> mixIds, Dictionary<uint, uint[]> warbondItemTokens,
        HashSet<uint> warbondItems, HashSet<uint> currencies)
    {
        WarbondTokens = warbondTokens;
        DlcItems = dlcItems;
        MixIds = mixIds;
        this.warbondItemTokens = warbondItemTokens;
        WarbondItems = warbondItems;
        Currencies = currencies;
    }

    public static PaidContentSet Load()
    {
        using var data = LocalBackend.LoadResource("progression-data.json");
        var paid = new HashSet<uint>();
        var tokensByItem = new Dictionary<uint, HashSet<uint>>();
        var freeItems = new HashSet<uint>();
        var warbondItems = new HashSet<uint>();
        var currencies = new HashSet<uint>(FixedCurrencies);
        foreach (var pass in data.RootElement.GetProperty("seasonPassDetails").EnumerateObject().Select(p => p.Value))
        {
            foreach (var medal in new[] { "medalsTotalMixId", "medalsConsumableMixId", "medalsSpentMixId" })
                currencies.Add(pass.GetProperty(medal).GetUInt32());
            var free = pass.GetProperty("id32").GetUInt32() == HellForge.Core.PremiumContent.FreeWarbond;
            var tokens = pass.GetProperty("premiumTokenMixIds").EnumerateArray().Select(t => t.GetUInt32()).ToArray();
            if (!free) paid.UnionWith(tokens);
            foreach (var item in WarbondPageItems(pass))
            {
                warbondItems.Add(item);
                if (free) freeItems.Add(item);
                else if (tokensByItem.TryGetValue(item, out var known)) known.UnionWith(tokens);
                else tokensByItem[item] = [.. tokens];
            }
        }
        var warbondTokens = new HashSet<uint>(paid);
        var earned = PackageRewards(data.RootElement);
        bool Bought(uint item) => !warbondItems.Contains(item) && !earned.Contains(item);
        var dlc = new HashSet<uint>(DlcGear(data.RootElement).Where(Bought));
        paid.UnionWith(SoldForSuperCredits().Where(Bought));
        paid.UnionWith(dlc);
        var gated = tokensByItem.Where(entry => !freeItems.Contains(entry.Key)).ToDictionary(entry => entry.Key, entry => entry.Value.ToArray());
        return new PaidContentSet(warbondTokens, dlc, paid, gated, warbondItems, currencies) { Warbonds = ReadWarbonds(data.RootElement) };
    }

    static List<Warbond> ReadWarbonds(JsonElement data) =>
        [.. data.GetProperty("seasonPassDetails").EnumerateObject().Select(entry => entry.Value).Select(pass =>
        {
            var id = pass.GetProperty("id32").GetUInt32();
            var tokens = id == HellForge.Core.PremiumContent.FreeWarbond ? [] : pass.GetProperty("premiumTokenMixIds").EnumerateArray().Select(t => t.GetUInt32()).ToArray();
            var items = pass.GetProperty("seasonPassPages").EnumerateArray()
                .SelectMany(page => page.GetProperty("seasonPassItems").EnumerateArray())
                .Select(item => (item.GetProperty("seasonPassItemId").GetInt64(), item.GetProperty("itemMixId").GetUInt32()))
                .ToArray();
            return new Warbond(id, tokens, items);
        })];

    // The warbonds an inventory owns: the free one, and each one whose token it holds. Ownership is never chosen in an
    // import: it always follows the sealed snapshot.
    public IEnumerable<Warbond> OwnedWarbonds(IReadOnlySet<uint> inventory) =>
        Warbonds.Where(warbond => warbond.Tokens.Length == 0 || warbond.Tokens.Any(inventory.Contains));

    static IEnumerable<uint> WarbondPageItems(JsonElement pass) =>
        pass.GetProperty("seasonPassPages").EnumerateArray()
            .SelectMany(page => page.GetProperty("seasonPassItems").EnumerateArray())
            .Select(item => item.GetProperty("itemMixId").GetUInt32());

    // Catalogue items whose price names Super Credits.
    static List<uint> SoldForSuperCredits()
    {
        using var catalog = LocalBackend.LoadResource("item-catalog.json");
        return [.. catalog.RootElement.GetProperty("items").EnumerateArray()
            .Where(item => item.TryGetProperty("buyPrice", out var price) && price.ValueKind == JsonValueKind.Array &&
                price.EnumerateArray().Any(cost => cost.TryGetProperty("mixId", out var currency) && currency.TryGetUInt32(out var id) && id == SuperCreditPrice))
            .Select(item => item.GetProperty("mixId").GetUInt32())];
    }

    // The DLC packages and what they hold.
    static IEnumerable<uint> DlcGear(JsonElement data) =>
        data.GetProperty("itemPackages").EnumerateArray()
            .Where(package => SteamDlcPackages.Contains(package.GetProperty("itemMixId32").GetUInt32()))
            .SelectMany(package => package.GetProperty("items").EnumerateArray().Select(item => item.GetProperty("mixId").GetUInt32()))
            .Concat(SteamDlcPackages);

    // Items progression packages (player levels, ship modules, weapon levels) give as level rewards: earned, never bought.
    // A package's requiredItems are what it needs (a weapon's mastery needs the weapon), not what it gives.
    static HashSet<uint> PackageRewards(JsonElement data) =>
        [.. data.GetProperty("progressionPackages").EnumerateArray()
            .Where(package => package.TryGetProperty("levels", out _))
            .SelectMany(package => package.GetProperty("levels").EnumerateArray())
            .Where(level => level.TryGetProperty("rewards", out _))
            .SelectMany(level => level.GetProperty("rewards").EnumerateArray())
            .Where(reward => reward.TryGetProperty("type", out var type) && type.GetInt32() == 1 && reward.TryGetProperty("id32", out _))
            .Select(reward => reward.GetProperty("id32").GetUInt32())];

    public bool IsPaid(uint mixId) => MixIds.Contains(mixId);

    public static bool IsSuperCredits(uint mixId) => SuperCreditIds.Contains(mixId);

    // A premium warbond's item: the tokens of the warbonds that offer it, one of which the player must own.
    public bool TryGetWarbondTokens(uint mixId, [System.Diagnostics.CodeAnalysis.MaybeNullWhen(false)] out uint[] tokens) =>
        warbondItemTokens.TryGetValue(mixId, out tokens);
}
