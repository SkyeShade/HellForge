using System.Diagnostics;
using System.Runtime.InteropServices;
using HellForge.Core;

// LinuxPrefixCase: HellForge's prefix made case-sensitive for the sandbox's overlay. The conversion is checked with a
// stand-in for ext4's casefold flag (a folder is "case-insensitive" while it is the very folder, by inode, that was
// marked), and, where this PC allows a user namespace and a casefold tmpfs, on a real case-insensitive drive_c with the
// overlay mounted over the result.
static class LinuxPrefixCaseTests
{
    [DllImport("libc", SetLastError = true, EntryPoint = "stat")]
    static extern int StatPath([MarshalAs(UnmanagedType.LPUTF8Str)] string path, IntPtr buffer);

    static ulong Inode(string path)
    {
        var buffer = Marshal.AllocHGlobal(256);
        try { return StatPath(path, buffer) == 0 ? (ulong)Marshal.ReadInt64(buffer, 8) : 0; }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    // The folders marked case-insensitive, by inode: a rebuilt folder is a new inode, so it reads as case-sensitive.
    static Func<string, bool> Marked(IEnumerable<string> folders)
    {
        var marked = folders.ToDictionary(folder => folder, Inode);
        return folder => marked.TryGetValue(folder, out var inode) && Inode(folder) == inode;
    }

    static string Prefix(string root, string name)
    {
        var prefix = Path.Combine(root, "prefix-case", name);
        var driveC = LinuxPrefixCase.DriveC(prefix);
        Directory.CreateDirectory(Path.Combine(driveC, "windows", "system32"));
        Directory.CreateDirectory(Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge", "game"));
        File.WriteAllText(Path.Combine(prefix, "pfx", "system.reg"), "reg");
        File.WriteAllText(Path.Combine(driveC, "windows", "system32", "Kernel32.dll"), "k32");
        File.WriteAllText(Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge", "game", "File.TXT"), "game");
        File.WriteAllText(Path.Combine(driveC, "users", "steamuser", "Save.dat"), "save");
        return prefix;
    }

    static string[] Folders(string prefix) => Directory
        .EnumerateDirectories(LinuxPrefixCase.DriveC(prefix), "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint })
        .Prepend(LinuxPrefixCase.DriveC(prefix)).ToArray();

    static string Listing(string prefix) => string.Join("\n", Directory
        .EnumerateFileSystemEntries(Path.Combine(prefix, "pfx"), "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint })
        .Select(entry => Path.GetRelativePath(prefix, entry) + (File.Exists(entry) ? "=" + File.ReadAllText(entry) : "/"))
        .Order(StringComparer.Ordinal));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux prefix case: a case-insensitive drive_c is rebuilt case-sensitive by renames, top-down, with every entry kept", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux only)"); return; }
            var prefix = Prefix(root, "convert");
            var driveC = LinuxPrefixCase.DriveC(prefix);
            // A link to a folder is moved as a link, never followed.
            Directory.CreateSymbolicLink(Path.Combine(driveC, "users", "steamuser", "Linked"), Path.Combine(driveC, "windows"));
            var before = Listing(prefix);
            var marked = Folders(prefix);
            var casefolded = Marked(marked);
            check(LinuxPrefixCase.Needed(prefix, casefolded) && LinuxPrefixCase.Problem(prefix, casefolded) != null,
                "A case-insensitive drive_c was not reported.");
            var (converted, problem) = LinuxPrefixCase.Convert(prefix, () => null, casefolded);
            check(problem == null, "The conversion failed: " + problem);
            check(converted == marked.Length, $"Rebuilt {converted} folders, expected {marked.Length}.");
            check(Listing(prefix) == before, "The prefix's entries changed:\n" + before + "\n---\n" + Listing(prefix));
            check(Folders(prefix).All(folder => !casefolded(folder)), "A folder is still marked case-insensitive.");
            check(!File.Exists(Path.Combine(prefix, LinuxPrefixCase.JournalFile)), "The journal was left behind.");
            check(new FileInfo(Path.Combine(driveC, "users", "steamuser", "Linked")).LinkTarget == Path.Combine(driveC, "windows"),
                "The folder link was not kept as a link.");
            check(!LinuxPrefixCase.Needed(prefix, casefolded), "A converted prefix still needs converting.");
            check(LinuxPrefixCase.Convert(prefix, () => "must not be asked", casefolded) == new LinuxPrefixCase.Result(0, null),
                "A case-sensitive prefix was converted again, or asked whether Wine runs.");
        });

        test("Linux prefix case: a conversion stopped mid-folder is finished on the next run; a running Wine refuses it unchanged", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux only)"); return; }
            var prefix = Prefix(root, "resume");
            var driveC = LinuxPrefixCase.DriveC(prefix);
            var before = Listing(prefix);
            var casefolded = Marked(Folders(prefix));

            var (refusedCount, refused) = LinuxPrefixCase.Convert(prefix, () => "A Windows program is still running", casefolded);
            check(refusedCount == 0 && refused == "A Windows program is still running" && Listing(prefix) == before &&
                !File.Exists(Path.Combine(prefix, LinuxPrefixCase.JournalFile)), "A refused conversion changed the prefix.");

            // Stopped while rebuilding users: half its entries in the staging sibling. And windows: removed, its staging
            // sibling not yet renamed back.
            File.WriteAllText(Path.Combine(prefix, LinuxPrefixCase.JournalFile), "in progress");
            var users = Path.Combine(driveC, "users");
            var stagedUsers = Path.Combine(driveC, ".hf-cs-users");
            Directory.CreateDirectory(stagedUsers);
            Directory.Move(Path.Combine(users, "steamuser"), Path.Combine(stagedUsers, "steamuser"));
            File.WriteAllText(Path.Combine(users, "Public.txt"), "public");
            var windows = Path.Combine(driveC, "windows");
            Directory.Move(windows, Path.Combine(driveC, ".hf-cs-windows"));
            var expected = string.Join("\n", Listing(prefix).Replace(".hf-cs-", "", StringComparison.Ordinal).Split('\n').Distinct().Order(StringComparer.Ordinal));
            check(LinuxPrefixCase.Needed(prefix, path => false), "A journal alone did not mark the prefix as needing a run.");

            var (_, problem) = LinuxPrefixCase.Convert(prefix, () => null, casefolded);
            check(problem == null, "The resumed conversion failed: " + problem);
            var after = Listing(prefix);
            check(after == expected && !after.Contains(".hf-cs-", StringComparison.Ordinal),
                "The resumed conversion lost or misplaced entries:\n" + expected + "\n---\n" + after);
            check(Folders(prefix).All(folder => !casefolded(folder)) && !File.Exists(Path.Combine(prefix, LinuxPrefixCase.JournalFile)),
                "The resumed conversion left a case-insensitive folder or its journal.");
        });

        test("Linux prefix case: only the prefix holding the installation is HellForge's own", () =>
        {
            var inside = new InstallLayout(Path.Combine(root, "own-prefix", "pfx", "drive_c", "users", "steamuser", "Documents", "HellForge"));
            check(LinuxPrefixCase.OwnPrefix(inside) == Path.Combine(root, "own-prefix"), "The installation's prefix was not found: " + LinuxPrefixCase.OwnPrefix(inside));
            check(LinuxPrefixCase.OwnPrefix(new InstallLayout(Path.Combine(root, "plain-install"))) == null, "An installation outside a prefix named one.");
        });

        test("Linux prefix case: on a real case-insensitive drive_c (casefold tmpfs), the CLI converts it and the sandbox's overlay reads it", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux only)"); return; }
            var cli = Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "src", "HellForge.Cli", "bin", "Release", "net10.0", "HellForge.Cli");
            cli = Path.GetFullPath(cli);
            if (!File.Exists(cli) || !Available("unshare") || !Available("chattr")) { Console.WriteLine("  (skipped: needs the built CLI, unshare and chattr)"); return; }
            var mount = Directory.CreateDirectory(Path.Combine(root, "casefold-mount")).FullName;
            var script = Path.Combine(root, "casefold.sh");
            File.WriteAllText(script, """
                set -u
                mnt=$1; cli=$2
                mount -t tmpfs -o casefold=utf8-12.1.0 tmpfs "$mnt" 2>/dev/null || { echo SKIP no casefold tmpfs; exit 0; }
                cd "$mnt"
                mkdir -p prefix/pfx
                mkdir prefix/pfx/drive_c
                chattr +F prefix/pfx/drive_c || { echo SKIP no chattr +F; exit 0; }
                install=prefix/pfx/drive_c/users/steamuser/Documents/HellForge
                mkdir -p "$install/logs" "$install/game" prefix/pfx/drive_c/windows
                echo '{}' > "$install/hellforge-install.json"
                echo game > "$install/game/File.TXT"
                echo k32 > prefix/pfx/drive_c/windows/Kernel32.dll
                echo before $(lsattr -d prefix/pfx/drive_c "$install" | cut -c18)
                "$cli" sandbox-prefix-case --install "$mnt/$install"; echo cli=$?
                echo after $(lsattr -d prefix/pfx/drive_c "$install" "$install/game" prefix/pfx/drive_c/windows | cut -c18)
                cf=$(find prefix -type d -exec lsattr -d {} + | cut -c18 | grep -c F)
                echo casefolded=$cf
                # The sandbox's layout: the prefix as the lower layer, the session's layers inside the installation.
                session=$install/logs/sessions/s1/sandbox
                mkdir -p "$session/upper" "$session/work" merged
                mount -t overlay overlay -o "lowerdir=$mnt/prefix,upperdir=$mnt/$session/upper,workdir=$mnt/$session/work,userxattr" merged && echo overlay=ok
                cat merged/pfx/drive_c/users/steamuser/Documents/HellForge/game/File.TXT
                echo new > merged/pfx/drive_c/windows/New.dll && echo write=ok
                cat prefix/pfx/drive_c/windows/New.dll 2>/dev/null || echo lower=untouched
                umount merged
                """);
            var start = new ProcessStartInfo("unshare") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            foreach (var argument in new[] { "-rm", "sh", script, mount, cli }) start.ArgumentList.Add(argument);
            start.Environment.Remove("STEAM_COMPAT_DATA_PATH");
            using var process = Process.Start(start)!;
            var output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0 && output.Contains("unshare", StringComparison.Ordinal)) { Console.WriteLine("  (skipped: no user namespace here)"); return; }
            if (output.Contains("SKIP", StringComparison.Ordinal)) { Console.WriteLine("  (skipped: " + output.Trim() + ")"); return; }
            check(output.Contains("before F F", StringComparison.Ordinal) && output.Contains("cli=0", StringComparison.Ordinal) &&
                output.Contains("after - - - -", StringComparison.Ordinal) && output.Contains("casefolded=0", StringComparison.Ordinal) &&
                output.Contains("overlay=ok", StringComparison.Ordinal) && output.Contains("game\n", StringComparison.Ordinal) &&
                output.Contains("write=ok", StringComparison.Ordinal) && output.Contains("lower=untouched", StringComparison.Ordinal),
                "The real case-insensitive prefix was not converted and sandboxed:\n" + output);
        });
    }

    static bool Available(string program) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':').Any(folder => File.Exists(Path.Combine(folder, program)));
}
