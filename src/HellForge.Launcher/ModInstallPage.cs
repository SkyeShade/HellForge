using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using HellForge.Core;

namespace HellForge.Launcher;

// ADD MODS and REMOVE on the Mods page (ModInstaller): mods are added from their zips, or a folder mod by its
// hellforge-mod.json, one or more at once; one replacing an installed mod (the same id) waits for a confirmation, and
// REMOVE asks before deleting a mod's files. Neither runs while a session has the mods loaded.
public partial class MainWindow
{
    List<ModInstaller.Candidate>? pendingAdd;
    List<string> pendingAddProblems = [];
    string? removeAsked; // the mod whose REMOVE waits for its confirmation
    string? modsBusy; // what Add mods is doing, while it reads or copies
    List<string> modsRefused = []; // the files the last Add mods did not take, and why

    async Task PickMods()
    {
        if (paths.InstallRoot == null || modsLocked) return;
        var install = new InstallLayout(paths.InstallRoot);
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Add mods: their .zip (or .rar, .7z), or a folder mod's hellforge-mod.json or a pack's manifest.json",
            AllowMultiple = true,
            FileTypeFilter = [new FilePickerFileType("Mods and cosmetic packs (.zip, .rar, .7z, hellforge-mod.json, manifest.json)")
                { Patterns = ["*.zip", "*.rar", "*.7z", ModCatalog.ManifestName, CosmeticPacks.ManifestName] }]
        });
        var chosen = files.Select(f => f.TryGetLocalPath()).OfType<string>().ToList();
        if (chosen.Count == 0) return;
        modsRefused = [];
        SetModsBusy(chosen.Count == 1 ? $"Reading {Path.GetFileName(chosen[0])}..." : $"Reading {chosen.Count} files...");
        var (mods, problems) = await Task.Run(() => ModInstaller.Inspect(install, chosen));
        if (mods.Any(m => m.Replaces != null))
        {
            pendingAdd = mods;
            pendingAddProblems = problems;
            SetModsBusy(null);
            return;
        }
        await AddMods(install, mods, problems);
    }

    // Copies the mods in off the window's thread (a pack can be hundreds of MB), then shows them.
    async Task AddMods(InstallLayout install, List<ModInstaller.Candidate> mods, List<string> problems)
    {
        pendingAdd = null;
        pendingAddProblems = [];
        foreach (var problem in problems) Append("Not added: " + problem, Red);
        modsRefused = problems;
        foreach (var mod in mods)
        {
            SetModsBusy($"Adding {mod.Name}...");
            try
            {
                await Task.Run(() => ModInstaller.Add(install, mod));
                Append($"Added {mod.Name} {mod.Version} ({mod.Id}){(mod.Replaces != null ? ", replacing the one installed" : "")}.", Green);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            { Append($"{mod.Name} was not added: {error.Message}", Red); }
        }
        SetModsBusy(null);
    }

    // What Add mods is doing, shown above the list; the page refreshes when it is done.
    void SetModsBusy(string? status)
    {
        modsBusy = status;
        AddModsButton.IsEnabled = status == null && paths.InstallRoot != null;
        RefreshMods();
    }

    // Above the list while adding waits: the mods that replace installed ones, and the files that were not mods.
    void AddPendingCard()
    {
        if (modsBusy != null)
        {
            var busy = new StackPanel { Spacing = 8 };
            busy.Children.Add(new TextBlock { Text = modsBusy, Foreground = Light });
            busy.Children.Add(new ProgressBar { IsIndeterminate = true, Classes = { "detail" } });
            ModsList.Children.Add(new Border { Classes = { "card" }, Child = busy });
            return;
        }
        if (pendingAdd == null || paths.InstallRoot == null) return;
        var install = new InstallLayout(paths.InstallRoot);
        var panel = new StackPanel { Spacing = 6 };
        panel.Children.Add(new TextBlock { Text = "ADD MODS", Classes = { "h2" }, Margin = new(0) });
        foreach (var mod in pendingAdd)
            panel.Children.Add(new TextBlock
            {
                TextWrapping = TextWrapping.Wrap, Foreground = mod.Replaces != null ? Orange : Light,
                Text = $"{mod.Name} {mod.Version} ({mod.Id})" + (mod.Replaces != null ? $": replaces the installed one, whose files are deleted ({mod.Replaces})" : "")
            });
        foreach (var problem in pendingAddProblems)
            panel.Children.Add(new TextBlock { Text = "Not a mod: " + problem, TextWrapping = TextWrapping.Wrap, Foreground = Red });
        var add = new Button { Content = "Add", Classes = { "primary" }, IsEnabled = !modsLocked };
        var cancel = new Button { Content = "Cancel", Classes = { "ghost" } };
        add.Click += async (_, _) => await AddMods(install, pendingAdd!, pendingAddProblems);
        cancel.Click += (_, _) => { pendingAdd = null; pendingAddProblems = []; DrawMods(); };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(add);
        buttons.Children.Add(cancel);
        panel.Children.Add(buttons);
        ModsList.Children.Add(new Border { Classes = { "card" }, Child = panel });
    }

    MenuItem RemoveItem(ModManifest mod)
    {
        var remove = new MenuItem { Header = "Remove...", IsEnabled = !modsLocked && !modKitBusy };
        ToolTip.SetShowOnDisabled(remove, true);
        ToolTip.SetTip(remove, modsLocked ? "Removing waits until the session ends: the game has the mods loaded." : "Deletes the mod's files from the mods folder (asks first).");
        remove.Click += (_, _) => { removeAsked = mod.Id; DrawMods(); };
        return remove;
    }

    // Under a mod's details once REMOVE was pressed: what goes, and the choice.
    Control? RemoveConfirmation(ModManifest mod)
    {
        if (removeAsked != mod.Id || paths.InstallRoot == null) return null;
        var install = new InstallLayout(paths.InstallRoot);
        var what = mod.Archive != null ? Path.Combine(install.Mods, mod.Archive) : mod.Folder;
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Text = $"This deletes {what} permanently: the mod's files{(mod.HasSource ? ", its sources included" : "")}. Keep a copy if you may want it back."
        });
        var delete = new Button { Content = "Delete mod", Classes = { "danger" }, IsEnabled = !modsLocked };
        var cancel = new Button { Content = "Cancel", Classes = { "ghost" } };
        delete.Click += (_, _) =>
        {
            removeAsked = null;
            try
            {
                ModInstaller.Remove(install, mod);
                Append($"Removed {mod.Name} ({mod.Id}): {what} is deleted.", Green);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            { Append($"{mod.Name} was not removed: {error.Message}", Red); }
            RefreshMods();
        };
        cancel.Click += (_, _) => { removeAsked = null; DrawMods(); };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(delete);
        buttons.Children.Add(cancel);
        panel.Children.Add(buttons);
        return new Border { BorderBrush = Red, BorderThickness = new(1), CornerRadius = new(4), Padding = new(12, 10), Child = panel };
    }
}
