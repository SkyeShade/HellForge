using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// A weapon's mastery track: progression package type 2 (progression-data.json), whose required item is the weapon.
// Level 1 gives the weapon's default attachments; each later level unlocks more (README, "Weapon levels").
public sealed record WeaponMastery(uint Package, uint Weapon, int MaxLevel, string Name, int ArmoryType);

static class WeaponLevelData
{
    static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true };

    // Every weapon with a mastery track, named from the newest session's arsenal.tsv (written by the mod loader's
    // weapon registry) when there is one, in armory order.
    public static List<WeaponMastery> Load(LauncherPaths paths)
    {
        List<WeaponMastery> weapons = [];
        if (SessionFiles.ReadJson(paths.ProgressionFile) is not { ValueKind: JsonValueKind.Object } data ||
            !data.TryGetProperty("progressionPackages", out var packages) || packages.ValueKind != JsonValueKind.Array) return weapons;
        var names = Names(paths);
        foreach (var package in packages.EnumerateArray())
        {
            if (!package.TryGetProperty("type", out var type) || type.GetInt32() != 2 ||
                !package.TryGetProperty("requiredItems", out var required) || required.GetArrayLength() == 0 ||
                !package.TryGetProperty("levels", out var levels)) continue;
            var weapon = required[0].GetUInt32();
            var (name, armoryType) = names.TryGetValue(weapon, out var known) ? known : ($"Weapon {weapon:X8}", 99);
            weapons.Add(new(package.GetProperty("id32").GetUInt32(), weapon, levels.GetArrayLength(), name, armoryType));
        }
        return weapons.OrderBy(w => w.ArmoryType).ThenBy(w => w.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    static Dictionary<uint, (string, int)> Names(LauncherPaths paths)
    {
        Dictionary<uint, (string, int)> names = [];
        try
        {
            var newest = Directory.Exists(paths.RunsFolder)
                ? new DirectoryInfo(paths.RunsFolder).GetFiles("arsenal.tsv", SearchOption.AllDirectories).MaxBy(f => f.LastWriteTimeUtc) : null;
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
        return levels;
    }

    // Sets the levels in profile.json: a progress entry per package (none at level 0), and the items every level up to
    // the new one gives or unlocks added to the inventory (nothing is taken away). Returns an error, or null.
    public static string? Save(LauncherPaths paths, string profileFile, IReadOnlyDictionary<uint, int> levels)
    {
        try
        {
            var profile = JsonNode.Parse(File.ReadAllText(profileFile))!.AsObject();
            var progression = profile["progression"]!.AsObject();
            var progress = progression["packageProgress"] as JsonArray ?? [];
            progression["packageProgress"] = progress;
            var held = profile["inventory"]!["items"]!.AsArray();
            var have = held.Select(i => Whole(i?["mixId"])).ToHashSet();
            using var data = JsonDocument.Parse(File.ReadAllText(paths.ProgressionFile));
            var packages = data.RootElement.GetProperty("progressionPackages").EnumerateArray()
                .ToDictionary(p => p.GetProperty("id32").GetUInt32());
            void Grant(uint mixId)
            {
                if (have.Add(mixId)) held.Add(new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
            }
            foreach (var (package, level) in levels) ApplyPackageLevel(progress, package, level, packages, Grant);
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

// The Weapon levels card on the profile page: each weapon's mastery level, editable between sessions.
public partial class MainWindow
{
    readonly Dictionary<uint, NumericUpDown> weaponLevelInputs = [];
    List<WeaponMastery> weaponMasteries = [];
    // The levels the inputs were last filled with (or saved), and whose profile: the launcher refreshes the profile page
    // every second, and rebuilding the inputs each time threw away levels typed but not yet saved.
    Dictionary<uint, int> weaponLevelsShown = [];
    string? weaponLevelsProfile;

    void InitializeWeaponLevels()
    {
        SetAllLevelsButton.Click += (_, _) =>
        {
            var level = (int)(AllLevelsInput.Value ?? 0);
            foreach (var weapon in weaponMasteries)
                if (weaponLevelInputs.TryGetValue(weapon.Package, out var input)) input.Value = Math.Min(level, weapon.MaxLevel);
        };
        SaveLevelsButton.Click += (_, _) =>
        {
            if (runner.IsRunning) return;
            var levels = weaponLevelInputs.ToDictionary(p => p.Key, p => (int)(p.Value.Value ?? 0));
            var error = WeaponLevelData.Save(paths, Selected.ProfileFile, levels);
            WeaponLevelsStatus.Text = error ?? "Saved. The next session uses these levels.";
            WeaponLevelsStatus.Foreground = error != null ? Red : Light;
            if (error == null) weaponLevelsShown = levels;
            ShowProfile();
        };
    }

    void ShowWeaponLevels(JsonElement? profile)
    {
        if (profile is not { } data)
        {
            WeaponLevelsList.Children.Clear();
            weaponLevelInputs.Clear();
            weaponLevelsShown = [];
            weaponLevelsProfile = null;
            WeaponLevelsCard.IsVisible = false;
            return;
        }
        var levels = WeaponLevelData.Levels(data);
        // Same profile, already filled in: keep the inputs while the player has unsaved changes, and when the saved levels
        // are still the ones shown. Only another profile, or levels a session changed on disk, refill them.
        if (weaponLevelsProfile == Selected.ProfileFile && weaponLevelInputs.Count > 0)
        {
            var edited = weaponLevelInputs.Any(p => (int)(p.Value.Value ?? 0) != weaponLevelsShown.GetValueOrDefault(p.Key));
            var unchanged = weaponMasteries.All(w => Math.Min(levels.GetValueOrDefault(w.Package), w.MaxLevel) == weaponLevelsShown.GetValueOrDefault(w.Package));
            if (edited || unchanged)
            {
                SetWeaponLevelsLocked(runner.IsRunning);
                return;
            }
        }
        WeaponLevelsList.Children.Clear();
        weaponLevelInputs.Clear();
        weaponMasteries = WeaponLevelData.Load(paths);
        WeaponLevelsCard.IsVisible = weaponMasteries.Count > 0;
        weaponLevelsProfile = Selected.ProfileFile;
        weaponLevelsShown = [];
        foreach (var weapon in weaponMasteries) weaponLevelsShown[weapon.Package] = Math.Min(levels.GetValueOrDefault(weapon.Package), weapon.MaxLevel);
        foreach (var weapon in weaponMasteries)
        {
            var input = new NumericUpDown
            {
                Minimum = 0, Maximum = weapon.MaxLevel, Increment = 1, FormatString = "0", Width = 120,
                Value = weaponLevelsShown[weapon.Package], VerticalAlignment = VerticalAlignment.Center,
            };
            weaponLevelInputs[weapon.Package] = input;
            var row = new DockPanel { Margin = new(0, 0, 24, 6) };
            DockPanel.SetDock(input, Dock.Right);
            row.Children.Add(input);
            row.Children.Add(new TextBlock { Text = weapon.Name, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            WeaponLevelsList.Children.Add(row);
        }
        SetWeaponLevelsLocked(runner.IsRunning);
    }

    void SetWeaponLevelsLocked(bool locked)
    {
        foreach (var input in weaponLevelInputs.Values) input.IsEnabled = !locked;
        AllLevelsInput.IsEnabled = SetAllLevelsButton.IsEnabled = SaveLevelsButton.IsEnabled = !locked;
    }
}
