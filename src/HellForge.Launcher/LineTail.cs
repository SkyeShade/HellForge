using System.Buffers;
using System.Text;

namespace HellForge.Launcher;

// Follows a JSON-lines file in bounded chunks. Only complete lines are returned; the bytes of a line that is still being
// written stay buffered for the next poll, so a line or a UTF-8 character split between reads is not broken.
sealed class LineTail
{
    internal const int ChunkSize = 1 << 20;
    readonly MemoryStream pending = new();
    long offset;

    // True once every byte of the file has been read. A partial last line is not returned until its newline arrives.
    public bool AtEnd { get; private set; }

    public void Reset()
    {
        offset = 0;
        pending.SetLength(0);
        AtEnd = false;
    }

    public List<string> ReadNewLines(string path)
    {
        var lines = new List<string>();
        try
        {
            if (!File.Exists(path))
            {
                AtEnd = true;
                return lines;
            }
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length < offset) Reset();  // Replaced or truncated.
            stream.Seek(offset, SeekOrigin.Begin);
            var count = (int)Math.Min(ChunkSize, stream.Length - offset);
            if (count == 0)
            {
                AtEnd = true;
                return lines;
            }
            var buffer = ArrayPool<byte>.Shared.Rent(count);
            try
            {
                var read = stream.Read(buffer, 0, count);
                if (read <= 0) return lines;
                offset += read;
                var end = Array.LastIndexOf(buffer, (byte)'\n', read - 1, read);
                if (end < 0)
                {
                    pending.Write(buffer, 0, read);
                    AtEnd = offset >= stream.Length;
                    return lines;
                }
                if (pending.Length > 0)
                {
                    pending.Write(buffer, 0, end + 1);
                    AddLines(pending.GetBuffer(), (int)pending.Length, lines);
                    pending.SetLength(0);
                }
                else AddLines(buffer, end + 1, lines);
                // The bytes after the last newline begin a line that is still being written.
                pending.Write(buffer, end + 1, read - end - 1);
                AtEnd = offset >= stream.Length;
                return lines;
            }
            finally { ArrayPool<byte>.Shared.Return(buffer); }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return lines;
    }

    static void AddLines(byte[] bytes, int count, List<string> lines)
    {
        foreach (var line in Encoding.UTF8.GetString(bytes, 0, count).Split('\n'))
        {
            var trimmed = line.TrimStart('\uFEFF').TrimEnd('\r');
            if (trimmed.Length > 0) lines.Add(trimmed);
        }
    }
}
