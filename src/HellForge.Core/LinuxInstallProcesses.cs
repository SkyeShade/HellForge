using System.Globalization;

namespace HellForge.Core;

// A process as /proc reports it: its command name, executable, arguments, environment and working directory.
public sealed record LinuxProcessInfo(string Name, string? Executable, IReadOnlyList<string> Arguments,
    IReadOnlyList<string> Environment, string? WorkingDirectory);

// Finds processes that belong to an installation, using the evidence hf-inner.sh uses: a path under the installation,
// INSTALL=<root> in the environment, or the installation as the working directory. The launcher has no state file of its
// own; a session's play-session.json is left behind after it ends, so the live processes are what decides.
public static class LinuxInstallProcesses
{
    public static IReadOnlyList<string> Running(string installRoot)
    {
        if (!OperatingSystem.IsLinux()) return [];
        var root = LocalInstall.FullPath(installRoot);
        return RunningNames(process => BelongsTo(process, root));
    }

    // A prefix is in use while any process has WINEPREFIX=<prefix>/pfx or an executable or working folder inside it. The
    // launcher itself and its parent chain are ignored: uninstall runs from inside the prefix it is asked about.
    public static IReadOnlyList<string> RunningInPrefix(string prefix)
    {
        if (!OperatingSystem.IsLinux()) return [];
        var root = LocalInstall.FullPath(prefix);
        return RunningNames(process => UsesPrefix(process, root));
    }

    static IReadOnlyList<string> RunningNames(Func<LinuxProcessInfo, bool> belongs)
    {
        var ignored = new HashSet<int> { Environment.ProcessId };
        var parent = Parent(Environment.ProcessId);
        while (parent is { } pid && pid > 1 && ignored.Add(pid)) parent = Parent(pid);
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var folder in Directory.EnumerateDirectories("/proc"))
        {
            if (!int.TryParse(Path.GetFileName(folder), NumberStyles.None, CultureInfo.InvariantCulture, out var pid) ||
                ignored.Contains(pid)) continue;
            try
            {
                if (Read(pid) is { } process && belongs(process)) names.Add(process.Name);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        }
        return names.Order(StringComparer.Ordinal).ToArray();
    }

    public static bool UsesPrefix(LinuxProcessInfo process, string prefix)
    {
        var root = LocalInstall.FullPath(prefix);
        var pfx = Path.Combine(root, "pfx");
        if (process.Environment.Any(entry => entry.StartsWith("WINEPREFIX=", StringComparison.Ordinal) &&
                SamePath(entry["WINEPREFIX=".Length..], pfx))) return true;
        return Under(process.Executable, root) || Under(process.WorkingDirectory, root);
    }

    static bool SamePath(string path, string expected)
    {
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

    static bool UnderHostOrWine(string path, string root)
    {
        if (Under(path, root)) return true;
        try
        {
            var prefix = WinePath.PrefixFromInstallRoot(root);
            return Under(WinePath.ToHostPath(path, prefix), root);
        }
        catch (Exception error) when (error is InvalidOperationException or InvalidDataException or ArgumentException or IOException) { return false; }
    }

    static bool Under(string? path, string root)
    {
        if (string.IsNullOrEmpty(path)) return false;
        try { return LocalInstall.Within(path, root); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    static LinuxProcessInfo? Read(int pid)
    {
        var folder = Path.Combine("/proc", pid.ToString(CultureInfo.InvariantCulture));
        var arguments = ReadList(Path.Combine(folder, "cmdline"));
        var name = ReadText(Path.Combine(folder, "comm")).Trim();
        if (name.Length == 0 && arguments.Count > 0) name = Path.GetFileName(arguments[0]) ?? "";
        if (name.Length == 0) return null;
        return new(name, ReadLink(Path.Combine(folder, "exe")), arguments, ReadList(Path.Combine(folder, "environ")),
            ReadLink(Path.Combine(folder, "cwd")));
    }

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
