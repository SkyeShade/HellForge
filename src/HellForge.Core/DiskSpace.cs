namespace HellForge.Core;

// A drive without room for what setup is about to write (the game copy, a build patch and the files it patches). Setup
// asks for space instead of another download, which would need the same space again.
public sealed class NotEnoughSpaceException(string message) : IOException(message);

// The file system a folder's writes land on: the name a player knows it by and the free bytes on it, or null when they
// cannot be read.
public sealed record DiskSpaceInfo(string Name, long? FreeBytes);

public static class DiskSpace
{
    // Kept free beyond what is written: the file system's own needs, and a file's .partial beside its old copy.
    public const long Margin = 256L << 20;

    // Throws NotEnoughSpaceException unless each file system has what is written to it (folders on one file system add
    // up) and the margin free. A file system whose free space cannot be read is skipped: nothing is known to be too
    // little, and the write itself still fails cleanly if the disk is full.
    public static void Require(params (string Folder, long Bytes)[] writes) => Require(ForFolder, writes);

    // The same check with the file-system lookup supplied; MatchMount-style pure testing uses this.
    public static void Require(Func<string, DiskSpaceInfo> lookup, params (string Folder, long Bytes)[] writes)
    {
        var fileSystems = writes.Where(write => write.Bytes > 0)
            .Select(write => (Write: write, Space: lookup(write.Folder)))
            .GroupBy(entry => entry.Space.Name,
                OperatingSystem.IsLinux() ? StringComparer.Ordinal : StringComparer.OrdinalIgnoreCase);
        foreach (var fileSystem in fileSystems)
        {
            var space = fileSystem.First().Space;
            if (space.FreeBytes is not { } free) continue;
            long needed = fileSystem.Sum(entry => entry.Write.Bytes) + Margin;
            if (free < needed)
                throw new NotEnoughSpaceException($"Not enough free space on {space.Name}: {GB(needed - free)} more is needed " +
                    $"({GB(needed)} to write, {GB(free)} free).");
        }
    }

    // The file system holding folder. On Windows that is its drive, exactly as before. On Linux Path.GetPathRoot is
    // always "/", so the mount point that is the longest part of the folder's full path is used instead.
    public static DiskSpaceInfo ForFolder(string folder)
    {
        var full = LocalInstall.FullPath(folder);
        if (!OperatingSystem.IsLinux())
        {
            var root = Path.GetPathRoot(full) ?? throw new InvalidDataException("The folder has no file system root: " + folder);
            var drive = new DriveInfo(root);
            return new(drive.Name, drive.AvailableFreeSpace);
        }
        try
        {
            var mount = MountPoint(full, MountPoints());
            if (mount.Length > 0 && FromMount(mount) is { } space) return space;
        }
        catch (Exception error) when (DiskLookupFailed(error)) { }
        // When the mount list cannot be read, the old Linux measurement of the root file system still runs.
        return Fallback(full);
    }

    // The longest mount point that holds fullPath, at a path separator so /home does not hold /homework and /proc is
    // chosen only for paths below it. Pure: the caller passes the mount list and an already resolved full path.
    public static string MatchMount(string fullPath, IEnumerable<string> mountPoints)
    {
        var path = Path.TrimEndingDirectorySeparator(fullPath);
        var best = "";
        foreach (var raw in mountPoints)
        {
            if (string.IsNullOrEmpty(raw)) continue;
            var mount = Path.TrimEndingDirectorySeparator(raw);
            if (mount.Length == 0 || mount.Length <= best.Length || !Holds(path, mount)) continue;
            best = mount;
        }
        return best.Length > 0 ? best : Path.GetPathRoot(path) ?? "";
    }

    // The mount point for a folder that may not exist yet: its nearest existing ancestor is resolved through links,
    // then the mount match runs on the resolved path.
    public static string MountPoint(string folder, IEnumerable<string> mountPoints)
    {
        var full = LocalInstall.FullPath(folder);
        var existing = NearestExisting(full);
        var real = RealPath(existing);
        var tail = Path.GetRelativePath(existing, full);
        var resolved = tail == "." ? real : Path.Combine(real, tail);
        return MatchMount(resolved, mountPoints);
    }

    static DiskSpaceInfo? FromMount(string mount)
    {
        try
        {
            var drive = new DriveInfo(mount);
            return drive.IsReady ? new(drive.Name, drive.AvailableFreeSpace) : null;
        }
        catch (Exception error) when (DiskLookupFailed(error)) { return null; }
    }

    static IReadOnlyList<string> MountPoints()
    {
        var mounts = new List<string>();
        foreach (var drive in DriveInfo.GetDrives())
        {
            try
            {
                if (!string.IsNullOrEmpty(drive.Name)) mounts.Add(drive.Name);
            }
            catch (Exception error) when (DiskLookupFailed(error)) { }
        }
        return mounts;
    }

    static DiskSpaceInfo Fallback(string full)
    {
        var root = Path.GetPathRoot(full) ?? "/";
        try
        {
            var drive = new DriveInfo(root);
            return drive.IsReady ? new(drive.Name, drive.AvailableFreeSpace) : new(root, null);
        }
        catch (Exception error) when (DiskLookupFailed(error)) { return new(root, null); }
    }

    static string NearestExisting(string full)
    {
        var current = full;
        while (!Directory.Exists(current) && !File.Exists(current))
        {
            var parent = Path.GetDirectoryName(current);
            if (parent == null) return Path.GetPathRoot(full) ?? full;
            current = parent;
        }
        return current;
    }

    // Replaces a link anywhere in an existing path with its real target, one component at a time. A loop or a failed
    // lookup leaves the path as it was, so the caller's fallback still applies.
    static string RealPath(string path)
    {
        try
        {
            var full = LocalInstall.FullPath(path);
            var root = Path.GetPathRoot(full);
            if (root == null) return full;
            var pending = new Queue<string>(Parts(full[root.Length..]));
            var current = root;
            for (var links = 0; pending.Count > 0;)
            {
                var next = Path.Combine(current, pending.Dequeue());
                FileSystemInfo info = Directory.Exists(next) ? new DirectoryInfo(next) : new FileInfo(next);
                var target = info.ResolveLinkTarget(returnFinalTarget: false);
                if (target == null) { current = next; continue; }
                if (++links > 40) return full;
                var targetPath = target.FullName;
                if (!Path.IsPathFullyQualified(targetPath))
                    targetPath = Path.GetFullPath(targetPath, Path.GetDirectoryName(next)!);
                targetPath = Path.GetFullPath(targetPath);
                current = Path.GetPathRoot(targetPath)!;
                var rest = new Queue<string>(Parts(targetPath[current.Length..]));
                foreach (var part in pending) rest.Enqueue(part);
                pending = rest;
            }
            return current;
        }
        catch (Exception error) when (DiskLookupFailed(error)) { return LocalInstall.FullPath(path); }
    }

    static IEnumerable<string> Parts(string relative) =>
        relative.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries);

    static bool Holds(string path, string mount)
    {
        if (path.Equals(mount, StringComparison.Ordinal)) return true;
        if (!path.StartsWith(mount, StringComparison.Ordinal)) return false;
        return Path.EndsInDirectorySeparator(mount) || path[mount.Length] == Path.DirectorySeparatorChar;
    }

    static bool DiskLookupFailed(Exception error) =>
        error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException or InvalidOperationException or
            System.Security.SecurityException;

    public static string GB(long bytes) => $"{Math.Max(bytes, 0) / 1e9:0.0} GB";
}
