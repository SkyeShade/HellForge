using System.Diagnostics;
using System.Globalization;
using HellForge.Core;

// The kept versions (KeptVersions): list them, keep the installed app before an update puts a new one in, and switch to
// one. A switch moves app/, this program's own folder, so it runs from a copy in temp/ that waits for this process (and
// --wait-pid's) to end; its outcome goes to logs/version-switch.json.
static class VersionsCommand
{
    public static object List(Dictionary<string, string> options)
    {
        var install = Launch.Install(options);
        var current = Directory.Exists(install.App) ? KeptVersions.Identify(install.App) : null;
        return new
        {
            current = current == null ? null : new { name = current.Name, version = current.Version, commit = current.Commit, engine = current.Engine, gameBuild = current.GameBuild },
            kept = KeptVersions.List(install).Select(k => new
            {
                name = k.Name, version = k.Version, commit = k.Commit, engine = k.Engine, gameBuild = k.GameBuild, keptUtc = k.KeptUtc, data = k.Data,
                newer = current != null && k.Identity.IsNewerThan(current),
                problem = KeptVersions.Problem(install, k), gameBuildNote = KeptVersions.GameBuildNote(install, k)
            }),
            keeps = KeptVersions.Count,
            log = KeptVersions.LogFile(install)
        };
    }

    public static object Keep(Dictionary<string, string> options, string newApp)
    {
        var install = Launch.Install(options);
        if (!File.Exists(Path.Combine(newApp, "app-build.json"))) throw new InvalidDataException($"{newApp} is not a HellForge app folder (no app-build.json).");
        var result = KeptVersions.KeepCurrent(install, Path.GetFullPath(newApp));
        return new { kept = result.Kept, name = result.Name, message = result.Message, dropped = result.Dropped };
    }

    public static int Switch(Dictionary<string, string> options, string to)
    {
        var install = Launch.Install(options);
        if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine)
            throw new InvalidDataException("Switching to a kept version works on Windows installations for now.");
        var waitFor = (options.GetValueOrDefault("--wait-pid") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries)
            .Select(p => int.TryParse(p, NumberStyles.None, CultureInfo.InvariantCulture, out var pid) ? pid : throw new ArgumentException("--wait-pid takes process ids.")).ToList();
        var restart = options.GetValueOrDefault("--restart") switch { null or "no" => false, "yes" => true, _ => throw new ArgumentException("--restart takes yes or no.") };
        // the copy reports every outcome in its result file, and starts the launcher again whatever happened
        if (options.ContainsKey("--detached")) return Run(install, to, waitFor, restart);
        var target = KeptVersions.Find(install, to);
        if (KeptVersions.Problem(install, target) is { } problem) throw new InvalidDataException(problem);
        var self = AppContext.BaseDirectory;
        if (LocalInstall.Within(self, install.Root) && !KeptVersions.IsSwitcherCopy(install, self))
        {
            if (KeptVersions.Blockers(install, waitFor) is { Count: > 0 } running)
                throw new InvalidDataException("Close these first: " + string.Join(", ", running) + ".");
            var copy = KeptVersions.SwitcherCopy(install, self);
            var start = new ProcessStartInfo(Path.Combine(copy, "HellForge.Cli.exe")) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = copy };
            foreach (var arg in new[] { "versions-switch", "--install", install.Root, "--to", target.Name, "--wait-pid",
                string.Join(',', waitFor.Append(Environment.ProcessId)), "--restart", restart ? "yes" : "no", "--detached", "yes" })
                start.ArgumentList.Add(arg);
            using var started = Process.Start(start) ?? throw new IOException("The switch could not start.");
            Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(new
            {
                status = "started", from = KeptVersions.Identify(install.App).Text, to = target.Text, result = KeptVersions.ResultFile(install),
                note = "The switch runs once this program and the --wait-pid processes have ended."
            }, CopyPreparer.JsonOptions));
            return 0;
        }
        return Run(install, target.Name, waitFor, restart);
    }

    static int Run(InstallLayout install, string to, List<int> waitFor, bool restart)
    {
        var from = Directory.Exists(install.App) ? KeptVersions.Identify(install.App).Text : null;
        KeptVersions.SwitchOutcome outcome;
        try
        {
            foreach (var pid in waitFor) WaitForExit(pid);
            var done = KeptVersions.Switch(install, to);
            outcome = new("done", done.From, done.To, $"HellForge {done.To} is installed; {done.From} is kept." + (done.GameBuildNote is { } note ? " " + note : "") +
                (done.SwitchedOff.Count > 0 ? " Mods added since it last ran are off in its list: " + string.Join(", ", done.SwitchedOff) + "." : ""), DateTime.UtcNow);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or TimeoutException)
        {
            KeptVersions.Log(install, $"Going to {to} stopped: {error.Message}");
            outcome = new("failed", from, to, error.Message, DateTime.UtcNow);
        }
        KeptVersions.WriteResult(install, outcome);
        Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(outcome, CopyPreparer.JsonOptions));
        if (restart)
        {
            var starter = Path.Combine(install.Root, "HellForge.exe");
            try { Process.Start(new ProcessStartInfo(starter) { UseShellExecute = true, WorkingDirectory = install.Root })?.Dispose(); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                KeptVersions.Log(install, "The launcher could not be started again: " + error.Message);
            }
        }
        return outcome.Status == "done" ? 0 : 1;
    }

    static void WaitForExit(int pid)
    {
        Process process;
        try { process = Process.GetProcessById(pid); }
        catch (ArgumentException) { return; } /* gone already */
        using (process)
            if (!process.WaitForExit(TimeSpan.FromSeconds(90)))
                throw new TimeoutException($"{process.ProcessName} ({pid}) did not close within 90 seconds; nothing was switched.");
    }
}
