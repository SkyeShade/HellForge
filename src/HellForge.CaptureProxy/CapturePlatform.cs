using System.Diagnostics;
using System.Security.Principal;

namespace HellForge.CaptureProxy;

// What differs between the Windows capture proxy and the native Linux one. The capture logic stays in Program.cs; this file
// keeps Windows' hosts path, root store, access checks and process names out of the Linux build.
static class CapturePlatform
{
    public static bool IsWindows => OperatingSystem.IsWindows();
    public static bool ManagesSystemTrustStore => IsWindows;

    // The native Linux supervisor starts the proxy with --no-hosts, so this path is used only by plumbing runs and
    // --cleanup. Wine's prefix hosts file is not the game's resolver (review-C3.md section 3).
    public static string HostsPath => IsWindows
        ? Path.Combine(Environment.SystemDirectory, "drivers", "etc", "hosts")
        : "/etc/hosts";

    public static bool IsElevated
    {
        get
        {
            if (!OperatingSystem.IsWindows()) return false;
            using var identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
    }

    // Wine reports the game with its Windows executable name, while Linux's /proc comm is limited to 15 bytes and may
    // expose it with or without ".exe". Ask for both forms.
    static readonly string[] WindowsProcessNames = ["helldivers2", "EasyAntiCheat_EOS", "EasyAntiCheat", "start_protected_game"];
    static readonly string[] LinuxProcessNames =
        ["helldivers2.exe", "helldivers2", "EasyAntiCheat_EOS.exe", "EasyAntiCheat_EOS", "EasyAntiCheat", "start_protected_game.exe"];
    public static IReadOnlyList<string> ProcessNames => IsWindows ? WindowsProcessNames : LinuxProcessNames;

    public static IReadOnlyList<Process> FindProcesses(IEnumerable<string> names)
    {
        var found = new Dictionary<int, Process>();
        foreach (var name in names.Distinct(StringComparer.Ordinal))
            foreach (var process in Process.GetProcessesByName(name))
                if (!found.TryAdd(process.Id, process)) process.Dispose();
        return [.. found.Values];
    }

    public static bool IsProcessRunning(string name)
    {
        var found = Process.GetProcessesByName(name);
        foreach (var process in found) process.Dispose();
        return found.Length > 0;
    }

    // The sync folder and everything the proxy writes there stay private to the player. Windows keeps the icacls call in
    // Program.cs; Linux gets the same guarantee from the directory and file modes.
    public static void RestrictDirectory(string path)
    {
        if (!OperatingSystem.IsLinux()) return;
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    }

    public static void RestrictFile(string path)
    {
        if (!OperatingSystem.IsLinux() || !File.Exists(path)) return;
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
    }

    public static void FlushDns()
    {
        if (IsWindows) { Run("ipconfig", "/flushdns"); return; }
        // Wine's lookups end in the host resolver. systemd-resolved is the only common cache on the supported systems;
        // a missing or different resolver needs no action because /etc/hosts is read per lookup.
        foreach (var resolvectl in new[] { "/usr/bin/resolvectl", "/bin/resolvectl" })
            if (File.Exists(resolvectl)) { Run(resolvectl, "flush-caches"); return; }
    }

    static void Run(string file, string arguments)
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo(file, arguments) { UseShellExecute = false, CreateNoWindow = true });
            process?.WaitForExit(8000);
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            // A DNS cache flush is best effort; the hosts file itself is what the next lookup reads.
        }
    }
}
