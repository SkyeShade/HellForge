using System.Runtime.InteropServices;

namespace HellForge.Core;

// A file at a second path without a second copy of its bytes: a hard link where the drive has them (NTFS, ReFS, ext4,
// btrfs, XFS, ntfs-3g; under Wine, CreateHardLinkW makes the host's link), else a copy (File.Copy). A link falls back to
// the copy on a drive without links (FAT32, exFAT), across drives, and on a share.
public static class FileLinks
{
    // source at destination, which must not exist: linked when link is true and the drive takes it, else copied. True: linked.
    public static bool LinkOrCopy(string source, string destination, bool link)
    {
        if (link && TryLink(source, destination)) return true;
        File.Copy(source, destination);
        return false;
    }

    // A hard link at destination (which must not exist) to source's file; false when the drive or system refuses it.
    public static bool TryLink(string source, string destination)
    {
        try
        {
            if (OperatingSystem.IsWindows()) return CreateHardLinkW(Extended(destination), Extended(source), IntPtr.Zero);
            if (OperatingSystem.IsLinux() || OperatingSystem.IsMacOS()) return link(Path.GetFullPath(source), Path.GetFullPath(destination)) == 0;
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { }
        return false;
    }

    // Whether two paths name one file (a hard link and its source): the volume and file index on Windows (Wine, measured
    // with Proton Experimental: the index is the host's inode, the volume 0), the device and inode on Linux. Null when it cannot tell: a path is missing or
    // unreadable, or the system gives no file index.
    public static bool? SameFile(string first, string second)
    {
        try
        {
            if (OperatingSystem.IsWindows()) return WindowsId(first) is { } a && WindowsId(second) is { } b ? a == b : null;
            if (OperatingSystem.IsLinux()) return UnixId(first) is { } a && UnixId(second) is { } b ? a == b : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or DllNotFoundException or EntryPointNotFoundException) { }
        return null;
    }

    static (uint Volume, uint High, uint Low)? WindowsId(string path)
    {
        using var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (!GetFileInformationByHandle(handle, out var info) || (info.FileIndexHigh == 0 && info.FileIndexLow == 0)) return null;
        return (info.VolumeSerialNumber, info.FileIndexHigh, info.FileIndexLow);
    }

    // struct stat starts with st_dev and st_ino, 8 bytes each, on x86_64 and arm64 (glibc and musl)
    static (ulong Device, ulong Inode)? UnixId(string path)
    {
        var buffer = new byte[256];
        if (stat(Path.GetFullPath(path), buffer) != 0) return null;
        return (BitConverter.ToUInt64(buffer, 0), BitConverter.ToUInt64(buffer, 8));
    }

    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation
    {
        public uint Attributes;
        public uint CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
        public uint VolumeSerialNumber, SizeHigh, SizeLow, NumberOfLinks, FileIndexHigh, FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GetFileInformationByHandle(Microsoft.Win32.SafeHandles.SafeFileHandle file, out FileInformation information);

    [DllImport("libc", SetLastError = true)]
    static extern int stat([MarshalAs(UnmanagedType.LPUTF8Str)] string path, byte[] buffer);

    // A Windows path in its \\?\ form, so a link deep in a pack's folders is not refused at 260 characters (the .NET file
    // calls lengthen paths themselves; a direct Win32 call does not).
    static string Extended(string path)
    {
        var full = Path.GetFullPath(path);
        if (full.StartsWith(@"\\?\", StringComparison.Ordinal)) return full;
        return full.StartsWith(@"\\", StringComparison.Ordinal) ? @"\\?\UNC\" + full[2..] : @"\\?\" + full;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CreateHardLinkW(string fileName, string existingFileName, IntPtr securityAttributes);

    [DllImport("libc", SetLastError = true)]
    static extern int link([MarshalAs(UnmanagedType.LPUTF8Str)] string oldPath, [MarshalAs(UnmanagedType.LPUTF8Str)] string newPath);
}
