using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The Mods page: every mod in <installation>/mods as a row, in two groups: gameplay mods (friends need the same ones;
// the order is up to them) and cosmetic ones (packs of game patches; only on this PC). Within a group a row is dragged anywhere
// on it into order: the bottom mod loads first, so the top one wins where two change the same thing (ModCatalog.
// LoadOrder). Switches, the order and each pack's options are saved in hellforge-mods.json (ModSettings). A session
// loads the enabled mods when it starts, so the page is locked while one runs (docs/mods.md). The page follows the
// mods folder: a mod dropped into it (or removed) shows at once.
public partial class MainWindow
{
    readonly List<Control> modSwitches = [];
    readonly HashSet<string> modsExpanded = [];
    readonly Dictionary<(string Path, int Width), (DateTime Written, Bitmap? Image)> modImages = [];
    bool modsLocked;
    ModDrag? modDrag;
    bool modDragged; // a drag just ended: the click it ends with does not open the row's details
    readonly Dictionary<Border, TextBlock> modNumbers = []; // each row's place number, renumbered when a row moves
    FileSystemWatcher? modsWatcher;
    DispatcherTimer? modsChanged;

    // A row being dragged: its group's rows (top first), where it was, and where it would go.
    sealed class ModDrag(string id, StackPanel panel, List<(ModManifest Mod, Border Row)> rows, int from)
    {
        public string Id { get; } = id;
        public StackPanel Panel { get; } = panel;
        public List<(ModManifest Mod, Border Row)> Rows { get; } = rows;
        public int From { get; } = from;
        public int To { get; set; } = from;
    }

    static readonly BoxShadows DropAbove = BoxShadows.Parse("0 -4 0 0 #E5392B"), DropBelow = BoxShadows.Parse("0 4 0 0 #E5392B");
    // a row's card, and the row being dragged (the app's accent wash and red)
    static readonly IBrush RowFill = Solid("#191C21"), RowEdge = Solid("#282C33"), DragFill = Solid("#2A1714"), DragEdge = Solid("#E5392B");

    void InitializeMods()
    {
        OpenModsButton.Click += (_, _) => { if (paths.ModsFolder != null) OpenFolder(paths.ModsFolder); };
        RefreshModsButton.Click += (_, _) => RefreshMods();
        AddModsButton.Click += async (_, _) => await PickMods();
        OpenModsButton.IsEnabled = RefreshModsButton.IsEnabled = paths.ModsFolder != null;
        AddModsButton.IsEnabled = paths.InstallRoot != null;
        WatchMods();
    }

    // Changes in the mods folder refresh the page shortly after they stop (a copy writes many files); HellForge's own
    // writes there are left out: dot folders (.unpacked: zips' unpacked copies) and dot files, and the page's own saves of its
    // settings (another program's changes to them do refresh it).
    void WatchMods()
    {
        if (paths.ModsFolder == null) return;
        try
        {
            Directory.CreateDirectory(paths.ModsFolder);
            var folder = Path.GetFullPath(paths.ModsFolder);
            modsChanged = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
            modsChanged.Tick += (_, _) =>
            {
                if (modDrag != null) return; // after the drag, at the next tick
                modsChanged.Stop();
                if (ModsPage.IsVisible) RefreshMods(); // else showing the page refreshes it
            };
            void Changed(string path)
            {
                var relative = Path.GetRelativePath(folder, path).Replace('\\', '/');
                var name = Path.GetFileName(relative);
                if (relative.Split('/').Any(p => p.StartsWith('.')) || name.StartsWith(ModSettings.LegacyFileName, StringComparison.OrdinalIgnoreCase)) return;
                // Office's lock and temporary files come and go as a document kept beside a mod is opened and saved
                if (name.StartsWith("~$", StringComparison.Ordinal) || name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) return;
                // the page's own saves are drawn already; another program's (HellForge.Cli, a mod manager) are loaded
                if (name.StartsWith(ModSettings.FileName, StringComparison.OrdinalIgnoreCase) &&
                    (!name.Equals(ModSettings.FileName, StringComparison.OrdinalIgnoreCase) || OwnSettings(path))) return;
                Dispatcher.UIThread.Post(() => { modsChanged.Stop(); modsChanged.Start(); });
            }
            modsWatcher = new FileSystemWatcher(folder) { IncludeSubdirectories = true, InternalBufferSize = 64 * 1024,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size };
            modsWatcher.Created += (_, e) => Changed(e.FullPath);
            modsWatcher.Changed += (_, e) => Changed(e.FullPath);
            modsWatcher.Deleted += (_, e) => Changed(e.FullPath);
            modsWatcher.Renamed += (_, e) => { Changed(e.OldFullPath); Changed(e.FullPath); };
            modsWatcher.Error += (_, _) => Dispatcher.UIThread.Post(() => { modsChanged.Stop(); modsChanged.Start(); });
            modsWatcher.EnableRaisingEvents = true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Append("The Mods page will not follow the mods folder (use Refresh): " + error.Message, Orange);
        }
    }

    // The settings file holds what this launcher saved last (ModSettings.LastWritten). Unreadable counts as changed.
    static bool OwnSettings(string path)
    {
        try { return File.ReadAllText(path) == ModSettings.LastWritten; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    void RefreshMods()
    {
        modDrag = null;
        ModsList.Children.Clear();
        modSwitches.Clear();
        modNumbers.Clear();
        if (paths.ModsFolder == null)
        {
            ModsSummary.Text = "Mods load in installations made with HellForge-Setup, from the installation's mods folder.";
            ModsDigestText.Text = "";
            return;
        }
        List<ModManifest> mods;
        List<string> problems;
        try { (mods, problems) = ModCatalog.Load(paths.ModsFolder); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            ModsSummary.Text = "Could not read the mods folder: " + Privacy.Scrub(error.Message);
            return;
        }
        // Local Stats is the in-game editor's (its saved changes, always on, never compared): not listed, so nobody thinks
        // friends must match it
        mods = mods.Where(m => m.Id != ModCatalog.LocalStatsId).ToList();
        var active = ModCatalog.Active(mods);
        ModsSummary.Text = mods.Count == 0 && problems.Count == 0
            ? "No mods yet. Add mods... takes HellForge mods (their .zip, .rar or .7z, or a folder mod's hellforge-mod.json) and cosmetic packs (.zip, .rar, .7z or the pack's manifest.json), and copies them into the mods folder."
            : $"{active.Count} of {mods.Count} mod{(mods.Count == 1 ? "" : "s")} on. Drag a mod to change the order: the bottom one loads first, the top one wins. " +
              "Changes apply to the next session" + (modsLocked ? "; the running session keeps the mods it started with." : ".");
        // what friends compare (ModCatalog.Digest)
        var gameplay = active.Count(m => m.Gameplay);
        string compared;
        try
        {
            compared = gameplay == 0 ? "No gameplay mods to compare: you can play with friends who have none."
                : $"Modlist code {ModCatalog.Digest(ModCatalog.LoadOrder(active))[..8]}: friends need the same {gameplay} gameplay mod{(gameplay == 1 ? "" : "s")}; the order is up to you.";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // a file in a mod's folder that another program holds (issue 18): the list still shows
            compared = "Modlist code unavailable: a mod's file could not be read (" + Privacy.Scrub(error.Message) + "). Close the program using it, then Refresh.";
        }
        ModsDigestText.Text = compared;

        // the folder's problems, and what the last Add mods refused (until the next one)
        var notices = problems.Concat(modsRefused.Select(p => "Not added: " + p)).ToList();
        if (notices.Count > 0)
        {
            var list = new StackPanel { Spacing = 4 };
            list.Children.Add(new TextBlock { Text = "MOD NOTICES", Classes = { "h2" }, Margin = new(0), Foreground = Red });
            foreach (var notice in notices) list.Children.Add(new TextBlock { Text = notice, TextWrapping = TextWrapping.Wrap, Foreground = Red });
            ModsList.Children.Add(new Border { Classes = { "card" }, Child = list });
        }
        AddPendingCard();
        AddModKitCards(mods.Select(m => Path.GetFullPath(m.Folder)).ToList());
        if (mods.Count > 0)
        {
            var number = 0;
            AddModGroup(mods, mods.Where(m => m.Gameplay).ToList(), "GAMEPLAY", "Friends need the same ones; keeping the same order avoids surprises.",
                "No gameplay mods.", ref number);
            AddModGroup(mods, mods.Where(m => m.Cosmetic).ToList(), "COSMETIC", "Only on your PC: friends need not have the same ones.",
                "No cosmetic mods. Add a cosmetic pack (its .zip, .rar, .7z or manifest.json) with Add mods...", ref number);
        }
        AddModSyncCard(paths.ModsFolder);
        SetModsLocked(modsLocked);
    }

    // Mod sync (ModSyncPlan, docs/mods.md "Playing with other mods"): whether players who join may download this player's
    // mods, the mods whose lobbies are never offered (blocked from the in-game MOD SYNC prompt), and the downloaded mods.
    void AddModSyncCard(string modsFolder)
    {
        var settings = ModSettings.Read(modsFolder);
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(new TextBlock { Text = "MOD SYNC", Classes = { "h2" }, Margin = new(0) });
        panel.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" }, Text =
            "Join players who run other gameplay mods: HellForge downloads the host's mods and restarts the game with them." });
        // What downloads from whom (ModSyncPlan), folded away: the rules matter once, the switches every day.
        var rules = new StackPanel { Spacing = 4, Margin = new(12, 0, 0, 4), IsVisible = false };
        foreach (var rule in new[]
        {
            "From any host: what HellForge checks itself (stats, network field types, data specs that don't replace values).",
            "From a Steam friend only: mods that run code (a DLL, sources or Lua) or carry data HellForge cannot check (data patches, raw stats, " +
                "component writes, data specs that replace values), and only when HellForge has verified that exact version or you trusted it on the prompt.",
            "Anything else you install yourself: a mod's GET IT button shows where, when its author gave a link."
        })
            rules.Children.Add(new TextBlock { Text = "•  " + rule, TextWrapping = TextWrapping.Wrap, Classes = { "muted" } });
        var rulesButton = new Button { Content = "How downloads are checked ▸", Classes = { "flat" }, Padding = new(0), HorizontalAlignment = HorizontalAlignment.Left };
        rulesButton.Click += (_, _) =>
        {
            rules.IsVisible = !rules.IsVisible;
            rulesButton.Content = rules.IsVisible ? "How downloads are checked ▾" : "How downloads are checked ▸";
        };
        panel.Children.Add(rulesButton);
        panel.Children.Add(rules);

        panel.Children.Add(Section("When I host"));
        panel.Children.Add(SettingSwitch("Share my gameplay mods with players who join", "Mods that run code or carry unchecked data go to Steam friends only.",
            settings.Share, on => ModSettings.SetShare(modsFolder, on), "the mod sync setting"));
        // a Public lobby is listed on other HellForge players' Galactic Map unless this is off (DirectoryService)
        panel.Children.Add(SettingSwitch("List my Public lobbies on the Galactic Map", "Other HellForge players can find and join them there.",
            settings.ListPublic, on => ModSettings.SetListPublic(modsFolder, on), "the public lobby setting"));
        // friends keep their own in-game editor changes unless this is on (StatsShare); either way every game loads the
        // packages the others' changes need (AssetShare)
        panel.Children.Add(SettingSwitch("Everyone plays with my editor changes", "Off: each player keeps their own changes.",
            settings.HostStats, on => ModSettings.SetHostStats(modsFolder, on), "the shared stats setting"));

        panel.Children.Add(Section("When I join"));
        // joiners of a host who shares edited stats play with them: such public lobbies are hidden on the Galactic Map unless shown
        panel.Children.Add(SettingSwitch("Show lobbies whose host shares edited stats", "You would play with the host's changes, not your own.",
            settings.ShowEditedStats, on => ModSettings.SetShowEditedStats(modsFolder, on), "the mod sync setting"));

        // Trusted (TRUST on the in-game prompt): each exact mod, who hosted it and when. Untrusting it deletes its downloaded
        // copy too, so no synced session runs it again unless it is trusted (or verified) again.
        panel.Children.Add(Section(settings.Trusted.Count == 0 ? "Trusted mods" : $"Trusted mods ({settings.Trusted.Count})"));
        panel.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" }, Text = settings.Trusted.Count == 0
            ? "None. TRUST on the in-game MOD SYNC prompt lets a Steam friend's mod download that HellForge has not verified."
            : "These exact versions download from Steam friends without HellForge's check. They run their own code on your PC." });
        foreach (var trusted in settings.Trusted)
        {
            var row = new DockPanel();
            var untrust = new Button { Content = "Untrust", Classes = { "flat" }, IsEnabled = !modsLocked, VerticalAlignment = VerticalAlignment.Center };
            ToolTip.SetTip(untrust, "Take the trust back and delete the copy downloaded from your friend.");
            untrust.Click += (_, _) =>
            {
                try
                {
                    ModSettings.Untrust(modsFolder, trusted.Id, trusted.Hash);
                    SyncedStore.Forget(modsFolder, trusted.Id, trusted.Hash);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not untrust " + trusted.Id + ": " + error.Message, Red); }
                RefreshMods();
            };
            DockPanel.SetDock(untrust, Dock.Right);
            row.Children.Add(untrust);
            var since = DateTime.TryParse(trusted.Since, System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.RoundtripKind, out var when)
                ? "  ·  " + when.ToLocalTime().ToString("yyyy-MM-dd") : "";
            // the host's name without its Steam account id ("Riggs (11000010838EA1C)"), which the tooltip keeps
            var from = System.Text.RegularExpressions.Regex.Replace(trusted.From, @"\s*\([0-9A-Fa-f]{6,}\)$", "");
            var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(new TextBlock { Text = $"{trusted.Name} {trusted.Version}", TextWrapping = TextWrapping.Wrap });
            text.Children.Add(new TextBlock { Text = $"from {from}{since}", Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap });
            ToolTip.SetTip(text, $"{trusted.Id}, from {trusted.From}. Content hash {trusted.Hash}");
            row.Children.Add(text);
            panel.Children.Add(row);
        }

        panel.Children.Add(Section("Blocked mods"));
        panel.Children.Add(new TextBlock { TextWrapping = TextWrapping.Wrap, Classes = { "muted" }, Text = settings.Blocked.Count == 0
            ? "None. BLOCK on the in-game MOD SYNC prompt hides lobbies that run a mod."
            : "Lobbies running these mods are not offered." });
        foreach (var id in settings.Blocked.Order(StringComparer.Ordinal))
        {
            var row = new DockPanel();
            var unblock = new Button { Content = "Unblock", Classes = { "flat" } };
            unblock.Click += (_, _) =>
            {
                try { ModSettings.SetBlocked(modsFolder, id, false); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not unblock " + id + ": " + error.Message, Red); }
                RefreshMods();
            };
            DockPanel.SetDock(unblock, Dock.Right);
            row.Children.Add(unblock);
            row.Children.Add(new TextBlock { Text = id, VerticalAlignment = VerticalAlignment.Center });
            panel.Children.Add(row);
        }

        var store = SyncedStore.Root(modsFolder);
        var stored = Directory.Exists(store) ? Directory.GetDirectories(store).Where(d => !Path.GetFileName(d).StartsWith('.')).ToList() : [];
        if (stored.Count > 0)
        {
            panel.Children.Add(Section("Downloaded mods"));
            long bytes = 0;
            try { bytes = stored.Sum(d => Directory.GetFiles(d, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            var row = new DockPanel();
            var clear = new Button { Content = "Delete downloaded mods", Classes = { "flat" }, IsEnabled = !modsLocked };
            clear.Click += (_, _) =>
            {
                try { Directory.Delete(store, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not delete the downloaded mods: " + error.Message, Red); }
                RefreshMods();
            };
            DockPanel.SetDock(clear, Dock.Right);
            row.Children.Add(clear);
            row.Children.Add(new TextBlock { Text = $"{stored.Count} mod{(stored.Count == 1 ? "" : "s")} from hosts ({bytes / 1048576.0:0.0} MB), used only when you play with their mods.",
                TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center, Classes = { "muted" } });
            panel.Children.Add(row);
        }
        ModsList.Children.Add(new Border { Classes = { "card" }, Child = panel });
    }

    // A part of a card under its title: a small heading with room above it.
    static TextBlock Section(string title) => new() { Text = title, FontWeight = FontWeight.SemiBold, Margin = new(0, 10, 0, 0) };

    // A switch with its label and a one-line hint under it, saved as it changes (what names the setting in a failure).
    Control SettingSwitch(string label, string hint, bool isChecked, Action<bool> save, string what)
    {
        var toggle = new ToggleSwitch { IsChecked = isChecked, OnContent = null, OffContent = null, MinWidth = 0, VerticalAlignment = VerticalAlignment.Top };
        toggle.IsCheckedChanged += (_, _) =>
        {
            try { save(toggle.IsChecked == true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save " + what + ": " + error.Message, Red); }
        };
        var text = new StackPanel { Spacing = 1, Margin = new(8, 4, 0, 0) };
        text.Children.Add(new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap });
        text.Children.Add(new TextBlock { Text = hint, Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap });
        var row = new DockPanel();
        DockPanel.SetDock(toggle, Dock.Left);
        row.Children.Add(toggle);
        row.Children.Add(text);
        return row;
    }

    void AddModGroup(List<ModManifest> all, List<ModManifest> group, string title, string hint, string empty, ref int number)
    {
        var header = new DockPanel { Margin = new(2, 6, 2, 0) };
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        name.Children.Add(new TextBlock { Text = title, Classes = { "h2" }, Margin = new(0), VerticalAlignment = VerticalAlignment.Center });
        name.Children.Add(new TextBlock { Text = group.Count.ToString(), FontSize = 12, Foreground = Light, VerticalAlignment = VerticalAlignment.Center });
        DockPanel.SetDock(name, Dock.Left);
        header.Children.Add(name);
        header.Children.Add(new TextBlock { Text = hint, Classes = { "muted" }, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center });
        var section = new StackPanel { Spacing = 6 };
        section.Children.Add(header);
        section.Children.Add(new Border { Height = 1, Background = Solid("#282C33"), Margin = new(0, 0, 0, 2) });
        if (group.Count == 0) section.Children.Add(new TextBlock { Text = empty, Classes = { "muted" }, FontSize = 12, Margin = new(2, 4) });
        var panel = new StackPanel { Spacing = 4 };
        var rows = new List<(ModManifest Mod, Border Row)>();
        foreach (var mod in group)
        {
            var row = ModRow(mod, ++number, panel, rows, all);
            rows.Add((mod, row));
            panel.Children.Add(row);
        }
        section.Children.Add(panel);
        ModsList.Children.Add(section);
    }

    Border ModRow(ModManifest mod, int number, StackPanel panel, List<(ModManifest Mod, Border Row)> rows, List<ModManifest> all)
    {
        var grid = new Grid { ColumnDefinitions = new("Auto,Auto,Auto,Auto,*,Auto,Auto"), RowDefinitions = new("Auto,Auto") };
        var row = new Border
        {
            Background = RowFill, BorderBrush = RowEdge, BorderThickness = new(1), CornerRadius = new(4),
            Padding = new(6, 8, 8, 8), Child = grid, Opacity = mod.Enabled ? 1 : 0.55
        };

        // the row drags within its group; the handle marks it
        var handle = new TextBlock
        {
            Text = "⠿", FontSize = 18, Width = 22, Foreground = Grey, TextAlignment = TextAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center, Cursor = new Cursor(StandardCursorType.SizeNorthSouth),
            Background = Brushes.Transparent
        };
        ToolTip.SetTip(handle, "Drag the mod to change the load order: higher wins.");
        Draggable(row, mod, panel, rows, all);
        grid.Children.Add(handle);

        var place = new TextBlock { Text = number.ToString(), Width = 26, FontSize = 12, Foreground = Grey, TextAlignment = TextAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center, Margin = new(0, 0, 8, 0) };
        modNumbers[row] = place;
        Grid.SetColumn(place, 1);
        grid.Children.Add(place);

        var toggle = new ToggleSwitch { IsChecked = mod.Enabled, OnContent = null, OffContent = null, MinWidth = 0, VerticalAlignment = VerticalAlignment.Center, Margin = new(0, 0, 4, 0) };
        toggle.IsCheckedChanged += (_, _) =>
        {
            if (paths.ModsFolder == null || modsLocked) return;
            try { ModSettings.SetEnabled(paths.ModsFolder, mod.Id, toggle.IsChecked == true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save the mod switch: " + error.Message, Red); }
            RefreshMods();
        };
        modSwitches.Add(toggle);
        Grid.SetColumn(toggle, 2);
        grid.Children.Add(toggle);

        var thumb = Thumbnail(mod);
        Grid.SetColumn(thumb, 3);
        grid.Children.Add(thumb);

        // name and one line of description; a click shows the details
        var build = BuildState(mod);
        var expanded = modsExpanded.Contains(mod.Id) || removeAsked == mod.Id || build?.Colour == Red;
        var text = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Margin = new(12, 0, 8, 0), Background = Brushes.Transparent, Cursor = new Cursor(StandardCursorType.Hand) };
        var title = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        title.Children.Add(new TextBlock { Text = mod.Name, FontWeight = FontWeight.SemiBold, FontSize = 14, VerticalAlignment = VerticalAlignment.Center });
        if (mod.Version.Length > 0) title.Children.Add(new TextBlock { Text = mod.Version, FontSize = 12, Classes = { "muted" }, VerticalAlignment = VerticalAlignment.Center });
        text.Children.Add(title);
        var summary = mod.Pack is { } p && p.Options.Count > 0 ? $"{p.Options.Count(o => !mod.PackChoice.Off.Contains(o.Name))} of {p.Options.Count} options on" + (mod.Description.Length > 0 ? " · " + mod.Description : "")
            : mod.Description;
        if (summary.Length > 0)
            text.Children.Add(new TextBlock { Text = summary, FontSize = 12, Classes = { "muted" }, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1, IsVisible = !expanded });
        text.Tapped += (_, _) =>
        {
            if (modDragged) { modDragged = false; return; } // the end of a drag, not a click
            if (!modsExpanded.Remove(mod.Id)) modsExpanded.Add(mod.Id);
            RefreshMods();
        };
        ToolTip.SetTip(text, expanded ? "Click to hide the details." : "Click for the details.");
        Grid.SetColumn(text, 4);
        grid.Children.Add(text);

        var tags = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center };
        foreach (var (label, colour) in Tags(mod, build)) tags.Children.Add(ModTag(label, colour));
        Grid.SetColumn(tags, 5);
        grid.Children.Add(tags);

        var menu = new Button { Content = "⋮", Classes = { "flat" }, FontSize = 18, Padding = new(10, 0), Margin = new(6, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center,
            Flyout = ModMenu(mod, rows, all) };
        ToolTip.SetTip(menu, "More: build, zip, open its folder, remove.");
        row.ContextFlyout = ModMenu(mod, rows, all);
        Grid.SetColumn(menu, 6);
        grid.Children.Add(menu);

        if (expanded)
        {
            var details = Details(mod, build);
            Grid.SetRow(details, 1);
            Grid.SetColumn(details, 4);
            Grid.SetColumnSpan(details, 3);
            grid.Children.Add(details);
        }
        return row;
    }

    // Short markers at the row's right; the details say more.
    List<(string Label, string Colour)> Tags(ModManifest mod, (string Text, IBrush Colour)? build)
    {
        var tags = new List<(string, string)>();
        if (build?.Colour == Red) tags.Add(("DID NOT COMPILE", "#FF6054"));
        if (BundledMods.ShippedFiles(mod.Folder) != null) tags.Add(("BUNDLED", "#9AA0A8"));
        if (mod.Pack != null) tags.Add(("PACK", "#4FA3FF"));
        if (mod.Lua) tags.Add(("LUA", "#F2A541"));
        if (mod.HasCode) tags.Add(("CODE", "#E05591"));
        if (mod.HasNative) tags.Add(("NATIVE", "#9B85FF"));
        if (mod.Stats.Count > 0) tags.Add(("STATS", "#9B85FF"));
        if (mod.HasDataSpec || mod.DataFiles.Count > 0) tags.Add(("DATA", "#5FD08A"));
        if (mod.Archive != null) tags.Add(("ZIP", "#9AA0A8"));
        return tags;
    }

    static Border ModTag(string text, string colour)
    {
        var c = Color.Parse(colour);
        return new Border
        {
            BorderBrush = new SolidColorBrush(c), Background = new SolidColorBrush(Color.FromArgb(0x22, c.R, c.G, c.B)), BorderThickness = new(1),
            CornerRadius = new(2), Padding = new(5, 1), VerticalAlignment = VerticalAlignment.Center,
            Child = new TextBlock { Text = text, FontSize = 10, FontWeight = FontWeight.Bold, LetterSpacing = 1, Foreground = new SolidColorBrush(c) }
        };
    }

    // The mod's picture (its manifest's icon, a pack's IconPath), else its initials.
    Control Thumbnail(ModManifest mod)
    {
        var box = new Border { Width = 40, Height = 40, CornerRadius = new(3), ClipToBounds = true, Background = Solid("#23262C"), VerticalAlignment = VerticalAlignment.Center };
        if (mod.Icon != null && Picture(mod.Icon, 80) is { } image) box.Child = new Image { Source = image, Stretch = Stretch.UniformToFill };
        else
        {
            var words = mod.Name.Split(' ', StringSplitOptions.RemoveEmptyEntries).Where(w => char.IsLetterOrDigit(w[0])).Take(2).Select(w => char.ToUpperInvariant(w[0]));
            box.Child = new TextBlock { Text = string.Concat(words), FontWeight = FontWeight.Bold, FontSize = 14, Foreground = Grey,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        }
        return box;
    }

    // An image from a mod's folder, decoded small and kept while the file is unchanged; null when it does not decode.
    Bitmap? Picture(string path, int width)
    {
        try
        {
            var written = File.GetLastWriteTimeUtc(path);
            if (modImages.TryGetValue((path, width), out var known) && known.Written == written) return known.Image;
            Bitmap? image = null;
            try
            {
                using var stream = File.OpenRead(path);
                image = Bitmap.DecodeToWidth(stream, width, BitmapInterpolationMode.HighQuality);
            }
            catch (Exception) { } // a third party's picture that does not decode: the row shows initials
            modImages[(path, width)] = (written, image);
            return image;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    // A source mod's last build (HellForge builds it before each session): when, or the compiler's message.
    (string Text, IBrush Colour)? BuildState(ModManifest mod)
    {
        if (!mod.HasSource || paths.InstallRoot == null) return null;
        var record = ModBuilds.Record(new InstallLayout(paths.InstallRoot), mod.Id);
        bool current;
        try { current = record != null && Sdk is { } sdk && ModKit.ToolsReady(sdk) && record.Key == ModBuilds.Key(mod.Folder, sdk, SupportedBuild.Current.BuildId); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return ("Its sources could not be read: " + Privacy.Scrub(error.Message), Orange);
        }
        return record == null ? ("Built from its sources when a session starts.", Light)
            : record.Error != null && current ? ("Did not compile; it stays off until fixed:\n" + record.Error, Red)
            : record.Error != null ? ("Its last build did not compile; the sources changed since, so the next session builds it again.", Orange)
            : current ? ($"Built {record.Built.ToLocalTime():g}; sessions reuse this build until its sources change.", Light)
            : ("Its sources, the SDK or the game build changed: the next session builds it again.", Orange);
    }

    Control Details(ModManifest mod, (string Text, IBrush Colour)? build)
    {
        var details = new StackPanel { Spacing = 8, Margin = new(12, 10, 0, 2) };
        if (mod.Description.Length > 0) details.Children.Add(new TextBlock { Text = mod.Description, TextWrapping = TextWrapping.Wrap, FontSize = 13, Foreground = Light });
        var changes = ModChanges(mod);
        if (changes.Count > 0) details.Children.Add(new TextBlock { Text = string.Join("\n", changes), FontSize = 12, Foreground = Light, TextWrapping = TextWrapping.Wrap });
        if (build is { } b)
            details.Children.Add(new TextBlock { Text = b.Text, FontSize = 12, Foreground = b.Colour, TextWrapping = TextWrapping.Wrap,
                FontFamily = b.Colour == Red ? new FontFamily("Consolas") : FontFamily.Default });
        if (mod.Pack is { Options.Count: > 0 } pack) details.Children.Add(PackOptions(mod, pack));
        // where players get it (its manifest's "url", an https:// address): opened in the browser
        if (mod.Url is { } url)
        {
            var link = new Button { Content = url, Classes = { "flat" }, FontSize = 12, Padding = new(0), HorizontalAlignment = HorizontalAlignment.Left,
                Foreground = Solid("#4FA3FF"), Cursor = new Cursor(StandardCursorType.Hand) };
            ToolTip.SetTip(link, "Where to get this mod: opens in your browser.");
            link.Click += (_, _) => OpenLink(url);
            details.Children.Add(link);
        }
        details.Children.Add(new TextBlock { Text = $"{mod.Id}  ·  {mod.Archive ?? Path.GetFileName(mod.Folder)}", FontSize = 11, Classes = { "muted" } });
        if (RemoveConfirmation(mod) is { } confirm) details.Children.Add(confirm);
        return details;
    }

    // The one-line summaries of what a mod changes: network field types, component writes, stats, data files, a data spec, code.
    static List<string> ModChanges(ModManifest mod)
    {
        var changes = new List<string>();
        foreach (var type in mod.NetworkFieldTypes) changes.Add($"network field {type.Name}: {type.ExpectBits} -> {type.Bits} bits");
        foreach (var write in mod.ComponentWrites)
            foreach (var field in write.Fields) changes.Add($"{write.Component} {field.Name}: {field.Expect} -> {field.Value}");
        if (mod.Stats.Count > 0) changes.Add($"{mod.Stats.Count} stats change{(mod.Stats.Count == 1 ? "" : "s")}");
        if (mod.DataFiles.Count > 0) changes.Add($"{mod.DataFiles.Count} game data file{(mod.DataFiles.Count == 1 ? "" : "s")}");
        if (mod.HasDataSpec) changes.Add("game data built from your game copy (a data spec)");
        if (mod.HasCode) changes.Add($"runs code inside the game: {mod.CodeDll} (only enable mods from people you trust)");
        return changes;
    }

    // A pack's options: each switched on or off, with its variant (sub-option) where it has some; saved per pack.
    Control PackOptions(ModManifest mod, CosmeticPacks.Pack pack)
    {
        void Save(CosmeticPacks.Choice choice)
        {
            if (paths.ModsFolder == null || modsLocked) return;
            try { ModSettings.SetChoice(paths.ModsFolder, mod.Id, choice); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save the pack's options: " + error.Message, Red); }
            RefreshMods();
        }
        var off = mod.PackChoice.Off.ToHashSet(StringComparer.Ordinal);
        var sub = new Dictionary<string, string>(mod.PackChoice.Sub, StringComparer.Ordinal);
        var panel = new StackPanel { Spacing = 6 };
        var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        header.Children.Add(new TextBlock { Text = "OPTIONS", Classes = { "h2" }, Margin = new(0), VerticalAlignment = VerticalAlignment.Center });
        var allOn = new Button { Content = "All on", Classes = { "flat" }, IsEnabled = !modsLocked && off.Count > 0 };
        var allOff = new Button { Content = "All off", Classes = { "flat" }, IsEnabled = !modsLocked && off.Count < pack.Options.Count };
        allOn.Click += (_, _) => Save(new([], sub));
        allOff.Click += (_, _) => Save(new(pack.Options.Select(o => o.Name).ToHashSet(StringComparer.Ordinal), sub));
        header.Children.Add(allOn);
        header.Children.Add(allOff);
        panel.Children.Add(header);
        var options = new WrapPanel { Orientation = Orientation.Horizontal };
        foreach (var option in pack.Options)
        {
            var cell = new StackPanel { Width = 250, Spacing = 2, Margin = new(0, 0, 10, 8) };
            var check = new CheckBox { Content = option.Name, IsChecked = !off.Contains(option.Name), IsEnabled = !modsLocked };
            check.IsCheckedChanged += (_, _) =>
            {
                var next = off.ToHashSet(StringComparer.Ordinal);
                if (check.IsChecked == true) next.Remove(option.Name); else next.Add(option.Name);
                Save(new(next, sub));
            };
            ToolTip.SetTip(check, OptionTip(option));
            cell.Children.Add(check);
            if (option.SubOptions.Count > 0)
            {
                var chosen = sub.TryGetValue(option.Name, out var name) ? name : option.SubOptions[0].Name;
                var variants = new ComboBox
                {
                    ItemsSource = option.SubOptions.Select(s => s.Name).ToList(), SelectedItem = option.SubOptions.Any(s => s.Name == chosen) ? chosen : option.SubOptions[0].Name,
                    IsEnabled = !modsLocked && !off.Contains(option.Name), FontSize = 12, MinWidth = 200, Margin = new(28, 0, 0, 0)
                };
                variants.SelectionChanged += (_, _) =>
                {
                    if (variants.SelectedItem is not string picked || picked == chosen) return;
                    Save(new(off, new Dictionary<string, string>(sub, StringComparer.Ordinal) { [option.Name] = picked }));
                };
                cell.Children.Add(variants);
            }
            options.Children.Add(cell);
        }
        panel.Children.Add(options);
        return panel;
    }

    // An option's preview: its picture and description, as its pack shows them.
    object? OptionTip(CosmeticPacks.Option option)
    {
        var tip = new StackPanel { Spacing = 6, MaxWidth = 340 };
        if (option.Image != null && Picture(option.Image, 320) is { } image) tip.Children.Add(new Image { Source = image, Width = 320, Stretch = Stretch.Uniform });
        if (option.Description.Length > 0) tip.Children.Add(new TextBlock { Text = option.Description, TextWrapping = TextWrapping.Wrap });
        return tip.Children.Count > 0 ? tip : null;
    }

    // The ⋮ menu (and the row's right click): details, order, build, zip, folder, remove.
    MenuFlyout ModMenu(ModManifest mod, List<(ModManifest Mod, Border Row)> rows, List<ModManifest> all)
    {
        var menu = new MenuFlyout();
        var details = new MenuItem { Header = modsExpanded.Contains(mod.Id) ? "Hide details" : "Details" };
        details.Click += (_, _) => { if (!modsExpanded.Remove(mod.Id)) modsExpanded.Add(mod.Id); RefreshMods(); };
        menu.Items.Add(details);
        // the group's rows are all there once the menu opens (this row is made before it joins them)
        int At() => rows.FindIndex(r => r.Mod.Id == mod.Id);
        var up = new MenuItem { Header = "Move up" };
        var down = new MenuItem { Header = "Move down" };
        menu.Opening += (_, _) =>
        {
            up.IsEnabled = !modsLocked && At() > 0;
            down.IsEnabled = !modsLocked && At() >= 0 && At() < rows.Count - 1;
        };
        up.Click += (_, _) => MoveMod(rows, At(), At() - 1, all);
        down.Click += (_, _) => MoveMod(rows, At(), At() + 2, all);
        menu.Items.Add(up);
        menu.Items.Add(down);
        menu.Items.Add(new Separator());
        // BUILD for a mod with sources; ZIP and REMOVE for the player's own mods (not the ones HellForge ships, which come
        // back by themselves); ZIP not for a zip, which is one already, nor a
        // cosmetic pack, which is shared as its author made it
        var own = paths.InstallRoot != null && BundledMods.ShippedFiles(mod.Folder) == null;
        if (Sdk != null && mod.HasSource) menu.Items.Add(BuildItem(mod));
        if (own && mod.Archive == null && mod.Pack == null) menu.Items.Add(ZipItem(mod));
        var folder = new MenuItem { Header = "Open folder" };
        folder.Click += (_, _) => OpenFolder(mod.Archive != null && paths.ModsFolder != null ? paths.ModsFolder : mod.Folder);
        menu.Items.Add(folder);
        if (own) menu.Items.Add(RemoveItem(mod));
        return menu;
    }

    // Saves the group's new order (the other group's as it is) and shows it.
    void MoveMod(List<(ModManifest Mod, Border Row)> rows, int from, int to, List<ModManifest> all)
    {
        if (paths.ModsFolder == null || modsLocked || from < 0 || from >= rows.Count) return;
        var at = Math.Clamp(to > from ? to - 1 : to, 0, rows.Count - 1);
        var cosmetic = rows[from].Mod.Cosmetic;
        // the rows move at once, renumbered; the page is loaded again once that is drawn (the modlist code, a pack's place)
        var numbers = rows.Select(r => modNumbers.GetValueOrDefault(r.Row)?.Text).ToList();
        var moved = rows[from];
        rows.RemoveAt(from);
        rows.Insert(at, moved);
        if (moved.Row.Parent is Panel panel && panel.Children.IndexOf(moved.Row) is var index and >= 0)
        {
            panel.Children.Move(index, index + (at - from));
            for (var i = 0; i < rows.Count; i++)
                if (modNumbers.TryGetValue(rows[i].Row, out var number)) number.Text = numbers[i];
        }
        var ids = rows.Select(r => r.Mod.Id);
        var order = cosmetic ? all.Where(m => m.Gameplay).Select(m => m.Id).Concat(ids) : ids.Concat(all.Where(m => m.Cosmetic).Select(m => m.Id));
        try { ModSettings.SetOrder(paths.ModsFolder, order.ToList()); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save the mod order: " + error.Message, Red); }
        Dispatcher.UIThread.Post(RefreshMods, DispatcherPriority.Background);
    }

    // Dragging a row from anywhere on it (not its switch or buttons, which take their own clicks): once the pointer has
    // moved a little with the button down, a red line shows where it goes; letting go there saves the order. A press that
    // does not move stays a click (the name opens the details).
    void Draggable(Border row, ModManifest mod, StackPanel panel, List<(ModManifest Mod, Border Row)> rows, List<ModManifest> all)
    {
        Point? pressed = null;
        row.PointerPressed += (_, e) =>
        {
            modDragged = false;
            pressed = null;
            if (modsLocked || paths.ModsFolder == null || rows.Count < 2 || !e.GetCurrentPoint(row).Properties.IsLeftButtonPressed) return;
            pressed = e.GetPosition(panel);
        };
        row.PointerMoved += (_, e) =>
        {
            if (pressed is not { } start) return;
            var y = e.GetPosition(panel).Y;
            if (modDrag == null)
            {
                if (!e.GetCurrentPoint(row).Properties.IsLeftButtonPressed) { pressed = null; return; }
                if (Math.Abs(y - start.Y) < 5 || modsLocked) return;
                var from = rows.FindIndex(r => r.Mod.Id == mod.Id);
                if (from < 0) { pressed = null; return; }
                modDrag = new(mod.Id, panel, rows, from);
                e.Pointer.Capture(row);
                // the row being moved lights up in the accent colours
                row.Background = DragFill;
                row.BorderBrush = DragEdge;
            }
            if (modDrag is not { } drag || drag.Id != mod.Id) return;
            var to = drag.Rows.Count(r => r.Row.Bounds.Top + r.Row.Bounds.Height / 2 < y);
            drag.To = to;
            // the line shows where it lands, its own place too (just above or below itself: nothing moves)
            for (var i = 0; i < drag.Rows.Count; i++) drag.Rows[i].Row.BoxShadow = default;
            if (drag.To < drag.Rows.Count) drag.Rows[drag.To].Row.BoxShadow = DropAbove;
            else drag.Rows[^1].Row.BoxShadow = DropBelow;
        };
        row.PointerReleased += (_, e) =>
        {
            pressed = null;
            if (modDrag is not { } drag || drag.Id != mod.Id) return;
            modDrag = null;
            modDragged = true;
            e.Pointer.Capture(null);
            EndDrag(drag);
            if (drag.To != drag.From && drag.To != drag.From + 1) MoveMod(drag.Rows, drag.From, drag.To, all);
        };
        row.PointerCaptureLost += (_, _) =>
        {
            pressed = null;
            if (modDrag is not { } drag || drag.Id != mod.Id) return;
            modDrag = null;
            EndDrag(drag);
        };
    }

    static void EndDrag(ModDrag drag)
    {
        foreach (var (_, row) in drag.Rows)
        {
            row.BoxShadow = default;
            row.Background = RowFill;
            row.BorderBrush = RowEdge;
        }
    }

    // Called with the session state every second; a running session keeps the mods it started with.
    void SetModsLocked(bool locked)
    {
        var changed = modsLocked != locked;
        modsLocked = locked;
        foreach (var toggle in modSwitches) toggle.IsEnabled = !locked;
        if (changed && ModsPage.IsVisible) RefreshMods();
    }
}
