using System.Runtime.InteropServices;

namespace HellForge.Core;

// Proton 11 makes a new prefix's drive_c case-insensitive (ext4's casefold flag) where the filesystem supports it, as
// SteamOS's does. The Linux sandbox lays an overlay over the prefix, and the kernel's overlayfs serves no file from a
// case-insensitive folder in a merged overlay: the mount is refused when the layers differ, and with matching layers
// every read below drive_c fails with "Object is remote" (measured on 7.2.12 with a casefold tmpfs; darcien's Steam
// Machine on #112). Wine finds names case-insensitively by itself, as it does on every other filesystem, so HellForge
// keeps its own prefix case-sensitive instead.
//
// The flag can only change on an empty folder, so a case-insensitive folder is rebuilt: a case-sensitive sibling is
// made, every entry is renamed into it (renames only, nothing is copied, and the names cannot clash because they were
// unique ignoring case), the old folder is removed and the new one takes its name. Top-down, so each new folder's
// parent is already case-sensitive and passes that on to it. A journal file in the prefix marks a conversion in
// progress; a later run finishes a folder left half-moved (its sibling still named .hf-cs-<name>).
public static class LinuxPrefixCase
{
    public const string JournalFile = ".hellforge-case-sensitive.journal";
    const string Staging = ".hf-cs-";
    const uint CasefoldFlag = 0x40000000; // FS_CASEFOLD_FL
    const uint GetFlagsRequest = 0x80086601; // FS_IOC_GETFLAGS

    public sealed record Result(int Converted, string? Problem);

    // The prefix (the compatibility data folder, holding pfx/) whose drive_c holds this installation, or null when the
    // installation is not inside one: only HellForge's own prefix is ever converted.
    public static string? OwnPrefix(InstallLayout install)
    {
        var marker = Path.DirectorySeparatorChar + "pfx" + Path.DirectorySeparatorChar + "drive_c" + Path.DirectorySeparatorChar;
        var root = install.Root + Path.DirectorySeparatorChar;
        var at = root.IndexOf(marker, StringComparison.Ordinal);
        return at <= 0 ? null : root[..at];
    }

    // A prefix path as the installation's is held: absolute, no trailing separator, linked folders above it resolved.
    public static string Normalize(string path) =>
        LocalInstall.ResolveParentLinks(Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)));

    public static string DriveC(string prefix) => Path.Combine(prefix, "pfx", "drive_c");

    // True when the prefix needs converting: drive_c is case-insensitive, or an earlier conversion did not finish.
    public static bool Needed(string prefix, Func<string, bool>? isCasefolded = null)
    {
        isCasefolded ??= IsCasefolded;
        var driveC = DriveC(prefix);
        return File.Exists(Path.Combine(prefix, JournalFile)) || (Directory.Exists(driveC) && isCasefolded(driveC));
    }

    // The reason the sandbox cannot use this prefix as it is, or null. For readiness and the session notice.
    public static string? Problem(string prefix, Func<string, bool>? isCasefolded = null) =>
        Needed(prefix, isCasefolded)
            ? "the prefix's drive_c is case-insensitive (Proton does this on SteamOS), which the kernel's overlay cannot sandbox; HellForge makes it case-sensitive before the next session"
            : null;

    // Converts every case-insensitive folder below drive_c. running: a Wine program or the game using the prefix, as
    // a reason to refuse (the caller's check); nothing is changed then. Returns how many folders were rebuilt.
    public static Result Convert(string prefix, Func<string?> running, Func<string, bool>? isCasefolded = null)
    {
        isCasefolded ??= IsCasefolded;
        if (!Needed(prefix, isCasefolded)) return new(0, null);
        if (running() is { } busy) return new(0, busy);
        var driveC = DriveC(prefix);
        if (!Directory.Exists(driveC) && !Directory.Exists(StagingPath(driveC)))
            return new(0, "the prefix has no drive_c: " + driveC);
        var journal = Path.Combine(prefix, JournalFile);
        try
        {
            File.WriteAllText(journal, "HellForge is making this prefix's drive_c case-sensitive; it finishes on the next run.\n");
            var converted = ConvertTree(driveC, isCasefolded);
            var left = FirstCasefolded(driveC, isCasefolded);
            if (left != null) return new(converted, "a folder is still case-insensitive after the conversion: " + left);
            File.Delete(journal);
            return new(converted, null);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return new(0, "HellForge could not make the prefix case-sensitive (" + error.Message + "); it tries again before the next session.");
        }
    }

    // Top-down from drive_c: each folder is rebuilt if it is case-insensitive, then its children are visited. Returns
    // how many were rebuilt.
    static int ConvertTree(string driveC, Func<string, bool> isCasefolded)
    {
        var converted = 0;
        FinishStaged(driveC);
        var pending = new Stack<string>();
        pending.Push(driveC);
        while (pending.Count > 0)
        {
            var folder = pending.Pop();
            if (isCasefolded(folder))
            {
                Rebuild(folder);
                converted++;
            }
            // A child left staged by a stopped run is finished first, so it is converted like the others.
            foreach (var staged in Directory.EnumerateDirectories(folder, Staging + "*").ToArray())
                FinishStaged(Path.Combine(folder, Path.GetFileName(staged)[Staging.Length..]));
            foreach (var child in Directory.EnumerateDirectories(folder).Order(StringComparer.Ordinal))
                if ((File.GetAttributes(child) & FileAttributes.ReparsePoint) == 0) pending.Push(child);
        }
        return converted;
    }

    static string StagingPath(string folder) =>
        Path.Combine(Path.GetDirectoryName(folder)!, Staging + Path.GetFileName(folder));

    // A run stopped while rebuilding folder: its staging sibling holds some (or all) of its entries.
    static void FinishStaged(string folder)
    {
        var staged = StagingPath(folder);
        if (!Directory.Exists(staged)) return;
        if (Directory.Exists(folder))
        {
            MoveEntries(folder, staged);
            Directory.Delete(folder);
        }
        Rename(staged, folder);
    }

    static void Rebuild(string folder)
    {
        var staged = StagingPath(folder);
        Directory.CreateDirectory(staged);
        if (OperatingSystem.IsLinux()) File.SetUnixFileMode(staged, File.GetUnixFileMode(folder));
        MoveEntries(folder, staged);
        var touched = Directory.GetLastWriteTimeUtc(folder);
        Directory.Delete(folder);
        Rename(staged, folder);
        Directory.SetLastWriteTimeUtc(folder, touched);
    }

    // renameat2(2) for every entry, whatever it is (file, folder, link to either, socket): it is moved as it is, never
    // followed or copied, and RENAME_NOREPLACE refuses rather than replace anything already at the target.
    static void MoveEntries(string from, string to)
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(from, "*", new EnumerationOptions { AttributesToSkip = 0 }).ToArray())
            Rename(entry, Path.Combine(to, Path.GetFileName(entry)));
    }

    static void Rename(string from, string to)
    {
        if (RenameNoReplace(CurrentFolder, from, CurrentFolder, to, NoReplace) != 0)
            throw new IOException("could not move " + from + " to " + to + " (errno " + Marshal.GetLastPInvokeError() + ")");
    }

    static string? FirstCasefolded(string root, Func<string, bool> isCasefolded)
    {
        if (isCasefolded(root)) return root;
        foreach (var child in Directory.EnumerateDirectories(root, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint }))
            if (isCasefolded(child)) return child;
        return null;
    }

    // ext4's (and tmpfs's) casefold flag on a folder; false where the filesystem has no such flag.
    public static bool IsCasefolded(string folder)
    {
        if (!OperatingSystem.IsLinux()) return false;
        var fd = Open(folder, ReadOnly | OpenDirectory | NoFollow | CloseOnExec);
        if (fd < 0) return false;
        try
        {
            uint flags = 0;
            return Ioctl(fd, GetFlagsRequest, ref flags) == 0 && (flags & CasefoldFlag) != 0;
        }
        finally { Close(fd); }
    }

    // A Wine program still running in this prefix (its wineserver socket answers), as the refusal; or null.
    public static string? WineRunning(string prefix)
    {
        var server = LinuxCapture.WineserverServer(LinuxCapture.WinePrefix(prefix));
        if (server == null) return "Cannot tell whether a Windows program is running in HellForge's prefix: close HellForge's game first.";
        foreach (var socket in LinuxCapture.WineserverSocketPaths(server, LinuxSteamBridge.SocketDirectory(), GetUid()))
            if (LinuxSteamBridge.Check(socket, out _) != LinuxSteamBridge.SocketState.NotListening)
                return "A Windows program is still running in HellForge's prefix: close HellForge's game first.";
        return null;
    }

    const int ReadOnly = 0, OpenDirectory = 0x10000, NoFollow = 0x20000, CloseOnExec = 0x80000;

    [DllImport("libc", SetLastError = true, EntryPoint = "open")]
    static extern int Open([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int flags);

    [DllImport("libc", SetLastError = true, EntryPoint = "ioctl")]
    static extern int Ioctl(int fd, ulong request, ref uint flags);

    const int CurrentFolder = -100, NoReplace = 1; // AT_FDCWD, RENAME_NOREPLACE

    [DllImport("libc", SetLastError = true, EntryPoint = "renameat2")]
    static extern int RenameNoReplace(int fromFolder, [MarshalAs(UnmanagedType.LPUTF8Str)] string from, int toFolder,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string to, uint flags);

    [DllImport("libc", EntryPoint = "close")]
    static extern int Close(int fd);

    [DllImport("libc", EntryPoint = "getuid")]
    static extern uint GetUid();
}
