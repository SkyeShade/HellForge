using System.Diagnostics;

// linux/hf-wineserver.sh, which hellforge-offline.sh sources before it makes the network namespace: a wineserver of the
// prefix still running outside the namespace is waited for (polled once a second), then refused; none goes on at once,
// and a state that cannot be told refuses at once. The tests source the helper with a stand-in for the native CLI's
// `steam-bridge check <socket>` (0 accepted, 1 not listening, 3 cannot tell), as ContainerCheckTests source theirs.
static class WineserverWaitTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The Linux wrapper waits for a wineserver that is ending, goes on at once without one, and refuses one that stays", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux wineserver wait)"); return; }
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-wineserver.sh");
            check(File.Exists(helper), "linux/hf-wineserver.sh was not found: " + helper);
            var folder = Directory.CreateDirectory(Path.Combine(root, "wineserver-wait")).FullName;
            var prefix = Directory.CreateDirectory(Path.Combine(folder, "pfx")).FullName;
            var runtime = Directory.CreateDirectory(Path.Combine(folder, "runtime")).FullName;

            // No wineserver: both sockets are checked once and the wrapper goes on without waiting (the 20 s wait was
            // inverted: it polled while none ran and stopped as soon as one did).
            var none = Run(helper, folder, prefix, runtime, "exit 1", null);
            check(none.ExitCode == 0 && none.Calls.Length == 2 && none.Seconds < 5,
                $"No wineserver did not go on at once: exit {none.ExitCode}, {none.Calls.Length} check(s), {none.Seconds:0.0} s: {none.Error}");
            check(none.Calls[0] == "steam-bridge check " + Path.Combine(runtime, "wine", Server(prefix), "socket") &&
                none.Calls[1] == $"steam-bridge check /tmp/.wine-{Uid()}/{Server(prefix)}/socket",
                "The wineserver sockets checked are not the prefix's: " + string.Join(" | ", none.Calls));

            // The fetches' wineserver still ending: polled until it is gone (two polls running, then none), not refused.
            var ending = Run(helper, folder, prefix, runtime, "[ \"$n\" -le 2 ] && exit 0; exit 1", null);
            check(ending.ExitCode == 0 && ending.Calls.Length == 4 && ending.Seconds >= 1.5 && !ending.Error.Contains("still running", StringComparison.Ordinal),
                $"An ending wineserver was not waited for: exit {ending.ExitCode}, {ending.Calls.Length} check(s), {ending.Seconds:0.0} s: {ending.Error}");

            // One that stays through the polls (two here) is refused with the action to take.
            var stays = Run(helper, folder, prefix, runtime, "exit 0", "2");
            check(stays.ExitCode == 1 && stays.Calls.Length == 2 &&
                stays.Error.Contains("A Windows program is still running in the Wine prefix " + prefix, StringComparison.Ordinal) &&
                stays.Error.Contains("Close it (or give HellForge a prefix of its own", StringComparison.Ordinal),
                $"A wineserver that stayed was not refused: exit {stays.ExitCode}, {stays.Calls.Length} check(s): {stays.Error}");

            // Cannot tell: refused at once, fail closed.
            var unknown = Run(helper, folder, prefix, runtime, "exit 3", null);
            check(unknown.ExitCode == 1 && unknown.Calls.Length == 1 && unknown.Seconds < 5 &&
                unknown.Error.Contains("hellforge-offline: cannot tell whether a wineserver of " + prefix + " is running outside the namespace; refusing to start.", StringComparison.Ordinal),
                $"An unreadable wineserver state did not refuse at once: exit {unknown.ExitCode}, {unknown.Calls.Length} check(s): {unknown.Error}");

            // No prefix yet (a first setup): nothing to wait for, nothing checked.
            var missing = Run(helper, folder, Path.Combine(folder, "no-prefix"), runtime, "exit 0", null);
            check(missing.ExitCode == 0 && missing.Calls.Length == 0, "A missing prefix was checked or refused: " + missing.Error);
        });
    }

    // Sources the helper and runs hf_wineserver_wait with a stand-in CLI whose body decides each check's exit code ($n is
    // the check's number, from 1). Returns the exit code, the stand-in's calls, the seconds it took and stderr.
    static (int ExitCode, string[] Calls, double Seconds, string Error) Run(string helper, string folder, string prefix, string runtime,
        string body, string? polls)
    {
        var count = Path.Combine(folder, "calls.count");
        var log = Path.Combine(folder, "calls.log");
        File.Delete(count);
        File.Delete(log);
        var cli = Path.Combine(folder, "HellForge.Cli");
        File.WriteAllText(cli, "#!/usr/bin/env bash\n" +
            $"n=$(( $(cat '{count}' 2>/dev/null || echo 0) + 1 )); echo \"$n\" > '{count}'; echo \"$*\" >> '{log}'\n" + body + "\n");
        if (OperatingSystem.IsLinux()) File.SetUnixFileMode(cli, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.Environment["XDG_RUNTIME_DIR"] = runtime;
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add("set -euo pipefail; source \"$1\"; hf_wineserver_wait \"$2\" \"$3\" \"$(id -u)\" ${4:+\"$4\"}");
        start.ArgumentList.Add("bash");
        start.ArgumentList.Add(helper);
        start.ArgumentList.Add(cli);
        start.ArgumentList.Add(prefix);
        start.ArgumentList.Add(polls ?? "");
        var clock = Stopwatch.StartNew();
        using var process = Process.Start(start) ?? throw new InvalidDataException("bash did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        clock.Stop();
        output.GetAwaiter().GetResult();
        var calls = File.Exists(log) ? File.ReadAllLines(log) : [];
        return (process.ExitCode, calls, clock.Elapsed.TotalSeconds, error.GetAwaiter().GetResult());
    }

    // Wine's socket folder name for a prefix: its device and inode in hex (LinuxCapture.WineserverServer).
    static string Server(string prefix) => HellForge.Core.LinuxCapture.WineserverServer(prefix) ??
        throw new InvalidDataException("The prefix's wineserver name could not be read: " + prefix);

    static string Uid()
    {
        var start = new ProcessStartInfo("id", "-u") { UseShellExecute = false, RedirectStandardOutput = true };
        using var process = Process.Start(start) ?? throw new InvalidDataException("id did not start.");
        var uid = process.StandardOutput.ReadToEnd().Trim();
        process.WaitForExit();
        return uid;
    }

    static string RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        return Environment.CurrentDirectory;
    }
}
