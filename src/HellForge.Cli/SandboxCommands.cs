using System.Diagnostics;
using HellForge.Core;

namespace HellForge.Cli;

// F.4d's native commands live here so E.5 can add steam-bridge to the same program without another switch in Program.cs.
static class SandboxCommands
{
    static readonly string[] SuperviseOptions = ["--install", "--prefix", "--session-file", "--parent-pid"];
    static readonly string[] CopyBackOptions = ["--install", "--run", "--profile"];
    static readonly string[] ReadinessOptions = ["--install"];

    // True when argv[0] is one of these commands; the caller returns exitCode without entering Program's other commands.
    public static bool TryRun(string[] args, out int exitCode)
    {
        exitCode = 0;
        if (args.Length == 0 || args[0] is not ("sandbox-supervise" or "sandbox-copy-back" or "sandbox-readiness" or "sandbox-prefix-case")) return false;
        try
        {
            if (!OperatingSystem.IsLinux() || LinuxNetworkBlock.UnderWine)
                throw new InvalidDataException("The Linux sandbox commands must run on native Linux.");
            if (args[0] == "sandbox-readiness")
            {
                exitCode = RunReadiness(ExistingInstall(Parse(args, ReadinessOptions)));
            }
            else if (args[0] == "sandbox-prefix-case")
            {
                var problem = PrefixCaseProblem(ExistingInstall(Parse(args, ReadinessOptions)));
                if (problem != null) Console.Error.WriteLine("HellForge: " + problem);
                exitCode = problem == null ? 0 : 1;
            }
            else if (args[0] == "sandbox-supervise")
            {
                var options = Parse(args, SuperviseOptions);
                exitCode = SandboxSupervisor.Run(Required(options, "--install"), Required(options, "--prefix"),
                    Required(options, "--session-file"), Positive(options, "--parent-pid"));
            }
            else
            {
                var options = Parse(args, CopyBackOptions);
                var install = ExistingInstall(options);
                var receipt = NativeSandboxCopyBack.Run(install, Required(options, "--run"), Required(options, "--profile"));
                Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(receipt, SandboxJson.Default.SandboxCopyBackReceipt));
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or
                                         InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Console.Error.WriteLine("HellForge: " + error.Message);
            exitCode = 1;
        }
        return true;
    }

    static InstallLayout ExistingInstall(Dictionary<string, string> options)
    {
        var install = new InstallLayout(Required(options, "--install"));
        if (!install.Exists) throw new InvalidDataException(install.Root + " is not a HellForge installation.");
        return install;
    }

    // Run by hellforge-linux.sh before a session enters the namespace. A current pass is reused; a missing or stale
    // record runs the same self-test the launcher runs, and records the installed app's build string and this kernel.
    static int RunReadiness(InstallLayout install)
    {
        string appBuild;
        try { appBuild = LinuxSandboxReadiness.InstalledAppBuild(install); }
        catch (InvalidDataException error)
        {
            // Keep a reason for the session even when the package's build file is too old to identify it; the failure
            // record uses the CLI's own build only in this error path, never for a pass.
            LinuxSandboxReadiness.Save(install, false, error.Message,
                mode: LinuxSandboxReadiness.ExpectedMode(install));
            Console.Error.WriteLine("HellForge: " + error.Message);
            return 1;
        }
        var kernel = LinuxSandboxReadiness.KernelRelease();
        var expected = LinuxSandboxReadiness.ExpectedMode(install);
        if (PrefixCaseProblem(install) is { } caseProblem)
        {
            LinuxSandboxReadiness.Save(install, false, caseProblem, appBuild: appBuild, kernelRelease: kernel, mode: expected);
            Console.Error.WriteLine("HellForge: " + caseProblem);
            return 1;
        }
        var current = LinuxSandboxReadiness.Read(install);
        if (LinuxSandboxReadiness.IsCurrent(current, appBuild, kernel, expected.Mode, expected.RuntimeIdentity))
        {
            var problem = LinuxSandboxReadiness.Problem(current, expected, appBuild, kernel);
            if (problem == null) return 0;
            Console.Error.WriteLine("HellForge: " + problem);
            return 1;
        }
        try { File.Delete(LinuxSandboxReadiness.ModeFileFor(install)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var (passed, reason) = RunSelfTest(install);
        var (mode, recorded, recordedReason) = LinuxSandboxReadiness.EvaluateSelfTest(install, passed ? 0 : 1, reason);
        LinuxSandboxReadiness.Save(install, recorded, recordedReason, appBuild: appBuild, kernelRelease: kernel, mode: mode);
        if (recorded) return 0;
        Console.Error.WriteLine("HellForge: " + recordedReason);
        return 1;
    }

    // The sandbox's overlay cannot use a prefix whose drive_c is case-insensitive (LinuxPrefixCase). HellForge's own
    // prefix, the one holding this installation, is made case-sensitive here; another prefix is only reported.
    static string? PrefixCaseProblem(InstallLayout install)
    {
        var own = LinuxPrefixCase.OwnPrefix(install) is { } found ? LinuxPrefixCase.Normalize(found) : null;
        var configured = Environment.GetEnvironmentVariable("STEAM_COMPAT_DATA_PATH");
        var prefix = string.IsNullOrWhiteSpace(configured) ? own : LinuxPrefixCase.Normalize(configured);
        if (prefix == null || !LinuxPrefixCase.Needed(prefix)) return null;
        if (own == null || own != prefix)
            return "the sandbox's prefix (" + prefix + ") has a case-insensitive drive_c, which the kernel's overlay cannot sandbox; HellForge converts only its own prefix";
        var (converted, problem) = LinuxPrefixCase.Convert(prefix, () => LinuxPrefixCase.WineRunning(prefix));
        if (problem != null) return "the prefix's drive_c is case-insensitive and could not be made case-sensitive yet: " + problem;
        Console.Error.WriteLine("HellForge: made the prefix's drive_c case-sensitive for the sandbox (" + converted + " folders).");
        return null;
    }

    static (bool Passed, string Reason) RunSelfTest(InstallLayout install)
    {
        var wrapper = Path.Combine(install.App, "linux", "hellforge-linux.sh");
        if (!File.Exists(wrapper)) return (false, "app/linux/hellforge-linux.sh is missing");
        var start = new ProcessStartInfo("bash")
        {
            WorkingDirectory = install.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add(wrapper);
        start.ArgumentList.Add("--sandbox-self-test");
        start.Environment["INSTALL"] = install.Root;
        start.Environment.Remove("HELLFORGE_WRAPPER_LOG");
        Process process;
        try { process = Process.Start(start) ?? throw new IOException("The Linux sandbox self-test did not start."); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or
                                         System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return (false, "the Linux sandbox self-test could not run: " + error.Message);
        }
        using (process)
        {
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(180_000))
            {
                try { process.Kill(entireProcessTree: true); }
                catch (Exception error) when (error is InvalidOperationException or NotSupportedException or
                                                 System.ComponentModel.Win32Exception) { }
                return (false, "the Linux sandbox self-test did not finish in 3 minutes");
            }
            var output = errorTask.GetAwaiter().GetResult() + "\n" + outputTask.GetAwaiter().GetResult();
            try
            {
                Directory.CreateDirectory(install.Logs);
                File.WriteAllText(Path.Combine(install.Logs, "sandbox-self-test.txt"), output);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            var passed = process.ExitCode == 0;
            return passed ? (true, "") : (false, LinuxSandboxReadiness.SelfTestReason(output, process.ExitCode));
        }
    }

    static Dictionary<string, string> Parse(string[] args, IReadOnlyCollection<string> allowed)
    {
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 1; i < args.Length; i += 2)
        {
            if (!allowed.Contains(args[i]) || i + 1 >= args.Length || !options.TryAdd(args[i], args[i + 1]))
                throw new ArgumentException("Unknown, duplicate, or incomplete option: " + args[i]);
        }
        return options;
    }

    static string Required(Dictionary<string, string> options, string name) =>
        options.TryGetValue(name, out var value) && !string.IsNullOrWhiteSpace(value)
            ? value : throw new ArgumentException("Missing " + name + ".");

    static int Positive(Dictionary<string, string> options, string name) =>
        int.TryParse(Required(options, name), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var value) && value > 0
            ? value : throw new ArgumentException(name + " must be a positive process id.");
}
