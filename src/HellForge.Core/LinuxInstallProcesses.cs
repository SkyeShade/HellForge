using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;

namespace HellForge.Core;

// A process as /proc reports it: its command name, executable, arguments, environment and working directory.
public sealed record LinuxProcessInfo(string Name, string? Executable, IReadOnlyList<string> Arguments,
    IReadOnlyList<string> Environment, string? WorkingDirectory);

// A process of an installation, with the id needed to end it.
public sealed record LinuxInstallProcess(int Id, string Name);

// Finds processes that belong to an installation, using the evidence hf-inner.sh uses: a path under the installation,
// INSTALL=<root> in the environment, or the installation as the working directory. The launcher has no state file of its
// own; a session's play-session.json is left behind after it ends, so the live processes are what decides.
public static class LinuxInstallProcesses
{
    public static IReadOnlyList<string> Running(string installRoot) =>
        Find(installRoot).Select(process => process.Name).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();

    // The processes themselves, for the launcher's "End these processes" action.
    public static IReadOnlyList<LinuxInstallProcess> Find(string installRoot)
    {
        if (!OperatingSystem.IsLinux()) return [];
        var root = LocalInstall.FullPath(installRoot);
        return Matching(process => BelongsTo(process, root));
    }

    // The launcher's check before PLAY and its "End these processes" action: this installation's processes except the
    // Steam bridge. The bridge outlives sessions on purpose: a player may start hf-bridge-host.sh by hand or as a Steam
    // shortcut (for NixOS's steam-run, where the launcher cannot read Steam's sockets), and the wrapper uses a bridge that
    // answers (docs/linux.md "Sessions"). keep is a process the launcher runs itself and is not a leftover: the capture of
    // its own Account Sync, which keeps the normal game connected until the game closes.
    public static IReadOnlyList<string> SessionLeftovers(string installRoot, int? keep = null) =>
        FindSessionLeftovers(installRoot, keep).Select(process => process.Name).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();

    static IReadOnlyList<LinuxInstallProcess> FindSessionLeftovers(string installRoot, int? keep)
    {
        if (!OperatingSystem.IsLinux()) return [];
        var root = LocalInstall.FullPath(installRoot);
        return Matching(process => BelongsTo(process, root) && !IsSteamBridge(process, root)).Where(process => process.Id != keep).ToArray();
    }

    // The bridge itself, or a program that only starts it. hf-bridge-host.sh execs app/cli-linux/HellForge.Cli steam-bridge
    // host, which is matched by its command line: /proc shows that to every process, while the executable, environment
    // and working folder of a process started in another sandbox (Steam's, from steam-run) can be unreadable. A starter
    // (Steam's reaper, the shell before its exec) names the bridge script and no other path of the installation and runs
    // no program from it; its working folder may be the script's (a Steam shortcut's Start In).
    public static bool IsSteamBridge(LinuxProcessInfo process, string installRoot)
    {
        var root = LocalInstall.FullPath(installRoot);
        var app = Path.Combine(root, "app");
        if (process.Arguments is [var program, "steam-bridge", "host"])
            return SameHostPath(program, Path.Combine(app, "cli-linux", "HellForge.Cli"));
        if (Under(process.Executable, root)) return false;
        var script = Path.Combine(app, "linux", "hf-bridge-host.sh");
        var named = process.Arguments.Where(argument => UnderHostOrWine(argument, root)).ToArray();
        return named.Length > 0 && named.All(argument => SameHostPath(argument, script));
    }

    // Ends every live session leftover of this installation (not the Steam bridge) and returns the names it asked the
    // kernel to end. The next scan decides whether they are gone; a process that exits between the scan and the kill is
    // already in the desired state.
    public static IReadOnlyList<string> EndSessionLeftovers(string installRoot, int? keep = null)
    {
        var ended = new List<string>();
        foreach (var process in FindSessionLeftovers(installRoot, keep))
        {
            try
            {
                using var handle = Process.GetProcessById(process.Id);
                handle.Kill(entireProcessTree: true);
                handle.WaitForExit(5000);
                ended.Add(process.Name);
            }
            catch (Exception error) when (error is ArgumentException or InvalidOperationException or Win32Exception or NotSupportedException) { }
        }
        return ended.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
    }

    // A prefix is in use while any process has WINEPREFIX=<prefix>/pfx or an executable or working folder inside it. The
    // launcher itself and its parent chain are ignored: uninstall runs from inside the prefix it is asked about.
    public static IReadOnlyList<string> RunningInPrefix(string prefix)
    {
        if (!OperatingSystem.IsLinux()) return [];
        var root = LocalInstall.FullPath(prefix);
        return Matching(process => UsesPrefix(process, root)).Select(process => process.Name)
            .Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
    }

    static IReadOnlyList<LinuxInstallProcess> Matching(Func<LinuxProcessInfo, bool> belongs)
    {
        var ignored = new HashSet<int> { Environment.ProcessId };
        var parent = Parent(Environment.ProcessId);
        while (parent is { } pid && pid > 1 && ignored.Add(pid)) parent = Parent(pid);
        var processes = new List<LinuxInstallProcess>();
        foreach (var folder in Directory.EnumerateDirectories("/proc"))
        {
            if (!int.TryParse(Path.GetFileName(folder), NumberStyles.None, CultureInfo.InvariantCulture, out var pid) ||
                ignored.Contains(pid)) continue;
            try
            {
                if (Read(pid) is { } process && belongs(process)) processes.Add(new(pid, process.Name));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        }
        return processes.OrderBy(process => process.Name, StringComparer.Ordinal).ThenBy(process => process.Id).ToArray();
    }

    public static bool UsesPrefix(LinuxProcessInfo process, string prefix)
    {
        var root = LocalInstall.FullPath(prefix);
        var pfx = Path.Combine(root, "pfx");
        if (process.Environment.Any(entry => entry.StartsWith("WINEPREFIX=", StringComparison.Ordinal) &&
                SamePath(entry["WINEPREFIX=".Length..], root, pfx))) return true;
        return UnderHostOrWine(process.Executable, root, root) || UnderHostOrWine(process.WorkingDirectory, root, root);
    }

    // A path from another process is matched only when it is absolute: /proc gives absolute readlink paths, but a
    // WINEPREFIX or an argument may be relative to the process that has it. Resolving one here would use this process's
    // working directory, which setup sets to the installation. A Windows path is mapped with WinePath first; a relative
    // path maps to itself and the absolute check rejects it.
    static bool SamePath(string? path, string prefix, string expected)
    {
        if (SameHostPath(path, expected)) return true;
        if (string.IsNullOrEmpty(path)) return false;
        try { return SameHostPath(WinePath.ToHostPath(path, prefix), expected); }
        catch (Exception error) when (error is InvalidDataException or ArgumentException or NotSupportedException or IOException) { return false; }
    }

    static bool SameHostPath(string? path, string expected)
    {
        if (string.IsNullOrEmpty(path) || !Path.IsPathRooted(path)) return false;
        try { return LocalInstall.FullPath(path).Equals(expected, StringComparison.Ordinal); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    public static bool BelongsTo(LinuxProcessInfo process, string installRoot)
    {
        var root = LocalInstall.FullPath(installRoot);
        if (Under(process.Executable, root)) return true;
        if (process.Arguments.Any(argument => UnderHostOrWine(argument, root))) return true;
        if (process.Environment.Any(entry => entry.Equals("INSTALL=" + root, StringComparison.Ordinal))) return true;
        return Under(process.WorkingDirectory, root);
    }

    // prefix is the Proton prefix that owns root; UsesPrefix already has it and BelongsTo derives it from the
    // installation. A path that is neither absolute on the host nor a Windows path is never matched.
    static bool UnderHostOrWine(string? path, string root, string? prefix = null)
    {
        if (Under(path, root)) return true;
        if (string.IsNullOrEmpty(path)) return false;
        try
        {
            var winePrefix = prefix ?? WinePath.PrefixFromInstallRoot(root);
            return Under(WinePath.ToHostPath(path, winePrefix), root);
        }
        catch (Exception error) when (error is InvalidOperationException or InvalidDataException or ArgumentException or NotSupportedException or IOException) { return false; }
    }

    static bool Under(string? path, string root)
    {
        if (string.IsNullOrEmpty(path) || !Path.IsPathRooted(path)) return false;
        try { return LocalInstall.Within(path, root); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    static LinuxProcessInfo? Read(int pid)
    {
        var folder = Path.Combine("/proc", pid.ToString(CultureInfo.InvariantCulture));
        var arguments = ReadList(Path.Combine(folder, "cmdline"));
        var executable = ReadLink(Path.Combine(folder, "exe"));
        var name = DisplayName(ReadText(Path.Combine(folder, "comm")).Trim(), executable, arguments);
        if (name.Length == 0) return null;
        return new(name, executable, arguments, ReadList(Path.Combine(folder, "environ")), ReadLink(Path.Combine(folder, "cwd")));
    }

    // The kernel keeps 15 characters of a command name ("HellForge.Captu"). A cut name is replaced by the program's file
    // name it starts (the executable's, else the first argument's, which is the Windows path for a Wine program).
    internal static string DisplayName(string comm, string? executable, IReadOnlyList<string> arguments)
    {
        if (comm.Length == 0) return arguments.Count > 0 ? FileName(arguments[0]) : "";
        if (comm.Length < CommLength) return comm;
        foreach (var path in new[] { executable, arguments.Count > 0 ? arguments[0] : null })
            if (path is { Length: > 0 } && FileName(path) is var file && file.Length > comm.Length &&
                file.StartsWith(comm, StringComparison.Ordinal)) return file;
        return comm;
    }

    const int CommLength = 15;

    static string FileName(string path) => path[(path.LastIndexOfAny(['/', '\\']) + 1)..];

    static IReadOnlyList<string> ReadList(string path)
    {
        try
        {
            var text = File.ReadAllText(path);
            if (text.Length == 0) return [];
            return text.TrimEnd('\0').Split('\0');
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    static string ReadText(string path)
    {
        try { return File.ReadAllText(path); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return ""; }
    }

    static string? ReadLink(string path)
    {
        try { return new FileInfo(path).LinkTarget; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static int? Parent(int pid)
    {
        try
        {
            foreach (var line in File.ReadLines(Path.Combine("/proc", pid.ToString(CultureInfo.InvariantCulture), "status")))
                if (line.StartsWith("PPid:", StringComparison.Ordinal) &&
                    int.TryParse(line[5..].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parent))
                    return parent;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return null;
    }
}
