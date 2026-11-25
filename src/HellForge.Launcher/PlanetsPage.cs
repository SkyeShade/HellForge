using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.Core.GameData;

namespace HellForge.Launcher;

// The Galactic war page (docs/planet-modifiers.md): the galaxy map of the war the next session plays, and beside it a WAR tab
// (the war choice, its server and the all-planets war's deals, from MainWindow.axaml) and a PLANET tab (the picked planet's
// enemy, subfactions and enemy groups, surge or none). Every planet change is saved to the installation's
// planet-modifiers.json at once; a running session's backend reads it again, and a host sends it to its lobby.
public partial class MainWindow
{
    const double MapSize = 1000;
    static readonly IBrush SuperEarthBrush = Solid("#5AB0F0"), TerminidBrush = Solid("#FFA235"), AutomatonBrush = Solid("#E5392B"),
        IlluminateBrush = Solid("#A77BFF"), LinkBrush = Solid("#30353D"), MapBrush = Solid("#0C0E11");
    static readonly string[] EnemyModes = ["As usual", "Surge (x10)", "None"];
    PlanetPlan planetPlan = PlanetPlan.Empty;
    IReadOnlyDictionary<int, PlanetView> planetViews = new Dictionary<int, PlanetView>();
    // the war's own, without the planet modifiers
    IReadOnlyDictionary<int, PlanetView> planetWarViews = new Dictionary<int, PlanetView>();
    int? selectedPlanet;
    readonly Dictionary<int, Ellipse> planetDots = [];
    readonly Dictionary<int, Ellipse> planetRings = [];
    // the factions' emblems from the installation's game data (WarEmblems), by race; none in a build without a game copy,
    // and then the map keeps its dots
    IReadOnlyDictionary<int, Geometry> emblemShapes = new Dictionary<int, Geometry>();
    readonly Dictionary<int, Avalonia.Controls.Shapes.Path> planetEmblems = [];
    readonly Canvas planetMap = new() { Width = MapSize, Height = MapSize, Background = MapBrush };
    readonly Ellipse planetCursor = new() { Width = 30, Height = 30, Stroke = Brushes.White, StrokeThickness = 3, IsHitTestVisible = false, IsVisible = false };
    readonly TextBlock planetLabel = new() { FontSize = 22, FontWeight = FontWeight.SemiBold, IsHitTestVisible = false, IsVisible = false };
    readonly TextBox planetSearch = new() { Watermark = "Find a planet or sector", Width = 230 };
    readonly CheckBox planetChangedOnly = new() { Content = "Only changed", VerticalAlignment = VerticalAlignment.Center };
    readonly StackPanel planetPanel = new() { Spacing = 10 };
    readonly Button warTab = new() { Classes = { "tab", "selected" }, Content = "WAR" };
    readonly Button planetTab = new() { Classes = { "tab" }, Content = "PLANET" };
    readonly ScrollViewer sideScroll = new() { Padding = new(0, 0, 12, 0) };
    // shown when the next session's war is not Custom planets, whose planets these are
    readonly Border planetInactive = new() { Classes = { "card" }, Padding = new(14, 10), Margin = new(0, 0, 0, 12), IsVisible = false };
    readonly TextBlock planetStatus = new() { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new(0, 10, 0, 0) };
    string planetSource = "";

    string PlanetFile => PlanetModifiers.PathFor(paths.HostRoot);

    void InitializePlanets()
    {
        var reset = new Button { Classes = { "ghost" }, Content = "Reset all", VerticalAlignment = VerticalAlignment.Center };
        reset.Click += (_, _) => SavePlanets(PlanetPlan.Empty, "Every planet is back to the war's own.");
        var tools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Bottom, Children = { planetSearch, planetChangedOnly, reset } };
        Grid.SetColumn(tools, 1);
        var header = new Grid
        {
            ColumnDefinitions = new("*,Auto"), Margin = new(0, 0, 0, 16),
            Children =
            {
                new StackPanel
                {
                    Children =
                    {
                        new TextBlock { Text = "GALACTIC WAR", Classes = { "eyebrow" } },
                        new TextBlock { Text = "War and planets", Classes = { "h1" } },
                        new TextBlock
                        {
                            Classes = { "muted" }, TextWrapping = TextWrapping.Wrap, Margin = new(0, 6, 0, 0), MaxWidth = 760, HorizontalAlignment = HorizontalAlignment.Left,
                            Text = "Pick the war your sessions play. With Custom planets, click a planet to choose who holds it and what fights there; " +
                                   "every HellForge player in a lobby you host plays with your planets.",
                        },
                    },
                },
                tools,
            },
        };
        DockPanel.SetDock(header, Dock.Top);
        DockPanel.SetDock(planetStatus, Dock.Bottom);
        var useCustom = new Button { Classes = { "primary" }, Content = "Play Custom planets", VerticalAlignment = VerticalAlignment.Center };
        useCustom.Click += (_, _) => WarCustomRadio.IsChecked = true; /* saves the war, as its radio does */
        // the map shows the war the next session plays: any change of it, or of the all-planets war's deals, redraws it
        foreach (var radio in new[] { WarHellForgeRadio, WarLiveRadio, WarFrozenRadio, WarOpenRadio, WarCustomRadio })
            radio.IsCheckedChanged += (_, _) => { if (radio.IsChecked == true && PlanetsPage.IsVisible) RefreshPlanetViews(); };
        foreach (var box in new[] { DealSubfactionsBox, DealSeafBox })
            box.IsCheckedChanged += (_, _) => { if (PlanetsPage.IsVisible) RefreshPlanetViews(); };
        SubfactionSeedBox.LostFocus += (_, _) => { if (PlanetsPage.IsVisible) RefreshPlanetViews(); };
        DockPanel.SetDock(useCustom, Dock.Right);
        planetInactive.Child = new DockPanel
        {
            Children =
            {
                useCustom,
                new TextBlock
                {
                    TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center, Margin = new(0, 0, 12, 0), Foreground = Orange,
                    Text = "Your planets are for the Custom planets war. The next session plays another war, shown here as it is.",
                },
            },
        };
        DockPanel.SetDock(planetInactive, Dock.Top);
        foreach (var planet in PlanetModifiers.Catalog.Planets)
            foreach (var link in planet.Links)
                if (PlanetModifiers.Catalog.Planet(link) is { } other)
                {
                    var (x1, y1) = MapPoint(planet);
                    var (x2, y2) = MapPoint(other);
                    planetMap.Children.Add(new Line { StartPoint = new(x1, y1), EndPoint = new(x2, y2), Stroke = LinkBrush, StrokeThickness = 1.5, IsHitTestVisible = false });
                }
        foreach (var planet in PlanetModifiers.Catalog.Planets)
        {
            var size = planet.Index == 0 ? 24 : 13;
            var (x, y) = MapPoint(planet);
            var dot = new Ellipse { Width = size, Height = size, Fill = SuperEarthBrush, Cursor = new Cursor(StandardCursorType.Hand) };
            Canvas.SetLeft(dot, x - size / 2.0);
            Canvas.SetTop(dot, y - size / 2.0);
            ToolTip.SetTip(dot, planet.Name);
            var index = planet.Index;
            dot.PointerPressed += (_, _) => SelectPlanet(index);
            var ring = new Ellipse { Width = size + 10, Height = size + 10, Stroke = Gold, StrokeThickness = 2.5, IsHitTestVisible = false, IsVisible = false };
            Canvas.SetLeft(ring, x - (size + 10) / 2.0);
            Canvas.SetTop(ring, y - (size + 10) / 2.0);
            var emblemSize = planet.Index == 0 ? 30 : 19;
            var emblem = new Avalonia.Controls.Shapes.Path { Width = emblemSize, Height = emblemSize, Stretch = Stretch.Uniform, IsHitTestVisible = false, IsVisible = false };
            Canvas.SetLeft(emblem, x - emblemSize / 2.0);
            Canvas.SetTop(emblem, y - emblemSize / 2.0);
            planetMap.Children.Add(ring);
            planetMap.Children.Add(dot);
            planetMap.Children.Add(emblem);
            planetDots[index] = dot;
            planetRings[index] = ring;
            planetEmblems[index] = emblem;
        }
        planetMap.Children.Add(planetCursor);
        planetMap.Children.Add(planetLabel);
        var map = new Border
        {
            Classes = { "card" }, Padding = new(6), ClipToBounds = true,
            Child = new Viewbox { Stretch = Stretch.Uniform, Child = planetMap },
        };
        // the war choice lives in MainWindow.axaml under this page; it moves into the WAR tab
        PlanetsPage.Children.Remove(WarSettings);
        warTab.Click += (_, _) => ShowSideTab(planet: false);
        planetTab.Click += (_, _) => ShowSideTab(planet: true);
        var tabs = new StackPanel { Orientation = Orientation.Horizontal, Margin = new(0, 0, 0, 12), Children = { warTab, planetTab } };
        DockPanel.SetDock(tabs, Dock.Top);
        var side = new Border { Classes = { "card" }, Padding = new(18, 14, 6, 14), Child = new DockPanel { Children = { tabs, sideScroll } } };
        map.Margin = new(16, 0, 0, 0);
        Grid.SetColumn(map, 1);
        var body = new Grid { ColumnDefinitions = new("400,*"), Children = { side, map } };
        ShowSideTab(planet: false);
        PlanetsPage.Children.Add(header);
        PlanetsPage.Children.Add(planetInactive);
        PlanetsPage.Children.Add(planetStatus);
        PlanetsPage.Children.Add(body);
        LoadEmblems();
        planetSearch.TextChanged += (_, _) => PaintPlanets();
        planetChangedOnly.IsCheckedChanged += (_, _) => PaintPlanets();
        planetSearch.KeyDown += (_, e) =>
        {
            if (e.Key != Key.Enter) return;
            var match = PlanetModifiers.Catalog.Planets.FirstOrDefault(Matches);
            if (match != null) SelectPlanet(match.Index);
        };
    }

    // The emblems are read off the UI thread (a first read scans the game's menus, under a second; then the cache). They are
    // decoration: any failure to read them leaves the dots.
    void LoadEmblems()
    {
        if (paths.InstallRoot is not { } root) return;
        var install = new InstallLayout(root);
        Task.Run(() =>
        {
            try { return WarEmblems.Load(System.IO.Path.Combine(install.Game, "data"), install.Temp); }
            catch (Exception) { return new Dictionary<int, WarEmblem>(); }
        }).ContinueWith(read => Dispatcher.UIThread.Post(() =>
        {
            var shapes = new Dictionary<int, Geometry>();
            foreach (var emblem in read.Result.Values)
                try { shapes[emblem.Race] = Shape(emblem); }
                catch (Exception error) when (error is FormatException or InvalidDataException or ArgumentException) { }
            emblemShapes = shapes;
            PaintPlanets();
            ShowPlanetPanel();
        }), TaskScheduler.Default);
    }

    static Geometry Shape(WarEmblem emblem)
    {
        var group = new GeometryGroup { FillRule = FillRule.NonZero };
        foreach (var part in emblem.Paths)
        {
            var geometry = Geometry.Parse(part.Data);
            if (part.Left != 0 || part.Top != 0) geometry.Transform = new TranslateTransform(part.Left, part.Top);
            group.Children.Add(geometry);
        }
        return group.Children.Count == 1 ? group.Children[0] : group;
    }

    // A faction's emblem at a size, or its coloured dot without one.
    Control FactionMark(int race, double size) => emblemShapes.TryGetValue(race, out var shape)
        ? new Avalonia.Controls.Shapes.Path { Data = shape, Fill = FactionBrush(race), Stretch = Stretch.Uniform, Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center }
        : new Ellipse { Width = size * 0.75, Height = size * 0.75, Fill = FactionBrush(race), VerticalAlignment = VerticalAlignment.Center };

    // Map units from the war's position (x and y from -1 to 1, y up).
    static (double X, double Y) MapPoint(PlanetInfo planet) => ((planet.X + 1) / 2 * MapSize, (1 - planet.Y) / 2 * MapSize);

    // The page shown: the file and the war read again (a session, the CLI or another launcher may have changed either).
    void ShowPlanets()
    {
        try
        {
            planetPlan = PlanetPlan.Read(PlanetFile, out var problems);
            planetStatus.Text = problems.Count > 0 ? "planet-modifiers.json: " + string.Join("; ", problems.Take(3)) : "";
            planetStatus.Foreground = problems.Count > 0 ? Orange : Grey;
        }
        catch (Exception error) when (error is System.Text.Json.JsonException or IOException or UnauthorizedAccessException)
        {
            planetPlan = PlanetPlan.Empty;
            planetStatus.Text = "planet-modifiers.json could not be read (" + error.Message + "). A change here replaces it.";
            planetStatus.Foreground = Red;
        }
        RefreshPlanetViews();
    }

    void RefreshPlanetViews()
    {
        (planetViews, planetSource, var active) = PlanetWarView.Read(paths, planetPlan);
        planetInactive.IsVisible = !active;
        planetWarViews = PlanetWarView.Read(paths, PlanetPlan.Empty).Planets;
        PaintPlanets();
        ShowPlanetPanel();
    }

    bool Matches(PlanetInfo planet)
    {
        var text = planetSearch.Text?.Trim() ?? "";
        return text.Length == 0 || planet.Name.Contains(text, StringComparison.OrdinalIgnoreCase) || planet.Sector.Contains(text, StringComparison.OrdinalIgnoreCase) ||
            planet.Index.ToString(System.Globalization.CultureInfo.InvariantCulture) == text;
    }

    static IBrush FactionBrush(int race) => race switch { 2 => TerminidBrush, 3 => AutomatonBrush, 4 => IlluminateBrush, _ => SuperEarthBrush };

    void PaintPlanets()
    {
        var changedOnly = planetChangedOnly.IsChecked == true;
        foreach (var planet in PlanetModifiers.Catalog.Planets) PaintPlanet(planet, changedOnly);
        if (selectedPlanet is { } index && PlanetModifiers.Catalog.Planet(index) is { } selected)
        {
            var (x, y) = MapPoint(selected);
            Canvas.SetLeft(planetCursor, x - 15);
            Canvas.SetTop(planetCursor, y - 15);
            planetLabel.Text = selected.Name;
            planetLabel.Foreground = Brushes.White;
            Canvas.SetLeft(planetLabel, Math.Min(x + 18, MapSize - 240));
            Canvas.SetTop(planetLabel, y - 15);
            planetCursor.IsVisible = planetLabel.IsVisible = true;
        }
        else planetCursor.IsVisible = planetLabel.IsVisible = false;
    }

    void PaintPlanet(PlanetInfo planet, bool changedOnly)
    {
        var dot = planetDots[planet.Index];
        var view = planetViews.GetValueOrDefault(planet.Index);
        var owner = view?.Owner ?? planet.Owner;
        var changed = planetPlan.For(planet.Index) != null;
        planetRings[planet.Index].IsVisible = changed;
        // an enemy's planet (and Super Earth itself) shows its emblem; the dot under it stays as what is clicked
        var emblem = planetEmblems[planet.Index];
        var shape = owner != PlanetModifiers.SuperEarth || planet.Index == 0 ? emblemShapes.GetValueOrDefault(owner) : null;
        emblem.IsVisible = shape != null;
        (emblem.Data, emblem.Fill) = (shape, FactionBrush(owner));
        dot.Fill = shape != null ? Brushes.Transparent : FactionBrush(owner);
        var shown = Matches(planet) && (!changedOnly || changed);
        dot.Opacity = emblem.Opacity = !shown ? 0.15 : view is { Playable: false } && planet.Index != 0 ? 0.55 : 1;
    }

    void SelectPlanet(int? index)
    {
        selectedPlanet = index;
        PaintPlanets();
        ShowPlanetPanel();
        ShowSideTab(planet: true);
    }

    void ShowSideTab(bool planet)
    {
        warTab.Classes.Set("selected", !planet);
        planetTab.Classes.Set("selected", planet);
        sideScroll.Content = planet ? planetPanel : WarSettings;
    }

    static string FactionName(int race) => PlanetModifiers.Catalog.FactionOf(race)?.Name ?? "Super Earth";

    static TextBlock Eyebrow(string text) => new() { Text = text, Classes = { "eyebrow" }, Margin = new(0, 8, 0, 0) };
    static TextBlock PlanetNote(string text) => new() { Text = text, Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };

    void ShowPlanetPanel()
    {
        planetPanel.Children.Clear();
        if (selectedPlanet is not { } index || PlanetModifiers.Catalog.Planet(index) is not { } planet)
        {
            ShowPlanetSummary();
            return;
        }
        var catalog = PlanetModifiers.Catalog;
        var view = planetViews.GetValueOrDefault(index);
        var choice = planetPlan.For(index) ?? new PlanetOverride(index, null, null, new Dictionary<string, string>());
        var card = new StackPanel { Spacing = 8 };
        planetPanel.Children.Add(card);
        card.Children.Add(new TextBlock { Text = planet.Name, FontSize = 22, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap });
        card.Children.Add(PlanetNote(string.Join(" · ", new[] { planet.Sector + " sector", planet.Biome.Replace('_', ' ') }.Where(s => s.Trim().Length > 0))));
        card.Children.Add(new TextBlock { Text = Describe(planet, view), TextWrapping = TextWrapping.Wrap, FontSize = 13 });

        if (index == 0)
        {
            card.Children.Add(PlanetNote("Super Earth has no enemies to change."));
            return;
        }
        // whom the planet's missions fight with these choices
        var enemy = catalog.Faction(choice.Faction)?.Race ?? view?.Enemy ?? (planet.Owner != PlanetModifiers.SuperEarth ? planet.Owner : 0);
        AddEnemyChoice(card, planet, choice);
        if (enemy == 0)
            card.Children.Add(PlanetNote("Super Earth holds this planet and nothing attacks it, so it has no missions. Pick an enemy to make it a liberation."));
        else
        {
            AddSubfactionChoice(card, choice, enemy);
            AddGroupChoice(card, choice, enemy);
        }
        if (!choice.IsEmpty)
        {
            var back = new Button { Classes = { "ghost" }, Content = "Back to the war's own", Margin = new(0, 8, 0, 0) };
            back.Click += (_, _) => SavePlanets(planetPlan.Without(index), planet.Name + " is back to the war's own.");
            card.Children.Add(back);
        }
    }

    void AddEnemyChoice(StackPanel card, PlanetInfo planet, PlanetOverride choice)
    {
        card.Children.Add(Eyebrow("ENEMY"));
        var factions = new WrapPanel { Orientation = Orientation.Horizontal };
        void FactionChip(string label, string? key)
        {
            var race = PlanetModifiers.Catalog.Faction(key)?.Race;
            var chip = new Button
            {
                Classes = { "chip" }, Margin = new(0, 0, 6, 6), IsEnabled = key == null || !planet.Home,
                Content = race is { } r ? new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { FactionMark(r, 14), new TextBlock { Text = label } } } : label,
            };
            if (choice.Faction == key) chip.Classes.Add("selected");
            chip.Click += (_, _) => SetFaction(choice, key);
            factions.Children.Add(chip);
        }
        FactionChip(planetWarViews.GetValueOrDefault(planet.Index) is { } war ? "As the war has it (" + FactionName(war.Owner) + ")" : "As the war has it", null);
        foreach (var f in PlanetModifiers.Catalog.Factions) FactionChip(f.Name, f.Key);
        card.Children.Add(factions);
        if (planet.Home) card.Children.Add(PlanetNote("A home world keeps its owner."));
    }

    void AddSubfactionChoice(StackPanel card, PlanetOverride choice, int enemy)
    {
        card.Children.Add(Eyebrow("SUBFACTIONS"));
        var keep = new CheckBox { Content = "As the war has it", IsChecked = choice.Subfactions == null };
        card.Children.Add(keep);
        var boxes = new List<(CheckBox Box, string Key)>();
        foreach (var s in PlanetModifiers.Catalog.Subfactions.Where(s => s.Faction == enemy || s.Faction == 0))
        {
            var box = new CheckBox { Content = s.Name, IsChecked = choice.Subfactions?.Contains(s.Key) == true, IsEnabled = choice.Subfactions != null, Margin = new(20, 0, 0, 0) };
            boxes.Add((box, s.Key));
            card.Children.Add(box);
        }
        keep.IsCheckedChanged += (_, _) => SavePlanet(choice with { Subfactions = keep.IsChecked == true ? null : [] });
        foreach (var (box, _) in boxes)
            box.IsCheckedChanged += (_, _) => SavePlanet(choice with { Subfactions = [.. boxes.Where(b => b.Box.IsChecked == true).Select(b => b.Key)] });
        if (enemy == PlanetModifiers.Illuminate)
            card.Children.Add(PlanetNote("An Illuminate planet always keeps one: with none ticked it gets Invasion Fleet, or its squads would have nothing to spawn."));
    }

    void AddGroupChoice(StackPanel card, PlanetOverride choice, int enemy)
    {
        card.Children.Add(Eyebrow("ENEMIES"));
        foreach (var group in PlanetModifiers.Catalog.Enemies.Where(g => g.Faction == enemy))
        {
            var mode = choice.Enemies.GetValueOrDefault(group.Key) switch { PlanetOverride.Surge => 1, PlanetOverride.None => 2, _ => 0 };
            var pick = new ComboBox { ItemsSource = EnemyModes, SelectedIndex = mode, Width = 140 };
            var key = group.Key;
            pick.SelectionChanged += (_, _) =>
            {
                var enemies = new Dictionary<string, string>(choice.Enemies);
                if (pick.SelectedIndex == 0) enemies.Remove(key);
                else enemies[key] = pick.SelectedIndex == 1 ? PlanetOverride.Surge : PlanetOverride.None;
                SavePlanet(choice with { Enemies = enemies });
            };
            var row = new DockPanel { LastChildFill = true };
            DockPanel.SetDock(pick, Dock.Right);
            row.Children.Add(pick);
            row.Children.Add(new TextBlock { Text = group.Name, VerticalAlignment = VerticalAlignment.Center, Foreground = mode == 0 ? Light : Gold });
            card.Children.Add(row);
        }
        card.Children.Add(PlanetNote("A surge makes squads with these enemies ten times as likely; None leaves those squads out."));
    }

    // "Held by the Automatons: a liberation. Jet Brigade. Surging: Hulks."
    string Describe(PlanetInfo planet, PlanetView? view)
    {
        if (view == null) return "Not in this war.";
        var parts = new List<string>
        {
            view.Owner != PlanetModifiers.SuperEarth ? $"Held by the {FactionName(view.Owner)}" + (view.Playable ? ", a liberation." : ", no missions.")
            : view.Enemy != 0 ? $"Super Earth's, under attack by the {FactionName(view.Enemy)}." : planet.Index == 0 ? "Super Earth." : "Super Earth's, no missions.",
        };
        if (view.Subfactions.Count > 0) parts.Add(string.Join(", ", view.Subfactions) + ".");
        if (view.Surges.Count > 0) parts.Add("Surging: " + string.Join(", ", view.Surges) + ".");
        if (view.Left.Count > 0) parts.Add("Left out: " + string.Join(", ", view.Left) + ".");
        return string.Join(" ", parts);
    }

    // No planet picked: the changed ones, to jump to.
    void ShowPlanetSummary()
    {
        var card = new StackPanel { Spacing = 6 };
        planetPanel.Children.Add(card);
        card.Children.Add(new TextBlock { Text = "Pick a planet on the map", FontSize = 18, FontWeight = FontWeight.SemiBold });
        card.Children.Add(PlanetNote("Colours show who holds each planet in " + planetSource + ". A gold ring marks a planet you changed; faded ones have no missions."));
        var legend = new WrapPanel { Margin = new(0, 6, 0, 0) };
        foreach (var (race, name) in new[] { (1, "Super Earth"), (2, "Terminids"), (3, "Automatons"), (4, "Illuminate") })
            legend.Children.Add(new StackPanel
            {
                Orientation = Orientation.Horizontal, Spacing = 6, Margin = new(0, 0, 14, 4),
                Children = { FactionMark(race, 14), new TextBlock { Text = name, FontSize = 12, VerticalAlignment = VerticalAlignment.Center } },
            });
        card.Children.Add(legend);
        if (planetPlan.Planets.Count == 0) return;
        card.Children.Add(Eyebrow("CHANGED"));
        foreach (var o in planetPlan.Planets)
        {
            if (PlanetModifiers.Catalog.Planet(o.Index) is not { } planet) continue;
            var go = new Button { Classes = { "flat" }, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left };
            go.Content = new StackPanel
            {
                Children =
                {
                    new TextBlock { Text = planet.Name, FontWeight = FontWeight.SemiBold },
                    PlanetNote(Describe(planet, planetViews.GetValueOrDefault(o.Index))),
                },
            };
            var index = o.Index;
            go.Click += (_, _) => SelectPlanet(index);
            card.Children.Add(go);
        }
    }

    // A new enemy drops the choices that were another enemy's (SEAF allies stay with any).
    void SetFaction(PlanetOverride choice, string? key)
    {
        if (choice.Faction == key) return;
        var catalog = PlanetModifiers.Catalog;
        var race = catalog.Faction(key)?.Race ?? planetWarViews.GetValueOrDefault(choice.Index)?.Enemy ?? 0;
        var subfactions = choice.Subfactions?.Where(s => catalog.Subfaction(s) is { } sf && (sf.Faction == 0 || sf.Faction == race)).ToList();
        var enemies = choice.Enemies.Where(e => catalog.Enemy(e.Key)?.Faction == race).ToDictionary(e => e.Key, e => e.Value);
        SavePlanet(choice with { Faction = key, Subfactions = subfactions, Enemies = enemies });
    }

    void SavePlanet(PlanetOverride choice)
    {
        var name = PlanetModifiers.Catalog.Planet(choice.Index)?.Name ?? choice.Index.ToString(System.Globalization.CultureInfo.InvariantCulture);
        SavePlanets(choice.IsEmpty ? planetPlan.Without(choice.Index) : planetPlan.With(choice), name + " saved.");
    }

    void SavePlanets(PlanetPlan plan, string done)
    {
        try
        {
            plan.Write(PlanetFile);
            planetPlan = plan;
            planetStatus.Text = done + (ProfileStore.LoadWar(paths) != ProfileStore.WarCustom ? " It applies with the Custom planets war."
                : runner.IsRunning ? " The game picks it up within a minute." : " The next session plays with it.");
            planetStatus.Foreground = Light;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            planetStatus.Text = "Not saved: " + error.Message;
            planetStatus.Foreground = Red;
        }
        RefreshPlanetViews();
    }
}
