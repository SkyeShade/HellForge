using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Co-op over Steam relays turned on while a Linux session runs (the lobby browser's ask; docs/linux.md "Co-op on
// Linux"). The session's backend cannot change its own network: it runs inside the session's network namespace. It asks
// in a file in its folder (<run>/backend/coop-relays-request.json), the launcher, outside the namespace, carries the
// ask out (app/linux/hf-coop-relays.sh, which puts in place the network a session that starts with co-op has) and
// answers in another (coop-relays-answer.json), which the backend reads. Either file is written whole (a temporary file,
// then a rename).
public static class CoopRelaysSwitch
{
    public const string RequestFile = "coop-relays-request.json", AnswerFile = "coop-relays-answer.json";
    public const string Script = "hf-coop-relays.sh";

    public sealed record Answer(bool Open, string Message);

    // The backend's ask: an old answer goes first, so the one read next is this ask's.
    public static void Ask(string backendFolder)
    {
        TryDelete(Path.Combine(backendFolder, AnswerFile));
        Write(Path.Combine(backendFolder, RequestFile), new JsonObject { ["version"] = 1, ["askedUtc"] = DateTime.UtcNow.ToString("O", System.Globalization.CultureInfo.InvariantCulture) });
    }

    // The launcher's side: true once for each ask (the file is gone after).
    public static bool TakeRequest(string backendFolder)
    {
        var path = Path.Combine(backendFolder, RequestFile);
        if (!File.Exists(path)) return false;
        return TryDelete(path);
    }

    public static void WriteAnswer(string backendFolder, bool open, string message) =>
        Write(Path.Combine(backendFolder, AnswerFile), new JsonObject { ["version"] = 1, ["state"] = open ? "open" : "failed", ["message"] = message });

    // The backend's side: the launcher's answer, and the file gone; null while there is none (or it is being written).
    public static Answer? TakeAnswer(string backendFolder)
    {
        var path = Path.Combine(backendFolder, AnswerFile);
        if (!File.Exists(path)) return null;
        try
        {
            if (JsonNode.Parse(File.ReadAllText(path)) is not JsonObject root) return null;
            TryDelete(path);
            var state = root["state"] is JsonValue s && s.TryGetValue<string>(out var text) ? text : "";
            var message = root["message"] is JsonValue m && m.TryGetValue<string>(out var words) ? words : "";
            return new Answer(state == "open", message);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // The launcher's work: the installation's hf-coop-relays.sh, whose last line is "OPEN" or "FAILED <reason>". Open, or
    // the reason it is not (with what the script said on the way, for the launcher's log).
    public static (bool Open, string Message, string Log) TurnOn(string installRoot, TimeSpan timeout)
    {
        var script = Path.Combine(installRoot, "app", "linux", Script);
        if (!File.Exists(script)) return (false, "this installation has no " + Script + " (reinstall HellForge)", "");
        var start = new ProcessStartInfo("bash") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add(script);
        start.Environment["INSTALL"] = installRoot;
        using var process = Process.Start(start);
        if (process == null) return (false, "bash did not start", "");
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit((int)timeout.TotalMilliseconds))
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            return (false, $"turning it on took longer than {(int)timeout.TotalSeconds} s", "");
        }
        var lines = output.Result.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        var last = lines.Length > 0 ? lines[^1] : "";
        var log = errors.Result.Trim();
        if (process.ExitCode == 0 && last == "OPEN") return (true, "", log);
        return (false, last.StartsWith("FAILED ", StringComparison.Ordinal) ? last["FAILED ".Length..] : $"{Script} exited with code {process.ExitCode}", log);
    }

    static void Write(string path, JsonObject value)
    {
        File.WriteAllText(path + ".tmp", value.ToJsonString());
        File.Move(path + ".tmp", path, overwrite: true);
    }

    static bool TryDelete(string path)
    {
        try { File.Delete(path); return true; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }
}
