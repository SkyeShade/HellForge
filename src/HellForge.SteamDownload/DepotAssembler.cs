using System.Buffers;
using System.Security.Cryptography;
using HellForge.Core;
using Microsoft.Win32.SafeHandles;

namespace HellForge.SteamDownload;

// Fetches one chunk of a depot from Valve's servers, decrypted and decompressed into buffer; returns its length.
public interface IChunkFetcher
{
    Task<int> FetchAsync(uint depot, DepotChunk chunk, byte[] buffer, CancellationToken cancellation);
}

// Bytes of the build in place so far (of all), of them downloaded and copied from this PC, and the file being written.
public sealed record AssembleProgress(long Done, long Total, long Downloaded, long Copied, string File);

// Writes the planned files into a game folder. A file already there with the build's content is kept. Every other file is
// written chunk by chunk into <file>.partial: each chunk from the first local copy whose SHA-1 is the chunk's, else from
// Valve's servers (checked the same way), and the whole file must then have the build's size and SHA-1 before it is renamed
// into place; one that does not is deleted. Writing is in order, so a stopped download leaves a correct beginning, which
// the next run copies from (<file>.resume) instead of downloading it again.
public sealed class DepotAssembler(IChunkFetcher fetcher, int parallel = 16)
{
    long done, downloaded, copied;

    public async Task AssembleAsync(DepotPlan plan, string destination, Action<AssembleProgress>? progress, CancellationToken cancellation)
    {
        destination = LocalInstall.FullPath(destination);
        LocalInstall.NoLinks(destination);
        Directory.CreateDirectory(destination);
        DiskSpace.Require((destination, plan.Files.Where(f => !Present(Path.Combine(destination, f.File.Path), f.File)).Sum(f => f.File.Size)));
        done = downloaded = copied = 0;
        foreach (var file in plan.Files)
        {
            cancellation.ThrowIfCancellationRequested();
            var path = Path.Combine(destination, file.File.Path);
            if (!SupportedBuild.Accepts(path, file.File))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                await WriteFileAsync(file, path, progress, plan.TotalBytes, cancellation);
            }
            else done += file.File.Size;
            progress?.Invoke(new(done, plan.TotalBytes, downloaded, copied, file.File.Path));
        }
    }

    static bool Present(string path, SupportedFile file)
    {
        var info = new FileInfo(path);
        return info.Exists && (info.Length == file.Size || SupportedBuild.IsReplaceable(file.Path));
    }

    async Task WriteFileAsync(PlannedFile file, string path, Action<AssembleProgress>? progress, long total, CancellationToken cancellation)
    {
        LocalInstall.NoLinks(path);
        var partial = path + ".partial";
        var resume = KeepResume(partial, path + ".resume");
        using var sources = new SourceFiles();
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
        var start = done;
        var pending = new Queue<Task<(byte[] Buffer, int Length)>>();
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        try
        {
            await using var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.Asynchronous);
            foreach (var chunk in file.Chunks)
            {
                pending.Enqueue(Task.Run(() => Resolve(file.File, chunk, resume, sources, stop.Token), stop.Token));
                if (pending.Count >= parallel) await WriteNext(pending, output, sha, file.File.Path, progress, total, cancellation);
            }
            while (pending.Count > 0) await WriteNext(pending, output, sha, file.File.Path, progress, total, cancellation);
            await output.FlushAsync(cancellation);
            output.Flush(flushToDisk: true);
        }
        finally
        {
            // stopped or failed: the chunks still being fetched are abandoned, their buffers returned, before the files close
            await stop.CancelAsync();
            await Drain(pending);
            sources.Dispose();
        }
        var hash = Convert.ToHexString(sha.GetHashAndReset());
        if (new FileInfo(partial).Length != file.File.Size || !hash.Equals(file.File.Sha1, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(partial);
            done = start;
            throw new InvalidDataException($"{file.File.Path} did not come out as the supported build's file; it was not kept. Start setup again.");
        }
        File.Move(partial, path, overwrite: true);
        if (resume != null) File.Delete(resume);
    }

    // A stopped run's partial file becomes the resume file (the longer one, when both are there: each holds a correct
    // beginning of the file).
    static string? KeepResume(string partial, string resume)
    {
        var partialLength = File.Exists(partial) ? new FileInfo(partial).Length : -1;
        var resumeLength = File.Exists(resume) ? new FileInfo(resume).Length : -1;
        if (partialLength > 0 && partialLength >= resumeLength) File.Move(partial, resume, overwrite: true);
        else if (partialLength >= 0) File.Delete(partial);
        return File.Exists(resume) ? resume : null;
    }

    static async Task Drain(Queue<Task<(byte[] Buffer, int Length)>> pending)
    {
        while (pending.Count > 0)
        {
            try { ArrayPool<byte>.Shared.Return((await pending.Dequeue()).Buffer); }
            catch (Exception) { } /* cancelled, or failed while the first error is already on its way */
        }
    }

    async Task WriteNext(Queue<Task<(byte[] Buffer, int Length)>> pending, FileStream output, IncrementalHash sha, string file,
        Action<AssembleProgress>? progress, long total, CancellationToken cancellation)
    {
        var (buffer, length) = await pending.Dequeue();
        try
        {
            sha.AppendData(buffer, 0, length);
            await output.WriteAsync(buffer.AsMemory(0, length), cancellation);
            done += length;
            progress?.Invoke(new(done, total, Interlocked.Read(ref downloaded), Interlocked.Read(ref copied), file));
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }

    // One chunk's bytes: from the first local copy that holds them, else downloaded (three tries, each checked).
    async Task<(byte[] Buffer, int Length)> Resolve(SupportedFile file, PlannedChunk planned, string? resume, SourceFiles sources,
        CancellationToken cancellation)
    {
        var chunk = planned.Chunk;
        var length = (int)chunk.OriginalSize;
        var buffer = ArrayPool<byte>.Shared.Rent(Math.Max(length, (int)chunk.CompressedSize));
        try
        {
            IEnumerable<ChunkSource> local = resume == null ? planned.Local : [new(resume, chunk.Offset), .. planned.Local];
            foreach (var source in local)
                if (sources.TryRead(source, buffer, length) && Holds(buffer, length, chunk))
                {
                    Interlocked.Add(ref copied, length);
                    return (buffer, length);
                }
            for (var attempt = 1; ; attempt++)
            {
                cancellation.ThrowIfCancellationRequested();
                var got = await fetcher.FetchAsync(file.Depot, chunk, buffer, cancellation);
                if (got == length && Holds(buffer, length, chunk))
                {
                    Interlocked.Add(ref downloaded, length);
                    return (buffer, length);
                }
                if (attempt == 3)
                    throw new InvalidDataException($"A part of {file.Path} from Steam's servers did not match its hash three times; start setup again.");
            }
        }
        catch
        {
            ArrayPool<byte>.Shared.Return(buffer);
            throw;
        }
    }

    static bool Holds(byte[] buffer, int length, DepotChunk chunk) =>
        Convert.ToHexString(SHA1.HashData(buffer.AsSpan(0, length))).Equals(chunk.Sha1, StringComparison.OrdinalIgnoreCase);

    // The local files chunks are read from, each opened once for reading and read at any offset from several threads.
    sealed class SourceFiles : IDisposable
    {
        readonly Dictionary<string, SafeFileHandle?> open = new(StringComparer.Ordinal);

        public bool TryRead(ChunkSource source, byte[] buffer, int length)
        {
            var handle = Open(source.Path);
            if (handle == null) return false;
            try
            {
                var read = 0;
                while (read < length)
                {
                    var n = RandomAccess.Read(handle, buffer.AsSpan(read, length - read), (long)source.Offset + read);
                    if (n == 0) return false;
                    read += n;
                }
                return true;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ObjectDisposedException) { return false; }
        }

        SafeFileHandle? Open(string path)
        {
            lock (open)
            {
                if (open.TryGetValue(path, out var handle)) return handle;
                try
                {
                    LocalInstall.NoLinks(path);
                    handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { handle = null; }
                open[path] = handle;
                return handle;
            }
        }

        public void Dispose()
        {
            lock (open)
            {
                foreach (var handle in open.Values) handle?.Dispose();
                open.Clear();
            }
        }
    }
}
