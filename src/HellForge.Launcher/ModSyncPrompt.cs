using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// The MOD SYNC prompt in the launcher, for a session whose game has no in-game editor to show it (the editor switched off
// on the Mods page): the offer the session's backend writes (<run>/backend/mod-sync.json, ModSyncOffer) on the Play page,
// with the prompt's answers, written back for the backend to take (docs/mods.md "Playing with other mods"). With the
// editor on, the game shows the prompt and this card stays hidden.
public partial class MainWindow
{
    bool sessionHasEditor = true; // the editor was on when this session started (a session keeps the mods it started with)
    string? modSyncShown;         // what the card shows now: version, state, progress
    int modSyncAnswered = -1;     // the offer version last answered from here
    string? modSyncTrust;         // the mod whose TRUST warning shows in the card's place

    // At a session's start: whether its game shows the prompt itself.
    void NoteSessionEditor()
    {
        modSyncShown = null;
        modSyncAnswered = -1;
        modSyncTrust = null;
        sessionHasEditor = true;
        if (paths.ModsFolder == null) return; // not an installation: no session starts
        try
        {
            var (mods, _) = ModCatalog.Load(paths.ModsFolder);
            sessionHasEditor = mods.Any(m => m.Id == "hellforge.editor" && m.Enabled);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            sessionHasEditor = true; // unknown: leave the prompt to the game
            Append("Could not read the mods folder for mod sync: " + error.Message, Red);
        }
    }

    // Each refresh while a session runs: the card follows the offer.
    void ShowModSync(string runDirectory)
    {
        var offer = sessionHasEditor ? null : ModSyncOffer.Read(Path.Combine(runDirectory, "backend"));
        if (offer == null || offer.State is "none" or "" || offer.Version == modSyncAnswered)
        {
            ModSyncCard.IsVisible = false;
            modSyncShown = null;
            return;
        }
        var key = $"{offer.Version}|{offer.State}|{offer.Done}|{offer.Mods.Count}|{string.Join(",", offer.Mods.Select(m => m.Action))}|{modSyncTrust}";
        if (key == modSyncShown) return;
        if (modSyncShown == null) Append($"MOD SYNC: {offer.Message}", LauncherLine);
        modSyncShown = key;
        RenderModSync(runDirectory, offer);
    }

    void RenderModSync(string runDirectory, ModSyncOffer offer)
    {
        ModSyncPanel.Children.Clear();
        if (offer.Mods.FirstOrDefault(m => m.Id == modSyncTrust && m.Action == "trust") is { } trusting)
        {
            RenderTrust(runDirectory, offer, trusting);
            return;
        }
        modSyncTrust = null;
        ModSyncPanel.Children.Add(new TextBlock { Text = $"MOD SYNC  ·  {offer.Host}", Classes = { "h2" }, Margin = new(0) });
        ModSyncPanel.Children.Add(new TextBlock { Text = offer.Message, TextWrapping = TextWrapping.Wrap });
        foreach (var mod in offer.Mods) ModSyncPanel.Children.Add(ModSyncRow(runDirectory, offer, mod));
        if (offer.State == "downloading")
            ModSyncPanel.Children.Add(new ProgressBar { Classes = { "detail" }, Minimum = 0, Maximum = Math.Max(1, offer.Bytes), Value = offer.Done, Margin = new(0, 4, 0, 0) });
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Margin = new(0, 6, 0, 0) };
        void Add(string label, string answer, bool primary)
        {
            var button = new Button { Content = label, Classes = { primary ? "primary" : "ghost" } };
            button.Click += (_, _) => AnswerModSync(runDirectory, offer, answer);
            buttons.Children.Add(button);
        }
        switch (offer.State)
        {
            case "offer" when offer.Bytes == 0: Add("RESTART TO JOIN", "restart", true); Add("CANCEL", "decline", false); break;
            case "offer": Add("DOWNLOAD AND RESTART", "accept", true); Add("CANCEL", "decline", false); break;
            case "downloading": Add("CANCEL", "decline", false); break;
            case "ready": Add("RESTART TO JOIN", "restart", true); Add("CANCEL", "decline", false); break;
            default: Add("OK", "decline", false); break; // blocked or failed: nothing to do but close it
        }
        ModSyncPanel.Children.Add(buttons);
        ModSyncCard.IsVisible = true;
    }

    // The in-game prompt's TRUST warning (app.cpp DrawTrust), in the card's place; its button takes a click only after a
    // second, so a click meant for the card underneath never trusts a mod.
    void RenderTrust(string runDirectory, ModSyncOffer offer, ModSyncOffer.Mod mod)
    {
        var name = (mod.Name.Length > 0 ? mod.Name : mod.Id) + " " + mod.Version;
        var host = offer.Host.Length > 0 ? offer.Host : "the host";
        var warning = (mod.Class == "raw"
            ? name + " carries game data HellForge cannot check, which goes straight to the game. Data like this can break your game, or " +
              "through a flaw in the game run code on your PC that can do anything your Windows account can."
            : name + " runs its own code on your PC. A mod like this can do anything your Windows account can: read and change your " +
              "files, or install other programs.") + " Only trust it if you trust " + host + " and know where the mod comes from.";
        ModSyncPanel.Children.Add(new TextBlock { Text = "TRUST THIS MOD?", Classes = { "h2" }, Foreground = Brush.Parse("#E05555"), Margin = new(0) });
        ModSyncPanel.Children.Add(new TextBlock { Text = warning, TextWrapping = TextWrapping.Wrap });
        ModSyncPanel.Children.Add(new TextBlock { Text = "HellForge still checks that every file you download is exactly the one " + host +
            " has. You can take the trust back on the Mods page (MOD SYNC).", TextWrapping = TextWrapping.Wrap, Classes = { "muted" } });
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Margin = new(0, 6, 0, 0) };
        var trust = new Button { Content = "TRUST THIS MOD", Classes = { "primary" }, IsEnabled = false };
        trust.Click += (_, _) => { modSyncTrust = null; AnswerModSync(runDirectory, offer, "trust", mod.Id); };
        var back = new Button { Content = "BACK", Classes = { "ghost" } };
        back.Click += (_, _) => { modSyncTrust = null; modSyncShown = null; RenderModSync(runDirectory, offer); };
        buttons.Children.Add(trust);
        buttons.Children.Add(back);
        ModSyncPanel.Children.Add(buttons);
        ModSyncCard.IsVisible = true;
        Avalonia.Threading.DispatcherTimer.RunOnce(() => trust.IsEnabled = true, TimeSpan.FromSeconds(1));
    }

    Control ModSyncRow(string runDirectory, ModSyncOffer offer, ModSyncOffer.Mod mod)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (mod.Class == "code") row.Children.Add(ModTag("CODE", "#E05591"));
        else if (mod.Class == "raw") row.Children.Add(ModTag("RAW DATA", "#F2A541"));
        row.Children.Add(new TextBlock { Text = mod.Name.Length > 0 ? mod.Name : mod.Id, VerticalAlignment = VerticalAlignment.Center });
        if (mod.Version.Length > 0) row.Children.Add(new TextBlock { Text = mod.Version, FontSize = 12, Classes = { "muted" }, VerticalAlignment = VerticalAlignment.Center });
        var what = mod.Action switch
        {
            "have" => "you have it",
            "download" => $"downloads ({mod.Bytes / 1048576.0:0.0} MB)",
            "manual" => "install it yourself",
            "blocked" => "blocked",
            "update" => "needs another HellForge",
            "off" => "off for this session",
            "trust" => "needs your trust",
            _ => mod.Action
        };
        row.Children.Add(new TextBlock { Text = "·  " + what, FontSize = 12, Classes = { "muted" }, VerticalAlignment = VerticalAlignment.Center });
        if (mod.Action == "trust" && offer.State is "offer" or "blocked")
        {
            var trust = new Button { Content = "TRUST", Classes = { "ghost" } };
            trust.Click += (_, _) => { modSyncTrust = mod.Id; modSyncShown = null; RenderModSync(runDirectory, offer); };
            row.Children.Add(trust);
        }
        if (mod.Url != null && mod.Action is "manual" or "trust")
        {
            var get = new Button { Content = "GET IT", Classes = { "ghost" } };
            get.Click += (_, _) => AnswerModSync(runDirectory, offer, "open", mod.Id);
            row.Children.Add(get);
        }
        return row;
    }

    void AnswerModSync(string runDirectory, ModSyncOffer offer, string answer, string? modId = null)
    {
        try { ModSyncOffer.Answer(Path.Combine(runDirectory, "backend"), offer.Version, answer, modId); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            Append("Could not send the mod sync answer: " + error.Message, Red);
            return;
        }
        Append($"MOD SYNC: {answer}{(modId != null ? " " + modId : "")} sent to the session.", LauncherLine);
        if (answer == "decline")
        {
            modSyncAnswered = offer.Version;
            ModSyncCard.IsVisible = false;
        }
        modSyncShown = null;
    }
}
