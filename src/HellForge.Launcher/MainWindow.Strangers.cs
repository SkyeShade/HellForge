using HellForge.Core;

namespace HellForge.Launcher;

// MainWindow's Settings > MULTIPLAYER "Play with strangers": the player's choice, else on while the next session is
// sandboxed (ModSettings.StrangersEffective), with the reason and "Use default" under the switch.
public partial class MainWindow
{
    void InitializeStrangers()
    {
        StrangersBox.IsEnabled = paths.ModsFolder != null;
        StrangersBox.IsCheckedChanged += (_, _) =>
        {
            if (updatingStrangersUi || paths.ModsFolder == null) return;
            try { ModSettings.SetPlayWithStrangers(paths.ModsFolder, StrangersBox.IsChecked == true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save Play with strangers: " + error.Message, Red); }
            ApplyStrangersSetting();
        };
        StrangersDefaultButton.Click += (_, _) =>
        {
            if (paths.ModsFolder == null) return;
            try { ModSettings.SetPlayWithStrangers(paths.ModsFolder, null); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save Play with strangers: " + error.Message, Red); }
            ApplyStrangersSetting();
        };
        ApplyStrangersSetting();
    }

    // The switch shows what the next session will do: the player's choice, else whether that session is sandboxed. The line
    // under it names the default, and "Use default" takes an explicit choice back.
    void ApplyStrangersSetting()
    {
        var available = paths.ModsFolder != null;
        var chosen = available ? ModSettings.PlayWithStrangers(paths.ModsFolder!) : null;
        var sandboxed = NextSessionSandboxed();
        var effective = available && ModSettings.StrangersEffective(chosen, sandboxed);
        updatingStrangersUi = true;
        try
        {
            StrangersBox.IsEnabled = available;
            StrangersBox.IsChecked = effective;
            StrangersWarning.IsVisible = effective;
            StrangersEffectiveText.Text = sandboxed ? "On by default while your game is sandboxed" : "Off by default: " + SandboxOffReason();
            StrangersDefaultButton.IsVisible = available && chosen != null;
        }
        finally { updatingStrangersUi = false; }
    }

    // The sandbox state the next session would have, as Settings > Sandbox shows it.
    bool NextSessionSandboxed() => SandboxBox.IsChecked == true;

    string SandboxOffReason()
    {
        if (!SessionRunner.HasSteamBridge(paths)) return "this HellForge was built without the Steam bridge";
        return "Settings > Sandbox is off";
    }

    // The existing stranger warning, once, for the first session that runs with strangers on by default (never chosen, and
    // sandboxed). The file remembers that it was shown.
    const string StrangerWarningText = "Strangers' games send yours data you cannot check: a modified game can lie about its mods, and a flaw in the game's network code could let it run code on your PC. Turn this on only if you accept that risk. It applies within a few seconds, also during a session.";

    void ShowStrangersDefaultNotice()
    {
        StrangersNoticeCard.IsVisible = false;
        if (paths.ModsFolder == null) return;
        var settings = ModSettings.Read(paths.ModsFolder);
        if (settings.StrangersWarned || settings.PlayWithStrangers != null || !ModSettings.StrangersEffective(null, NextSessionSandboxed())) return;
        StrangersNoticeText.Text = StrangerWarningText;
        StrangersNoticeCard.IsVisible = true;
        try { ModSettings.SetStrangersWarned(paths.ModsFolder); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save that the stranger warning was shown: " + error.Message, Red); }
    }
}
