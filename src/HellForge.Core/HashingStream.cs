using System.Security.Cryptography;

namespace HellForge.Core;

// A stream that hashes what passes through it, read or written, so a file is checked in the same pass that moves it.
public sealed class HashingStream(Stream inner, HashAlgorithmName algorithm, bool leaveOpen = false) : Stream
{
    readonly IncrementalHash hash = IncrementalHash.CreateHash(algorithm);

    public long Bytes { get; private set; }

    // Told each count of bytes that passes, for progress.
    public Action<int>? Passed { get; init; }

    // The hash of everything so far, in upper-case hex (as Steam's and the build list's hashes are compared).
    public string Hex() => Convert.ToHexString(hash.GetCurrentHash());

    public override int Read(byte[] buffer, int offset, int count)
    {
        var read = inner.Read(buffer, offset, count);
        hash.AppendData(buffer, offset, read);
        Bytes += read;
        Passed?.Invoke(read);
        return read;
    }

    public override int Read(Span<byte> buffer)
    {
        var read = inner.Read(buffer);
        hash.AppendData(buffer[..read]);
        Bytes += read;
        Passed?.Invoke(read);
        return read;
    }

    public override void Write(byte[] buffer, int offset, int count) => Write(buffer.AsSpan(offset, count));

    public override void Write(ReadOnlySpan<byte> buffer)
    {
        inner.Write(buffer);
        hash.AppendData(buffer);
        Bytes += buffer.Length;
        Passed?.Invoke(buffer.Length);
    }

    // Reads what is left, so the hash covers the whole of a stream the reader did not finish.
    public void Drain()
    {
        var buffer = new byte[1 << 16];
        while (Read(buffer, 0, buffer.Length) > 0) { }
    }

    public override bool CanRead => inner.CanRead;
    public override bool CanSeek => false;
    public override bool CanWrite => inner.CanWrite;
    public override long Length => inner.Length;
    public override long Position { get => Bytes; set => throw new NotSupportedException(); }
    public override void Flush() => inner.Flush();
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            hash.Dispose();
            if (!leaveOpen) inner.Dispose();
        }
        base.Dispose(disposing);
    }
}
