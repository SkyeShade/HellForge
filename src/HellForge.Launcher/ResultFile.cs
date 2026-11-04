using System.Text.Json;
using HellForge.Core;

namespace HellForge.Launcher;

// result.json is written whole and moved into place, and it embeds every backend request, so it can be large. Keeps the
// last parsed state and parses again only when the file's size or last-write time changed. Once the runner is not
// running the run is over: even a result without completedUtc cannot change, and the launcher shows that it ended.
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

    // The current state, or null when there is none or the run has finished. A state is final once the runner is not
    // running, whether or not its cleanup managed to write completedUtc.
    public JsonElement? Read(string path, bool runnerRunning)
    {
        if (finished) return null;
        var current = ReadChanged(path);
        if (current is { ValueKind: JsonValueKind.Object } && !runnerRunning) finished = true;
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
