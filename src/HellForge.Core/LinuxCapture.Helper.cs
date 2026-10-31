using System.Diagnostics;

namespace HellForge.Core;

// Installing and removing the root-owned Account Sync helper without a terminal: the launcher shows one confirmation,
// then runs the shipped allow-capture.sh through pkexec, so the desktop's own polkit dialog asks for the password.
// The launcher never keeps root or a password, and only that absolute, link-checked shipped script is elevated.
public static partial class LinuxCapture
{
    // Absolute, so the password dialog always comes from the system pkexec, never from a program earlier on PATH.
    public const string PkexecPath = "/usr/bin/pkexec";
    public const string BashPath = "/usr/bin/bash";
    public const string SudoersPath = "/etc/sudoers.d/hellforge-capture";
    static readonly TimeSpan HelperInstallTimeout = TimeSpan.FromMinutes(5);

    // Succeeded: the installer/remover exited 0 and the post-check agreed. Cancelled: the polkit dialog was dismissed or
    // denied, so nothing changed. NeedsManual: the launcher could not show a dialog; Message carries the terminal command.
    public sealed record HelperSetupResult(bool Succeeded, bool Cancelled, bool NeedsManual, string Message);

    public static string HelperInstallDescription(string installRoot)
    {
        var installer = Path.Combine(installRoot, "app", "linux", HelperInstaller);
        return "Sync Inventory needs a small root-owned helper and relay to change /etc/hosts and serve the capture on " +
            "this PC. Installing them copies capture-helper.sh and capture-relay to /usr/local/lib/hellforge, records " +
            "their SHA-256, and adds the one sudoers rule /etc/sudoers.d/hellforge-capture so this account can run only " +
            "that helper's check, start, stop and cleanup commands without another password. The shipped installer " +
            installer + " runs through pkexec after you continue, so the desktop's polkit dialog asks for your password. " +
            "Remove them later with: sudo bash " + installer + " --remove";
    }

    public static bool HelperInstalled() =>
        File.Exists(HelperPath) || File.Exists(RelayPath) || File.Exists(HelperHashPath) ||
        File.Exists(RelayHashPath) || File.Exists(SudoersPath);

    public static HelperSetupResult InstallHelper(string installRoot, string pkexecPath = PkexecPath,
        string bashPath = BashPath) =>
        RunHelperInstaller(installRoot, remove: false, pkexecPath, bashPath, HelperProblem);

    internal static HelperSetupResult InstallHelper(string installRoot, string pkexecPath, string bashPath,
        Func<string, string?>? verify) =>
        RunHelperInstaller(installRoot, remove: false, pkexecPath, bashPath, verify);

    public static HelperSetupResult RemoveHelper(string installRoot, string pkexecPath = PkexecPath,
        string bashPath = BashPath) =>
        RunHelperInstaller(installRoot, remove: true, pkexecPath, bashPath,
            _ => HelperInstalled() ? "the helper, relay, hashes or sudoers rule is still present" : null);

    internal static HelperSetupResult RemoveHelper(string installRoot, string pkexecPath, string bashPath,
        Func<string, string?>? verify) =>
        RunHelperInstaller(installRoot, remove: true, pkexecPath, bashPath, verify);

    static HelperSetupResult RunHelperInstaller(string installRoot, bool remove, string pkexecPath, string bashPath,
        Func<string, string?>? verify)
    {
        string installer;
        try
        {
            installer = LocalInstall.NoLinks(Path.Combine(installRoot, "app", "linux", HelperInstaller), installRoot);
            installer = LocalInstall.FullPath(installer);
            if (!LocalInstall.Within(installer, installRoot))
                return Fallback(remove, installRoot, "The capture installer is outside this installation.");
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException or
            UnauthorizedAccessException or InvalidDataException)
        {
            return Fallback(remove, installRoot, "The capture installer could not be checked: " + error.Message);
        }
        if (!File.Exists(installer))
            return Fallback(remove, installRoot, "This installation's " + HelperInstaller + " is missing. Reinstall HellForge.");

        var start = new ProcessStartInfo(pkexecPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add(bashPath);
        start.ArgumentList.Add(installer);
        if (remove) start.ArgumentList.Add("--remove");
        try
        {
            using var process = Process.Start(start);
            if (process == null) return Fallback(remove, installRoot, "pkexec did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit((int)HelperInstallTimeout.TotalMilliseconds))
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                return Fallback(remove, installRoot, "pkexec did not finish within five minutes.");
            }
            var stdout = output.GetAwaiter().GetResult().Trim();
            var stderr = error.GetAwaiter().GetResult().Trim();
            return process.ExitCode switch
            {
                0 => VerifyHelperSetup(installRoot, remove, verify, stdout),
                126 => Cancelled(remove),
                127 when NoAuthenticationAgent(stderr) => Fallback(remove, installRoot,
                    "No polkit authentication agent is running (for example in Game Mode or a bare window manager), so the " +
                    "launcher cannot show the password dialog."),
                127 => Cancelled(remove, " The password was not accepted or the dialog was denied."),
                _ => Fallback(remove, installRoot, "pkexec exited with code " + process.ExitCode + ": " +
                    Tail(string.Join(Environment.NewLine, new[] { stderr, stdout }.Where(text => text.Length != 0))))
            };
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or FileNotFoundException)
        {
            return Fallback(remove, installRoot,
                "pkexec is not installed, so the launcher cannot show the desktop password dialog.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            return Fallback(remove, installRoot, "pkexec could not run: " + error.Message);
        }
    }

    static HelperSetupResult VerifyHelperSetup(string installRoot, bool remove, Func<string, string?>? verify, string stdout)
    {
        if (verify != null && verify(installRoot) is { } problem)
            return Fallback(remove, installRoot, "The helper installer finished, but " + problem + ".");
        return new(true, false, false, remove
            ? "Removed HellForge's Account Sync helper, relay, hashes and sudoers rule."
            : "Installed the Account Sync helper and relay; the sync continues." + (stdout.Length == 0 ? "" : " " + Tail(stdout)));
    }

    static HelperSetupResult Cancelled(bool remove, string detail = "") =>
        new(false, true, false, HelperAction(remove) + detail + (remove ? "" : SteamOsPasswordHint()));

    static HelperSetupResult Fallback(bool remove, string installRoot, string reason) =>
        new(false, false, true, reason + " " + (remove ? "Remove it from a terminal: " : "Install it from a terminal: ") +
            HelperInstallCommand(installRoot) + (remove ? "" : SteamOsPasswordHint()));

    static string HelperAction(bool remove) => remove
        ? "The Account Sync helper was not removed; nothing changed."
        : "The capture helper was not installed; nothing changed.";

    static bool NoAuthenticationAgent(string stderr) =>
        stderr.Contains("no authentication agent", StringComparison.OrdinalIgnoreCase) ||
        stderr.Contains("no polkit agent", StringComparison.OrdinalIgnoreCase) ||
        stderr.Contains("authentication agent is not available", StringComparison.OrdinalIgnoreCase);

    internal static string SteamOsPasswordHint(bool steamOs) => steamOs
        ? " SteamOS 3.x may have no password set: run passwd first, then sync again."
        : "";

    static string SteamOsPasswordHint() => SteamOsPasswordHint(IsSteamOs());

    public static bool IsSteamOs(string osReleasePath = "/etc/os-release")
    {
        try
        {
            return File.Exists(osReleasePath) && File.ReadLines(osReleasePath).Any(line =>
            {
                var text = line.Trim();
                return text == "ID=steamos" || text == "ID=\"steamos\"";
            });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }
}
