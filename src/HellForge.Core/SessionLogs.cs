using System.Text.RegularExpressions;

namespace HellForge.Core;

// A session's logs (logs/sessions/<run id>: the session runner's result, the mod loader's and the backend's logs, the
// game's own, a crash dump) stay for the next sessions to compare, but not for ever: with crash dumps and engine logs
// they reached gigabytes. When the launcher opens and when a session ends, only the newest Keep stay, and of the older
// ones the newest KeepCrashes that ended in a crash (CrashReport.Crashed): a crash report is often asked for after a few
// more sessions. Only folders named as a run id, directly in the sessions folder, are removed; a folder in use (a
// session still writing) is left.
public static partial class SessionLogs
{
    public const int Keep = 10;
    public const int KeepCrashes = 5;

    [GeneratedRegex("^[0-9a-f]{32}$")]
    private static partial Regex RunId();

    public sealed record Pruned(int Removed, long Bytes, int Skipped);

    public static Pruned Prune(string sessionsFolder, int keep = Keep, int keepCrashes = KeepCrashes)
    {
        if (!Directory.Exists(sessionsFolder)) return new(0, 0, 0);
        LocalInstall.NoLinks(sessionsFolder);
        var runs = new DirectoryInfo(sessionsFolder).GetDirectories()
            .Where(d => RunId().IsMatch(d.Name) && (d.Attributes & FileAttributes.ReparsePoint) == 0)
            .OrderByDescending(d => d.CreationTimeUtc).ThenByDescending(d => d.Name, StringComparer.Ordinal).ToList();
        int removed = 0, skipped = 0;
        long bytes = 0;
        var older = runs.Skip(keep).ToList();
        var crashes = older.Where(run => CrashReport.Crashed(run.FullName)).Take(keepCrashes).Select(run => run.FullName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var run in older.Where(run => !crashes.Contains(run.FullName)))
        {
            long size = 0;
            try
            {
                size = run.EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                run.Delete(recursive: true);
                removed++;
                bytes += size;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { skipped++; }
        }
        return new(removed, bytes, skipped);
    }

    // A session as its run folder and result.json tell it (HellForge.Cli logs; launchers of their own list these).
    public sealed record Run(string Id, string Folder, DateTime StartedUtc, string? Status, string? Failure, string? GameExitCode,
        DateTime? CompletedUtc);

    // The sessions in the sessions folder, newest first. A run whose result.json cannot be read (a session still starting,
    // or one cut short) is listed with what its folder tells.
    public static List<Run> Runs(string sessionsFolder)
    {
        if (!Directory.Exists(sessionsFolder)) return [];
        var runs = new List<Run>();
        foreach (var folder in new DirectoryInfo(sessionsFolder).GetDirectories().Where(d => RunId().IsMatch(d.Name)))
        {
            string? Text(System.Text.Json.JsonElement root, string name) =>
                root.TryGetProperty(name, out var value) && value.ValueKind == System.Text.Json.JsonValueKind.String ? value.GetString() : null;
            DateTime? Time(string? text) => DateTime.TryParse(text, System.Globalization.CultureInfo.InvariantCulture,
                System.Globalization.DateTimeStyles.AdjustToUniversal | System.Globalization.DateTimeStyles.AssumeUniversal, out var time) ? time : null;
            string? status = null, failure = null, exit = null;
            DateTime? started = null, completed = null;
            try
            {
                var result = Path.Combine(folder.FullName, "result.json");
                if (File.Exists(result))
                {
                    using var stream = new FileStream(result, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    using var document = System.Text.Json.JsonDocument.Parse(stream);
                    var root = document.RootElement;
                    (status, failure, exit) = (Text(root, "status"), Text(root, "failure"), Text(root, "gameExitCode"));
                    (started, completed) = (Time(Text(root, "startedUtc")), Time(Text(root, "completedUtc")));
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { }
            runs.Add(new(folder.Name, folder.FullName, started ?? folder.CreationTimeUtc, status, failure, exit, completed));
        }
        return [.. runs.OrderByDescending(r => r.StartedUtc).ThenByDescending(r => r.Id, StringComparer.Ordinal)];
    }

    public sealed record Exported(string Zip, int Files, int Skipped);

    // Whether a file of a run (its path in the run) goes into the zip. A sandboxed session keeps the game's AppData and temp
    // (game/environment) and its user data (game/userdata: saves, the cloud folder, the shader cache) in its run folder
    // (docs/steam-bridge.md, "The rest of a low-integrity session"); they are the player's, not logs, and another
    // program the game started may have left its own there (a browser's profile, on one tester's PC). Only the game's
    // logs and crash data of game/userdata go.
    static bool InExport(string relative)
    {
        var parts = relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (parts.Length < 3 || !parts[0].Equals("game", StringComparison.OrdinalIgnoreCase)) return true;
        if (parts[1].Equals("environment", StringComparison.OrdinalIgnoreCase)) return false;
        return !parts[1].Equals("userdata", StringComparison.OrdinalIgnoreCase) ||
            parts[2].Equals("logs", StringComparison.OrdinalIgnoreCase) || parts[2].Equals("crash_data", StringComparison.OrdinalIgnoreCase);
    }

    // A session's logs zipped to send to whoever helps (the launcher's Save logs to Desktop, HellForge.Cli logs --export).
    // Text files and names in it do not name the PC (Privacy), whoever wrote them (a mod's own log, the game's console
    // logs); other files (a crash dump) go in as they are. A running session still has its logs open, so each file is
    // read with shared access, and one in use is left out (Skipped). The game's console logs are copied into the run
    // (engine-logs) when it ends: while it runs, or if that copy failed, the ones the game wrote since the session started
    // come from gameLogs (the installation's userdata/logs).
    public static Exported Export(string run, string? gameLogs, string target)
    {
        int written, skipped;
        using (var zip = System.IO.Compression.ZipFile.Open(target, System.IO.Compression.ZipArchiveMode.Create))
            (written, skipped) = AddTo(zip, run, gameLogs, new(Privacy.Current.ApplyBytes, Privacy.Scrub, "", Dumps: true));
        return new(target, written, skipped);
    }

    // How a run's files go into a zip: the text files' scrub, the names' scrub, the folder they go in (a crash report's
    // logs/) and whether crash dumps (.dmp) go too.
    internal sealed record ExportScrub(Func<byte[], byte[]> Bytes, Func<string, string> Name, string Prefix, bool Dumps);

    internal static (int Written, int Skipped) AddTo(System.IO.Compression.ZipArchive zip, string run, string? gameLogs, ExportScrub scrub)
    {
        int written = 0, skipped = 0;
        foreach (var (file, name) in ExportFiles(run, gameLogs))
        {
            if (!scrub.Dumps && Path.GetExtension(file).Equals(".dmp", StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                using var source = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                var entry = zip.CreateEntry(scrub.Prefix + scrub.Name(name).Replace(Path.DirectorySeparatorChar, '/'), System.IO.Compression.CompressionLevel.Optimal);
                using var destination = entry.Open();
                if (Privacy.IsText(file) && source.Length < 200L * 1024 * 1024)
                {
                    using var buffer = new MemoryStream();
                    source.CopyTo(buffer);
                    destination.Write(scrub.Bytes(buffer.ToArray()));
                }
                else source.CopyTo(destination);
                written++;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { skipped++; }
        }
        return (written, skipped);
    }

    static List<(string File, string Name)> ExportFiles(string run, string? gameLogs)
    {
        // programs are left out (the run's steam-link folder holds a copy of Valve's steam_api64.dll, which is no log), and
        // so is what a sandboxed game kept of the player's (InExport)
        var files = Directory.EnumerateFiles(run, "*", SearchOption.AllDirectories)
            .Where(file => !Path.GetExtension(file).Equals(".dll", StringComparison.OrdinalIgnoreCase) &&
                           !Path.GetExtension(file).Equals(".exe", StringComparison.OrdinalIgnoreCase) && InExport(Path.GetRelativePath(run, file)))
            .Select(file => (file, name: Path.Combine(Path.GetFileName(run), Path.GetRelativePath(run, file)))).ToList();
        if (!Directory.Exists(Path.Combine(run, "engine-logs")) && gameLogs != null && Directory.Exists(gameLogs))
        {
            var started = Directory.GetCreationTimeUtc(run);
            files.AddRange(new DirectoryInfo(gameLogs).GetFiles("console_*.txt")
                .Where(f => f.LastWriteTimeUtc >= started && f.Length < 200L * 1024 * 1024)
                .Select(f => (f.FullName, Path.Combine(Path.GetFileName(run), "engine-logs", "logs-" + f.Name))));
        }
        return files;
    }
}
