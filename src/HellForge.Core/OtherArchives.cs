using SharpCompress.Archives;
using SharpCompress.Common;
using SharpCompress.Readers;

namespace HellForge.Core;

// Mods shared as .rar or .7z, which .NET cannot read: SharpCompress (managed code, MIT) unpacks them on every system, where
// Windows' own tar only could on Windows 11. A mod comes out as a folder (ModInstaller copies it into mods/), never loaded
// from its archive. Only plain files and folders come out, each within the target: no links, absolute or parent paths,
// password-protected entries or archives split into parts, at most MaxFiles files and MaxBytes in all (an archive can say
// any size, so the bytes are counted as they are written).
public static class OtherArchives
{
    public static readonly IReadOnlyList<string> Extensions = [".rar", ".7z"];
    public const int MaxFiles = 10000;
    public const long MaxBytes = 16L << 30;

    public static bool Is(string path) => Extensions.Any(e => path.EndsWith(e, StringComparison.OrdinalIgnoreCase));

    // Unpacks archive into target (which must not exist yet: a staging folder of its own is moved there whole).
    public static void Unpack(string archive, string target)
    {
        var kind = Path.GetExtension(archive).ToLowerInvariant();
        var staging = target + ".partial-" + Guid.NewGuid().ToString("N")[..8];
        try
        {
            Extract(archive, new Sink(staging, kind));
            try { Directory.Move(staging, target); }
            catch (IOException) when (Directory.Exists(target)) { Directory.Delete(staging, recursive: true); }
        }
        catch (Exception error) when (Unreadable(error))
        {
            Discard(staging);
            throw new InvalidDataException($"HellForge could not unpack the {kind} ({error.Message}): unpack it yourself and add its folder.", error);
        }
        catch
        {
            Discard(staging);
            throw;
        }
    }

    // Whatever else the archive's bytes make SharpCompress throw (not an archive, a damaged or unsupported one) is the
    // archive's problem; HellForge's own refusals and the drive's errors stay what they are.
    static bool Unreadable(Exception error) =>
        error is not (InvalidDataException or UnauthorizedAccessException or OutOfMemoryException) && (error is not IOException || error is EndOfStreamException);

    static void Discard(string staging)
    {
        try { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        catch (IOException) { }
    }

    // A solid archive (and any 7z) reads in one pass; the others entry by entry.
    static void Extract(string archive, Sink sink)
    {
        using var opened = ArchiveFactory.OpenArchive(archive, new ReaderOptions());
        Directory.CreateDirectory(sink.Staging);
        if (opened.IsSolid || opened.Type == ArchiveType.SevenZip)
        {
            using var reader = opened.ExtractAllEntries();
            while (reader.MoveToNextEntry()) sink.Write(reader.Entry, reader.WriteEntryTo);
        }
        else
            foreach (var entry in opened.Entries) sink.Write(entry, to => entry.WriteTo(to));
    }

    // Where the entries go, and how many files and bytes have.
    sealed class Sink(string staging, string kind)
    {
        readonly string root = Path.GetFullPath(staging) + Path.DirectorySeparatorChar;
        int files;
        long bytes;
        public string Staging => staging;

        public void Write(IEntry entry, Action<Stream> copy)
        {
            var destination = Destination(entry);
            if (destination.Length == 0) return; // the archive's root folder
            if (entry.IsDirectory) { Directory.CreateDirectory(destination); return; }
            if (++files > MaxFiles) throw new InvalidDataException($"the {kind} holds more than {MaxFiles} files.");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            using var file = new FileStream(destination, FileMode.CreateNew, FileAccess.Write);
            using var counted = new Counted(file, MaxBytes - bytes, kind);
            copy(counted);
            bytes += counted.Written;
        }

        // The entry's place in the staging folder, when it is a plain file or folder inside it; "" for the archive's root
        // folder itself ("." or "./", as Windows' tar writes it). "." parts of a path are left out.
        string Destination(IEntry entry)
        {
            var name = entry.Key?.Replace('\\', '/') ?? "";
            if (entry.IsEncrypted) throw new InvalidDataException($"the {kind} is password-protected: unpack it yourself and add its folder.");
            if (entry.IsSplitAfter || entry.VolumeIndexFirst != entry.VolumeIndexLast) throw new InvalidDataException($"the {kind} is split into parts: unpack it yourself and add its folder.");
            if (!string.IsNullOrEmpty(entry.LinkTarget)) throw new InvalidDataException($"the {kind} contains a link ({name}); HellForge adds only plain files and folders.");
            var parts = name.Split('/').Where(part => part is not ("" or ".")).ToList();
            if (parts.Count == 0 && entry.IsDirectory && !name.StartsWith('/')) return "";
            var safe = parts.Count > 0 && !name.StartsWith('/') && !name.Contains(':') && !name.Any(char.IsControl) && !parts.Contains("..");
            var destination = safe ? Path.GetFullPath(Path.Combine([staging, .. parts])) : "";
            return safe && destination.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? destination
                : throw new InvalidDataException($"the {kind} contains an unsafe path ({entry.Key}).");
        }
    }

    // The bytes written through it, past which it throws.
    sealed class Counted(Stream inner, long room, string kind) : Stream
    {
        public long Written { get; private set; }
        public override void Write(byte[] buffer, int offset, int count) => Write(buffer.AsSpan(offset, count));
        public override void Write(ReadOnlySpan<byte> buffer)
        {
            Written += buffer.Length;
            if (Written > room) throw new InvalidDataException($"the {kind} unpacks to more than {MaxBytes >> 30} GB.");
            inner.Write(buffer);
        }
        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => Written;
        public override long Position { get => Written; set => throw new NotSupportedException(); }
        public override void Flush() => inner.Flush();
        public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }
}
