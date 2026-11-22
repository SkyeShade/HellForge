using System.Globalization;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The sidebar's version dropdown: which HellForge version sessions play with. The installed one first (this launcher's),
// then each version an update kept. Picking one swaps that version's programs and data in (KeptVersions.Activate) while
// this launcher keeps running, then the window is made again from that data; nothing restarts. With nothing kept the
// version stays a plain label. Settings > Installation sets how many updates keep.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> keptVersions = [];
    // the last switch's outcome, said under the dropdown until this window closes
    string? switchOutcome;
    bool versionPickerFilling, versionSwitching;
    readonly DispatcherTimer versionBlockCheck = new() { Interval = TimeSpan.FromSeconds(3) };
    // the version sessions play with (SessionRuntime): an older one has no counterpart for some of this launcher's features
    SessionRuntime? playedRuntime;

    bool PlaysOlder => playedRuntime is { IsCurrent: false };

    void InitializeKeptVersions(InstallLayout install)
    {
        if (!CanSwitchVersions)
        {
            KeepVersionsRow.IsVisible = KeptVersionsText.IsVisible = false;
            return;
        }
        KeepVersionsBox.ItemsSource = Enumerable.Range(KeptVersions.MinCount, KeptVersions.MaxCount - KeptVersions.MinCount + 1).ToList();
        KeepVersionsBox.SelectedItem = KeptVersions.KeepCount(install);
        KeepVersionsBox.SelectionChanged += (_, _) =>
        {
            if (KeepVersionsBox.SelectedItem is not int count || count == KeptVersions.KeepCount(install)) return;
            try { KeptVersions.SetKeepCount(install, count); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { Append("The number of kept versions was not saved: " + error.Message, Red); }
            ShowKeepCost(install);
        };
        ShowKeepCost(install);
        // Setup's or HellForge.Cli's switch, made while no launcher was open
        if (KeptVersions.TakeResult(install) is { } last)
        {
            var done = last.Status == "done";
            Append((done ? "" : "Switching versions failed: ") + last.Message, done ? Green : Red);
            switchOutcome = Privacy.Scrub(done ? last.Message : "The last switch did not happen: " + last.Message);
        }
        VersionPicker.SelectionChanged += async (_, _) => await PickVersion(install);
        VersionPicker.DropDownOpened += async (_, _) => await CheckVersionBlockers(install);
        versionBlockCheck.Tick += async (_, _) => await CheckVersionBlockers(install);
        Closed += (_, _) => versionBlockCheck.Stop();
        FillVersionPicker(install);
        ShowOlderVersionLimits();
    }

    static bool CanSwitchVersions => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine;

    // A kept version is the app without its Linux build plus a copy of the version's data: about 0.6 GB.
    void ShowKeepCost(InstallLayout install)
    {
        var kept = KeptVersions.List(install);
        KeptVersionsText.Text = $"About 0.6 GB each; {kept.Count} kept now ({KeptVersions.KeptBytes(install) / 1e9:0.0} GB). The oldest goes only when " +
            "an update would keep more, never the version you just came from. Pick the version to play with in the bottom-left corner.";
    }

    // The dropdown's items: the installed version, then each kept one (greyed with the reason when it can't be played),
    // the one played with now selected.
    void FillVersionPicker(InstallLayout install)
    {
        keptVersions = KeptVersions.List(install);
        var showPicker = keptVersions.Count > 0;
        VersionText.IsVisible = !showPicker;
        VersionPickerPanel.IsVisible = showPicker;
        if (!showPicker) { versionBlockCheck.Stop(); return; }
        var installed = KeptVersions.Installed(install);
        var active = KeptVersions.Active(install);
        VersionBuildText.Text = $"HELLDIVERS 2 build {SupportedBuild.Current.BuildId}";
        var items = new List<ComboBoxItem> { Item("VersionItemCurrent", "HellForge " + installed.Text, true) };
        var selected = 0;
        for (var index = 0; index < keptVersions.Count; index++)
        {
            var kept = keptVersions[index];
            var playing = kept.Name == active.Name;
            if (playing) selected = index + 1;
            var problem = playing ? null : KeptVersions.RuntimeProblem(install, kept);
            var text = "HellForge " + kept.Text + (kept.Identity.IsNewerThan(installed) ? "  ·  newer" : "") + "\nkept " +
                kept.KeptUtc.ToLocalTime().ToString("d MMM yyyy, HH:mm", CultureInfo.CurrentCulture) + (problem != null ? "\n" + problem : "");
            items.Add(Item("VersionItem" + (index + 1).ToString(CultureInfo.InvariantCulture), text, problem == null));
        }
        versionPickerFilling = true;
        VersionPicker.ItemsSource = items;
        VersionPicker.SelectedIndex = selected;
        versionPickerFilling = false;
        ShowVersionNote(null);
        versionBlockCheck.Start();
    }

    static ComboBoxItem Item(string id, string text, bool enabled)
    {
        var item = new ComboBoxItem { Name = id, IsEnabled = enabled, Content = text, FontSize = 11 };
        AutomationProperties.SetAutomationId(item, id);
        return item;
    }

    // Why no version can be picked now (the game, a session, a program from the installation), or null; the dropdown is
    // disabled meanwhile and says why under it and in its tooltip.
    async Task CheckVersionBlockers(InstallLayout install)
    {
        if (!VersionPickerPanel.IsVisible || versionSwitching) return;
        var blocked = Refusal() ?? (await Task.Run(() => KeptVersions.Blockers(install)) is { Count: > 0 } running ? "Close these to switch versions: " + string.Join(", ", running) + "." : null);
        if (blocked != null && VersionPicker.IsDropDownOpen) VersionPicker.IsDropDownOpen = false;
        VersionPicker.IsEnabled = blocked == null;
        ToolTip.SetTip(VersionPicker, blocked);
        ShowVersionNote(blocked);
    }

    string? Refusal() => runner.IsRunning ? "Versions don't switch while a session runs." : setupRunning ? "Versions don't switch while setup runs."
        : SyncBusy ? "Versions don't switch while an inventory sync runs." : null;

    void ShowVersionNote(string? blocked)
    {
        var text = blocked ?? switchOutcome;
        VersionPickerNote.Text = text ?? "";
        VersionPickerNote.IsVisible = text != null;
    }

    async Task PickVersion(InstallLayout install)
    {
        if (versionPickerFilling || versionSwitching || VersionPicker.SelectedIndex < 0 || VersionPicker.SelectedIndex > keptVersions.Count) return;
        var name = VersionPicker.SelectedIndex == 0 ? KeptVersions.Installed(install).Name : keptVersions[VersionPicker.SelectedIndex - 1].Name;
        if (name == KeptVersions.Active(install).Name) return;
        if (Refusal() is { } refused)
        {
            switchOutcome = refused;
            FillVersionPicker(install);
            return;
        }
        // no confirmation: each version keeps its own data and mod list, so a switch loses nothing and switching back is one click
        versionSwitching = true;
        VersionPicker.IsEnabled = false;
        ShowVersionNote("Switching...");
        var clock = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            var done = await Task.Run(() => KeptVersions.Activate(install, name));
            var outcome = $"Plays with HellForge {done.To} now (switched in {clock.Elapsed.TotalSeconds:0.0} s)." +
                (done.SwitchedOff.Count > 0 ? " Mods added since it last ran are off in its list: " + string.Join(", ", done.SwitchedOff) + "." : "");
            Reopen(Privacy.Scrub(outcome));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            versionSwitching = false;
            switchOutcome = Privacy.Scrub("The switch did not happen: " + error.Message);
            Append(switchOutcome, Red);
            FillVersionPicker(install);
        }
    }

    // The window made again from the data now in place (settings, profiles, mods, the version's own templates), where this
    // one is and on the same page; this process goes on.
    void Reopen(string outcome)
    {
        var next = new MainWindow(paths.InstallRoot != null ? LauncherPaths.ForInstall(new InstallLayout(paths.InstallRoot)) : paths) { WindowStartupLocation = WindowStartupLocation.Manual, Position = Position, Width = Width, Height = Height };
        next.switchOutcome = outcome;
        next.ShowVersionNote(null);
        next.Append(outcome, Green);
        next.Nav.SelectedIndex = Nav.SelectedIndex;
        if (WindowState == WindowState.Maximized) next.WindowState = WindowState.Maximized;
        if (Avalonia.Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop) desktop.MainWindow = next;
        next.Show();
        closeConfirmed = true;
        Close();
    }

    // What this launcher has that the version played with does not: greyed, with why.
    void ShowOlderVersionLimits()
    {
        if (playedRuntime is not { IsCurrent: false } runtime) return;
        WarHellForgeRadio.IsEnabled = false;
        ToolTip.SetTip(WarHellForgeRadio, runtime.Lacks("The HellForge war"));
        WarServerOptions.IsVisible = false;
        if (ProfileStore.LoadWar(paths) == ProfileStore.WarHellForge)
        {
            WarServerWarning.Text = $"{runtime.Text} has no HellForge war: its sessions play HellForge's frozen war.";
            WarServerWarning.IsVisible = true;
        }
        OlderVersionNote.Text = $"You play with HellForge {runtime.Text}. Inventory sync, account import, Check and repair and making downpatch files " +
            "work with the installed version only: pick it first in the version list, bottom left, to use them.";
        OlderVersionNote.IsVisible = true;
        SyncOlderNote.Text = runtime.Lacks("Inventory sync") + $" {runtime.Text} keeps the inventory it last synced.";
        SyncOlderNote.IsVisible = true;
        SyncButton.IsEnabled = false;
        RepairButton.IsEnabled = DownpatchMakeButton.IsEnabled = false;
        ToolTip.SetTip(RepairButton, runtime.Lacks("Check and repair"));
        ToolTip.SetTip(DownpatchMakeButton, runtime.Lacks("Making a downpatch file"));
    }

    // An older version played with (the version list): which one, and the war it plays instead of one it lacks.
    void AppendRuntimeLines()
    {
        if (playedRuntime is not { } runtime) return;
        if (!runtime.IsCurrent) Append($"Plays with HellForge {runtime.Text} (the version list, bottom left).", LauncherLine);
        if (runtime.WarFallback(ProfileStore.LoadWar(paths)) != null) Append($"{runtime.Text} has no HellForge war: this session plays HellForge's frozen war.", Orange);
    }
}
