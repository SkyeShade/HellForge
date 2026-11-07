using System.Runtime.InteropServices;

namespace HellForge.Core;

// A file at a second path without a second copy of its bytes: a hard link where the drive has them (NTFS, ReFS, ext4,
// btrfs, XFS, ntfs-3g), else a copy. The copy is File.Copy, which on Linux clones the file's extents first where the file
// system can (.NET's SystemNative_CopyFile tries the FICLONE ioctl: btrfs, XFS), so it costs no space there either. A link
// falls back to the copy on a drive without links (FAT32, exFAT), across drives, and on a share.
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
