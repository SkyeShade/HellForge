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

        test("The Steam stand-in is told each language DLC and whether its pack is in the copy", () =>
        {
            var japanese = new SupportedLanguage("japanese", 2506270, "1", [new SupportedFile("data/2b6904ecb991fcf1", 3, "", 2506270)]);
            var german = new SupportedLanguage("german", 2506220, "1", [new SupportedFile("data/2d79d624be0debf8", 2, "", 2506220)]);
            var list = new SupportedLanguages(1, SupportedBuild.Current.BuildId, [japanese, german]);
            var copy = Directory.CreateDirectory(Path.Combine(root, "dlc-copy", "data")).Parent!.FullName;
            File.WriteAllBytes(Path.Combine(copy, "data", "2b6904ecb991fcf1"), [1, 2, 3]);
            check(list.DlcInCopy(copy) == "2506270=1,2506220=0", "Listed: " + list.DlcInCopy(copy));
            File.WriteAllBytes(Path.Combine(copy, "data", "2d79d624be0debf8"), [1]);
            check(list.DlcInCopy(copy) == "2506270=1,2506220=0", "A pack with a file of the wrong size counted: " + list.DlcInCopy(copy));
            // the stand-in's parser takes at most 1023 characters (language_dlc.c)
            check(SupportedLanguages.Current.DlcInCopy(copy).Length < 1024, "The pinned list no longer fits the stand-in's buffer.");
        });

        test("The pinned packs share no file with each other, and each has a name the player can read", () =>
        {
            var packs = SupportedLanguages.Current.Languages;
            check(packs.Count >= 8, "Pinned: " + string.Join(", ", packs.Select(l => l.Name)));
            var shared = packs.SelectMany(l => l.Files).GroupBy(f => f.Path).FirstOrDefault(g => g.Count() > 1);
            check(shared == null, "In two packs: " + shared?.Key);
            check(packs.All(l => l.Files.All(f => f.Depot == l.Depot)), "A file names another depot.");
            check(SupportedLanguages.Current.Find("latam")?.Title == "Spanish (Latin America)" &&
                SupportedLanguages.Current.Find("german")?.Title == "German", "Titles: " + string.Join(", ", packs.Select(l => l.Title)));
        });
    }
}
