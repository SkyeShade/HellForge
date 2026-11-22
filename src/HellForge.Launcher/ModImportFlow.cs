using HellForge.Core;

namespace HellForge.Launcher;

// The Mods page's offer to bring over a kept version's mod list (KeptVersions.ImportSources). After an update the new
// version starts with its own empty list, and the mods themselves stay in the mods folder: one press brings the old
// list's choices over, the most recent kept version by default, another from the chooser.
public partial class MainWindow
{
    IReadOnlyList<KeptVersion> importSources = [];
    bool importWired;

    void ShowModImport()
    {
        if (paths.InstallRoot == null || modsData is not { Error: null } data || !CanSwitchVersions) { ModImportCard.IsVisible = false; return; }
        var install = new InstallLayout(paths.InstallRoot);
        try { importSources = KeptVersions.ImportSources(install, data.Mods, data.Settings, data.Bundled); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { importSources = []; }
        ModImportCard.IsVisible = importSources.Count > 0;
        if (importSources.Count == 0) return;
        if (!importWired)
        {
            importWired = true;
            ModImportButton.Click += (_, _) => ImportMods(install);
            ModImportDismissButton.Click += (_, _) => DismissModImport(install);
            ModImportChooser.SelectionChanged += (_, _) => DescribeImport(install);
        }
        var labels = importSources.Select(Label).ToList();
        ModImportChooser.ItemsSource = labels;
        ModImportChooser.SelectedIndex = 0;
        ModImportChooser.IsVisible = importSources.Count > 1;
        DescribeImport(install);
    }

    // "0.4.0-beta (d8a8b46)": two builds of one version read apart
    static string Label(KeptVersion kept) => kept.Text;

    KeptVersion? ImportSource => ModImportChooser.SelectedIndex is var i && i >= 0 && i < importSources.Count ? importSources[i] : null;

    void DescribeImport(InstallLayout install)
    {
        if (ImportSource is not { } from || modsData is not { } data) return;
        var on = KeptVersions.ModsOn(install, from, data.Mods, data.Bundled);
        ModImportText.Text = $"HellForge now keeps your earlier versions. Your {Label(from)} mods are still there ({on} on in it). " +
            "This version has its own mod list, which starts empty.";
        ModImportButton.Content = "Import all mods from " + Label(from);
    }

    void ImportMods(InstallLayout install)
    {
        if (ImportSource is not { } from || modsData is not { } data) return;
        if (runner.IsRunning) { ModImportText.Text = "Mods change between sessions: import once this one has ended."; return; }
        try
        {
            KeptVersions.ImportModList(install, from, data.Mods);
            Append($"Imported the mod list of {from.Text}.", Green);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            ModImportText.Text = "The mod list could not be imported: " + Privacy.Scrub(error.Message);
            return;
        }
        ModImportCard.IsVisible = false;
        RefreshMods();
    }

    void DismissModImport(InstallLayout install)
    {
        try { ModSettings.SetImportDone(install.Mods); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Append("Could not save that: " + error.Message, Red); }
        ModImportCard.IsVisible = false;
        RefreshMods();
    }
}
