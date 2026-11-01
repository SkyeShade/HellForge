using Avalonia.Threading;
using HellForge.Core;

namespace HellForge.Launcher;

// The Linux Proton choice on the setup page: discovery, the saved or Other... Proton, and filling the list. Part of
// MainWindow (SetupFlow.cs), kept here so that file stays below the module-size limit.
public partial class MainWindow
{
    // The Linux Proton chooser's found versions and the player's last valid choice. Other... adds a picked proton script.
    IReadOnlyList<ProtonVersion> protonVersions = [];
    ProtonVersion? lastProton;
    bool applyingProtons;

    // Steam's libraries and Proton installations are on disk and can take a while (many libraries, a slow disk): find them
    // away from the UI thread, then fill the setup page on it. A later EnterSetup supersedes an earlier lookup.
    async Task DiscoverProtons(InstallLayout install, int discovery)
    {
        (IReadOnlyList<ProtonVersion> Protons, string? Error, string? SavedProton, string? ConfiguredProton) found;
        try { found = await Task.Run(() => FindProtons(install)).ConfigureAwait(false); }
        catch (Exception error)
        {
            found = (Array.Empty<ProtonVersion>(), "Could not look up Steam's Proton installations: " + error.Message, null, null);
        }
        Dispatcher.UIThread.Post(() => ApplyProtons(found, discovery));
    }

    static (IReadOnlyList<ProtonVersion> Protons, string? Error, string? SavedProton, string? ConfiguredProton) FindProtons(InstallLayout install)
    {
        var steamRoot = SteamLocator.SteamRoot(install);
        IReadOnlyList<ProtonVersion> protons;
        string? protonError = null;
        try { protons = SteamLocator.ProtonVersions(steamRoot); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            protons = [];
            protonError = "Could not read Steam's Proton installations: " + error.Message;
        }
        var savedProton = SteamLocator.SelectedProtonScript(install);
        var configuredPath = Environment.GetEnvironmentVariable("PROTON");
        var configuredProton = !string.IsNullOrEmpty(configuredPath) && File.Exists(configuredPath)
            ? LocalInstall.FullPath(configuredPath) : null;
        return (Protons: protons, Error: protonError, SavedProton: savedProton, ConfiguredProton: configuredProton);
    }

    void ApplyProtons((IReadOnlyList<ProtonVersion> Protons, string? Error, string? SavedProton, string? ConfiguredProton) found, int discovery)
    {
        if (discovery != protonDiscovery) return;
        protonVersions = SteamLocator.IncludingSelected(found.Protons, found.SavedProton);
        var defaultProton = found.SavedProton ?? found.ConfiguredProton ?? protonVersions.LastOrDefault()?.ScriptPath;
        applyingProtons = true;
        try
        {
            ProtonCombo.ItemsSource = ProtonPicker.Items(protonVersions, proton => proton);
            ProtonCombo.SelectedIndex = protonVersions.ToList().FindIndex(proton => proton.ScriptPath == defaultProton);
        }
        finally { applyingProtons = false; }
        lastProton = ProtonCombo.SelectedItem as ProtonVersion;
        ProtonChoiceHint.Text = protonVersions.Count == 0
            ? found.Error ?? "No Proton versions were found. Install Proton in Steam, then reopen setup."
            : "This Proton version will be used for setup and future game sessions. Choose Other... to use a proton script outside the list.";
        SetupStartButton.IsEnabled = protonVersions.Count > 0;
    }
}
