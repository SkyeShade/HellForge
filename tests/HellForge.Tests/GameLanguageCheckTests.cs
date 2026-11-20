using HellForge.Core;

// The language check before a session (src/HellForge.Core/GameLanguageCheck.cs, #343): a text or voice language whose
// pack is not in the game copy is set back to English in the profile's settings, and nothing else in them changes.
static class GameLanguageCheckTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var folder = Path.Combine(root, "language-check");
        var game = Path.Combine(folder, "game");
        var japanese = new SupportedLanguage("japanese", 2506270, "1", [new SupportedFile("data/2b6904ecb991fcf1", 3, "", 2506270)]);
        var languages = new SupportedLanguages(1, SupportedBuild.Current.BuildId, [japanese]);
        const string Settings = "\nlanguage = \"us\"\nversion = 53\nvo_language = \"jp\"\nplatform_language = \"jp\"\nrender_settings = {\n\tlanguage = \"jp\"\n}\n";

        string Profile(string name, string text)
        {
            var saves = Directory.CreateDirectory(Path.Combine(folder, name, "saves")).FullName;
            File.WriteAllText(Path.Combine(saves, "110000115bf3081_user_settings.config"), text);
            return Path.Combine(folder, name);
        }
        string Read(string profile) => File.ReadAllText(Path.Combine(profile, "saves", "110000115bf3081_user_settings.config"));

        test("A voice language whose pack is not in the copy is set back to English, the rest of the settings kept (#343)", () =>
        {
            Directory.CreateDirectory(Path.Combine(game, "data"));
            var profile = Profile("japanese-missing", Settings);
            var fixes = GameLanguageCheck.Apply(profile, game, languages);
            check(fixes.Count == 1 && fixes[0].Key == "vo_language" && fixes[0].Code == "jp" && fixes[0].Pack == "japanese",
                "Fixes: " + string.Join(", ", fixes));
            check(Read(profile) == Settings.Replace("vo_language = \"jp\"", "vo_language = \"us\""), "The settings now: " + Read(profile));
            var said = GameLanguageCheck.Describe(fixes[0]);
            check(said.Contains("voice language was Japanese", StringComparison.Ordinal) && said.Contains("It is English now", StringComparison.Ordinal) &&
                said.Contains("language-add --name japanese", StringComparison.Ordinal), "Said: " + said);
            check(GameLanguageCheck.Apply(profile, game, languages).Count == 0, "A second check changed something.");
        });

        test("A language whose pack is in the copy, English and an empty profile are left alone", () =>
        {
            File.WriteAllBytes(Path.Combine(game, "data", "2b6904ecb991fcf1"), [1, 2, 3]);
            var profile = Profile("japanese-present", Settings);
            check(GameLanguageCheck.Apply(profile, game, languages).Count == 0 && Read(profile) == Settings, "Japanese with its pack was changed.");
            check(GameLanguageCheck.Apply(Path.Combine(folder, "no-profile"), game, languages).Count == 0, "A profile without saves.");
            // an unknown code: kept while the copy has some pack (it may be that one), set back with none
            var other = Profile("unknown-code", "vo_language = \"zz\"\n");
            check(GameLanguageCheck.Apply(other, game, languages).Count == 0, "An unknown code was set back beside a pack.");
            File.Delete(Path.Combine(game, "data", "2b6904ecb991fcf1"));
            var fixes = GameLanguageCheck.Apply(other, game, languages);
            check(fixes.Count == 1 && fixes[0].Key == "vo_language" && fixes[0].Pack == null && Read(other) == "vo_language = \"us\"\n", "Unknown code: " + Read(other));
            check(GameLanguageCheck.Describe(fixes[0]).Contains("voice language was \"zz\"", StringComparison.Ordinal) &&
                GameLanguageCheck.Describe(fixes[0]).Contains("cannot add that language yet", StringComparison.Ordinal), GameLanguageCheck.Describe(fixes[0]));
            var german = GameLanguageCheck.Apply(Profile("german", "vo_language = \"de\"\nlanguage = \"gb\"\n"), game, languages);
            // every text language is in the base build: German text without its pack is left alone
            check(GameLanguageCheck.Apply(Profile("german-text", "language = \"de\"\nvo_language = \"us\"\n"), game, languages).Count == 0,
                "German text, which the base build carries, was set back.");
            var latam = GameLanguageCheck.Apply(Profile("latam", "language = \"ms\"\nvo_language = \"ms\"\n"), game, languages);
            check(latam.Count == 1 && latam[0].Key == "vo_language" && latam[0].Pack == "latam" &&
                GameLanguageCheck.Describe(latam[0]).Contains("was Spanish (Latin America)", StringComparison.Ordinal),
                "Latin American Spanish (ms): " + string.Join(", ", latam));
            check(german.Count == 1 && german[0].Pack == "german" && GameLanguageCheck.Describe(german[0]).Contains("language-add --name german", StringComparison.Ordinal),
                "German, which this list lacks but HellForge pins: " + string.Join(", ", german));
        });
    }
}
