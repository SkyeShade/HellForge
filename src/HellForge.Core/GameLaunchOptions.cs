namespace HellForge.Core;

// The player's launch options for the game, as Steam's Launch Options take them (--use-d3d11 for DirectX 11, say). The
// launcher keeps the text in its settings; the session runner adds the options after its own on the game's command line.
// Steam's form "%command% --use-d3d11" is understood: what follows %command% is used, and nothing may come before it (that
// part sets Linux environment variables, which do nothing here). The switches a session sets itself are refused: the
// bundle folder is the verified copy's.
public static class GameLaunchOptions
{
    public const int MaxLength = 1024;
    const string SteamCommand = "%command%";
    static readonly string[] SessionOwned = ["--bundle-dir"];

    // The options, one per argument, or InvalidDataException with what is wrong (the launcher shows it as it is).
    public static List<string> Parse(string? text)
    {
        text ??= "";
        if (text.Length > MaxLength) throw new InvalidDataException($"Launch options are limited to {MaxLength} characters.");
        if (text.Any(char.IsControl)) throw new InvalidDataException("Launch options are one line of text.");
        var options = Split(text);
        var command = options.FindIndex(o => o.Equals(SteamCommand, StringComparison.OrdinalIgnoreCase));
        if (command > 0)
            throw new InvalidDataException($"Nothing goes before {SteamCommand} here (on Linux it sets environment variables): write only the game's options, such as --use-d3d11.");
        if (command == 0) options.RemoveAt(0);
        if (options.FirstOrDefault(o => o.Contains(SteamCommand, StringComparison.OrdinalIgnoreCase)) is { } repeated)
            throw new InvalidDataException($"{repeated}: {SteamCommand} may appear once, first.");
        foreach (var option in options)
            if (SessionOwned.FirstOrDefault(s => option.Equals(s, StringComparison.OrdinalIgnoreCase) || option.StartsWith(s + "=", StringComparison.OrdinalIgnoreCase)) is { } owned)
                throw new InvalidDataException($"{owned} is set by HellForge for every session and cannot be changed.");
        return options;
    }

    // What the options do that a player would not expect, in plain words (the launcher shows each under the box and when
    // a session starts). --toaster_mode turns the game's texture streaming off: Armory icons are pictures the game renders
    // once their textures have streamed in, so with it they stay blank.
    public static List<string> Warnings(IEnumerable<string> options)
    {
        var warnings = new List<string>();
        if (options.Any(o => o.Equals("--toaster_mode", StringComparison.OrdinalIgnoreCase)))
            warnings.Add("--toaster_mode turns the game's texture streaming off, so Armory icons stay blank (the game draws them only once their textures have loaded). Remove it to see them.");
        return warnings;
    }

    // The options as they go on a command line: quoted where they hold spaces.
    public static string CommandLine(IEnumerable<string> options) =>
        string.Join(" ", options.Select(o => o.Any(char.IsWhiteSpace) || o.Length == 0 ? "\"" + Backslashes(o) + "\"" : o));

    // Windows' rules for the cases a player types: spaces separate, double quotes group (and are dropped).
    static List<string> Split(string text)
    {
        var options = new List<string>();
        var current = new System.Text.StringBuilder();
        bool quoted = false, any = false;
        foreach (var c in text)
        {
            if (c == '"') { quoted = !quoted; any = true; }
            else if (char.IsWhiteSpace(c) && !quoted)
            {
                if (any) options.Add(current.ToString());
                current.Clear();
                any = false;
            }
            else { current.Append(c); any = true; }
        }
        if (quoted) throw new InvalidDataException("Launch options have a quote that is not closed.");
        if (any) options.Add(current.ToString());
        return options;
    }

    // Backslashes before the closing quote are doubled, so they stay backslashes (C:\Some Folder\).
    static string Backslashes(string option)
    {
        var trailing = option.Length - option.TrimEnd('\\').Length;
        return option + new string('\\', trailing);
    }
}
