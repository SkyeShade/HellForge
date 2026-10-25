namespace HellForge.Core;

// A drive without room for what setup is about to write (the game copy, a build patch and the files it patches). Setup
// asks for space instead of another download, which would need the same space again.
public sealed class NotEnoughSpaceException(string message) : IOException(message);

public static class DiskSpace
{
    // Kept free beyond what is written: the file system's own needs, and a file's .partial beside its old copy.
    public const long Margin = 256L << 20;

    // Throws NotEnoughSpaceException unless each drive has what is written to it (folders on one drive add up) and the
    // margin free.
    public static void Require(params (string Folder, long Bytes)[] writes)
    {
        foreach (var drive in writes.Where(w => w.Bytes > 0).GroupBy(w => Path.GetPathRoot(Path.GetFullPath(w.Folder))!, StringComparer.OrdinalIgnoreCase))
        {
            var info = new DriveInfo(drive.Key);
            long needed = drive.Sum(w => w.Bytes) + Margin, free = info.AvailableFreeSpace;
            if (free < needed)
                throw new NotEnoughSpaceException($"Not enough free space on {info.Name}: {GB(needed - free)} more is needed " +
                    $"({GB(needed)} to write, {GB(free)} free).");
        }
    }

    public static string GB(long bytes) => $"{Math.Max(bytes, 0) / 1e9:0.0} GB";
}
