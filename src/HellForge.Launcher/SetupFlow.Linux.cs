using Avalonia.Platform.Storage;
using HellForge.Core;

namespace HellForge.Launcher;

// The Linux setup page's first decision: restore a force-ended session's stand-ins, then choose between ready, a repair
// with the first bad file, and first setup.
public partial class MainWindow
{
    void InitializeLinuxSetup(InstallLayout install)
    {
        InstallCard.IsVisible = true;
        try { SdkShimRecovery.Recover(install); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        var problem = SupportedBuild.Current.FirstCompletenessProblem(install.Game);
        var complete = problem == null;
        InstallText.Text = complete
            ? $"{Privacy.Scrub(install.Root)}  ·  HELLDIVERS 2 build {SupportedBuild.Current.BuildId}. Native setup and repair are available."
            : $"{Privacy.Scrub(install.Root)}  ·  Complete setup natively with Steam for Linux.";
        UninstallButton.IsVisible = true;
        UninstallConfirmButton.Content = "Uninstall this installation";
        UninstallDescription.Text = "Uninstall removes this HellForge installation, its desktop shortcut and the removed shell installer's leftovers when they are found, after the launcher closes. The Proton prefix is left unless this installer made it, in which case the confirmation also offers to delete it. Steam's depot download is a separate choice, off by default. Steam, your game and your Proton versions are left intact.";
        DesktopShortcutBox.IsVisible = StartMenuShortcutBox.IsVisible = ImportBox.IsVisible = false;
        SetupIntro.Text = "Choose a Proton version first. Setup finds the supported game files, confirms Steam ownership, then copies the game if needed and checks the Linux network namespace.";
        if (complete) return;
        if (SetupReceipt.Read(install) is { } receipt)
            EnterSetup(repair: true, reason: $"Setup finished {receipt.FinishedUtc.ToLocalTime():g}, but {problem}. " +
                "Check and repair the game copy; completed files are kept.");
        else EnterSetup(repair: false);
    }

    // The Proton chooser's Other... entry: ProtonPicker asks for and validates the script, adds it to the list and shows
    // it; this records the setup page's selection and puts the previous one back when the picker is cancelled or refused.
    async Task ProtonChoiceChanged()
    {
        if (applyingProtons || ProtonCombo.SelectedItem is not ProtonVersion selected) return;
        if (selected.ScriptPath.Length > 0)
        {
            lastProton = selected;
            return;
        }
        applyingProtons = true;
        try
        {
            var picked = await ProtonPicker.PickAndShow(this, ProtonCombo, protonVersions, proton => proton,
                problem => ProtonChoiceHint.Text = problem);
            if (picked is not { } result)
            {
                ProtonCombo.SelectedItem = lastProton;
                return;
            }
            protonVersions = result.Versions;
            var proton = protonVersions[result.Selected];
            lastProton = proton;
            ProtonChoiceHint.Text = ProtonPicker.Using(proton);
            SetupStartButton.IsEnabled = protonVersions.Count > 0;
        }
        finally { applyingProtons = false; }
    }

    // Steam was not found in its usual folders: ask for the folder that holds steamapps, check it and save it in the
    // installation so sessions and external launches use it too. Null when the player closes the picker.
    async Task<string?> ChooseSteamFolder(InstallLayout install)
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose Steam's folder (the one that holds steamapps)",
            AllowMultiple = false
        });
        var chosen = folders.Select(folder => folder.Path.LocalPath).FirstOrDefault();
        if (chosen == null) return null;
        if (SteamLocator.SteamFolderProblem(chosen) is { } problem) throw new SetupStopped(problem);
        var steamRoot = SteamLocator.SteamFolder(chosen);
        SteamLocator.SaveSteamSelection(install, steamRoot);
        return steamRoot;
    }

    // Setup is done: the highlighted button on the right always moves forward, which here is Continue. Checking again
    // stays available under Settings > Installation.
    void ReadyToPlay(InstallLayout install)
    {
        if (OperatingSystem.IsLinux()) SetupReceipt.Write(install);
        SetupTitle.Text = "Ready to play";
        SetupStartButton.IsVisible = false;
        SetupBackButton.Content = "Continue";
        SetBackButtonPrimary(true);
        SetupBackButton.IsVisible = true;
        Nav.IsVisible = true;
    }
}
