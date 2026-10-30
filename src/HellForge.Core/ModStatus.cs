namespace HellForge.Core;

// What each code mod told the player during a session (SDK 16, set_status), as the mod loader keeps it in the session's
// mod-status.tsv: "<mod id>\t<text>" a line, rewritten whole on a change (native/mod-loader/mod_status.c). The
// launcher's Mods page shows it on the mod's row while the session runs and after it ends.
public static class ModStatus
{
    public const string FileName = "mod-status.tsv";

    // The file of a run, beside modloader.log (in the run folder, or in run/game when sandboxed), or null.
    public static string? FileIn(string run) =>
        CrashReport.GameFolders(run).Select(f => Path.Combine(f, FileName)).FirstOrDefault(File.Exists);

    // When the file was last written, to tell a change; DateTime.MinValue when there is none.
    public static DateTime WrittenUtc(string run) => FileIn(run) is { } file ? File.GetLastWriteTimeUtc(file) : DateTime.MinValue;

    // The statuses of a run by mod id (empty when there are none), or null when the file cannot be read now (the loader
    // may be moving a new one into place: the next read gets it).
    public static Dictionary<string, string>? Read(string run)
    {
        var statuses = new Dictionary<string, string>(StringComparer.Ordinal);
        if (FileIn(run) is not { } file) return statuses;
        try
        {
            using var reader = new StreamReader(new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            for (var line = reader.ReadLine(); line != null; line = reader.ReadLine())
                if (line.IndexOf('\t') is var tab and > 0 && tab < line.Length - 1) statuses[line[..tab]] = Privacy.Scrub(line[(tab + 1)..]);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        return statuses;
    }
}
