using HellForge.Core;

namespace HellForge.Cli;

// F.4d's native commands live here so E.5 can add steam-bridge to the same program without another switch in Program.cs.
static class SandboxCommands
{
    static readonly string[] SuperviseOptions = ["--install", "--prefix", "--session-file", "--parent-pid"];
    static readonly string[] CopyBackOptions = ["--install", "--run", "--profile"];

    // True when argv[0] is one of these commands; the caller returns exitCode without entering Program's other commands.
    public static bool TryRun(string[] args, out int exitCode)
    {
        exitCode = 0;
        if (args.Length == 0 || args[0] is not ("sandbox-supervise" or "sandbox-copy-back")) return false;
        try
        {
            if (!OperatingSystem.IsLinux() || LinuxNetworkBlock.UnderWine)
                throw new InvalidDataException("The Linux sandbox commands must run on native Linux.");
            if (args[0] == "sandbox-supervise")
            {
                var options = Parse(args, SuperviseOptions);
                exitCode = SandboxSupervisor.Run(Required(options, "--install"), Required(options, "--prefix"),
                    Required(options, "--session-file"), Positive(options, "--parent-pid"));
            }
            else
            {
                var options = Parse(args, CopyBackOptions);
                var install = new InstallLayout(Required(options, "--install"));
                if (!install.Exists) throw new InvalidDataException(install.Root + " is not a HellForge installation.");
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
