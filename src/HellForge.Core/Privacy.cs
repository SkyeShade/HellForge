using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// Logs get passed around (Save logs to Desktop, bug reports, screenshots of the launcher), so what HellForge writes in
// them and shows as errors does not name the PC it ran on: the Windows user folder (C:\Users\<account>) reads
// %USERPROFILE%, a Linux home folder (Wine/Proton) ~, and the computer's name PC. Paths stay readable, and a
// %USERPROFILE% path still opens typed into Explorer. Only whole paths and whole names are replaced: a mod id that
// starts with the account's name stays as it is. Native logs do the same (native/common/hf_privacy.h).
public sealed class Privacy
{
    public const string ProfileMark = "%USERPROFILE%";
    public const string HomeMark = "~";
    public const string ComputerMark = "PC";

    // Text logs that the scrub may rewrite (Save logs to Desktop); anything else is copied as it is.
    static readonly HashSet<string> TextExtensions = new(StringComparer.OrdinalIgnoreCase)
        { ".log", ".txt", ".json", ".jsonl", ".tsv", ".csv", ".ini", ".config", ".xml", ".md" };

    static readonly Lazy<Privacy> current = new(() => new Privacy(CurrentProfile(), CurrentComputerNames(), CurrentUnixHome()));

    public static Privacy Current => current.Value;

    // Short-hands for the PC this runs on.
    public static string Scrub(string? text) => Current.Apply(text);
    public static string ScrubJson(JsonNode node) => Current.ApplyJson(node);

    readonly Regex? pattern;

    // profile: the Windows user folder; computerNames: the names this PC goes by (NetBIOS, DNS); unixHome: the Linux
    // home folder a Wine/Proton session sees (HOME). Names shorter than 3 characters are not replaced (too common).
    public Privacy(string? profile, IEnumerable<string?> computerNames, string? unixHome = null)
    {
        var parts = new List<string>();
        if (FolderPattern(profile) is { } user) parts.Add("(?<profile>" + user + ")");
        if (unixHome != null && unixHome.StartsWith('/') && FolderPattern(unixHome) is { } home) parts.Add("(?<home>" + home + ")");
        var names = computerNames.Where(n => !string.IsNullOrWhiteSpace(n) && n!.Trim().Length >= 3).Select(n => Regex.Escape(n!.Trim()))
            .Distinct(StringComparer.OrdinalIgnoreCase).OrderByDescending(n => n.Length).ToList();
        // A hyphen counts as a boundary: the game names its console logs console_<time>-<id>-<computer>.txt.
        if (names.Count > 0) parts.Add(@"(?<![\p{L}\p{N}_])(?<pc>" + string.Join("|", names) + @")(?![\p{L}\p{N}_])");
        pattern = parts.Count > 0 ? new Regex(string.Join("|", parts), RegexOptions.IgnoreCase | RegexOptions.CultureInvariant) : null;
    }

    // A folder as written in logs: either slash, a doubled backslash where JSON escapes it, any letter case; a whole
    // folder name only (C:\Users\Al does not match inside C:\Users\Alex).
    static string? FolderPattern(string? folder)
    {
        if (string.IsNullOrWhiteSpace(folder)) return null;
        var names = folder.Split('\\', '/').Where(s => s.Length > 0).ToList();
        if (names.Count < 2) return null;
        var separator = @"(?:\\\\|\\|/)";
        return (folder.StartsWith('/') ? separator : "") + string.Join(separator, names.Select(Regex.Escape)) + @"(?![\p{L}\p{N}_-])";
    }

    public string Apply(string? text)
    {
        if (string.IsNullOrEmpty(text) || pattern == null) return text ?? "";
        return pattern.Replace(text, m => m.Groups["profile"].Success ? ProfileMark : m.Groups["home"].Success ? HomeMark : ComputerMark);
    }

    // JSON as text with every string scrubbed. Non-ASCII stays unescaped, so a user folder with an accented name is found.
    public string ApplyJson(JsonNode node) =>
        Apply(node.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));

    public static bool IsText(string path) => TextExtensions.Contains(Path.GetExtension(path));

    // A text file's bytes scrubbed (UTF-8, a BOM kept), or the bytes as they are when it is not UTF-8 text.
    public byte[] ApplyBytes(byte[] bytes) => ApplyBytes(bytes, Apply);

    // The same with another scrub (ReportPrivacy's, which goes further).
    public static byte[] ApplyBytes(byte[] bytes, Func<string, string> apply)
    {
        var bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
        string text;
        try { text = new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0)); }
        catch (DecoderFallbackException) { return bytes; }
        if (text.Contains('\0')) return bytes;
        var scrubbed = apply(text);
        if (ReferenceEquals(scrubbed, text) || scrubbed == text) return bytes;
        var body = Encoding.UTF8.GetBytes(scrubbed);
        return bom ? [0xEF, 0xBB, 0xBF, .. body] : body;
    }

    static string? CurrentProfile()
    {
        try
        {
            var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return string.IsNullOrEmpty(profile) ? Environment.GetEnvironmentVariable("USERPROFILE") : profile;
        }
        catch (PlatformNotSupportedException) { return Environment.GetEnvironmentVariable("USERPROFILE"); }
    }

    static IEnumerable<string?> CurrentComputerNames()
    {
        var names = new List<string?> { Environment.GetEnvironmentVariable("COMPUTERNAME") };
        try { names.Add(Environment.MachineName); } catch (InvalidOperationException) { }
        try { names.Add(System.Net.Dns.GetHostName()); } catch (System.Net.Sockets.SocketException) { }
        return names;
    }

    static string? CurrentUnixHome() => Environment.GetEnvironmentVariable("HOME") is { } home && home.StartsWith('/') ? home : null;
}
