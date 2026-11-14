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
    // sudo reads the drop-ins in lexical order and the last matching rule wins: zz- sorts after a distribution's own
    // files, such as SteamOS's "sudo" and "wheel", which ask for a password.
    public const string SudoersPath = "/etc/sudoers.d/zz-hellforge-capture";
    // The rule's name before it sorted last. The installer removes it; a removal removes both.
    public const string LegacySudoersPath = "/etc/sudoers.d/hellforge-capture";
    // SteamOS drops /etc changes at an update unless a file here lists them; the installer lists the rule (SteamOS only).
    public const string SteamOsKeepListPath = "/etc/atomic-update.conf.d/hellforge.conf";
    static readonly TimeSpan HelperInstallTimeout = TimeSpan.FromMinutes(5);

    // Succeeded: the installer/remover exited 0 and the post-check agreed. Cancelled: the polkit dialog was dismissed or
    // denied, so nothing changed. NeedsManual: the launcher could not show a dialog; Message carries the terminal command.
    public sealed record HelperSetupResult(bool Succeeded, bool Cancelled, bool NeedsManual, string Message);

    // The installer's order on this system, as allow-capture.sh has it.
    internal static IReadOnlyList<string> InstallFolders(bool steamOs) => steamOs ? [.. HelperFolders.Skip(1)] : HelperFolders;

    // The folder the installed helper is in: the first in the installer's order that has one, then any other (a copy an
    // earlier install left where it can no longer be removed is passed over). With no helper, the first choice.
    internal static string HelperFolderOf(bool steamOs, Func<string, bool> exists)
    {
        var order = InstallFolders(steamOs).Concat(HelperFolders).Distinct().ToArray();
        // Joined with "/", not Path.Combine: these are a Linux system's paths whatever system reads this.
        return order.FirstOrDefault(folder => exists(folder + "/" + HelperName)) ?? order[0];
    }

    public static string HelperInstallDescription(string installRoot) => HelperInstallDescription(installRoot, IsSteamOs());

    internal static string HelperInstallDescription(string installRoot, bool steamOs)
    {
        var installer = Path.Combine(installRoot, "app", "linux", HelperInstaller);
        var folders = InstallFolders(steamOs);
        return "Sync Inventory needs a small root-owned helper and relay to change /etc/hosts and serve the capture on " +
            "this PC. Installing them copies capture-helper.sh and capture-relay to " + folders[0] + " (to " +
            string.Join(" or ", folders.Skip(1)) + " if that folder cannot be used), records " +
            "their SHA-256, and adds the one sudoers rule " + SudoersPath + " so this account can run only " +
            "that helper's check, start, stop and cleanup commands without another password. " +
            (steamOs ? "SteamOS's /usr/local is read-only, so it is not used; " + SteamOsKeepListPath + " lists the " +
                "rule so that a SteamOS update keeps it. " : "") + "The shipped installer " +
            installer + " runs through pkexec after you continue, so the desktop's polkit dialog asks for your password. " +
            "Remove them later with: sudo bash " + installer + " --remove";
    }

    public static bool HelperInstalled() => HelperInstalled(File.Exists);

    internal static bool HelperInstalled(Func<string, bool> exists) =>
        HelperFolders.Any(folder => new[] { HelperName, HelperName + ".sha256", RelayName, RelayName + ".sha256", ProxyCopyName + ".sha256" }
            .Any(file => exists(folder + "/" + file))) ||
        exists(SudoersPath) || exists(LegacySudoersPath) || exists(SteamOsKeepListPath);

    public static HelperSetupResult InstallHelper(string installRoot, string pkexecPath = PkexecPath,
        string bashPath = BashPath) =>
        RunHelperInstaller(installRoot, remove: false, pkexecPath, bashPath, HelperProblem, HelperCheckProblem);

    internal static HelperSetupResult InstallHelper(string installRoot, string pkexecPath, string bashPath,
        Func<string, string?>? verify, Func<string?>? check = null) =>
        RunHelperInstaller(installRoot, remove: false, pkexecPath, bashPath, verify, check);

    public static HelperSetupResult RemoveHelper(string installRoot, string pkexecPath = PkexecPath,
        string bashPath = BashPath) =>
        RunHelperInstaller(installRoot, remove: true, pkexecPath, bashPath,
            _ => HelperInstalled() ? "the helper, relay, hashes or sudoers rule is still present" : null);

    internal static HelperSetupResult RemoveHelper(string installRoot, string pkexecPath, string bashPath,
        Func<string, string?>? verify) =>
        RunHelperInstaller(installRoot, remove: true, pkexecPath, bashPath, verify);

    // verify: the installed files against the shipped ones. check: sudo runs the installed helper without a password.
    static HelperSetupResult RunHelperInstaller(string installRoot, bool remove, string pkexecPath, string bashPath,
        Func<string, string?>? verify, Func<string?>? check = null)
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
                0 => VerifyHelperSetup(installRoot, remove, verify, check, stdout),
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

    static HelperSetupResult VerifyHelperSetup(string installRoot, bool remove, Func<string, string?>? verify,
        Func<string?>? check, string stdout)
    {
        if (verify != null && verify(installRoot) is { } problem)
            return Fallback(remove, installRoot, "The helper installer finished, but " + problem + ".");
        // The files are right, so running the installer again from a terminal would change nothing: say what blocks sudo.
        if (check != null && check() is { } blocked)
            return new(false, false, false, "The helper and its sudoers rule were installed, but the helper's check " +
                "still fails. " + blocked);
        return new(true, false, false, remove
            ? "Removed HellForge's Account Sync helper, relay, hashes and sudoers rule."
            : "Installed the Account Sync helper and relay; the sync continues." + (stdout.Length == 0 ? "" : " " + Tail(stdout)));
    }

    // Whether sudo runs the installed helper for this account without a password, by running its harmless check
    // command. The files can all be right while another sudoers rule that sorts later still asks for a password; the
    // launcher then offers the installer again, which writes the rule under the name that sorts last. Null means ready.
    public static string? HelperCheckProblem() => HelperCheckProblem(RunSudo(["check"], TimeSpan.FromSeconds(10)));

    // Whether running the installer again can clear a failed check: it writes the rule and both root copies, so it
    // answers sudo's password and the helper's own "run allow-capture.sh again". Anything else (a read-only /etc/hosts,
    // a missing program) would only bring the password dialog back at every sync.
    public static bool InstallFixesCheck(string problem) =>
        problem.Contains("asks for a password", StringComparison.Ordinal) ||
        problem.Contains("run allow-capture.sh again", StringComparison.Ordinal);

    internal static string? HelperCheckProblem(WineResult check)
    {
        if (check.ExitCode == 0) return null;
        // sudo -n never prompts; this is its message when no NOPASSWD rule is the last match (read with LC_ALL=C).
        if (check.AllText.Contains("a password is required", StringComparison.OrdinalIgnoreCase))
            return "sudo asks for a password to run HellForge's capture helper: its sudoers rule (" + SudoersPath +
                ") is missing, or a rule read after it (a later file in /etc/sudoers.d, or a line in /etc/sudoers below " +
                "its include) asks for one.";
        return "The Linux capture helper's check failed: " + Tail(check.AllText) + ".";
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
