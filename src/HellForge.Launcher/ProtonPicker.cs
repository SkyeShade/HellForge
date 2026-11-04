using Avalonia.Controls;
using Avalonia.Platform.Storage;
using HellForge.Core;

namespace HellForge.Launcher;

// The Proton chooser's shared Other... entry: the file picker for a proton script outside the search, its validation,
// adding it to the list once and showing it. Both the installer and the setup page use this one copy.
static class ProtonPicker
{
    public static readonly ProtonVersion Other = new("Other...", "");

    public static T[] Items<T>(IEnumerable<ProtonVersion> protons, Func<ProtonVersion, T> display) =>
        protons.Append(Other).Select(display).ToArray();

    public static string Using(ProtonVersion proton) => "Using " + proton.Name + " at " + proton.ScriptPath + ".";

    // Ask for a proton script and show it in the list. Null when the player closed the picker or the file was refused;
    // showProblem receives the refusal reason, and the caller puts its previous choice back.
    public static async Task<(IReadOnlyList<ProtonVersion> Versions, int Selected)?> PickAndShow<T>(
        TopLevel owner, ComboBox combo, IReadOnlyList<ProtonVersion> protons, Func<ProtonVersion, T> display,
        Action<string> showProblem)
    {
        var files = await owner.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose a Proton's proton script",
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("proton scripts") { Patterns = ["proton"] },
                new FilePickerFileType("All files") { Patterns = ["*"] }]
        });
        var chosen = files.Select(file => file.TryGetLocalPath()).OfType<string>().FirstOrDefault();
        if (chosen == null) return null;
        if (SteamLocator.ProtonProblem(chosen) is { } problem)
        {
            showProblem(problem);
            return null;
        }
        var proton = SteamLocator.ProtonVersionFor(chosen);
        var versions = SteamLocator.IncludingSelected(protons, proton.ScriptPath);
        var selected = versions.ToList().FindIndex(version => version.ScriptPath == proton.ScriptPath);
        combo.ItemsSource = Items(versions, display);
        combo.SelectedIndex = selected;
        return (versions, selected);
    }
}
