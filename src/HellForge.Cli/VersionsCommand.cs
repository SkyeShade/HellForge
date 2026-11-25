using System.Diagnostics;
using System.Globalization;
using HellForge.Core;

// The kept versions (KeptVersions): list them, keep the installed app before an update puts a new one in, and switch the
// version sessions play with. A switch moves app/'s programs, this one's folder among them, so it runs from a copy in
// temp/ that waits for this process (and --wait-pid's) to end; its outcome goes to logs/version-switch.json, which the
// launcher shows when it opens.
static class VersionsCommand
{
    public static object List(Dictionary<string, string> options)
    {
        var install = Launch.Install(options);
        if (options.TryGetValue("--keep", out var keep))
            KeptVersions.SetKeepCount(install, int.TryParse(keep, NumberStyles.None, CultureInfo.InvariantCulture, out var count) ? count : throw new ArgumentException("--keep takes a number."));
        var current = Directory.Exists(install.App) ? KeptVersions.Installed(install) : null;
        var active = Directory.Exists(install.App) ? KeptVersions.Active(install) : null;
        return new
        {
            current = current == null ? null : new { name = current.Name, version = current.Version, commit = current.Commit, engine = current.Engine, gameBuild = current.GameBuild },
            playing = active == null ? null : new { name = active.Name, version = active.Version, commit = active.Commit, family = SessionRuntime.FamilyOf(active) },
            supported = SessionRuntime.Supported,
            kept = KeptVersions.List(install).Select(k => new
            {
                name = k.Name, version = k.Version, commit = k.Commit, engine = k.Engine, gameBuild = k.GameBuild, keptUtc = k.KeptUtc, data = k.Data,
                newer = current != null && k.Identity.IsNewerThan(current), playing = k.Name == active?.Name,
                problem = k.Name == active?.Name ? null : KeptVersions.RuntimeProblem(install, k), gameBuildNote = KeptVersions.GameBuildNote(install, k)
            }),
            keeps = KeptVersions.KeepCount(install),
            keptBytes = KeptVersions.KeptBytes(install),
            log = KeptVersions.LogFile(install)
        };
    }

    public static object Keep(Dictionary<string, string> options, string newApp)
    {
        var install = Launch.Install(options);
        if (!File.Exists(Path.Combine(newApp, "app-build.json"))) throw new InvalidDataException($"{newApp} is not a HellForge app folder (no app-build.json).");
        var fresh = options.GetValueOrDefault("--fresh-mods") switch { null or "yes" => true, "no" => false, _ => throw new ArgumentException("--fresh-mods takes yes or no.") };
        var result = KeptVersions.KeepCurrent(install, Path.GetFullPath(newApp), freshModList: fresh);
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
        var target = KeptVersions.SwitchTargets(install).FirstOrDefault(t => t.Name == to || t.Text == to)
            ?? throw new InvalidDataException($"No version {to} to switch to. These can be: " +
                (KeptVersions.SwitchTargets(install) is { Count: > 0 } all ? string.Join(", ", all.Select(t => t.Name)) : "none") + ".");
        var self = AppContext.BaseDirectory;
        if (LocalInstall.Within(self, install.Root) && !KeptVersions.IsSwitcherCopy(install, self))
        {
            if (KeptVersions.Blockers(install, waitFor) is { Count: > 0 } running)
                throw new InvalidDataException("Close these first: " + string.Join(", ", running) + ".");
            var copy = KeptVersions.SwitcherCopy(install, self);
            // Through the shell, which hands the copy none of this process's handles: an inherited output pipe would keep
            // whoever reads this program's output (the launcher, another launcher) waiting until the switch ends, and the
            // switch waits for them.
            var arguments = new[] { "versions-switch", "--install", install.Root, "--to", target.Name, "--wait-pid",
                string.Join(',', waitFor.Append(Environment.ProcessId)), "--restart", restart ? "yes" : "no", "--detached", "yes" };
            var start = new ProcessStartInfo(Path.Combine(copy, "HellForge.Cli.exe"), string.Join(' ', arguments.Select(Quote)))
            { UseShellExecute = true, WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = copy };
            using var started = Process.Start(start) ?? throw new IOException("The switch could not start.");
            Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(new
            {
                status = "started", from = KeptVersions.Active(install).Text, to = target.Text, result = KeptVersions.ResultFile(install),
                note = "The switch runs once this program and the --wait-pid processes have ended."
            }, CopyPreparer.JsonOptions));
            return 0;
        }
        return Run(install, target.Name, waitFor, restart);
    }

    static int Run(InstallLayout install, string to, List<int> waitFor, bool restart)
    {
        var from = Directory.Exists(install.App) ? KeptVersions.Active(install).Text : null;
        KeptVersions.SwitchOutcome outcome;
        try
        {
            var clock = Stopwatch.StartNew();
            foreach (var pid in waitFor) WaitForExit(pid);
            var waited = clock.Elapsed;
            var done = KeptVersions.SwitchRuntime(install, to);
            KeptVersions.Log(install, $"The switch took {(clock.Elapsed - waited).TotalSeconds:0.0} s, after {waited.TotalSeconds:0.0} s waiting for the launcher to close.");
            outcome = KeptVersions.Outcome(done);
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

    // One argument for the command line: quoted, with the backslashes before the closing quote doubled.
    static string Quote(string value) => "\"" + (value.EndsWith('\\') ? value + "\\" : value) + "\"";

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
