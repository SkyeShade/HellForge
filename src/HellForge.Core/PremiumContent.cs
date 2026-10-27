using System.Globalization;
using System.Text.Json;

namespace HellForge.Core;

// Arrowhead's premium content as a mod names it (its manifest's "premium" list, docs/modding/concepts.md), and whether a
// player owns it:
//   warbond:<id32>   a warbond (progression-data seasonPassDetails): owned with any of its tokens, or when it is free
//   dlc:<name>       a Steam DLC's gear (SteamDlc): owned when the player holds any item its packages grant
//   item:<mixId>     one item: owned when the player holds it
// Ownership comes only from the profile's account snapshot, sealed on this PC for the Steam account signed in now
// (SnapshotSeal), never from Steam's DLC list: a player who has not synced owns nothing premium.
public sealed class PremiumContent
{
    // Helldivers Mobilize: every account's, no token bought.
    public const uint FreeWarbond = 1929468580;

    // The item packages (progression-data itemPackages) the Steam DLCs with gear grant, known by their contents' names in
    // the armour data. The premium warbond DLCs are warbonds: warbond:<id32>.
    public static IReadOnlyDictionary<string, uint[]> SteamDlc { get; } = new Dictionary<string, uint[]>(StringComparer.Ordinal)
    {
        // MP-98 Knight, DP-53 Savior of the Free helmet and armour, "Will of the People" cape, two cosmetics
        ["super-citizen-edition"] = [1542124658],
        ["tr-117-alpha-commander"] = [651643972],
        ["tr-40-gold-eagle"] = [1999963265, 4146558611]
    };

    public const int MaxEntries = 32;

    readonly Dictionary<uint, uint[]> warbondTokens;
    readonly Dictionary<string, HashSet<uint>> dlcItems;
    readonly HashSet<uint> held;

    PremiumContent(Dictionary<uint, uint[]> warbondTokens, Dictionary<string, HashSet<uint>> dlcItems, HashSet<uint> held)
    {
        this.warbondTokens = warbondTokens;
        this.dlcItems = dlcItems;
        this.held = held;
    }

    // An entry as written in a manifest, in its one spelling; a malformed or unknown one is refused.
    public static string Normalize(string entry)
    {
        var colon = entry.IndexOf(':');
        var (kind, value) = colon > 0 ? (entry[..colon], entry[(colon + 1)..]) : ("", entry);
        return kind switch
        {
            "warbond" or "item" when uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id != 0 =>
                kind + ":" + id.ToString(CultureInfo.InvariantCulture),
            "dlc" when SteamDlc.ContainsKey(value) => "dlc:" + value,
            "dlc" => throw new InvalidDataException($"premium: unknown DLC \"{value}\"; HellForge knows {string.Join(", ", SteamDlc.Keys)}."),
            _ => throw new InvalidDataException($"premium: \"{entry}\" is not warbond:<id>, dlc:<name> or item:<mixId>.")
        };
    }

    // Owns nothing at all, not even the free warbond: for when the game's progression data cannot be read.
    public static PremiumContent Nothing { get; } = new([], [], []);

    // What the player owns: the snapshot's items when its seal holds on this PC for steamAccount, otherwise nothing.
    public static PremiumContent ForSnapshot(string progressionData, string? snapshotDir, ulong? steamAccount) =>
        Load(progressionData, OwnedItems(snapshotDir, steamAccount));

    // With the player's held items given directly (tests, and the backend, which has verified them already).
    public static PremiumContent Load(string progressionData, IEnumerable<uint> heldItems)
    {
        using var data = JsonDocument.Parse(File.ReadAllBytes(progressionData));
        var warbonds = new Dictionary<uint, uint[]>();
        foreach (var pass in data.RootElement.GetProperty("seasonPassDetails").EnumerateObject().Select(p => p.Value))
        {
            var id = pass.GetProperty("id32").GetUInt32();
            warbonds[id] = id == FreeWarbond ? [] : [.. pass.GetProperty("premiumTokenMixIds").EnumerateArray().Select(t => t.GetUInt32())];
        }
        var packages = data.RootElement.GetProperty("itemPackages").EnumerateArray()
            .ToDictionary(p => p.GetProperty("itemMixId32").GetUInt32(), p => p.GetProperty("items").EnumerateArray().Select(i => i.GetProperty("mixId").GetUInt32()).ToList());
        var dlc = SteamDlc.ToDictionary(entry => entry.Key,
            entry => entry.Value.SelectMany(package => packages.GetValueOrDefault(package, []).Append(package)).ToHashSet(), StringComparer.Ordinal);
        return new PremiumContent(warbonds, dlc, [.. heldItems]);
    }

    public bool Owns(string requirement)
    {
        var colon = requirement.IndexOf(':');
        var value = requirement[(colon + 1)..];
        return requirement[..colon] switch
        {
            "warbond" => warbondTokens.TryGetValue(uint.Parse(value, CultureInfo.InvariantCulture), out var tokens) && (tokens.Length == 0 || tokens.Any(held.Contains)),
            "dlc" => dlcItems.TryGetValue(value, out var items) && items.Any(held.Contains),
            _ => held.Contains(uint.Parse(value, CultureInfo.InvariantCulture))
        };
    }

    public IReadOnlyList<string> Missing(IEnumerable<string> requirements) => [.. requirements.Where(r => !Owns(r))];

    // For players: "the Super Citizen Edition", "warbond 610512218", "item 2161995568".
    public static string Describe(string requirement) => requirement switch
    {
        "dlc:super-citizen-edition" => "the Super Citizen Edition",
        "dlc:tr-117-alpha-commander" => "the TR-117 Alpha Commander DLC",
        "dlc:tr-40-gold-eagle" => "the TR-40 Gold Eagle DLC",
        _ => requirement.Replace(':', ' ')
    };

    static IEnumerable<uint> OwnedItems(string? snapshotDir, ulong? steamAccount)
    {
        if (snapshotDir == null || steamAccount == null || SnapshotSeal.Verify(snapshotDir, InventorySync.SealKeyFor(snapshotDir)) is not { } seal ||
            seal.SteamAccount != steamAccount) return [];
        try
        {
            using var inventory = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(snapshotDir, InventorySync.InventoryFile)));
            return [.. inventory.RootElement.GetProperty("items").EnumerateArray()
                .Where(item => item.ValueKind == JsonValueKind.Object && item.TryGetProperty("mixId", out var id) && id.TryGetUInt32(out _))
                .Select(item => item.GetProperty("mixId").GetUInt32())];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or KeyNotFoundException or InvalidOperationException)
        {
            return [];
        }
    }
}

// A session's mods and the premium content they declare: a mod whose list the player does not own wholly keeps its
// gameplay (friends still run the same mods) but its weapons and stratagems stay out of the player's armory (the backend's
// --premium-locked); a cosmetic one is not loaded at all.
public static class PremiumMods
{
    public sealed record Result(List<ModManifest> Loaded, List<string> Locked, List<string> Problems);

    // A session's check (mods-stage): ownership from the profile's snapshot, read only when a mod declares premium content.
    // Without the installation's progression data, nothing premium counts as owned.
    public static Result ForSession(InstallLayout install, string? snapshotDir, ulong? steamAccount, IReadOnlyList<ModManifest> loadOrder)
    {
        if (loadOrder.All(mod => mod.Premium.Count == 0)) return new Result([.. loadOrder], [], []);
        var data = Path.Combine(install.App, "progression-data.json");
        if (File.Exists(data)) return Apply(loadOrder, PremiumContent.ForSnapshot(data, snapshotDir, steamAccount));
        var result = Apply(loadOrder, PremiumContent.Nothing);
        result.Problems.Insert(0, "Premium mods: " + data + " is missing, so none of their premium content counts as owned. Reinstall HellForge.");
        return result;
    }

    public static Result Apply(IReadOnlyList<ModManifest> loadOrder, PremiumContent ownership)
    {
        List<ModManifest> loaded = [];
        List<string> locked = [], problems = [];
        foreach (var mod in loadOrder)
        {
            var missing = ownership.Missing(mod.Premium);
            if (missing.Count == 0)
            {
                loaded.Add(mod);
                continue;
            }
            var needs = string.Join(", ", missing.Select(PremiumContent.Describe));
            if (mod.Cosmetic)
            {
                problems.Add($"{mod.Name} is not loaded: it needs {needs}, which your synced account does not own.");
                continue;
            }
            loaded.Add(mod);
            locked.Add(mod.Id);
            problems.Add($"{mod.Name}'s weapons and stratagems stay locked: it needs {needs}, which your synced account does not own.");
        }
        return new Result(loaded, locked, problems);
    }

    public static void WriteLocked(string path, IEnumerable<string> modIds) =>
        File.WriteAllText(path, new System.Text.Json.Nodes.JsonObject
            { ["mods"] = new System.Text.Json.Nodes.JsonArray([.. modIds.Select(id => (System.Text.Json.Nodes.JsonNode?)id)]) }.ToJsonString() + "\n");

    // The ids in a --premium-locked file (none without one). One that cannot be read is refused, never taken as "none".
    public static HashSet<string> ReadLocked(string? path)
    {
        if (path == null) return [];
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllBytes(path));
            if (!document.RootElement.TryGetProperty("mods", out var mods) || mods.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("no mods list");
            return [.. mods.EnumerateArray().Select(m => m.ValueKind == JsonValueKind.String ? m.GetString()! : throw new InvalidDataException("a mod id is not text"))];
        }
        catch (JsonException error)
        {
            throw new InvalidDataException("The premium-locked mods file cannot be read: " + error.Message);
        }
    }
}
