using System.Runtime.InteropServices;
using HellForge.Core;

// The Linux copy-back (SandboxFiles through SandboxFolders/SandboxStats): only regular, singly linked files come back
// from a session folder. A symlink, FIFO, device node or hard link there is skipped, and no FIFO is ever opened.
static class SandboxCopyBackTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux sandbox copy-back copies regular files and skips links, FIFOs and hard links", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox copy-back)"); return; }
            var baseFolder = Path.Combine(root, "sandbox-copyback");
            var gameOut = Path.Combine(baseFolder, "gameOut");
            var destination = Path.Combine(baseFolder, "destination");
            var outside = Path.Combine(baseFolder, "outside");
            Directory.CreateDirectory(Path.Combine(gameOut, "profiles", "player"));
            Directory.CreateDirectory(destination);
            Directory.CreateDirectory(outside);
            File.WriteAllText(Path.Combine(gameOut, "profiles", "player", "regular.txt"), "regular");
            var target = Path.Combine(outside, "target.txt");
            File.WriteAllText(target, "outside");
            File.CreateSymbolicLink(Path.Combine(gameOut, "profiles", "player", "link.txt"), target);
            var hardA = Path.Combine(gameOut, "profiles", "player", "hard-a.txt");
            var hardB = Path.Combine(gameOut, "profiles", "player", "hard-b.txt");
            File.WriteAllText(hardA, "hard");
            Native.Link(hardA, hardB);
            var fifo = Path.Combine(gameOut, "profiles", "player", "fifo.txt");
            Native.MakeFifo(fifo);

            var copy = Task.Run(() => SandboxFolders.Copy(gameOut, destination, ["profiles/player"], fromSession: true));
            bool copyFinished;
            try { copyFinished = copy.Wait(TimeSpan.FromSeconds(20)); }
            catch (AggregateException) { copyFinished = true; }
            check(copyFinished, "The copy-back did not finish; a FIFO was probably opened.");
            check(!copy.IsFaulted, "The copy-back failed: " + copy.Exception?.InnerException?.Message);
            var copied = copy.Result;
            check(copied.Files == 1 && copied.Bytes == "regular".Length, $"Only the regular file should be copied (got {copied.Files} files).");
            check(File.ReadAllText(Path.Combine(destination, "profiles", "player", "regular.txt")) == "regular",
                "The regular session file was not copied.");
            foreach (var skipped in new[] { "link.txt", "hard-a.txt", "hard-b.txt", "fifo.txt" })
                check(!File.Exists(Path.Combine(destination, "profiles", "player", skipped)),
                    $"{skipped} from the session reached the destination.");
            check(copied.Skipped >= 4, $"The skipped entries were not counted ({copied.Skipped}).");
        });

        test("Linux sandbox copy-back skips a device node", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox copy-back)"); return; }
            var baseFolder = Path.Combine(root, "sandbox-copyback-device");
            var gameOut = Path.Combine(baseFolder, "gameOut");
            var destination = Path.Combine(baseFolder, "destination");
            Directory.CreateDirectory(Path.Combine(gameOut, "profiles", "player"));
            Directory.CreateDirectory(destination);
            var device = Path.Combine(gameOut, "profiles", "player", "device");
            if (!Native.TryMakeCharDevice(device))
            {
                Console.WriteLine("  (skipped: a device node needs CAP_MKNOD; run this test as root to include it)");
                return;
            }
            var copied = SandboxFolders.Copy(gameOut, destination, ["profiles/player"], fromSession: true);
            check(copied.Files == 0 && copied.Skipped >= 1, "A device node from the session was not skipped.");
            check(!File.Exists(Path.Combine(destination, "profiles", "player", "device")),
                "The device node reached the destination.");
        });

        test("Linux sandbox state cleanup removes mode-000 work/work", () => {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox state cleanup)"); return; }
            var session = Path.Combine(root, "sandbox-state-cleanup", "run");
            var sandbox = Path.Combine(session, "sandbox");
            Directory.CreateDirectory(Path.Combine(sandbox, "upper", "pfx"));
            File.WriteAllText(Path.Combine(sandbox, "upper", "pfx", "overlay-write"), "overlay");
            var work = Directory.CreateDirectory(Path.Combine(sandbox, "work", "work")).FullName;
            File.SetUnixFileMode(work, UnixFileMode.None);
            SandboxFolders.RemoveSessionState(session);
            check(!Directory.Exists(sandbox), "The sandbox upper/work folder survived session cleanup.");
            check(Directory.Exists(session), "Session cleanup removed the run folder itself.");
        });

        test("Linux sandbox stats refuse a linked changes file and a FIFO without hanging", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox stats)"); return; }
            var baseFolder = Path.Combine(root, "sandbox-stats-safe");
            var stats = Path.Combine(baseFolder, "stats");
            var outside = Path.Combine(baseFolder, "outside");
            Directory.CreateDirectory(stats);
            Directory.CreateDirectory(outside);
            var outsideChanges = Path.Combine(outside, "changes.json");
            File.WriteAllText(outsideChanges, """{"format":1,"changes":[]}""");
            File.CreateSymbolicLink(Path.Combine(stats, "changes.json"), outsideChanges);
            var linked = Task.Run(() => SandboxStats.Apply(stats, Path.Combine(baseFolder, "mods"), EntityProfile.Current));
            bool linkedFinished;
            try { linkedFinished = linked.Wait(TimeSpan.FromSeconds(20)); }
            catch (AggregateException) { linkedFinished = true; }
            check(linkedFinished, "A linked changes file blocked the stats copy-back.");
            check(linked.IsFaulted && linked.Exception?.InnerException is InvalidDataException or IOException,
                "A linked changes file was accepted by the stats copy-back.");

            File.Delete(Path.Combine(stats, "changes.json"));
            Native.MakeFifo(Path.Combine(stats, "changes.json"));
            var fifo = Task.Run(() => SandboxStats.Apply(stats, Path.Combine(baseFolder, "mods"), EntityProfile.Current));
            bool fifoFinished;
            try { fifoFinished = fifo.Wait(TimeSpan.FromSeconds(20)); }
            catch (AggregateException) { fifoFinished = true; }
            check(fifoFinished, "A FIFO changes file blocked the stats copy-back.");
            check(fifo.IsFaulted || fifo.Result == null, "A FIFO changes file was read as the editor's changes.");
        });
    }

    static class Native
    {
        [DllImport("libc", SetLastError = true)]
        static extern int mkfifo([MarshalAs(UnmanagedType.LPUTF8Str)] string path, uint mode);

        [DllImport("libc", SetLastError = true, EntryPoint = "mknod")]
        static extern int make_node([MarshalAs(UnmanagedType.LPUTF8Str)] string path, uint mode, ulong device);

        [DllImport("libc", SetLastError = true)]
        static extern int link([MarshalAs(UnmanagedType.LPUTF8Str)] string oldPath, [MarshalAs(UnmanagedType.LPUTF8Str)] string newPath);

        public static void MakeFifo(string path)
        {
            if (mkfifo(path, 0600) != 0) throw new IOException($"mkfifo {path} failed (errno {Marshal.GetLastPInvokeError()}).");
        }

        public static void Link(string oldPath, string newPath)
        {
            if (link(oldPath, newPath) != 0) throw new IOException($"link {newPath} failed (errno {Marshal.GetLastPInvokeError()}).");
        }

        // S_IFCHR | 0600 for the traditional /dev/null (major 1, minor 3). Unprivileged users get EPERM.
        public static bool TryMakeCharDevice(string path) => make_node(path, 0x2000 | 0x180, (1UL << 8) | 3) == 0;
    }
}
