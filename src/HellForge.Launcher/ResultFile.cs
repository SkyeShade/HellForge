using System.Text.Json;
using HellForge.Core;

namespace HellForge.Launcher;

// result.json is written whole and moved into place, and it embeds every backend request, so it can be large. Keeps the
// last parsed state and parses again only when the file's size or last-write time changed. Once it has completedUtc and
// the runner is not running, the run is over and the file is no longer looked at.
sealed class ResultFile
{
    readonly Func<string, JsonElement?> read;
    JsonElement? state;
    long length = -1;
    DateTime writeTimeUtc;
    bool finished;

    public ResultFile(Func<string, JsonElement?>? read = null) => this.read = read ?? SessionFiles.ReadJson;

    public bool Finished => finished;

    public void Reset()
    {
        state = null;
        length = -1;
        writeTimeUtc = default;
        finished = false;
    }

    // The current state, or null when there is none or the run has finished. The runner writes completedUtc before its
    // process exits, so a result is final only when the runner is not running.
    public JsonElement? Read(string path, bool runnerRunning)
    {
        if (finished) return null;
        var current = ReadChanged(path);
        if (current is { } value && value.ValueKind == JsonValueKind.Object && !runnerRunning &&
            value.TryGetProperty("completedUtc", out _))
            finished = true;
        return current;
    }

    JsonElement? ReadChanged(string path)
    {
        long fileLength;
        DateTime fileWriteTimeUtc;
        try
        {
            var file = new FileInfo(path);
            if (!file.Exists) return null;
            fileLength = file.Length;
            fileWriteTimeUtc = file.LastWriteTimeUtc;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        if (state is { } cached && fileLength == length && fileWriteTimeUtc == writeTimeUtc) return cached;
        if (read(path) is not { } parsed) return null;
        state = parsed;
        length = fileLength;
        writeTimeUtc = fileWriteTimeUtc;
        return parsed;
    }
}
