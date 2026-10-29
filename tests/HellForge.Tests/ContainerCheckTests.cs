using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;

// Area J.1: linux/hf-container.sh refuses Flatpak, Snap and Steam's runtime container for every wrapper mode, and lets
// other containers through with a note when a namespace step fails. The tests source the helper directly; the optional
// arguments they pass are not an environment hook, so a real run always reads the real paths and environment.
static class ContainerCheckTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The Linux container check refuses Flatpak, Snap and Steam's runtime container with the action to take", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux container check)"); return; }
            var sandbox = Path.Combine(root, "container-check-refusals");
            Directory.CreateDirectory(sandbox);
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-container.sh");
            check(File.Exists(helper), "linux/hf-container.sh was not found: " + helper);

            var flatpak = Path.Combine(sandbox, ".flatpak-info");
            File.WriteAllText(flatpak, "[Application]\nname=org.example.HellForge\n[Environment]\nname=not-this\n");
            var flatpakKind = RunBash(null, "source \"$1\"; hf_container_kind \"$2\" \"\" \"\" \"\"", helper, flatpak);
            check(flatpakKind.ExitCode == 0 && flatpakKind.Output.Trim() == "flatpak\torg.example.HellForge",
                "Flatpak was not detected by its [Application] name: " + flatpakKind.Output);
            var flatpakProblem = RunBash(null, "source \"$1\"; hf_container_problem \"$2\" \"\" \"\" \"\"", helper, flatpak);
            check(flatpakProblem.ExitCode == 0 && flatpakProblem.Output.Contains("Flatpak application \"org.example.HellForge\"", StringComparison.Ordinal) &&
                flatpakProblem.Output.Contains("network namespace", StringComparison.Ordinal) &&
                flatpakProblem.Output.Contains("app/launcher-linux/HellForge.Launcher", StringComparison.Ordinal) &&
                flatpakProblem.Output.Contains("Flatpak's Steam folder", StringComparison.Ordinal) &&
                !flatpakProblem.Output.Contains("flatpak-spawn", StringComparison.Ordinal), "The Flatpak refusal is incomplete: " + flatpakProblem.Output);

            var snapKind = RunBash(null, "source \"$1\"; hf_container_kind \"\" \"\" \"\" \"hellforge-snap\"", helper);
            check(snapKind.ExitCode == 0 && snapKind.Output.Trim() == "snap\thellforge-snap", "Snap was not detected from SNAP_NAME: " + snapKind.Output);
            var snapProblem = RunBash(null, "source \"$1\"; hf_container_problem \"\" \"\" \"\" \"hellforge-snap\"", helper);
            check(snapProblem.ExitCode == 0 && snapProblem.Output.Contains("Snap \"hellforge-snap\"", StringComparison.Ordinal) &&
                snapProblem.Output.Contains("app/launcher-linux/HellForge.Launcher", StringComparison.Ordinal) &&
                snapProblem.Output.Contains("Snap's Steam folder", StringComparison.Ordinal) &&
                !snapProblem.Output.Contains("flatpak-spawn", StringComparison.Ordinal), "The Snap refusal is incomplete: " + snapProblem.Output);

            var pressureKind = RunBash(null, "source \"$1\"; hf_container_kind \"\" \"\" pressure-vessel \"\"", helper);
            check(pressureKind.ExitCode == 0 && pressureKind.Output.Trim() == "pressure-vessel\tpressure-vessel",
                "container=pressure-vessel was not detected: " + pressureKind.Output);
            var manager = Path.Combine(sandbox, "container-manager");
            File.WriteAllText(manager, "pressure-vessel\n");
            var managerKind = RunBash(null, "source \"$1\"; hf_container_kind \"\" \"$2\" \"\" \"\"", helper, manager);
            check(managerKind.ExitCode == 0 && managerKind.Output.Trim() == "pressure-vessel\tpressure-vessel",
                "The container-manager file was not detected: " + managerKind.Output);
            var pressureProblem = RunBash(null, "source \"$1\"; hf_container_problem \"\" \"\" pressure-vessel \"\"", helper);
            check(pressureProblem.ExitCode == 0 && pressureProblem.Output.Contains("Steam's runtime container (pressure-vessel)", StringComparison.Ordinal) &&
                pressureProblem.Output.Contains("Properties > Compatibility", StringComparison.Ordinal) &&
                pressureProblem.Output.Contains("Force the use of a specific Steam Play compatibility tool", StringComparison.Ordinal) &&
                pressureProblem.Output.Contains("native program", StringComparison.Ordinal) &&
                !pressureProblem.Output.Contains("flatpak-spawn", StringComparison.Ordinal), "The pressure-vessel refusal is incomplete: " + pressureProblem.Output);
        });

        test("The Linux container check lets other containers and a plain host through, and names them only on a namespace failure", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux container check)"); return; }
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-container.sh");
            check(File.Exists(helper), "linux/hf-container.sh was not found: " + helper);
            foreach (var name in new[] { "podman", "docker", "oci", "toolbox", "distrobox" })
            {
                var kind = RunBash(null, "source \"$1\"; hf_container_kind \"\" \"\" \"$2\" \"\"", helper, name);
                check(kind.ExitCode == 0 && kind.Output.Trim() == "other\t" + name, name + " was not recognised as another container: " + kind.Output);
                var problem = RunBash(null, "source \"$1\"; hf_container_problem \"\" \"\" \"$2\" \"\"", helper, name);
                check(problem.ExitCode == 1 && problem.Output.Trim().Length == 0, name + " was refused, though toolbox and distrobox can have user namespaces.");
                var note = RunBash(null, "source \"$1\"; hf_container_note \"\" \"\" \"$2\" \"\"", helper, name);
                check(note.ExitCode == 0 && note.Output.Contains("container \"" + name + "\"", StringComparison.Ordinal),
                    "A namespace failure inside " + name + " did not name the container: " + note.Output);
            }
            var hostKind = RunBash(null, "source \"$1\"; hf_container_kind \"\" \"\" \"\" \"\"", helper);
            var hostProblem = RunBash(null, "source \"$1\"; hf_container_problem \"\" \"\" \"\" \"\"", helper);
            var hostNote = RunBash(null, "source \"$1\"; hf_container_note \"\" \"\" \"\" \"\"", helper);
            check(hostKind.ExitCode == 1 && hostKind.Output.Trim().Length == 0, "A plain host was reported as a container: " + hostKind.Output);
            check(hostProblem.ExitCode == 1 && hostProblem.Output.Trim().Length == 0, "A plain host was refused: " + hostProblem.Output);
            check(hostNote.ExitCode == 1 && hostNote.Output.Trim().Length == 0, "A plain host got a container note: " + hostNote.Output);
        });

        test("The wrapper writes its container refusal where the launcher and the sandbox probe report read it", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux container refusal record)"); return; }
            var sandbox = Path.Combine(root, "container-check-record");
            var install = Path.Combine(sandbox, "install");
            Directory.CreateDirectory(install);
            File.WriteAllText(Path.Combine(install, "hellforge-install.json"), "{}");
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-container.sh");
            var wrapper = Path.Combine(RepositoryRoot(), "linux", "hellforge-linux.sh");
            check(File.Exists(helper) && File.Exists(wrapper), "The Linux wrapper or its container helper was not found.");
            var environment = new Dictionary<string, string?>
            {
                ["INSTALL"] = install,
                ["container"] = "pressure-vessel",
                ["SNAP_NAME"] = null,
                ["HELLFORGE_WRAPPER_LOG"] = null
            };
            var reason = RunBash(environment, "source \"$1\"; hf_container_problem", helper);
            check(reason.ExitCode == 0 && reason.Output.Trim().Length > 0, "The container check found nothing in a pressure-vessel environment: " + reason.Error);
            var message = reason.Output.Trim();
            var modes = new[]
            {
                (Name: "--sandbox-probe", Command: "bash \"$1\" --sandbox-probe"),
                (Name: "--sandbox-self-test", Command: "bash \"$1\" --sandbox-self-test"),
                (Name: "a session", Command: "bash \"$1\" HellForge.Session.exe play")
            };
            foreach (var mode in modes)
            {
                var run = RunBash(environment, mode.Command, wrapper);
                check(run.ExitCode == 1, $"{mode.Name} did not refuse inside pressure-vessel (exit code {run.ExitCode}): {run.Error}");
                check(run.Error.Contains(message, StringComparison.Ordinal), $"{mode.Name} printed another reason: {run.Error}");
                check(SessionRunner.LinuxContainerRefusal(install) == message,
                    $"{mode.Name} did not write the launcher's refusal record.");
            }

            var written = Path.Combine(install, "logs", "linux-container-refusal.txt");
            check(File.Exists(written) && File.ReadAllText(written).Trim() == message,
                "The launcher's refusal record does not carry the wrapper's reason.");
            check(SessionRunner.LinuxContainerRefusal(install) == message,
                "SessionRunner did not read the refusal record the launcher shows.");
            using var report = JsonDocument.Parse(File.ReadAllText(Path.Combine(install, "logs", "sandbox-probe.json")));
            check(report.RootElement.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False &&
                report.RootElement.TryGetProperty("firstReason", out var firstReason) && firstReason.ValueKind == JsonValueKind.String &&
                firstReason.GetString() == message, "The sandbox probe readiness record does not carry the same reason.");
        });
    }

    static (int ExitCode, string Output, string Error) RunBash(IReadOnlyDictionary<string, string?>? environment,
        string command, params string[] arguments)
    {
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        if (environment != null)
            foreach (var entry in environment)
            {
                if (entry.Value == null) start.Environment.Remove(entry.Key);
                else start.Environment[entry.Key] = entry.Value;
            }
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add(command);
        start.ArgumentList.Add("bash");
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new InvalidDataException("bash did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        return (process.ExitCode, output.GetAwaiter().GetResult(), error.GetAwaiter().GetResult());
    }

    static string RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        return Environment.CurrentDirectory;
    }
}
