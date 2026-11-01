using System.Formats.Tar;
using System.IO.Compression;
using System.Text;
using HellForge.SteamDownload;

// SteamCMD on the native Linux launcher: the terminal window it runs in (found in SteamCmdTerminal.Known's order, the
// program and its arguments passed as they are, HOME in SteamCMD's folder, its exit code, a closed window, Stop), and the
// checks of Valve's Linux archive and of its two programs, which stand in for the Windows signature check. No network and
// no account: the terminal and SteamCMD are small shell scripts, the archives are made here.
static class SteamCmdTerminalTests
{
    // The start of Valve's steamcmd.sh, and an ELF header (32-bit little-endian x86 unless told otherwise) as
    // linux32/steamcmd has.
    static readonly byte[] Script = Encoding.ASCII.GetBytes("#!/usr/bin/env bash\n\nSTEAMROOT=\"$(cd \"${0%/*}\" && echo $PWD)\"\n");

    static byte[] Elf(byte elfClass = 1, byte machine = 3)
    {
        var bytes = new byte[64];
        bytes[0] = 0x7F;
        bytes[1] = (byte)'E';
        bytes[2] = (byte)'L';
        bytes[3] = (byte)'F';
        bytes[4] = elfClass;
        bytes[5] = 1;
        bytes[18] = machine;
        return bytes;
    }

    // A gzipped tar of entries; a link's content is its target.
    static MemoryStream Archive(params (string Name, TarEntryType Type, byte[] Content)[] entries)
    {
        var stream = new MemoryStream();
        using (var gzip = new GZipStream(stream, CompressionLevel.Fastest, leaveOpen: true))
        using (var writer = new TarWriter(gzip, TarEntryFormat.Gnu))
        {
            foreach (var (name, type, content) in entries)
            {
                var entry = new GnuTarEntry(type, name);
                if (type is TarEntryType.SymbolicLink or TarEntryType.HardLink) entry.LinkName = Encoding.ASCII.GetString(content);
                else if (type == TarEntryType.RegularFile) entry.DataStream = new MemoryStream(content);
                writer.WriteEntry(entry);
            }
        }
        stream.Position = 0;
        return stream;
    }

    static void Executable(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
        if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(path, (UnixFileMode)0b111_101_101);
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Valve's Linux SteamCMD archive is checked before anything in it is unpacked: plain files inside its folder, its two programs", () =>
        {
            (string, TarEntryType, byte[]) Plain(string name, byte[] content) => (name, TarEntryType.RegularFile, content);
            var script = Plain("steamcmd.sh", Script);
            var program = Plain("linux32/steamcmd", Elf());
            var library = Plain("linux32/libstdc++.so.6", Elf());
            using (var valves = Archive(script, ("linux32/", TarEntryType.Directory, []), program, library, Plain("linux32/crashhandler.so", Elf())))
                SteamCmd.CheckLinuxArchive(valves);
            void Refused(string what, MemoryStream archive)
            {
                using (archive)
                {
                    try
                    {
                        SteamCmd.CheckLinuxArchive(archive);
                        check(false, "Taken for Valve's SteamCMD: " + what);
                    }
                    catch (SteamCmdException error)
                    {
                        check(error.Message.Contains("Nothing of it was unpacked or started"), what + ": " + error.Message);
                    }
                }
            }
            Refused("a link", Archive(script, program, ("linux32/libstdc++.so.6", TarEntryType.SymbolicLink, Encoding.ASCII.GetBytes("/usr/lib/libstdc++.so.6"))));
            Refused("a hard link", Archive(script, program, ("linux32/other", TarEntryType.HardLink, Encoding.ASCII.GetBytes("steamcmd.sh"))));
            Refused("a name above its folder", Archive(script, program, Plain("../escape", [1])));
            Refused("a name inside then above", Archive(script, program, Plain("linux32/../../escape", [1])));
            Refused("an absolute name", Archive(script, program, Plain("/etc/escape", [1])));
            Refused("no linux32/steamcmd", Archive(script, library));
            Refused("no steamcmd.sh", Archive(program, library));
            Refused("steamcmd.sh twice", Archive(script, program, Plain("steamcmd.sh", Script)));
            Refused("another script", Archive(Plain("steamcmd.sh", Encoding.ASCII.GetBytes("#!/usr/bin/python3\nprint()\n")), program));
            Refused("a 64-bit program", Archive(script, Plain("linux32/steamcmd", Elf(elfClass: 2))));
            Refused("another machine's program", Archive(script, Plain("linux32/steamcmd", Elf(machine: 40))));
            Refused("not a program", Archive(script, Plain("linux32/steamcmd", Encoding.ASCII.GetBytes("#!/bin/sh\necho\n"))));
            Refused("not a gzipped tar", new MemoryStream(Encoding.ASCII.GetBytes("<html>not found</html>")));
        });

        test("SteamCMD's programs are checked again after it updated itself: a wrong one is not started", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux SteamCMD)"); return; }
            var steamCmd = new SteamCmd(Path.Combine(root, "steamcmd-programs", "tools", "steamcmd"));
            var program = Path.Combine(steamCmd.Folder, "linux32", "steamcmd");
            Directory.CreateDirectory(Path.GetDirectoryName(program)!);
            File.WriteAllBytes(steamCmd.Program, Script);
            File.WriteAllBytes(program, Elf());
            steamCmd.CheckSigned();
            check(File.Exists(steamCmd.Program), "Valve's SteamCMD was refused.");
            foreach (var (what, wrong) in new (string, Action)[]
                {
                    ("a 64-bit linux32/steamcmd", () => File.WriteAllBytes(program, Elf(elfClass: 2))),
                    ("a link out of its folder", () => { File.Delete(program); File.CreateSymbolicLink(program, "/bin/sh"); }),
                })
            {
                File.WriteAllBytes(steamCmd.Program, Script);
                if (File.Exists(program) || new FileInfo(program).LinkTarget != null) File.Delete(program);
                File.WriteAllBytes(program, Elf());
                wrong();
                try
                {
                    steamCmd.CheckSigned();
                    check(false, "Started with " + what);
                }
                catch (SteamCmdException) { check(!File.Exists(steamCmd.Program), "steamcmd.sh was kept after " + what); }
            }
        });

        test("The native Linux launcher finds a terminal for SteamCMD in order, and gives it the program and its arguments as they are", () =>
        {
            check(SteamCmdTerminal.Known.Take(5).Select(t => t.Name).SequenceEqual(["xdg-terminal-exec", "konsole", "gnome-terminal", "kgx", "xterm"]),
                "The order of preference.");
            var first = Path.Combine(root, "terminals", "a");
            var second = Path.Combine(root, "terminals", "b");
            foreach (var (folder, name) in new[] { (first, "xterm"), (first, "konsole"), (second, "gnome-terminal"), (second, "kgx"), (second, "xdg-terminal-exec"),
                (second, "konsole") })
                Executable(Path.Combine(folder, name), "#!/bin/sh\n");
            bool CanRun(string path) => File.Exists(path) && Path.GetFileName(path) != "xdg-terminal-exec";
            var found = SteamCmdTerminal.Available(string.Join(Path.PathSeparator, "relative", first, second), CanRun);
            check(found.Select(t => t.Name).SequenceEqual(["konsole", "gnome-terminal", "kgx", "xterm"]), "Found: " + string.Join(", ", found.Select(t => t.Name)));
            check(found[0].Program == Path.Combine(first, "konsole"), "The first PATH folder's konsole was not taken: " + found[0].Program);
            check(found[0].CommandLine(["/bin/sh", "-c", "a b; c", "two words"]).SequenceEqual(["-e", "/bin/sh", "-c", "a b; c", "two words"]) &&
                found[1].CommandLine(["/bin/sh"]).SequenceEqual(["--", "/bin/sh"]), "The program and its arguments were not passed as they are.");
            check(SteamCmdTerminal.Available(Path.Combine(root, "terminals", "none"), CanRun).Count == 0, "A terminal found where there is none.");
            if (OperatingSystem.IsLinux())
            {
                var modes = Path.Combine(root, "terminals", "modes");
                Executable(Path.Combine(modes, "konsole"), "#!/bin/sh\n");
                Directory.CreateDirectory(modes);
                File.WriteAllText(Path.Combine(modes, "xterm"), "#!/bin/sh\n");
                File.SetUnixFileMode(Path.Combine(modes, "xterm"), UnixFileMode.UserRead | UnixFileMode.UserWrite);
                check(SteamCmdTerminal.Available(modes).Select(t => t.Name).SequenceEqual(["konsole"]), "A terminal that cannot run was taken.");
            }
        });

        test("SteamCMD runs in a terminal window with HOME in its folder and its arguments as they are; its exit code, a closed window and Stop", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux terminal window)"); return; }
            var steamCmd = new SteamCmd(Path.Combine(root, "steamcmd-terminal", "tools", "steamcmd"));
            var seen = Path.Combine(steamCmd.Folder, "seen");
            var sleeper = Path.Combine(steamCmd.Folder, "sleeper");
            // SteamCMD's stand-in: says what it was started with, then ends with exit code 5, closes its window, or waits
            Executable(steamCmd.Program, "#!/bin/sh\nprintf '%s\\n' \"$HOME\" \"$PWD\" \"$@\" > \"${0%/*}/seen\"\n" +
                "case \"$1\" in close) kill -9 $PPID; exit 0;; wait) echo $$ > \"${0%/*}/sleeper\"; exec sleep 30;; esac\nexit 5\n");
            // a terminal that runs its command in place, and one that fails before opening anything
            var fake = Path.Combine(root, "steamcmd-terminal", "bin", "fake-terminal");
            Executable(fake, "#!/bin/sh\n[ \"$1\" = \"-e\" ] && shift\nexec \"$@\"\n");
            var working = new Terminal("fake-terminal", fake, ["-e"]);
            var broken = new Terminal("broken", "/bin/sh", ["-c", "exit 3"]);
            string[] arguments = ["+login", "some_one1", "two words; +quit", "+quit"];
            string? opened = null;
            var code = steamCmd.RunInTerminalAsync([broken, working], arguments, t => opened = t.Name, default).GetAwaiter().GetResult();
            var lines = File.ReadAllLines(seen);
            check(code == 5 && opened == "fake-terminal", $"Exit code {code} from {opened}.");
            check(lines.SequenceEqual([Path.Combine(steamCmd.Folder, "home"), steamCmd.Folder, .. arguments]), "SteamCMD was started with: " + string.Join(" | ", lines));
            check(!Directory.EnumerateFiles(steamCmd.Folder, "hellforge-terminal*").Any(), "The window's status files were left.");

            check(steamCmd.RunInTerminalAsync([working], ["close"], null, default).GetAwaiter().GetResult() == -1, "A closed window was not seen.");
            check(SteamCmd.Unfinished(-1, []).Contains("window was closed"), "A closed window's words: " + SteamCmd.Unfinished(-1, []));

            using (var stop = new CancellationTokenSource(TimeSpan.FromSeconds(3)))
            {
                try
                {
                    steamCmd.RunInTerminalAsync([working], ["wait"], null, stop.Token).GetAwaiter().GetResult();
                    check(false, "Stop did not stop SteamCMD.");
                }
                catch (OperationCanceledException) { }
            }
            var pid = File.ReadAllText(sleeper).Trim();
            for (var i = 0; i < 50 && Directory.Exists("/proc/" + pid); i++) Thread.Sleep(100);
            check(!Directory.Exists("/proc/" + pid), "SteamCMD still runs after Stop.");

            try
            {
                steamCmd.RunInTerminalAsync([], arguments, null, default).GetAwaiter().GetResult();
                check(false, "SteamCMD started with no terminal program.");
            }
            catch (SteamCmdException error) { check(error.Message == SteamCmd.NoTerminalFound, error.Message); }
        });
    }
}
