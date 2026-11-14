using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The ids a mod weapon's items take, as the client computes them. A weapon's child item (an attachment's id for that
// weapon) is a hash of the attachment and the weapon (game.dll 0x1493770); the mod loader names a copy's mastery package
// the same way (registry.c Rehome).
public static class WeaponIds
{
    const uint M = 0x5BD1E995;
    static readonly uint InverseM = Inverse(M);

    // The id of a weapon's child item.
    public static uint WeaponChild(uint attachment, uint weapon)
    {
        var k = attachment * M;
        k ^= k >> 24;
        return (k * M) ^ (weapon * M);
    }

    // The attachment a weapon's child item stands for (WeaponChild inverted).
    public static uint Attachment(uint child, uint weapon)
    {
        var k = (child ^ (weapon * M)) * InverseM;
        k ^= k >> 24;
        return k * InverseM;
    }

    // A base child's id under the new weapon.
    public static uint Rehome(uint child, uint from, uint to) => WeaponChild(Attachment(child, from), to);

    // The id of the new weapon's mastery package.
    public static uint PackageFor(uint basePackage, uint from, uint to) => Rehome(basePackage, from, to);

    static uint Inverse(uint a)
    {
        uint x = a; // Newton's iteration for the inverse modulo 2^32 (a odd)
        for (var i = 0; i < 5; i++) x *= 2 - a * x;
        return x;
    }
}

// What the profile keeps about mod weapons: progression.modPackageProgress, one entry per copy's mastery package, apart
// from packageProgress and the inventory, so a removed mod leaves vanilla data clean and an added-back one finds its level.
// An entry is a packageProgress entry (id32, level, progress, sortOrder, claimedLevels) and:
//   heldItems       the copy's own child items the player earned or bought;
//   grantedThrough  the launcher's Weapon levels page: every reward and unlock of the levels up to this one is held.
// The launcher has no catalogue, and the backend (RegisteredItems) turns the level into the copy's child ids.
// Local data only: the backend serves a copy's level in packageProgress, and never this section.
public static class ModPackages
{
    public const string Section = "modPackageProgress";
    public const string HeldItems = "heldItems", GrantedThrough = "grantedThrough";

    public static long? Whole(JsonNode? node) =>
        node is JsonValue value && value.GetValueKind() == JsonValueKind.Number &&
        long.TryParse(value.ToJsonString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number) ? number : null;

    public static JsonObject? Find(JsonObject progression, uint package) =>
        (progression[Section] as JsonArray)?.OfType<JsonObject>().FirstOrDefault(e => Whole(e["id32"]) == package);

    // A package's entry, made at level 0 with every field /Progression wants.
    public static JsonObject Entry(JsonObject progression, uint package)
    {
        if (Find(progression, package) is { } found) return found;
        if (progression[Section] is not JsonArray list) progression[Section] = list = [];
        var entry = new JsonObject { ["id32"] = package, ["level"] = 0, ["progress"] = 0, ["sortOrder"] = 0, ["claimedLevels"] = new JsonArray() };
        list.Add((JsonNode?)entry);
        return entry;
    }

    // The child items an entry holds by name; GrantedThrough's are the backend's to add.
    public static IEnumerable<uint> Held(JsonObject entry) =>
        (entry[HeldItems] as JsonArray ?? []).Select(Whole).Where(id => id is >= 0 and <= uint.MaxValue).Select(id => (uint)id!.Value);

    public static void Hold(JsonObject entry, uint mixId)
    {
        if (entry[HeldItems] is not JsonArray held) entry[HeldItems] = held = [];
        if (!Held(entry).Contains(mixId)) held.Add((JsonNode?)mixId);
    }

    // The launcher's edit, as ApplyPackageLevel does for a vanilla weapon: nothing held is taken away, and level 0 keeps
    // an entry only for what it holds.
    public static void SetLevel(JsonObject progression, uint package, int level)
    {
        var existing = Find(progression, package);
        if (level <= 0)
        {
            if (existing == null) return;
            if (existing[HeldItems] == null && existing[GrantedThrough] == null) ((JsonArray)progression[Section]!).Remove(existing);
            else (existing["level"], existing["progress"]) = (0, 0);
            return;
        }
        var entry = Entry(progression, package);
        if (Whole(entry["level"]) != level) (entry["level"], entry["progress"]) = (level, 0);
        entry[GrantedThrough] = Math.Max(level, Whole(entry[GrantedThrough]) ?? 0);
    }

    // The levels saved for mod packages.
    public static Dictionary<uint, int> Levels(JsonElement progression)
    {
        Dictionary<uint, int> levels = [];
        if (progression.TryGetProperty(Section, out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var entry in list.EnumerateArray())
                if (entry.TryGetProperty("id32", out var id) && id.TryGetUInt32(out var package) &&
                    entry.TryGetProperty("level", out var level) && level.TryGetInt32(out var value))
                    levels[package] = value;
        return levels;
    }

    // The weapons a session's registered-items.json lists (stratagems and difficulties have no mastery), as (weapon, base).
    public static List<(uint Weapon, uint Base)> RegisteredWeapons(string file)
    {
        List<(uint, uint)> weapons = [];
        try
        {
            using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var document = JsonDocument.Parse(stream);
            foreach (var item in document.RootElement.GetProperty("items").EnumerateArray())
                if (!item.TryGetProperty("kind", out _) && item.TryGetProperty("mixId", out var mix) && mix.TryGetUInt32(out var weapon) && weapon != 0 &&
                    item.TryGetProperty("copyOf", out var copy) && copy.TryGetUInt32(out var baseItem) && baseItem != 0)
                    weapons.Add((weapon, baseItem));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or KeyNotFoundException or InvalidOperationException) { }
        return weapons;
    }
}
