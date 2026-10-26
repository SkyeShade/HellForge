using HellForge.Core;

// Language packs (SupportedLanguages): listed from Steam's download at a pinned manifest, found in the player's Steam,
// copied into the game copy like the base build. Every file here is synthetic.
static class LanguagePackTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        string Download(string name)
        {
            var steam = Path.Combine(root, "language-" + name, "Steam");
            var depot = Path.Combine(steam, "steamapps", "content", "app_553850", "depot_2506270");
            Directory.CreateDirectory(Path.Combine(depot, "data"));
            File.WriteAllText(Path.Combine(depot, "data", "0123456789abcdef"), "synthetic japanese strings");
            File.WriteAllText(Path.Combine(depot, "data", "0123456789abcdef.stream"), "synthetic japanese voice");
            return steam;
        }

        test("A language pack is listed from Steam's download and copied into the game copy, checked like the base build", () =>
        {
            var steam = Download("copy");
            var depot = Path.Combine(steam, "steamapps", "content", "app_553850", "depot_2506270");
            var language = SupportedLanguages.FromDownload("japanese", 2506270, "3382210389809902281", depot, null);
            check(language.Files.Count == 2 && language.Files.All(f => f.Depot == 2506270) && language.Files[0].Path == "data/0123456789abcdef",
                "Files listed: " + string.Join(", ", language.Files.Select(f => f.Path)));

            var listPath = Path.Combine(root, "language-copy", "supported-languages.json");
            SupportedLanguages.With(listPath, language).Save(listPath);
            SupportedLanguages list;
            using (var stream = File.OpenRead(listPath)) list = SupportedLanguages.Load(stream);
            check(list.BuildId == SupportedBuild.Current.BuildId && list.Find("Japanese")?.ManifestId == "3382210389809902281", "List not saved.");
            SupportedLanguages.With(listPath, language).Save(listPath);
            using (var stream = File.OpenRead(listPath)) check(SupportedLanguages.Load(stream).Languages.Count == 1, "Adding a pack again listed it twice.");

            var pack = language.AsBuild(SupportedBuild.Current);
            check(SteamDepotDownload.Commands(pack).Single() == "download_depot 553850 2506270 3382210389809902281", "Wrong download command.");
            check(SteamDepotDownload.IsComplete(steam, pack), "The download is not seen as complete.");
            var game = Path.Combine(root, "language-copy", "HellForge", "game");
            GameCopier.CopyFromDepots(steam, game, pack);
            check(pack.IsComplete(game) && pack.Verify(game).Count == 0, "The copied pack does not verify.");

            File.WriteAllText(Path.Combine(game, "data", "0123456789abcdef"), "synthetic japanese strings, edited");
            check(pack.Verify(game).Count == 1, "A changed pack file verified.");
        });

        test("Only the language packs the player's Steam has installed are offered", () =>
        {
            var language = new SupportedLanguage("japanese", 2506270, "1", [new("data/a", 1, "00", 2506270)]);
            var german = new SupportedLanguage("german", 2506220, "2", [new("data/b", 1, "00", 2506220)]);
            var list = new SupportedLanguages(1, SupportedBuild.Current.BuildId, [language, german]);
            var manifest = Path.Combine(root, "language-installed", "appmanifest_553850.acf");
            Directory.CreateDirectory(Path.GetDirectoryName(manifest)!);
            File.WriteAllText(manifest, """
                "AppState" { "appid" "553850" "InstalledDepots" {
                    "553851" { "manifest" "1" "size" "1" }
                    "2506270" { "manifest" "4" "size" "1" "dlcappid" "2506270" } } }
                """);
            var found = list.InstalledIn(manifest);
            check(found.Count == 1 && found[0].Name == "japanese", "Offered: " + string.Join(", ", found.Select(l => l.Name)));
        });

        test("A language pack that would overwrite a base build file, or an empty download, is refused", () =>
        {
            var steam = Path.Combine(root, "language-clash", "Steam");
            var depot = Path.Combine(steam, "depot_2506270");
            var basePath = SupportedBuild.Current.Files[0].Path;
            Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(depot, basePath))!);
            File.WriteAllText(Path.Combine(depot, basePath), "synthetic");
            reject(() => SupportedLanguages.FromDownload("japanese", 2506270, "1", depot, null));
            var empty = Directory.CreateDirectory(Path.Combine(root, "language-empty", "depot_2506270")).FullName;
            reject(() => SupportedLanguages.FromDownload("japanese", 2506270, "1", empty, null));
        });
    }
}
