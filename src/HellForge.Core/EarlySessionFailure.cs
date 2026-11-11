using System.Text.RegularExpressions;

namespace HellForge.Core;

// Why a session stopped before its run folder existed. The session runner appends its reason to logs/session-runner.log
// (HellForge.Session's Program), and on Linux the wrapper keeps its own output in logs/linux-session-wrapper.log, which
// it starts anew for each run. The launcher and the CLI show the reason itself; naming the files alone left a player
// with "exited before starting" and nothing to act on.
public static partial class EarlySessionFailure
{
    public const string RunnerLog = "session-runner.log";
    public const string LinuxWrapperLog = "linux-session-wrapper.log";
    // A file's time can be older than its write by this much: FAT and exFAT cards keep times to two seconds.
    static readonly TimeSpan TimeSlack = TimeSpan.FromSeconds(2);

    [GeneratedRegex(@"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ")]
    private static partial Regex Stamp();

    // The session runner's reason for the start made at sinceUtc: its last entry in the log, without the time. Null when
    // the log was not written since then (the reason of an older start must not be shown for this one).
    public static string? Reason(string logsFolder, DateTime sinceUtc)
    {
        if (FreshLines(Path.Combine(logsFolder, RunnerLog), sinceUtc) is not { Count: > 0 } lines) return null;
        // An entry is a stamped line; a reason with line breaks continues on the lines after it.
        var first = lines.FindLastIndex(line => Stamp().IsMatch(line));
        var entry = first < 0 ? lines[^1] : string.Join(" ", lines.Skip(first));
        entry = Stamp().Replace(entry, "").Trim();
        return entry.Length > 0 ? entry : null;
    }

    // What the wrapper writes on its way out, after the line that says why it stops.
    internal static readonly string[] WrapperClosingLines =
    [
        "HellForge wrapper exiting with code",
        "HellForge wrapper received SIG",
        "Offline namespace command exited with code",
        "Steam bridge did not stop after",
        "HellForge is running inside the container"
    ];

    // Linux, for a stop before the session runner ran: the wrapper's own failure line for this start when it wrote one
    // (the lines SteamOwnershipCheck knows), else the last line (Bash, Proton) above its closing lines, named as that.
    public static string? WrapperReason(string logsFolder, DateTime sinceUtc)
    {
        if (FreshLines(Path.Combine(logsFolder, LinuxWrapperLog), sinceUtc) is not { Count: > 0 } lines) return null;
        if (SteamOwnershipCheck.LinuxWrapperProblem(string.Join('\n', lines)) is { } known)
        {
            // A failure line that ends in a colon has its command on the next line.
            var next = lines.FindIndex(line => line.Trim() == known) + 1;
            return known.EndsWith(':') && next < lines.Count ? known + " " + lines[next].Trim() : known;
        }
        var last = lines.FindLast(line => !WrapperClosingLines.Any(closing => line.Contains(closing, StringComparison.Ordinal)));
        return last == null ? null : "The last line of " + LinuxWrapperLog + " before the wrapper stopped: " + last.Trim();
    }

    static List<string>? FreshLines(string log, DateTime sinceUtc)
    {
        try
        {
            if (!File.Exists(log) || File.GetLastWriteTimeUtc(log) + TimeSlack < sinceUtc) return null;
            using var stream = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            return [.. reader.ReadToEnd().Split('\n').Select(line => line.TrimEnd('\r')).Where(line => line.Trim().Length > 0)];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }
}
