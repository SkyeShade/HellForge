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
            Title = "添加模组：.zip（或 .rar、.7z），或者文件夹模组的 hellforge-mod.json、包的 manifest.json",
            AllowMultiple = true,
            FileTypeFilter = [new FilePickerFileType("模组与外观包（.zip、.rar、.7z、hellforge-mod.json、manifest.json）")
                { Patterns = ["*.zip", "*.rar", "*.7z", ModCatalog.ManifestName, CosmeticPacks.ManifestName] }]
        });
        var chosen = files.Select(f => f.TryGetLocalPath()).OfType<string>().ToList();
        if (chosen.Count == 0) return;
        modsRefused = [];
        SetModsBusy(chosen.Count == 1 ? $"正在读取 {Path.GetFileName(chosen[0])}…" : $"正在读取 {chosen.Count} 个文件…");
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
        foreach (var problem in problems) Append("未添加：" + problem, Red);
        modsRefused = problems;
        foreach (var mod in mods)
        {
            SetModsBusy($"正在添加 {mod.Name}…");
            try
            {
                await Task.Run(() => ModInstaller.Add(install, mod));
                Append($"已添加 {mod.Name} {mod.Version}（{mod.Id}）{(mod.Replaces != null ? ", replacing the one installed" : "")}.", Green);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            { Append($"{mod.Name} 未能添加：{error.Message}", Red); }
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
        panel.Children.Add(new TextBlock { Text = "添加模组", Classes = { "h2" }, Margin = new(0) });
        foreach (var mod in pendingAdd)
            panel.Children.Add(new TextBlock
            {
                TextWrapping = TextWrapping.Wrap, Foreground = mod.Replaces != null ? Orange : Light,
                Text = $"{mod.Name} {mod.Version} ({mod.Id})" + (mod.Replaces != null ? $"：会替换已安装的那个，其文件将被删除（{mod.Replaces}）" : "")
            });
        foreach (var problem in pendingAddProblems)
            panel.Children.Add(new TextBlock { Text = "不是模组：" + problem, TextWrapping = TextWrapping.Wrap, Foreground = Red });
        var add = new Button { Content = "添加", Classes = { "primary" }, IsEnabled = !modsLocked };
        var cancel = new Button { Content = "取消", Classes = { "ghost" } };
        add.Click += async (_, _) => await AddMods(install, pendingAdd!, pendingAddProblems);
        cancel.Click += (_, _) => { pendingAdd = null; pendingAddProblems = []; RefreshMods(); };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(add);
        buttons.Children.Add(cancel);
        panel.Children.Add(buttons);
        ModsList.Children.Add(new Border { Classes = { "card" }, Child = panel });
    }

    MenuItem RemoveItem(ModManifest mod)
    {
        var remove = new MenuItem { Header = "移除…", IsEnabled = !modsLocked && !modKitBusy };
        ToolTip.SetShowOnDisabled(remove, true);
        ToolTip.SetTip(remove, modsLocked ? "移除会等到会话结束后进行：游戏已经加载了这些模组。" : "从 mods 文件夹删除该模组的文件（会先确认）。");
        remove.Click += (_, _) => { removeAsked = mod.Id; RefreshMods(); };
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
            Text = $"这会永久删除 {what}：该模组的文件{(mod.HasSource ? ", its sources included" : "")}。如果以后可能还要用，请先备份。"
        });
        var delete = new Button { Content = "删除模组", Classes = { "danger" }, IsEnabled = !modsLocked };
        var cancel = new Button { Content = "取消", Classes = { "ghost" } };
        delete.Click += (_, _) =>
        {
            removeAsked = null;
            try
            {
                ModInstaller.Remove(install, mod);
                Append($"已移除 {mod.Name}（{mod.Id}）：{what} 已删除。", Green);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            { Append($"{mod.Name} 未能移除：{error.Message}", Red); }
            RefreshMods();
        };
        cancel.Click += (_, _) => { removeAsked = null; RefreshMods(); };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(delete);
        buttons.Children.Add(cancel);
        panel.Children.Add(buttons);
        return new Border { BorderBrush = Red, BorderThickness = new(1), CornerRadius = new(4), Padding = new(12, 10), Child = panel };
    }
}
