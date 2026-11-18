using System.Text;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The game's voice language against the language packs in the game copy (#343). The game keeps it in each account's
// saves\<account>_user_settings.config, as `vo_language = "us"` beside the text's `language = "us"` (game.dll
// 0x102F7A0 writes them, 0x1030260 reads them back by name). A pack holds a language's voice; every text language is in
// the base build, so the text setting is left alone. A copy without the chosen voice's pack crashes the game, on every
// start, since the setting stays. Before a session starts, such a voice is set back to English, and the session says so.
public static partial class GameLanguageCheck
{
    // The voice the base build carries: English.
    static readonly HashSet<string> InBase = new(StringComparer.Ordinal) { "us", "gb" };

    // The game's codes of the voices HellForge pins, read from the settings file once each was chosen in game. Latin
    // American Spanish is "ms".
    static readonly Dictionary<string, string> Packs = new(StringComparer.Ordinal)
    {
        ["de"] = "german", ["fr"] = "french", ["it"] = "italian", ["es"] = "spanish", ["ms"] = "latam",
        ["bp"] = "brazilian", ["jp"] = "japanese", ["cn"] = "chinese",
    };

    // One setting set back: the file, the key (vo_language), the code it had and its pack's name if known.
    public sealed record Fix(string File, string Key, string Code, string? Pack);


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

    // A code the copy can play: one the base build carries, a pack in the copy, or an unknown code while the copy has
    // some pack (it may be that one).
    static bool Playable(string code, HashSet<string> inCopy) =>
        InBase.Contains(code) || (Packs.TryGetValue(code, out var pack) ? inCopy.Contains(pack) : inCopy.Count > 0);

    // What the session says for one fix.
    public static string Describe(Fix fix)
    {
        var pinned = fix.Pack != null ? SupportedLanguages.Current.Find(fix.Pack) : null;
        var language = pinned?.Title ?? (fix.Pack != null ? char.ToUpperInvariant(fix.Pack[0]) + fix.Pack[1..] : $"\"{fix.Code}\"");
        var add = pinned != null
            ? $"Add {language} in Settings > Languages, or with HellForge.Cli language-add --name {fix.Pack}, then " +
              "choose it again in the game."
            : "HellForge cannot add that language yet.";
        return $"The game's voice language was {language}, whose language pack is not in this installation's game copy, and " +
            $"the game would crash as it starts. It is English now. {add}";
    }

    // A top-level `vo_language = "xx"` line (nested blocks are indented).
    [GeneratedRegex(@"^(?<head>(?<key>vo_language)[ \t]*=[ \t]*)""(?<code>[^""\r\n]*)""", RegexOptions.Multiline)]
    private static partial Regex Setting();
}
