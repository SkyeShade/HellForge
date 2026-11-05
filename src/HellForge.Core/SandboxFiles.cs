using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace HellForge.Core;

// The sandbox's copy-back reads files the game wrote. On Linux a path name alone is not enough: the game can plant a
// symlink, a FIFO, a device node or a hard link to an inode outside the session folder. Everything here opens through
// openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS) with O_PATH|O_NOFOLLOW, then checks the opened inode with statx: only a
// regular file with one link is read, and at most the recorded size. Windows keeps SandboxFolders' and SandboxStats'
// existing checks; this class is Linux only.
internal static class SandboxFiles
{
    // openat2/statx syscall numbers on x86_64 Linux (asm-generic): stable kernel ABI values, not libc function numbers.
    const long SysOpenat2 = 437, SysStatx = 332;
    const int AtEmptyPath = 0x1000;
    const uint StatxType = 0x1, StatxNlink = 0x4;
    const uint SIfMt = 0xF000, SIfReg = 0x8000, SIfDir = 0x4000;
    const int OReadOnly = 0, OCloExec = 0x80000, OPath = 0x200000, ODirectory = 0x10000, ONoFollow = 0x20000;
    const int ONonBlock = 0x800;
    const int ResolveNoSymlinks = 0x04, ResolveBeneath = 0x08;
    const int ENoEnt = 2, EInval = 22, ENosys = 38;
    const int StatxBufferSize = 256;
    const int CopyBufferSize = 64 << 10;

    [StructLayout(LayoutKind.Sequential)]
    struct OpenHow
    {
        public ulong Flags;
        public ulong Mode;
        public ulong Resolve;
    }

    [DllImport("libc", SetLastError = true)]
    static extern int open([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int flags, int mode);

    [DllImport("libc", SetLastError = true)]
    static extern int close(int fd);

    // 0-based errno from the last P/Invoke, read immediately after the failing call.
    static int LastError() => Marshal.GetLastPInvokeError();

    // The syscall numbers and struct layouts here are the x86_64 Linux ABI. The package and hf-sandbox are x86_64; on any
    // other architecture fail closed instead of invoking a different syscall number.
    static void EnsureSupported()
    {
        if (RuntimeInformation.ProcessArchitecture != Architecture.X64)
            throw new InvalidDataException("The Linux sandbox copy-back is implemented for x86_64 only.");
    }

    static int OpenRoot(string path)
    {
        int fd = open(path, OPath | ODirectory | ONoFollow | OCloExec, 0);
        if (fd < 0) throw new InvalidDataException($"The session folder could not be opened safely: {path} (errno {LastError()}).");
        return fd;
    }

    static bool IsUnsafeOpenError(int error) => error is ENosys or EInval;

    // openat2's struct has to be passed by address. The symbol is libc's variadic syscall; on x86_64 the first six
    // integer arguments and the long return use the standard System V registers.
    [DllImport("libc", SetLastError = true, EntryPoint = "syscall")]
    static extern long SyscallOpenat2(long number, int directory, nint path, ref OpenHow how, nuint size);

    [DllImport("libc", SetLastError = true, EntryPoint = "syscall")]
    static extern long SyscallStatx(long number, int directory, nint path, int flags, uint mask, nint buffer);

    // Opens one name relative to an already-open folder. O_PATH does not read a FIFO or touch a device, and the resolver
    // refuses a symlink on any component. A missing entry or a symlink is skipped by the caller; a kernel that cannot do
    // the safe open is an error, never a fallback to a path-based read.
    static SafeFileHandle? OpenEntry(int directory, string relative, out int error)
    {
        error = 0;
        nint path = Marshal.StringToCoTaskMemUTF8(relative);
        OpenHow how = new()
        {
            Flags = (ulong)(OPath | ONoFollow | OCloExec),
            Mode = 0,
            Resolve = ResolveBeneath | ResolveNoSymlinks,
        };
        try
        {
            long fd = SyscallOpenat2(SysOpenat2, directory, path, ref how, (nuint)Marshal.SizeOf<OpenHow>());
            if (fd < 0)
            {
                error = LastError();
                if (IsUnsafeOpenError(error)) throw new InvalidDataException($"This kernel cannot open session files safely (openat2 errno {error}).");
                return null;
            }
            return new SafeFileHandle((nint)fd, ownsHandle: true);
        }
        finally
        {
            Marshal.FreeCoTaskMem(path);
        }
    }

    readonly record struct Stat(uint Mode, uint Links, long Size, ulong Inode);

    static Stat StatFile(int fd)
    {
        nint empty = Marshal.AllocHGlobal(1);
        nint buffer = Marshal.AllocHGlobal(StatxBufferSize);
        try
        {
            Marshal.WriteByte(empty, 0, 0);
            Marshal.Copy(new byte[StatxBufferSize], 0, buffer, StatxBufferSize);
            long result = SyscallStatx(SysStatx, fd, empty, AtEmptyPath, StatxType | StatxNlink, buffer);
            if (result != 0) throw new InvalidDataException($"A session file could not be inspected (statx errno {LastError()}).");
            uint mask = unchecked((uint)Marshal.ReadInt32(buffer, 0));
            if ((mask & (StatxType | StatxNlink)) != (StatxType | StatxNlink))
                throw new InvalidDataException("The kernel did not report the session file's type and link count.");
            uint links = unchecked((uint)Marshal.ReadInt32(buffer, 16));
            uint mode = unchecked((uint)(ushort)Marshal.ReadInt16(buffer, 28));
            ulong inode = unchecked((ulong)Marshal.ReadInt64(buffer, 32));
            long size = Marshal.ReadInt64(buffer, 40);
            return new(mode, links, size, inode);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
            Marshal.FreeHGlobal(empty);
        }
    }

    static bool IsRegular(Stat stat) => (stat.Mode & SIfMt) == SIfReg;
    static bool IsDirectory(Stat stat) => (stat.Mode & SIfMt) == SIfDir;

    // A fresh descriptor for the same regular inode. /proc/self/fd resolves the O_PATH descriptor, so a name swapped after
    // the check cannot redirect the read. O_NONBLOCK keeps a special file from blocking if one ever gets this far.
    static FileStream ReopenRegular(SafeFileHandle pathHandle, Stat stat)
    {
        int fd = open($"/proc/self/fd/{pathHandle.DangerousGetHandle()}", OReadOnly | OCloExec | ONonBlock, 0);
        if (fd < 0) throw new InvalidDataException($"A session file could not be read (errno {LastError()}).");
        var handle = new SafeFileHandle((nint)fd, ownsHandle: true);
        try
        {
            Stat again = StatFile(fd);
            if (!IsRegular(again) || again.Links != 1 || again.Inode != stat.Inode)
                throw new InvalidDataException("A session file changed while it was being opened.");
            return new FileStream(handle, FileAccess.Read, CopyBufferSize, isAsync: false);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    static byte[] ReadExact(FileStream stream, long length)
    {
        if (length < 0 || length > int.MaxValue) throw new InvalidDataException("The session file is too large.");
        byte[] bytes = new byte[(int)length];
        int offset = 0;
        while (offset < bytes.Length)
        {
            int got = stream.Read(bytes, offset, bytes.Length - offset);
            if (got <= 0) throw new InvalidDataException("The session file ended before its recorded size.");
            offset += got;
        }
        return bytes;
    }

    static string ReadEntryText(SafeFileHandle handle, Stat stat, long limit, string display)
    {
        if (!IsRegular(stat) || stat.Links != 1)
            throw new InvalidDataException($"{display} from the session is not a regular file with one link.");
        if (stat.Size > limit) throw new InvalidDataException($"{display} from the session is too large.");
        using var stream = ReopenRegular(handle, stat);
        byte[] bytes = ReadExact(stream, stat.Size);
        using var reader = new StreamReader(new MemoryStream(bytes, writable: false), Encoding.UTF8,
            detectEncodingFromByteOrderMarks: true);
        return reader.ReadToEnd();
    }

    // The text of one file under protectedRoot, opened the safe way. Throws InvalidDataException for anything that is
    // not one regular, singly linked file; a path with a link in it is also refused.
    public static string ReadText(string path, string protectedRoot, long limit)
    {
        EnsureSupported();
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(protectedRoot));
        string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        if (full != root && !full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            throw new InvalidDataException($"The session path is outside {root}.");
        string relative = full == root ? "." : full[(root.Length + 1)..];
        int rootFd = OpenRoot(root);
        try
        {
            using SafeFileHandle? handle = OpenEntry(rootFd, relative, out int error);
            if (handle == null)
            {
                if (error == ENoEnt) throw new FileNotFoundException($"The session file is missing: {path}", path);
                throw new InvalidDataException($"The session file is linked, special or unreadable: {path} (errno {error}).");
            }
            Stat stat = StatFile(handle.DangerousGetHandle().ToInt32());
            return ReadEntryText(handle, stat, limit, Path.GetFileName(path));
        }
        finally
        {
            close(rootFd);
        }
    }

    // The copy's running state, kept in one object: the lint parameter limit makes a long ref list the wrong shape.
    sealed class CopyState(string toRoot, long maxBytes, int maxFiles)
    {
        public string ToRoot { get; } = toRoot;
        public long MaxBytes { get; } = maxBytes;
        public int MaxFiles { get; } = maxFiles;
        public int Files { get; set; }
        public int Skipped { get; set; }
        public long Bytes { get; set; }
    }

    // Copies one tree (or the single files among parts) from a session folder to a trusted destination. Only regular,
    // singly linked files are copied; links, FIFOs, devices, sockets and directories' linked entries are skipped and
    // counted. The destination is checked with LocalInstall.NoLinks before every write.
    public static (int Files, long Bytes, int Skipped) CopyTree(string fromRoot, string toRoot, IEnumerable<string> parts,
        long maxBytes, int maxFiles)
    {
        EnsureSupported();
        int rootFd = OpenRoot(fromRoot);
        var state = new CopyState(toRoot, maxBytes, maxFiles);
        try
        {
            foreach (var part in parts)
            {
                if (!TryRelative(part, out string relative)) { state.Skipped++; continue; }
                using SafeFileHandle? handle = OpenEntry(rootFd, relative, out int error);
                if (handle == null)
                {
                    if (error != ENoEnt) state.Skipped++;
                    continue;
                }
                Stat stat = StatFile(handle.DangerousGetHandle().ToInt32());
                CopyEntry(handle, stat, Path.Combine(toRoot, relative), state);
            }
        }
        finally
        {
            close(rootFd);
        }
        return (state.Files, state.Bytes, state.Skipped);
    }

    static bool TryRelative(string part, out string relative)
    {
        relative = part.Replace('\\', '/').TrimStart('/');
        if (relative.Length == 0) return false;
        foreach (var piece in relative.Split('/', StringSplitOptions.RemoveEmptyEntries))
            if (piece is "." or "..") return false;
        return true;
    }

    static void CopyEntry(SafeFileHandle handle, Stat stat, string destination, CopyState state)
    {
        if (IsDirectory(stat))
        {
            CopyDirectory(handle, destination, state);
            return;
        }
        if (!IsRegular(stat) || stat.Links != 1) { state.Skipped++; return; }
        if (state.Files >= state.MaxFiles || stat.Size < 0 || stat.Size > state.MaxBytes - state.Bytes) { state.Skipped++; return; }
        Take(handle, stat, destination, state);
    }

    static void CopyDirectory(SafeFileHandle directory, string destination, CopyState state)
    {
        if (state.Files >= state.MaxFiles) return;
        LocalInstall.NoLinks(destination, state.ToRoot);
        Directory.CreateDirectory(destination);
        string directoryPath = $"/proc/self/fd/{directory.DangerousGetHandle()}";
        foreach (var entry in Directory.EnumerateFileSystemEntries(directoryPath))
        {
            if (state.Files >= state.MaxFiles) return;
            string name = Path.GetFileName(entry);
            using SafeFileHandle? child = OpenEntry(directory.DangerousGetHandle().ToInt32(), name, out int error);
            if (child == null)
            {
                if (error != ENoEnt) state.Skipped++;
                continue;
            }
            Stat childStat = StatFile(child.DangerousGetHandle().ToInt32());
            CopyEntry(child, childStat, Path.Combine(destination, name), state);
        }
    }

    static void Take(SafeFileHandle handle, Stat stat, string destination, CopyState state)
    {
        LocalInstall.NoLinks(destination, state.ToRoot);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        string temporary = destination + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            using (var input = ReopenRegular(handle, stat))
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, CopyBufferSize,
                FileOptions.SequentialScan))
            {
                byte[] buffer = new byte[CopyBufferSize];
                long remaining = stat.Size;
                while (remaining > 0)
                {
                    int got = input.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (got <= 0) throw new InvalidDataException($"The session file {Path.GetFileName(destination)} ended early.");
                    output.Write(buffer, 0, got);
                    remaining -= got;
                }
            }
            File.Move(temporary, destination, overwrite: true);
            state.Files++;
            state.Bytes += stat.Size;
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }
}
