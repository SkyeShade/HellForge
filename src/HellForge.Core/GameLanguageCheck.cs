using System.Text;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The game's text and voice language against the language packs in the game copy (#343). The game keeps them in each
// account's saves\<account>_user_settings.config, as `language = "us"` and `vo_language = "us"` (game.dll 0x102F7A0
// writes them, 0x1030260 reads them back by name). Every language but English comes in a pack of its own, and a copy
// without the chosen one crashes the game as it starts, on every start, since the setting stays. Before a session
// starts, a language whose pack the copy lacks is set back to English, and the session says so.
public static partial class GameLanguageCheck
{
    // English in both of its forms; anything else needs its pack.
    static readonly HashSet<string> English = new(StringComparer.Ordinal) { "us", "gb" };

    // The game's codes of the languages HellForge pins or knows the Steam name of.
    static readonly Dictionary<string, string> Packs = new(StringComparer.Ordinal) { ["jp"] = "japanese", ["de"] = "german" };

    // One setting set back: the file, the key (language or vo_language), the code it had and its pack's name if known.
    public sealed record Fix(string File, string Key, string Code, string? Pack);

    // The keys a session checks, as the player sees them.
    static string What(string key) => key == "vo_language" ? "voice language" : "text language";

    // Sets back every language in the profile's settings files that the copy has no pack for; the fixes made.
    public static IReadOnlyList<Fix> Apply(string gameProfile, string gameRoot, SupportedLanguages languages)
    {
        var saves = Path.Combine(gameProfile, "saves");
        if (!Directory.Exists(saves)) return [];
        var inCopy = languages.InCopy(gameRoot).Select(l => l.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var fixes = new List<Fix>();
        foreach (var file in LocalInstall.FilesNoLinks(saves, "*_user_settings.config", recursive: false))
        {
            var text = File.ReadAllText(file);
            var found = new List<Fix>();
            var fixedText = Setting().Replace(text, match =>
            {
                var code = match.Groups["code"].Value;
                if (Playable(code, inCopy)) return match.Value;
                found.Add(new(file, match.Groups["key"].Value, code, Packs.GetValueOrDefault(code)));
                return match.Groups["head"].Value + "\"us\"";
            });
            if (found.Count == 0) continue;
            File.WriteAllText(file, fixedText, new UTF8Encoding(false));
            fixes.AddRange(found);
        }
        return fixes;
    }

    // A code the copy can play: English, a pack in the copy, or an unknown code while the copy has some pack (it may be
    // that one).
    static bool Playable(string code, HashSet<string> inCopy) =>
        English.Contains(code) || (Packs.TryGetValue(code, out var pack) ? inCopy.Contains(pack) : inCopy.Count > 0);

    // What the session says for one fix.
    public static string Describe(Fix fix)
    {
        var language = fix.Pack != null ? char.ToUpperInvariant(fix.Pack[0]) + fix.Pack[1..] : $"\"{fix.Code}\"";
        var add = fix.Pack != null && SupportedLanguages.Current.Find(fix.Pack) != null
            ? $"Add {language} with HellForge.Cli language-add --name {fix.Pack} (Troubleshooting, \"Playing in a language other " +
              "than English\"), then choose it again in the game."
            : "HellForge cannot add that language yet.";
        return $"The game's {What(fix.Key)} was {language}, whose language pack is not in this installation's game copy, and " +
            $"the game would crash as it starts. It is English now. {add}";
    }

    // A top-level `language = "xx"` or `vo_language = "xx"` line (nested blocks are indented).
    [GeneratedRegex(@"^(?<head>(?<key>language|vo_language)[ \t]*=[ \t]*)""(?<code>[^""\r\n]*)""", RegexOptions.Multiline)]
    private static partial Regex Setting();
}
