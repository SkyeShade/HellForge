using System.Globalization;
using System.Net;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// A crash report is made to be attached to a public GitHub issue, so it leaves out more than Privacy does (the user
// folder, the Linux home folder, the PC's names): Steam ids (SteamID64 as a number, and the hexadecimal form the backend
// logs peers by), IP addresses other than loopback and 0.x (which version numbers look like), and the names of the players the session met, as far as
// the run's files name them (NamesIn). Each distinct id, address or name gets a numbered placeholder of its own
// (steam-id-1, ip-1, player-1), so lines about the same peer still read as the same peer. One instance per report.
public sealed partial class ReportPrivacy
{
    readonly Privacy privacy;
    readonly Regex? names;
    readonly Dictionary<string, string> tokens = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, int> counts = new(StringComparer.Ordinal);

    // SteamID64: individual accounts are 76561197960265728 and up, 7656119 followed by 10 digits, or 765612 followed by 11
    // from account id 2,039,734,272 on; in hexadecimal, 0x0110000100000000 and up
    [GeneratedRegex(@"(?<![0-9])(?:7656119[0-9]{10}|765612[0-9]{11})(?![0-9])")]
    private static partial Regex SteamDecimal();

    [GeneratedRegex(@"(?<![0-9A-Za-z])(?:0x)?0*1100001[0-9A-Fa-f]{8}(?![0-9A-Za-z])")]
    private static partial Regex SteamHex();

    [GeneratedRegex(@"(?<![0-9.])(?:(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])(?![0-9.])")]
    private static partial Regex IPv4();

    [GeneratedRegex(@"(?<![0-9A-Za-z_:])[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7}(?:%[0-9A-Za-z]+)?(?![0-9A-Za-z_:])")]
    private static partial Regex IPv6();

    public ReportPrivacy(Privacy privacy, IEnumerable<string?> playerNames)
    {
        this.privacy = privacy;
        var listed = playerNames.Where(n => !string.IsNullOrWhiteSpace(n) && n!.Trim().Length >= 3).Select(n => Regex.Escape(n!.Trim()))
            .Distinct(StringComparer.OrdinalIgnoreCase).OrderByDescending(n => n.Length).ToList();
        if (listed.Count > 0)
            names = new Regex(@"(?<![\p{L}\p{N}_])(?:" + string.Join("|", listed) + @")(?![\p{L}\p{N}_])", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    }

    string Token(string kind, string key)
    {
        if (tokens.TryGetValue(kind + "\n" + key, out var token)) return token;
        counts[kind] = counts.GetValueOrDefault(kind) + 1;
        return tokens[kind + "\n" + key] = $"{kind}-{counts[kind].ToString(CultureInfo.InvariantCulture)}";
    }

    // The decimal and the hexadecimal form of one account get the same placeholder.
    string SteamToken(ulong id) => Token("steam-id", id.ToString(CultureInfo.InvariantCulture));

    string IPv4Token(Match m) =>
        m.Value.StartsWith("127.", StringComparison.Ordinal) || m.Value.StartsWith("0.", StringComparison.Ordinal) ? m.Value : Token("ip", m.Value);

    // A candidate is an address only if it has "::" or at least five colons (a time of day, 12:34:56, has two), two
    // groups of digits and parses.
    string IPv6Token(Match m)
    {
        var text = m.Value;
        var groups = text.Split('%')[0].Split(':');
        if (!(text.Contains("::", StringComparison.Ordinal) || groups.Length >= 6) || groups.Count(g => g.Length > 0) < 2) return text;
        if (!IPAddress.TryParse(text, out var address) || IPAddress.IsLoopback(address) || address.Equals(IPAddress.IPv6Any)) return text;
        return Token("ip", address.ToString());
    }

    public string Apply(string? text)
    {
        var scrubbed = privacy.Apply(text);
        if (scrubbed.Length == 0) return scrubbed;
        scrubbed = SteamDecimal().Replace(scrubbed, m => SteamToken(ulong.Parse(m.Value, CultureInfo.InvariantCulture)));
        scrubbed = SteamHex().Replace(scrubbed, m => SteamToken(ulong.Parse(m.Value.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? m.Value[2..] : m.Value,
            NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)));
        scrubbed = IPv4().Replace(scrubbed, IPv4Token);
        scrubbed = IPv6().Replace(scrubbed, IPv6Token);
        return names == null ? scrubbed : names.Replace(scrubbed, m => Token("player", m.Value));
    }

    // For what HellForge writes from known fields (versions, mods): the PC's names and the players' only, so that a
    // four-part version number is not taken for an IP address.
    public string ApplyNames(string? text)
    {
        var scrubbed = privacy.Apply(text);
        return names == null || scrubbed.Length == 0 ? scrubbed : names.Replace(scrubbed, m => Token("player", m.Value));
    }

    public byte[] ApplyBytes(byte[] bytes) => Privacy.ApplyBytes(bytes, Apply);

    // The names of other players a run's files give, for the report to leave out: whose mods a mod sync played
    // (result.json mods.syncedWith), and the friends and hosts the backend's multiplayer log names. A best effort: the
    // game's own logs can name players in ways HellForge does not know, which is why the player reads the report first.
    public static List<string> NamesIn(string run)
    {
        var found = new List<string>();
        if (SessionFiles.ReadJson(Path.Combine(run, "result.json")) is { } result && result.TryGetProperty("mods", out var mods) &&
            SessionFiles.Text(mods, "syncedWith") is { Length: > 0 } synced && synced != "null")
            found.Add(synced);
        var log = Path.Combine(run, "backend", "multiplayer.log");
        if (!File.Exists(log)) return found;
        try
        {
            using var reader = new StreamReader(new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            for (var line = reader.ReadLine(); line != null; line = reader.ReadLine())
            {
                if (NamedPlayer().Match(line) is { Success: true } named) found.Add(named.Groups["name"].Value);
                if (FriendsList().Match(line) is { Success: true } listed) found.AddRange(listed.Groups["names"].Value.Split(", "));
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return [.. found.Where(n => !SteamHex().IsMatch(n)).Distinct(StringComparer.OrdinalIgnoreCase)];
    }

    // FriendPresence's "Profile of <name> received", ModSync's "<name>'s lobby" and AutoRejoin's "plays <name>'s mods"
    [GeneratedRegex(@"(?:Profile of (?<name>.+?) received for the friends list|Mod sync: (?<name>.+?)'s lobby:|Rejoin: this session plays (?<name>.+?)'s mods)")]
    private static partial Regex NamedPlayer();

    // FriendPresence's "Steam friends running HellForge: N (<name>, <name>)": friends counted whose profile never arrived
    // are named only here. A name with ", " in it is split, and its parts are left out.
    [GeneratedRegex(@"Steam friends running HellForge: [0-9]+ \((?<names>.+)\)\.$")]
    private static partial Regex FriendsList();
}
