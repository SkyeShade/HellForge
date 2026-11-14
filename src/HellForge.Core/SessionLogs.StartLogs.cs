using System.IO.Compression;

namespace HellForge.Core;

// What a start leaves when it stops before a session's folder exists: the session runner's reasons, and on Linux the
// wrapper's output and its refusal inside a container. They are in the installation's logs folder, not in a session's,
// so an export of sessions alone had nothing of the start a player wanted help with.
public static partial class SessionLogs
{
    // The folder they go in inside the zip, beside the session's.
    public const string StartFolder = "start";
    public static readonly IReadOnlyList<string> StartLogNames =
        ["session-runner.log", "linux-session-wrapper.log", "linux-container-refusal.txt"];

    // Those of the start logs that are in the installation's logs folder; a link is never followed.
    public static IReadOnlyList<string> StartLogs(string installLogs) =>
        [.. StartLogNames.SelectMany(name => LocalInstall.FilesNoLinks(installLogs, name, recursive: false))];

    // The zip for an installation with no session yet: the start logs and the programs still running. Null (and no file)
    // when there is no start log either.
    public static Exported? ExportStart(string installLogs, string target)
    {
        if (StartLogs(installLogs).Count == 0) return null;
        int written, skipped;
        using (var zip = ZipFile.Open(target, ZipArchiveMode.Create))
        {
            (written, skipped) = AddStartLogs(zip, installLogs);
            WriteProcesses(zip, StartFolder);
            written++;
        }
        return new(target, written, skipped);
    }

    // Scrubbed like a session's text files (the wrapper's output names the home folder); one in use is left out.
    static (int Written, int Skipped) AddStartLogs(ZipArchive zip, string installLogs)
    {
        int written = 0, skipped = 0;
        foreach (var file in StartLogs(installLogs))
            try
            {
                using var source = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var buffer = new MemoryStream();
                source.CopyTo(buffer);
                using var destination = zip.CreateEntry(StartFolder + "/" + Path.GetFileName(file), CompressionLevel.Optimal).Open();
                destination.Write(Privacy.Current.ApplyBytes(buffer.ToArray()));
                written++;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { skipped++; }
        return (written, skipped);
    }

    static void WriteProcesses(ZipArchive zip, string folder)
    {
        using var writer = new StreamWriter(zip.CreateEntry(folder + "/" + ProcessesFile, CompressionLevel.Optimal).Open(),
            new System.Text.UTF8Encoding(false));
        writer.Write(Privacy.Scrub(RunningProcesses(DateTime.UtcNow, System.Diagnostics.Process.GetProcesses())));
    }
}
