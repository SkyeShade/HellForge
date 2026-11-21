using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// The Weapon levels card on the profile page: each weapon's mastery level, editable between sessions.
public partial class MainWindow
{
    readonly Dictionary<uint, NumericUpDown> weaponLevelInputs = [];
    List<WeaponTrack> weaponMasteries = [];
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
            var error = WeaponLevels.Save(paths.ProgressionFile, Selected.ProfileFile, levels, weaponMasteries.Where(w => w.Mod).Select(w => w.Package).ToHashSet());
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
        var levels = WeaponLevels.Levels(data);
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
        weaponMasteries = WeaponLevels.Load(paths.ProgressionFile, paths.RunsFolder);
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
