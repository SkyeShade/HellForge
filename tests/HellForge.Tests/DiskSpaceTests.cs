using HellForge.Core;

// Which file system a folder's writes land on: the longest mount point on Linux, the drive root on Windows, and the
// nearest existing ancestor when the folder itself has not been made yet.
static class DiskSpaceTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The file system holding a folder is its longest mount point, matched on path boundaries", () =>
        {
            check(DiskSpace.MatchMount("/home/deck/x", ["/", "/home"]) == "/home", "The /home mount was not chosen.");
            check(DiskSpace.MatchMount("/homework", ["/", "/home"]) == "/", "/home was chosen for /homework.");
            check(DiskSpace.MatchMount("/home", ["/", "/home"]) == "/home", "An exact mount point was not chosen.");
            check(DiskSpace.MatchMount("/mnt/games/new/folder", ["/", "/mnt/games"]) == "/mnt/games",
                "A folder under a mount that does not exist yet was not put on that mount.");
            var mounts = new[] { "/", "/proc", "/sys", "/run" };
            check(DiskSpace.MatchMount("/proc/version", mounts) == "/proc", "/proc did not hold its own path.");
            check(DiskSpace.MatchMount("/procession/x", mounts) == "/", "/proc was chosen for /procession.");
            check(DiskSpace.MatchMount("/system/x", mounts) == "/", "/sys was chosen for /system.");
            check(DiskSpace.MatchMount("/run/media/me/usb", mounts) == "/run", "/run did not hold its own path.");
            check(DiskSpace.MatchMount("/runaway", mounts) == "/", "/run was chosen for /runaway.");
        });

        test("A folder that does not exist yet and a symlinked one use the file system of the nearest existing ancestor", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux mount points)"); return; }
            var sandbox = Directory.CreateDirectory(Path.Combine(root, "disk-space-mounts")).FullName;
            var real = Directory.CreateDirectory(Path.Combine(sandbox, "real", "nested")).FullName;
            var mounts = new[] { "/", real };
            var missing = Path.Combine(real, "not", "made", "yet");
            check(DiskSpace.MountPoint(missing, mounts) == real,
                "A folder that does not exist yet did not use its nearest existing ancestor.");
            var link = Path.Combine(sandbox, "link");
            Directory.CreateSymbolicLink(link, real);
            var throughLink = Path.Combine(link, "nested", "not", "made", "yet");
            check(DiskSpace.MountPoint(throughLink, mounts) == real,
                "A symlinked folder did not resolve to its target's file system.");
            var info = DiskSpace.ForFolder(throughLink);
            check(info.Name.Length > 0 && info.FreeBytes is > 0,
                "The free-space lookup failed for a symlinked folder that does not exist yet.");
        });

        test("An unreadable file system is unknown, so the space check does not refuse", () =>
        {
            var unknown = new DiskSpaceInfo("/home", null);
            DiskSpace.Require(_ => unknown, ("/home/deck/not-made", long.MaxValue / 4));
            try
            {
                DiskSpace.Require(_ => unknown with { FreeBytes = 1 }, ("/home/deck/not-made", long.MaxValue / 4));
                check(false, "A known-too-small file system was accepted.");
            }
            catch (NotEnoughSpaceException error) { check(error.Message.Contains("/home", StringComparison.Ordinal), error.Message); }
        });

        test("Windows drive roots still come from Path.GetPathRoot and DriveInfo", () =>
        {
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: Windows drive roots)"); return; }
            var folder = Path.Combine(root, "disk-space-windows", "not", "made", "yet");
            var rootPath = Path.GetPathRoot(Path.GetFullPath(folder))!;
            var expected = new DriveInfo(rootPath);
            var info = DiskSpace.ForFolder(folder);
            check(info.Name == expected.Name && info.Name == rootPath, $"The drive name changed: {info.Name} instead of {rootPath}.");
            check(info.FreeBytes is > 0, "The drive has no reported free space.");
        });
    }
}
