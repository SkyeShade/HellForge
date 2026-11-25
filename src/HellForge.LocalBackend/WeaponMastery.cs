using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// A progression package as the player progresses in it: its definition (levels, prices, rewards; a copy's are its base's),
// where its progress is saved, and whose items it gives. A vanilla package's progress is in packageProgress and its items
// in the inventory; a mod weapon's copy keeps both in its modPackageProgress entry (HellForge.Core ModPackages), and Own
// turns the base's child ids into the copy's. Item is the mod weapon, null for a vanilla package.
public sealed record MasteryTrack(uint Package, JsonElement Definition, Func<uint, uint> Own, RegisteredItems.Item? Item = null)
{
    public bool IsMod => Item != null;

    public JsonObject Entry(JsonObject progression) => IsMod ? ModPackages.Entry(progression, Package) : ProfileStore.PackageEntry(progression, Package);

    public bool Holds(JsonArray inventory, JsonObject entry, uint mixId) =>
        IsMod ? WeaponMastery.Owned(this, entry).Contains(Own(mixId)) : ProfileStore.Held(inventory, mixId) > 0;

    // The id a grant of mixId stores: a copy's own for a mod weapon.
    public uint Stored(uint mixId) => IsMod ? Own(mixId) : mixId;

    public void Give(JsonArray inventory, JsonObject entry, uint mixId, long amount)
    {
        if (IsMod) ModPackages.Hold(entry, Own(mixId));
        else ProfileStore.AddItem(inventory, mixId, amount);
    }
}

// Weapon mastery, earned by missions (MissionService.End). Package type 2 is a weapon's, 25 levels, each level's
// requirement counted on its own: progress is the XP towards the next level, and the armory shows requirement - progress
// (game.dll 0x1493AA2). requirementType 1 is XP. Level 1 is requirementType 3, requirement 10, on every weapon: the client
// never reads that type (a server's rule), and what it counts is not known, so it passes at the first mission with XP, and
// that mission's XP all counts towards level 2. A weapon holds nothing of its package before level 1, whose rewards are its
// default attachments. Later levels offer attachments to buy (Purchases).
public static class WeaponMastery
{
    const int WeaponPackage = 2, ExperienceRequirement = 1, UnreadRequirement = 3;

    static readonly Lazy<Dictionary<uint, JsonElement>> Packages = new(() =>
    {
        using var data = LocalBackend.LoadResource("progression-data.json");
        return data.RootElement.GetProperty("progressionPackages").EnumerateArray().ToDictionary(p => p.GetProperty("id32").GetUInt32(), p => p.Clone());
    });

    public static JsonElement? Definition(uint package) => Packages.Value.TryGetValue(package, out var found) ? found : null;

    // A vanilla package, or null.
    public static MasteryTrack? Vanilla(uint package) => Definition(package) is { } definition ? new MasteryTrack(package, definition, id => id) : null;

    public static bool IsWeapon(MasteryTrack track) => track.Definition.GetProperty("type").GetInt32() == WeaponPackage;

    public sealed record Earned(uint Package, int Level, long Progress);

    // Adds a mission's XP to a package and walks its levels: past the last one nothing counts. The rewards of each level
    // reached are given here, as nothing else will; what is held already stays as it is, and what grantable refuses (paid
    // items the player does not own, ProfileStore.Grantable) is not given.
    public static Earned Earn(MasteryTrack track, JsonObject progression, JsonArray inventory, long experience, Func<uint, bool>? grantable = null)
    {
        var entry = track.Entry(progression);
        var level = (int)(ModPackages.Whole(entry["level"]) ?? 0);
        var progress = (ModPackages.Whole(entry["progress"]) ?? 0) + Math.Max(0, experience);
        var steps = track.Definition.GetProperty("levels").EnumerateArray().ToDictionary(s => s.GetProperty("level").GetInt32());
        while (steps.TryGetValue(level + 1, out var next))
        {
            var kind = next.GetProperty("requirementType").GetInt32();
            if (kind != UnreadRequirement && (kind != ExperienceRequirement || progress < next.GetProperty("requirement").GetInt64())) break;
            if (kind == ExperienceRequirement) progress -= next.GetProperty("requirement").GetInt64();
            level++;
            foreach (var (mixId, amount) in Rewards(next))
                if (!track.Holds(inventory, entry, mixId) && grantable?.Invoke(track.Stored(mixId)) != false) track.Give(inventory, entry, mixId, amount);
        }
        if (!steps.ContainsKey(level + 1)) progress = 0;
        entry["level"] = level;
        entry["progress"] = progress;
        return new(track.Package, level, progress);
    }

    // A copy's items: what its entry holds, and the rewards and unlocks of the levels the launcher granted.
    public static HashSet<uint> Owned(MasteryTrack track, JsonObject entry)
    {
        HashSet<uint> owned = [.. ModPackages.Held(entry)];
        var through = ModPackages.Whole(entry[ModPackages.GrantedThrough]) ?? 0;
        foreach (var step in track.Definition.GetProperty("levels").EnumerateArray().Where(s => s.GetProperty("level").GetInt64() <= through))
        {
            foreach (var (mixId, _) in Rewards(step)) owned.Add(track.Own(mixId));
            foreach (var unlock in step.GetProperty("unlockableItems").EnumerateArray()) owned.Add(track.Own(unlock.GetProperty("mixId").GetUInt32()));
        }
        return owned;
    }

    static IEnumerable<(uint MixId, long Amount)> Rewards(JsonElement step) =>
        step.GetProperty("rewards").EnumerateArray().Where(r => r.GetProperty("type").GetInt32() == 1)
            .Select(r => (r.GetProperty("id32").GetUInt32(), r.GetProperty("amount").GetInt64()));
}
