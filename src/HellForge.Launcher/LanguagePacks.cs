using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using HellForge.Core;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// The Languages card in Settings: the language packs pinned for the supported build, and adding one to the game copy.
// On Windows Steam sends the pack for the signed-in account (as setup downloads the game); natively on Linux the
// launcher cannot reach Steam, so the Steam app downloads it from the console command and the card copies it in.
// Not while a session runs: the game holds its files.
public partial class MainWindow
{
    CancellationTokenSource? languageCancel;
    string languageState = "";

    void InitializeLanguages()
    {
        LanguageCancelButton.Click += (_, _) => languageCancel?.Cancel();
        // Steam's console download finishes on its own: while Settings shows, the card looks again so Add appears.
        DispatcherTimer.Run(() => { if (SettingsPage.IsVisible) UpdateLanguages(); return true; }, TimeSpan.FromSeconds(3));
        UpdateLanguages();
    }

    void UpdateLanguages()
    {
        var packs = SupportedLanguages.Current.Languages;
        LanguagesCard.IsVisible = paths.InstallRoot != null && packs.Count > 0;
        if (!LanguagesCard.IsVisible || languageCancel != null) return;
        var install = new InstallLayout(paths.InstallRoot!);
        var steamRoot = SteamLocator.SteamRoot(install);
        var inSteam = PacksInSteam(steamRoot);
        var unreadable = steamRoot != null && SteamDepotDownload.LibraryListProblem(steamRoot) != null;
        // Rebuilt only when something changed, so the timer does not drop a selection or a pressed button.
        var state = string.Join(";", packs.Select(l => $"{l.Name}:{l.AsBuild(SupportedBuild.Current).IsComplete(install.Game)}:" +
            $"{DownloadedBySteam(steamRoot, l)}:{inSteam.Contains(l.DlcAppId)}")) +
            $"|{runner.IsRunning || setupRunning}|{!OperatingSystem.IsLinux() && SteamLogins.Default.HasOwn}|{unreadable}";
        if (state == languageState) return;
        languageState = state;
        LanguageList.Children.Clear();
        // Said plainly, and the card goes on with what it can see: Steam's own folder is still looked at.
        if (unreadable)
            LanguageList.Children.Add(MutedNote("Steam's list of library folders (steamapps/libraryfolders.vdf) cannot be read right now, " +
                "so a pack that Steam has downloaded or installed in another library does not show here. HellForge looks again every few seconds."));
        AddLanguageNotes(packs, install, steamRoot);
        foreach (var language in packs) LanguageList.Children.Add(LanguageRow(language, install, steamRoot, inSteam.Contains(language.DlcAppId)));
    }

    // Whether Steam's console download of the pack is complete; a Steam folder that cannot be read (a malformed
    // libraryfolders.vdf, say) counts as not downloaded, so the card's refresh never takes the launcher down.
    static bool DownloadedBySteam(string? steamRoot, SupportedLanguage language)
    {
        if (steamRoot == null) return false;
        try { return SteamDepotDownload.IsComplete(steamRoot, language.AsBuild(SupportedBuild.Current)); }
        catch (Exception error) when (error is IOException or InvalidDataException or KeyNotFoundException or UnauthorizedAccessException
            or ArgumentException or NotSupportedException)
        {
            return false;
        }
    }

    // The console steps and the sign-in note, said once above the packs that need them rather than on every row.
    void AddLanguageNotes(IReadOnlyList<SupportedLanguage> packs, InstallLayout install, string? steamRoot)
    {
        var signedIn = !OperatingSystem.IsLinux() && SteamLogins.Default.HasOwn;
        var missing = packs.Any(l => !l.AsBuild(SupportedBuild.Current).IsComplete(install.Game) && !DownloadedBySteam(steamRoot, l));
        if (!missing || signedIn) return;
        LanguageList.Children.Add(MutedNote("Steam downloads a pack for you: open the Steam console (steam://open/console), paste " +
            "the pack's command and press Enter. When Steam says \"Depot download complete\", an Add button appears."));
        if (!OperatingSystem.IsLinux()) LanguageList.Children.Add(MutedNote(SignInNote));
    }

    // The packs Steam has installed, from the game's app manifests alone: the card refreshes every few seconds, and
    // SteamLocator.FindGame hashes the game's executable, which Windows refuses while Steam or the game holds it.
    static HashSet<uint> PacksInSteam(string? steamRoot)
    {
        if (steamRoot == null) return [];
        try
        {
            return [.. LocalInstall.FindManifests(steamRoot).SelectMany(SupportedLanguages.Current.InstalledIn).Select(l => l.DlcAppId)];
        }
        catch (Exception error) when (error is IOException or InvalidDataException or KeyNotFoundException or UnauthorizedAccessException)
        {
            return [];
        }
    }

    // The player's Steam install to copy unchanged files from, or none when it cannot be read now (the download then
    // fetches every file).
    static SteamGame? CopySource(string? steamRoot)
    {
        try { return SteamLocator.FindGame(steamRoot); }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException) { return null; }
    }

    Control LanguageRow(SupportedLanguage language, InstallLayout install, string? steamRoot, bool inSteam)
    {
        var pack = language.AsBuild(SupportedBuild.Current);
        var inCopy = pack.IsComplete(install.Game);
        var downloaded = !inCopy && DownloadedBySteam(steamRoot, language);
        var name = language.Title;
        var state = LanguageState(language, inCopy, downloaded, inSteam);
        var row = new StackPanel { Spacing = 6 };
        row.Children.Add(new TextBlock { Text = $"{name}: {state}", TextWrapping = TextWrapping.Wrap, Foreground = inCopy ? Green : Light });
        if (inCopy)
        {
            row.Children.Add(RemoveLanguageButton(language, install));
            return row;
        }

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        var busy = runner.IsRunning || setupRunning;
        if (busy) ToolTip.SetTip(buttons, "Stop the session first: the game holds its files.");
        if (downloaded)
        {
            buttons.Children.Add(LanguageButton($"Add {name}", true, busy, () => AddLanguageFromSteam(language, install, steamRoot!)));
            row.Children.Add(buttons);
            return row;
        }
        // With no saved sign-in the Steam app's own download comes first: a QR code asking for the account scares
        // players who only wanted a language. The sign-in stays offered, said plainly, as setup offers it.
        var signedIn = !OperatingSystem.IsLinux() && SteamLogins.Default.HasOwn;
        if (!signedIn) AddConsoleCommand(row, buttons, SteamDepotDownload.Commands(pack).Single());
        if (!OperatingSystem.IsLinux())
            buttons.Children.Add(LanguageButton(signedIn ? $"Download {name}" : "Download through HellForge instead", signedIn, busy,
                () => DownloadLanguage(language, install, steamRoot)));
        if (SignOutButton() is { } signOut) buttons.Children.Add(signOut);
        row.Children.Add(buttons);
        return row;
    }

    // Only the pack's own files: none of them is a file of the base build, so the game stays complete. A game left set to
    // that language is set back to English before the next session (GameLanguageCheck).
    Button RemoveLanguageButton(SupportedLanguage language, InstallLayout install)
    {
        var size = $"{language.TotalBytes / 1e9:0.0} GB";
        var button = new Button { Content = "Remove", Classes = { "ghost" }, IsEnabled = !runner.IsRunning && !setupRunning };
        ToolTip.SetTip(button, $"Deletes the pack's {language.Files.Count} files ({size}) from the game copy. It can be added again.");
        button.Click += (_, _) =>
        {
            if ((string?)button.Content == "Remove") { button.Content = $"Delete {size}?"; button.Classes.Set("danger", true); return; }
            if (runner.IsRunning || setupRunning) return;
            try
            {
                foreach (var file in language.Files)
                {
                    // no link in the way, as the copiers check: a link would delete what it points to
                    var path = LocalInstall.NoLinks(Path.Combine(install.Game, file.Path), install.Game);
                    if (File.Exists(path)) File.Delete(path);
                }
                LanguageStatus.Text = $"{language.Title} is removed from the game copy.";
                Append($"Language pack {language.Title}: removed from the game copy.", LauncherLine);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                LanguageStatus.Text = $"{language.Title} could not be removed: {Privacy.Scrub(error.Message)}";
            }
            languageState = "";
            UpdateLanguages();
        };
        return button;
    }

    static string LanguageState(SupportedLanguage language, bool inCopy, bool downloaded, bool inSteam) =>
        inCopy ? "in the game copy" : downloaded ? "downloaded by Steam, ready to add"
            : $"{language.TotalBytes / 1e9:0.0} GB" + (inSteam ? ", installed in Steam" : "");

    const string SignInNote = "Or HellForge downloads it: you approve a sign-in in the Steam app on your phone (a QR code; no password " +
        "is typed here). HellForge uses it only to download from Steam under your licence and keeps it for later downloads, as the " +
        "Mods page's asset fetch does; Sign out of Steam deletes it.";

    static TextBlock MutedNote(string text) => new() { Classes = { "muted" }, TextWrapping = TextWrapping.Wrap, Text = text };

    static Button LanguageButton(string content, bool primary, bool busy, Func<Task> action)
    {
        var button = new Button { Content = content, Classes = { primary ? "primary" : "ghost" }, IsEnabled = !busy };
        button.Click += async (_, _) => await action();
        return button;
    }

    // The Steam app's own download: the console command to paste, with a button that copies it (the steps are above the
    // list, AddLanguageNotes).
    void AddConsoleCommand(StackPanel row, StackPanel buttons, string command)
    {
        row.Children.Add(new SelectableTextBlock { Text = command, FontSize = 12, TextWrapping = TextWrapping.Wrap });
        var copy = new Button { Content = "Copy command", Classes = { "ghost" } };
        copy.Click += async (_, _) => { if (Clipboard is { } clipboard) await clipboard.SetTextAsync(command); };
        buttons.Children.Add(copy);
    }

    // From Steam's own download of the pack (the console command): every file checked against the pinned list, then the
    // download removed, as language-add does.
    async Task AddLanguageFromSteam(SupportedLanguage language, InstallLayout install, string steamRoot)
    {
        var pack = language.AsBuild(SupportedBuild.Current);
        await RunLanguageJob(language, token =>
        {
            GameCopier.CopyFromDepots(steamRoot, install.Game, pack, LanguageCopyProgress(), token);
            SteamDepotDownload.Delete(steamRoot, pack);
            return Task.CompletedTask;
        }, install);
    }

    // From Steam's servers for the signed-in account; the files Steam's own install already has are copied, not fetched.
    async Task DownloadLanguage(SupportedLanguage language, InstallLayout install, string? steamRoot)
    {
        var pack = language.AsBuild(SupportedBuild.Current);
        await RunLanguageJob(language, async token =>
        {
            LanguageStatusText("Connecting to Steam...");
            await using var session = await SteamDepotSession.SignInAsync(new LanguagePrompt(this), token, SteamLogins.Default);
            Dispatcher.UIThread.Post(() => LanguageQr.IsVisible = false);
            var progress = LanguageDownloadProgress();
            // Off the UI thread: finding the Steam install hashes its executable.
            await Task.Run(() => SteamBuildDownload.DownloadAsync(session, new BuildDownloadJob(pack, install.Game, steamRoot, CopySource(steamRoot)),
                LanguageStatusText, progress, token), token);
        }, install);
    }

    async Task RunLanguageJob(SupportedLanguage language, Func<CancellationToken, Task> work, InstallLayout install)
    {
        if (languageCancel != null || runner.IsRunning || setupRunning) return;
        languageCancel = new CancellationTokenSource();
        LanguageProgress.Value = 0;
        LanguageProgress.IsVisible = LanguageCancelButton.IsVisible = true;
        LanguageList.IsEnabled = false;
        string result;
        try
        {
            await Task.Run(() => work(languageCancel.Token), languageCancel.Token);
            var pack = language.AsBuild(SupportedBuild.Current);
            result = pack.IsComplete(install.Game)
                ? $"{language.Title} is in the game copy. Choose it as the voice in the game's options."
                : $"{language.Title} is not complete in the copy yet: try again.";
            Append($"Language pack {language.Title}: {result}", LauncherLine);
        }
        // Everything is caught here: the handlers are async void, so an escaped exception would end the launcher.
        catch (Exception error)
        {
            result = error is OperationCanceledException ? "Cancelled."
                : $"{language.Title} could not be added: {Privacy.Scrub(error.Message)} ({error.GetType().Name})";
            if (error is not OperationCanceledException) Append($"Language pack {language.Title}: {Privacy.Scrub(error.ToString())}", Red);
        }
        finally
        {
            languageCancel.Dispose();
            languageCancel = null;
            LanguageQr.IsVisible = LanguageProgress.IsVisible = LanguageCancelButton.IsVisible = false;
            LanguageList.IsEnabled = true;
        }
        LanguageStatus.Text = result;
        languageState = "";
        UpdateLanguages();
    }

    void LanguageStatusText(string text) => Dispatcher.UIThread.Post(() => LanguageStatus.Text = text);

    // At most five updates a second reach the window, as on the setup page.
    static Func<bool, bool> Throttle()
    {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        long last = -1;
        return final =>
        {
            var now = clock.ElapsedMilliseconds;
            var seen = Interlocked.Read(ref last);
            if (!final && seen >= 0 && now - seen < 200) return false;
            Interlocked.Exchange(ref last, now);
            return true;
        };
    }

    Action<long, long, string> LanguageCopyProgress()
    {
        var due = Throttle();
        return (done, total, file) =>
        {
            if (!due(done >= total)) return;
            Dispatcher.UIThread.Post(() =>
            {
                LanguageProgress.Value = total > 0 ? done * 1000.0 / total : 0;
                LanguageStatus.Text = $"Copying {done / 1e6:0} of {total / 1e6:0} MB  ·  {file}";
            });
        };
    }

    Action<AssembleProgress> LanguageDownloadProgress()
    {
        var due = Throttle();
        return p =>
        {
            if (!due(p.Done >= p.Total)) return;
            Dispatcher.UIThread.Post(() =>
            {
                LanguageProgress.Value = p.Total > 0 ? p.Done * 1000.0 / p.Total : 0;
                LanguageStatus.Text = $"{p.Done / 1e6:0} of {p.Total / 1e6:0} MB  ·  {p.Downloaded / 1e6:0} MB downloaded, " +
                    $"{p.Copied / 1e6:0} MB copied from your install";
            });
        };
    }

    // Sign-in's QR code and messages, on the card (SteamKit2 calls from its own threads).
    // Sign-in's refusals also go to the Console page: when Steam turns down the saved sign-in, the reason must outlast the
    // QR code that replaces it on the card.
    sealed class LanguagePrompt(MainWindow window) : ISignInPrompt
    {
        string? refused;

        public void ShowQr(string challenge) => Dispatcher.UIThread.Post(() =>
        {
            window.LanguageQrCode.Data = QrGeometry(challenge);
            window.LanguageQr.IsVisible = true;
            window.LanguageStatus.Text = (refused != null ? refused + " " : "") + "Scan the code with the Steam app to sign in.";
        });

        public void Status(string text)
        {
            if (text.StartsWith("Steam did not take", StringComparison.Ordinal) || text.StartsWith("Steam no longer accepts", StringComparison.Ordinal) ||
                text.Contains(" did not work: ", StringComparison.Ordinal))
            {
                refused = text;
                // Only refusals reach the Console page: other lines name the Steam account ("Signing in to Steam as ...").
                Dispatcher.UIThread.Post(() => window.Append("Steam sign-in (language pack): " + text, LauncherLine));
            }
            window.LanguageStatusText(text);
        }
    }
}
