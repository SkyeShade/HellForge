using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Session;

// What a session step failed on: the message is the session's failure (result.json), as the script's throws were.
sealed class SessionFailure(string message) : Exception(message);

// A stop the player asked for, rather than a failure: the result ends as an observed (ended) session.
sealed class SessionStopped : Exception { }

static class Tools
{
    public static string PowerShell => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");

    public static string Sha256(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    // A PowerShell literal string: single quotes, doubled inside.
    public static string Quote(string text) => "'" + text.Replace("'", "''") + "'";

    // Runs a program without a window; returns its exit code and output (stdout, stderr), or -1 on time out (killed).
    public static (int Exit, string Output, string Error) Run(string program, IEnumerable<string> arguments, string? workingDirectory = null, int timeoutMs = 120000)
    {
        var start = new ProcessStartInfo(program) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        if (workingDirectory != null) start.WorkingDirectory = workingDirectory;
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new SessionFailure($"{Path.GetFileName(program)} did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(timeoutMs))
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            return (-1, "", "timed out");
        }
        process.WaitForExit();
        return (process.ExitCode, output.Result, error.Result);
    }

    // A PowerShell command (Windows PowerShell 5.1, as the scripts expect), without a profile.
    public static (int Exit, string Output, string Error) PowerShellCommand(string command, int timeoutMs = 120000) =>
        Run(PowerShell, ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command], timeoutMs: timeoutMs);

    // A file never reached through a link (junction or symbolic link) anywhere on its path.
    public static void NoLinks(string path)
    {
        for (FileSystemInfo? item = File.Exists(path) ? new FileInfo(path) : new DirectoryInfo(path); item != null;
             item = item is FileInfo file ? file.Directory : ((DirectoryInfo)item).Parent)
            if (item.Exists && item.Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new SessionFailure("Linked target.");
    }

    public static JsonNode? ParseJson(string text) => string.IsNullOrWhiteSpace(text) ? null : JsonNode.Parse(text);

    public static JsonNode? ReadJson(string path) => File.Exists(path) ? JsonNode.Parse(File.ReadAllText(path)) : null;

    public static void WriteJson(string path, JsonNode node) =>
        File.WriteAllText(path, node.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));

    // Backend audit lines (requests.jsonl): all of them, or those after *offset, which moves past them.
    public static List<JsonNode> BackendRequests(string path, ref long offset)
    {
        var rows = new List<JsonNode>();
        if (!File.Exists(path)) return rows;
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        stream.Seek(offset, SeekOrigin.Begin);
        using var reader = new StreamReader(stream);
        while (reader.ReadLine() is { } line)
            if (line.Length > 0 && ParseJson(line) is { } row) rows.Add(row);
        offset = stream.Length;
        return rows;
    }

    // What an NTSTATUS-style exit code of the game means, as far as players can act on it.
    public static string ExitMeaning(int code) => unchecked((uint)code) switch
    {
        0xC0000135 => "a DLL the game needs could not be found",
        0xC0000142 => "a DLL the game loads failed to start",
        0xC000007B => "a DLL the game loads is damaged or for the wrong architecture",
        0xC0000005 => "the game crashed (access violation)",
        0xC0000409 => "the game stopped itself after detecting a problem",
        0xC0000022 => "Windows or security software denied the game access",
        0xC0000428 => "Windows blocked an unsigned DLL (Smart App Control or a code-integrity policy)",
        0xC0000361 => "a Windows policy blocked a file the game loads",
        _ => ""
    };
}
