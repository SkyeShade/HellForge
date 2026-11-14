using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// A weapon's mastery track: progression package type 2 (progression-data.json), whose required item is the weapon.
// Level 1 gives the weapon's default attachments; each later level unlocks more (README, "Weapon levels"). A mod weapon's
// track is a copy of its base's, saved apart from vanilla data (ModPackages).
public sealed record WeaponTrack(uint Package, uint Weapon, int MaxLevel, string Name, int ArmoryType, bool Mod = false);

public static class WeaponLevels
{
    static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true };

    // Every weapon with a mastery track, named from the newest session's arsenal.tsv (written by the mod loader's
    // weapon registry) when there is one, in armory order; the mod weapons of the newest session come after their kind.
    // progressionFile: the progression packages the backend serves (ISessionPaths/InstallSessionPaths.ProgressionFile);
    // runsFolder: the sessions' run folders.
    public static List<WeaponTrack> Load(string progressionFile, string runsFolder)
    {
        List<WeaponTrack> weapons = [];
        if (SessionFiles.ReadJson(progressionFile) is not { ValueKind: JsonValueKind.Object } data ||
            !data.TryGetProperty("progressionPackages", out var packages) || packages.ValueKind != JsonValueKind.Array) return weapons;
        var names = Names(runsFolder);
        foreach (var package in packages.EnumerateArray())
        {
            if (!package.TryGetProperty("type", out var type) || type.GetInt32() != 2 ||
                !package.TryGetProperty("requiredItems", out var required) || required.GetArrayLength() == 0 ||
                !package.TryGetProperty("levels", out var levels)) continue;
            var weapon = required[0].GetUInt32();
            var (name, armoryType) = names.TryGetValue(weapon, out var known) ? known : ($"Weapon {weapon:X8}", 99);
            weapons.Add(new(package.GetProperty("id32").GetUInt32(), weapon, levels.GetArrayLength(), name, armoryType));
        }
        foreach (var (weapon, baseItem) in ModWeapons(runsFolder))
            if (weapons.FirstOrDefault(w => w.Weapon == baseItem && !w.Mod) is { } source)
            {
                var (name, armoryType) = names.TryGetValue(weapon, out var known) ? known : ($"{source.Name} (mod {weapon:X8})", source.ArmoryType);
                weapons.Add(source with { Package = WeaponIds.PackageFor(source.Package, baseItem, weapon), Weapon = weapon, Name = name, ArmoryType = armoryType, Mod = true });
            }
        return weapons.OrderBy(w => w.ArmoryType).ThenBy(w => w.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    // The weapons the newest session's mods registered, each once; none before a first session.
    static List<(uint Weapon, uint Base)> ModWeapons(string runsFolder)
    {
        try
        {
            var newest = Directory.Exists(runsFolder)
                ? LocalInstall.FilesNoLinks(runsFolder, "registered-items.json").Select(path => new FileInfo(path)).MaxBy(f => f.LastWriteTimeUtc) : null;
            return newest == null ? [] : [.. ModPackages.RegisteredWeapons(newest.FullName).DistinctBy(w => w.Weapon)];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    static Dictionary<uint, (string, int)> Names(string runsFolder)
    {
        Dictionary<uint, (string, int)> names = [];
        try
        {
            var newest = Directory.Exists(runsFolder)
                ? LocalInstall.FilesNoLinks(runsFolder, "arsenal.tsv").Select(path => new FileInfo(path)).MaxBy(f => f.LastWriteTimeUtc) : null;
            if (newest == null) return names;
            foreach (var line in File.ReadLines(newest.FullName).Skip(1))
            {
                var f = line.Split('\t');
                if (f.Length >= 5 && uint.TryParse(f[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var id) &&
                    int.TryParse(f[1], CultureInfo.InvariantCulture, out var type))
                    names[id] = ($"{f[3]} {f[4]}".Trim(), type);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return names;
    }

    // Levels typed as "<package>=<level>,..." (HellForge.Cli weapon-levels --set): each package one of the weapons' tracks,
    // each level a whole number from 0 to that track's top level, each package once.
    public static Dictionary<uint, int> Parse(string text, IReadOnlyList<WeaponTrack> weapons)
    {
        Dictionary<uint, int> levels = [];
        foreach (var part in text.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            var pair = part.Split('=', StringSplitOptions.TrimEntries);
            if (pair.Length != 2 || !uint.TryParse(pair[0], NumberStyles.None, CultureInfo.InvariantCulture, out var package) ||
                !int.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out var level))
                throw new ArgumentException($"{part} is not <package>=<level>.");
            var weapon = weapons.FirstOrDefault(w => w.Package == package) ?? throw new ArgumentException($"No weapon has the mastery package {package}.");
            if (level > weapon.MaxLevel) throw new ArgumentException($"{weapon.Name} goes up to level {weapon.MaxLevel}.");
            if (!levels.TryAdd(package, level)) throw new ArgumentException($"{package} is listed twice.");
        }
        return levels.Count > 0 ? levels : throw new ArgumentException("No levels given.");
    }

    // One level for every weapon, each held at its own top level, as the card's SET ALL does.
    public static Dictionary<uint, int> All(int level, IReadOnlyList<WeaponTrack> weapons) =>
        level >= 0 ? weapons.ToDictionary(w => w.Package, w => Math.Min(level, w.MaxLevel)) : throw new ArgumentException("A level is 0 or more.");

    // A JSON integer, however its node was made (a JsonValue made from a uint does not convert to long).
    static long? Whole(JsonNode? node) =>
        node is JsonValue value && value.GetValueKind() == JsonValueKind.Number &&
        long.TryParse(value.ToJsonString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number) ? number : null;

    // The profile's level in each package (0 when it has no progress entry).
    public static Dictionary<uint, int> Levels(JsonElement profile)
    {
        Dictionary<uint, int> levels = [];
        if (profile.TryGetProperty("progression", out var progression) && progression.TryGetProperty("packageProgress", out var progress) &&
            progress.ValueKind == JsonValueKind.Array)
            foreach (var entry in progress.EnumerateArray())
                if (entry.TryGetProperty("id32", out var id) && id.TryGetUInt32(out var package) &&
                    entry.TryGetProperty("level", out var level) && level.TryGetInt32(out var value))
                    levels[package] = value;
        if (profile.TryGetProperty("progression", out var section))
            foreach (var (package, value) in ModPackages.Levels(section)) levels[package] = value;
        return levels;
    }

    // Sets the levels in profile.json: a progress entry per package (none at level 0), and the items every level up to
    // the new one gives or unlocks added to the inventory (nothing is taken away). Returns an error, or null.
    // A mod weapon's level goes to its own entry, and its items follow it there (ModPackages.SetLevel).
    public static string? Save(string progressionFile, string profileFile, IReadOnlyDictionary<uint, int> levels, IReadOnlySet<uint>? modPackages = null)
    {
        try
        {
            var profile = JsonNode.Parse(File.ReadAllText(profileFile))!.AsObject();
            var progression = profile["progression"]!.AsObject();
            var progress = progression["packageProgress"] as JsonArray ?? [];
            progression["packageProgress"] = progress;
            var held = profile["inventory"]!["items"]!.AsArray();
            var have = held.Select(i => Whole(i?["mixId"])).ToHashSet();
            using var data = JsonDocument.Parse(File.ReadAllText(progressionFile));
            var packages = data.RootElement.GetProperty("progressionPackages").EnumerateArray()
                .ToDictionary(p => p.GetProperty("id32").GetUInt32());
            void Grant(uint mixId)
            {
                if (have.Add(mixId)) held.Add(new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
            }
            foreach (var (package, level) in levels)
                if (modPackages?.Contains(package) == true) ModPackages.SetLevel(progression, package, level);
                else ApplyPackageLevel(progress, package, level, packages, Grant);
            var temporary = profileFile + ".tmp";
            File.WriteAllText(temporary, profile.ToJsonString(WriteOptions) + "\n");
            File.Move(temporary, profileFile, true);
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or
            NullReferenceException or KeyNotFoundException or FormatException)
        {
            return "Could not save the weapon levels: " + error.Message;
        }
    }

    // One weapon's mastery package set to level (0 removes it): the progress entry, and the reward and unlock items of
    // levels 1..level granted through Grant.
    static void ApplyPackageLevel(JsonArray progress, uint package, int level, IReadOnlyDictionary<uint, JsonElement> packages, Action<uint> Grant)
    {
        var entry = progress.OfType<JsonObject>().FirstOrDefault(e => Whole(e["id32"]) == package);
        if (level <= 0)
        {
            if (entry != null) progress.Remove(entry);
            return;
        }
        if (entry == null)
            progress.Add(new JsonObject { ["id32"] = package, ["level"] = level, ["progress"] = 0, ["sortOrder"] = 0, ["claimedLevels"] = new JsonArray() });
        else if (Whole(entry["level"]) != level)
        {
            entry["level"] = level;
            entry["progress"] = 0;
        }
        if (!packages.TryGetValue(package, out var definition)) return;
        foreach (var step in definition.GetProperty("levels").EnumerateArray().Where(s => s.GetProperty("level").GetInt32() <= level))
        {
            foreach (var reward in step.GetProperty("rewards").EnumerateArray())
                if (reward.GetProperty("type").GetInt32() == 1) Grant(reward.GetProperty("id32").GetUInt32());
            foreach (var unlock in step.GetProperty("unlockableItems").EnumerateArray()) Grant(unlock.GetProperty("mixId").GetUInt32());
        }
    }
}
