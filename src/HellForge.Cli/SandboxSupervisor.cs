using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.Cli;

// The native half of F.4d: it validates the Wine runner's request, starts hf-sandbox in its own process group, watches the
// trusted runner's heartbeat, and performs the native copy-back only after the domain is gone. It never invokes a shell.
sealed class SandboxSupervisor
{
    const int PrSetPdeathsig = 1, PrSetChildSubreaper = 36, SigTerm = 15, SigKill = 9;
    const string AppArmorHelper = "/usr/local/lib/hellforge/unshare";
    const string AppArmorProfile = "/etc/apparmor.d/hellforge-unshare";
    readonly InstallLayout install;
    readonly string prefix;
    readonly string sessionFile;
    readonly int parentPid;
    volatile bool stopping;

    SandboxSupervisor(InstallLayout install, string prefix, string sessionFile, int parentPid)
    {
        this.install = install;
        this.prefix = prefix;
        this.sessionFile = sessionFile;
        this.parentPid = parentPid;
    }

    public static int Run(string installRoot, string prefix, string sessionFile, int parentPid)
    {
        if (!OperatingSystem.IsLinux() || LinuxNetworkBlock.UnderWine)
            throw new InvalidDataException("The native sandbox supervisor must run on native Linux.");
        var install = new InstallLayout(installRoot);
        if (!install.Exists) throw new InvalidDataException(install.Root + " is not a HellForge installation.");
        prefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(prefix));
        if (!Directory.Exists(Path.Combine(prefix, "pfx")))
            throw new InvalidDataException("The Linux sandbox prefix must be the compatdata folder that holds pfx/: " + prefix);
        sessionFile = Path.TrimEndingDirectorySeparator(Path.GetFullPath(sessionFile));
        if (!string.Equals(sessionFile, Path.Combine(install.Logs, "play-session.json"), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox supervisor's session file is not this installation's logs/play-session.json.");
        if (parentPid <= 0 || !LinuxProcessIdentity.Alive(parentPid))
            throw new InvalidDataException("The sandbox supervisor's hf-inner parent is not running.");
        return new SandboxSupervisor(install, prefix, sessionFile, parentPid).Execute();
    }

    int Execute()
    {
        var status = 1;
        try { status = ExecuteCore(); return status; }
        finally { Say($"exiting with code {status}"); }
    }

    int ExecuteCore()
    {
        Say("starting");
        if (!OperatingSystem.IsLinux()) throw new InvalidDataException("The native sandbox supervisor must run on native Linux.");
        Pdeathsig();
        using var term = PosixSignalRegistration.Create(PosixSignal.SIGTERM, _ => { stopping = true; });
        using var interrupt = PosixSignalRegistration.Create(PosixSignal.SIGINT, _ => { stopping = true; });
        using var hangup = PosixSignalRegistration.Create(PosixSignal.SIGHUP, _ => { stopping = true; });
        try
        {
            var wait = ProtonTimeout();
            var previousRun = PublishedRunId();
            Say($"previous run {(previousRun.Length == 0 ? "none" : previousRun)}; waiting for a new run in {sessionFile}");
            var run = WaitForRun(previousRun, wait);
            if (run == null)
                return StopRefusal(null, $"the session runner did not publish a new run within {wait.TotalSeconds:0} seconds in {sessionFile} (previous run {(previousRun.Length == 0 ? "none" : previousRun)})");
            var (runId, runFolder) = run.Value;
            Say($"serving run {runId} in {runFolder}");
            var control = Path.Combine(runFolder, "control");
            Directory.CreateDirectory(control);
            var requestPath = Path.Combine(control, "start.json");
            Say($"waiting for the start request for run {runId} at {requestPath}");
            SandboxStartRequest? request;
            try { request = WaitForRequest(requestPath, runId, DateTime.UtcNow + wait); }
            catch (InvalidDataException error)
            {
                return StopRefusal(control, $"the start request for run {runId} at {requestPath} was refused: {error.Message}");
            }
            if (request == null)
                return StopRefusal(control, $"timed out waiting for the start request for run {runId} at {requestPath}");
            Say($"read start request for run {runId} from {requestPath}");
            SandboxStartPlan plan;
            try
            {
                plan = SandboxProtocol.Validate(install, runFolder, prefix, request);
                Say($"start request validation passed for run {runId}");
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException)
            {
                return StopRefusal(control, $"start request validation refused for run {runId}: {error.Message}");
            }
            if (!string.Equals(request.RunId.ToLowerInvariant(), runId, StringComparison.Ordinal))
                return StopRefusal(control, $"start request refused for run {runId}: it names {request.RunId}");
            Directory.CreateDirectory(Path.Combine(plan.GameOut, "domain"));
            WriteText(plan.LaunchSpec, JsonSerializer.Serialize(plan.Launch, SandboxJson.Default.SandboxLaunchSpec) + "\n");
            return StartDomain(control, plan, request);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException or
                                         InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception)
        {
            Say("failed: " + error.Message);
            return 1;
        }
    }

    string PublishedRunId()
    {
        var state = SessionFiles.ReadJson(sessionFile);
        return state is { } value ? SessionFiles.Text(value, "runId") : "";
    }

    (string RunId, string Run)? WaitForRun(string previousRunId, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!stopping && DateTime.UtcNow < deadline)
        {
            if (!LinuxProcessIdentity.Alive(parentPid)) return null;
            if (PublishedRun(previousRunId) is { } published) return published;
            Thread.Sleep(200);
        }
        return null;
    }

    // The run play-session.json names now, when it is a valid run folder of this installation other than previousRunId.
    (string RunId, string Run)? PublishedRun(string previousRunId)
    {
        var state = SessionFiles.ReadJson(sessionFile);
        var runId = state is { } value ? SessionFiles.Text(value, "runId") : "";
        var run = state is { } value2 ? SessionFiles.Text(value2, "run") : "";
        if (runId.Length != 32 || !runId.All(char.IsAsciiHexDigit) || run.Length == 0) return null;
        var nextRunId = runId.ToLowerInvariant();
        if (previousRunId.Length > 0 && string.Equals(nextRunId, previousRunId, StringComparison.OrdinalIgnoreCase)) return null;
        var host = SandboxProtocol.HostPath(run, prefix, "published run");
        var expected = Path.TrimEndingDirectorySeparator(Path.Combine(install.Sessions, nextRunId));
        return string.Equals(host, expected, StringComparison.Ordinal) && Directory.Exists(host) ? (nextRunId, host) : null;
    }

    SandboxStartRequest? WaitForRequest(string path, string runId, DateTime deadline)
    {
        while (!stopping && DateTime.UtcNow < deadline)
        {
            if (File.Exists(path))
            {
                try
                {
                    var request = JsonSerializer.Deserialize(File.ReadAllText(path), SandboxJson.Default.SandboxStartRequest);
                    if (request != null && string.Equals(request.RunId, runId, StringComparison.OrdinalIgnoreCase)) return request;
                    return null;
                }
                catch (Exception error) when (error is IOException or JsonException)
                {
                    throw new InvalidDataException("the sandbox start request could not be read as JSON", error);
                }
            }
            if (!LinuxProcessIdentity.Alive(parentPid)) return null;
            Thread.Sleep(200);
        }
        return null;
    }

    static string? ResolveDomainCommand(out string protonPath, out string shell)
    {
        protonPath = "";
        shell = "";
        var proton = Environment.GetEnvironmentVariable("PROTON");
        if (string.IsNullOrWhiteSpace(proton)) return "PROTON is not an absolute installed Proton script";
        protonPath = proton!;
        if (!Path.IsPathFullyQualified(protonPath) || !File.Exists(protonPath))
            return "PROTON is not an absolute installed Proton script";
        var setsid = new[] { "/usr/bin/setsid", "/bin/setsid" }.FirstOrDefault(File.Exists);
        if (setsid is not { } found) return "setsid is missing (it installs with util-linux)";
        shell = found;
        return null;
    }

    static string FinishDomainStopReason(Process process, string stopReason, SandboxDomainStatus? status)
    {
        if (stopReason.Length != 0 || status is not { State: "starting" or "running" or "stopping" }) return stopReason;
        KillDomain(process.Id);
        return "hf-sandbox ended while the domain was still running";
    }

    int StartDomain(string control, SandboxStartPlan plan, SandboxStartRequest request)
    {
        var problem = ResolveDomainCommand(out var protonPath, out var shell);
        if (problem != null) return StopRefusal(control, problem);
        var heartbeat = Path.Combine(control, "runner.heartbeat");
        var stopSignal = Path.Combine(Path.GetDirectoryName(control)!, "stop-observation.signal");
        var stopDomain = Path.Combine(plan.GameOut, "domain", "stop");
        var runtime = DomainRuntime(protonPath);
        Say(runtime.Mode == SteamRuntimeMode.Container
            ? $"Steam Linux Runtime: container \"{runtime.Name}\" {runtime.Version} at {runtime.Folder}; the domain runs inside it"
            : $"Steam Linux Runtime: host mode: {runtime.Reason}");
        var start = DomainStartInfo(plan, runtime, protonPath, shell);
        WriteJson(Path.Combine(control, "domain.json"), new JsonObject
        {
            ["state"] = "starting", ["supervisorPid"] = Environment.ProcessId, ["startedUtc"] = DateTime.UtcNow.ToString("o")
        });
        Process process;
        try { process = Process.Start(start) ?? throw new InvalidDataException("hf-sandbox did not start"); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return StopRefusal(control, "hf-sandbox could not start: " + error.Message);
        }
        Say($"domain started for run {request.RunId}: hf-sandbox pid {process.Id}");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        var timeout = ProtonTimeout();
        var clock = Stopwatch.StartNew();
        try
        {
            WriteJson(Path.Combine(control, "domain.json"), new JsonObject
            {
                ["state"] = "running", ["supervisorPid"] = Environment.ProcessId, ["childPid"] = process.Id,
                ["startedUtc"] = DateTime.UtcNow.ToString("o")
            });
            var stopReason = WaitDomain(process, heartbeat, stopSignal, stopDomain, clock, timeout);
            var status = SandboxDomainStatus.Read(Path.Combine(plan.GameOut, "domain", "status.json"));
            stopReason = FinishDomainStopReason(process, stopReason, status);
            var output = Safe(stdout);
            var errors = Safe(stderr);
            File.WriteAllText(Path.Combine(control, "domain.log"), output + errors);
            var exitCode = process.ExitCode;
            if (status == null && exitCode != 0)
            {
                var reason = FirstLine(errors);
                WriteJson(Path.Combine(control, "refused.json"), new JsonObject
                {
                    ["reason"] = reason.Length > 0 ? reason : "hf-sandbox refused the domain", ["exitCode"] = exitCode
                });
            }
            WriteJson(Path.Combine(control, "exit.json"), new JsonObject
            {
                ["state"] = exitCode == 0 ? "exited" : "failed", ["exitCode"] = exitCode, ["stopReason"] = stopReason,
                ["elapsedSeconds"] = Math.Round(clock.Elapsed.TotalSeconds, 2),
                ["sandboxRefused"] = status == null && exitCode != 0, ["completedUtc"] = DateTime.UtcNow.ToString("o")
            });
            Say($"domain ended for run {request.RunId}: exit code {exitCode}" +
                (stopReason.Length > 0 ? "; stop reason " + stopReason : "") + ".");
            DoCopyBack(control, plan, request);
            return exitCode == 0 || stopReason.Length > 0 ? 0 : 1;
        }
        finally
        {
            if (!process.HasExited)
            {
                KillDomain(process.Id);
                if (!process.WaitForExit(5000)) KillDomain(process.Id, SigKill);
                process.WaitForExit(5000);
            }
            process.Dispose();
            CleanupSandbox(plan);
        }
    }

    // Runs last, after any copy-back/evidence: remove the session's Steam links, then the overlay state and empty mount
    // point. This is native Linux, so the mode-000 overlay work folder can be made writable before deletion.
    void CleanupSandbox(SandboxStartPlan plan)
    {
        try { SandboxFolders.RemoveSteamHome(plan.GameOut); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or
                                         InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            Say("steam links cleanup: " + error.Message);
        }
        try { SandboxFolders.RemoveSessionState(plan.Run, plan.GameOut); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or
                                         InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            Say("sandbox cleanup: " + error.Message);
        }
    }

    string WaitDomain(Process process, string heartbeat, string stopSignal, string stopDomain, Stopwatch clock, TimeSpan timeout)
    {
        while (!process.WaitForExit(250))
        {
            if (stopping || !LinuxProcessIdentity.Alive(parentPid))
            {
                StopDomain(process, stopDomain);
                return "hf-inner ended";
            }
            if (File.Exists(stopSignal))
            {
                StopDomain(process, stopDomain);
                return "stop requested";
            }
            if (HeartbeatStale(heartbeat))
            {
                StopDomain(process, stopDomain);
                return "the trusted session runner stopped";
            }
            if (clock.Elapsed > timeout)
            {
                StopDomain(process, stopDomain);
                return "the domain exceeded HELLFORGE_PROTON_TIMEOUT";
            }
        }
        if (!process.HasExited) StopDomain(process, stopDomain, final: true);
        process.WaitForExit();
        return "";
    }

    void DoCopyBack(string control, SandboxStartPlan plan, SandboxStartRequest request)
    {
        Say($"copy-back: waiting for run {request.RunId}'s request");
        var status = SandboxDomainStatus.Read(Path.Combine(plan.GameOut, "domain", "status.json"));
        if (status is not { GamePid: > 0, State: "exited" or "stopped" })
        {
            const string reason = "the domain did not report a finished game process";
            Say("copy-back abandoned: " + reason);
            WriteJson(Path.Combine(control, "abandoned.json"), new JsonObject
            {
                ["reason"] = reason, ["utc"] = DateTime.UtcNow.ToString("o")
            });
            return;
        }
        var path = Path.Combine(control, "copy-back.request");
        var deadline = DateTime.UtcNow.AddSeconds(45);
        while (!File.Exists(path) && DateTime.UtcNow < deadline && !HeartbeatStale(Path.Combine(control, "runner.heartbeat"), 20))
            Thread.Sleep(200);
        if (!File.Exists(path))
        {
            const string reason = "the trusted runner ended without asking for the copy-back";
            Say("copy-back abandoned: " + reason);
            WriteJson(Path.Combine(control, "abandoned.json"), new JsonObject
            {
                ["reason"] = reason, ["utc"] = DateTime.UtcNow.ToString("o")
            });
            return;
        }
        Say($"copy-back: request read for run {request.RunId}");
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            var runId = root.TryGetProperty("runId", out var id) ? id.GetString() ?? "" : "";
            var nonce = root.TryGetProperty("nonce", out var n) ? n.GetString() ?? "" : "";
            var profile = root.TryGetProperty("profile", out var p) ? p.GetString() ?? "" : "";
            if (!string.Equals(runId, request.RunId, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(nonce, request.Nonce, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("the copy-back request is not for this run and request");
            var result = NativeSandboxCopyBack.Run(install, plan.Run, profile);
            WriteJson(Path.Combine(control, "copy-back.json"), new JsonObject
            {
                ["ok"] = true, ["files"] = result.Files, ["bytes"] = result.Bytes, ["skipped"] = result.Skipped,
                ["staging"] = result.Staging
            });
            Say($"copy-back finished: {result.Files} files, {result.Bytes} bytes");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            Say("copy-back failed: " + error.Message);
            WriteJson(Path.Combine(control, "copy-back.json"), new JsonObject { ["ok"] = false, ["error"] = error.Message });
        }
    }

    int StopRefusal(string? control, string reason)
    {
        Say("refused: " + reason);
        try
        {
            if (control != null) WriteJson(Path.Combine(control, "refused.json"),
                new JsonObject { ["reason"] = reason, ["utc"] = DateTime.UtcNow.ToString("o") });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Say("could not write the refusal: " + error.Message);
        }
        return 1;
    }

    static bool HeartbeatStale(string path, int seconds = 15)
    {
        try { return !File.Exists(path) || DateTime.UtcNow - File.GetLastWriteTimeUtc(path) > TimeSpan.FromSeconds(seconds); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return true; }
    }

    // The supervisor resolves the runtime itself from PROTON and Steam's folder, never from the session request.
    static SteamRuntime DomainRuntime(string protonPath)
    {
        var steam = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH");
        return string.IsNullOrWhiteSpace(steam)
            ? new(SteamRuntimeMode.Host, "STEAM_COMPAT_CLIENT_INSTALL_PATH is not set", null, null, null, [], [], null, null, null, false)
            : SteamRuntimeLocator.Resolve(protonPath, steam!);
    }

    // setsid, [Ubuntu's allowed unshare], [the runtime's entry point], hf-sandbox ... -- <proton> waitforexitandrun <domain
    // host>. The entry point stays outside hf-sandbox: hf-sandbox's seccomp refuses new namespaces, and pressure-vessel
    // needs them. In container mode the runtime's tool and mount variables make the installation, prefix and shader cache
    // visible inside it; hf-sandbox then builds the same view as on the host.
    ProcessStartInfo DomainStartInfo(SandboxStartPlan plan, SteamRuntime runtime, string protonPath, string shell)
    {
        var start = new ProcessStartInfo(shell)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = install.Root
        };
        AppArmorPrefix(start);
        foreach (var part in SteamRuntimeLocator.EntryPointArguments(runtime, "waitforexitandrun")) start.ArgumentList.Add(part);
        start.ArgumentList.Add(plan.HfSandbox);
        foreach (var (option, value) in new[] { ("--session", plan.Run), ("--prefix", prefix) })
        {
            start.ArgumentList.Add(option);
            start.ArgumentList.Add(value);
        }
        if (runtime.Mode == SteamRuntimeMode.Container && runtime.Folder is { } folder)
        {
            start.ArgumentList.Add("--steam-runtime");
            start.ArgumentList.Add(folder);
        }
        foreach (var bind in plan.ReadOnlyBinds)
        {
            start.ArgumentList.Add("--bind-ro");
            start.ArgumentList.Add(bind);
        }
        start.ArgumentList.Add("--");
        foreach (var part in SteamRuntimeLocator.ProtonArguments(runtime, protonPath, "waitforexitandrun")) start.ArgumentList.Add(part);
        foreach (var part in new[] { plan.DomainHost, "host", "--spec", WinePath.ToZPath(plan.LaunchSpec) }) start.ArgumentList.Add(part);
        if (runtime.Mode == SteamRuntimeMode.Container)
        {
            var shader = Environment.GetEnvironmentVariable("STEAM_COMPAT_SHADER_PATH");
            var invocation = LinuxProtonInvocation.From(runtime, (protonPath, Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH")!, prefix),
                "waitforexitandrun", [], new(InstallPath: install.Game, Mounts: [install.Root], ShaderPath: string.IsNullOrWhiteSpace(shader) ? null : shader));
            foreach (var pair in invocation.Environment) start.Environment[pair.Key] = pair.Value;
        }
        return start;
    }

    // Ubuntu's AppArmor user-namespace restriction: HellForge's allowed unshare goes first, so hf-sandbox (and in container
    // mode pressure-vessel's bwrap below it) runs in a user namespace that profile allows.
    static void AppArmorPrefix(ProcessStartInfo start)
    {
        var restricted = "";
        try { restricted = File.ReadAllText("/proc/sys/kernel/apparmor_restrict_unprivileged_userns").Trim(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        if (restricted == "1" && File.Exists(AppArmorHelper) && File.Exists(AppArmorProfile))
        {
            start.ArgumentList.Add(AppArmorHelper);
            start.ArgumentList.Add("--user");
            start.ArgumentList.Add("--map-user=" + getuid());
            start.ArgumentList.Add("--map-group=" + getgid());
            start.ArgumentList.Add("--");
        }
    }

    static TimeSpan ProtonTimeout()
    {
        var text = Environment.GetEnvironmentVariable("HELLFORGE_PROTON_TIMEOUT");
        return int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var seconds) && seconds > 0
            ? TimeSpan.FromSeconds(seconds) : TimeSpan.FromHours(12);
    }

    void StopDomain(Process process, string stopPath, bool final = false)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(stopPath)!);
            File.WriteAllText(stopPath, "stop\n");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        if (process.WaitForExit(final ? 5000 : 10000)) return;
        KillDomain(process.Id, SigTerm);
        if (process.WaitForExit(5000)) return;
        KillDomain(process.Id, SigKill);
    }

    // The domain is hf-sandbox's process group and, measured with Steam's runtime container, processes in new sessions
    // under it. Signal the group, then every descendant of this supervisor (the domain is its only child tree).
    static void KillDomain(int pid, int signal = SigTerm)
    {
        KillGroup(pid, signal);
        foreach (var descendant in Descendants(Environment.ProcessId)) KillPid(descendant, signal);
    }

    static IReadOnlyList<int> Descendants(int root)
    {
        var children = new Dictionary<int, List<int>>();
        foreach (var folder in Directory.EnumerateDirectories("/proc"))
        {
            if (!int.TryParse(Path.GetFileName(folder), NumberStyles.None, CultureInfo.InvariantCulture, out var pid)) continue;
            if (ParentOf(pid) is not { } parent) continue;
            if (!children.TryGetValue(parent, out var list)) children[parent] = list = [];
            list.Add(pid);
        }
        var found = new List<int>();
        var queue = new Queue<int>([root]);
        while (queue.Count > 0)
            foreach (var child in children.GetValueOrDefault(queue.Dequeue()) ?? [])
            {
                found.Add(child);
                queue.Enqueue(child);
            }
        return found;
    }

    // The PPid of /proc/<pid>/stat: the field after the command, which is in parentheses and may contain spaces.
    static int? ParentOf(int pid)
    {
        try
        {
            var stat = File.ReadAllText("/proc/" + pid.ToString(CultureInfo.InvariantCulture) + "/stat");
            var fields = stat[(stat.LastIndexOf(')') + 2)..].Split(' ');
            return fields.Length > 1 && int.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out var parent) ? parent : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentOutOfRangeException) { return null; }
    }

    static void KillPid(int pid, int signal)
    {
        try { kill(pid, signal); }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { }
    }

    static void KillGroup(int pid, int signal = SigTerm)
    {
        try { kill(-pid, signal); }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { }
    }

    static string Safe(Task<string> task)
    {
        try { return task.Wait(TimeSpan.FromSeconds(5)) ? task.Result : ""; }
        catch (AggregateException) { return ""; }
    }

    static void Say(string message) => Console.Error.WriteLine("hf-supervisor: " + message);

    static string FirstLine(string text) =>
        text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault() ?? "";

    static void WriteJson(string path, JsonNode value) => WriteText(path, value.ToJsonString() + "\n");

    static void WriteText(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var partial = path + ".partial";
        File.WriteAllText(partial, text);
        File.Move(partial, path, overwrite: true);
    }

    // Also a child subreaper: a domain process whose parent exits (pressure-vessel's own processes start new sessions,
    // and Wine's daemonize) is re-parented here instead of to init, so the stop below still finds it as a descendant.
    static void Pdeathsig()
    {
        try
        {
            prctl(PrSetPdeathsig, SigTerm);
            prctl(PrSetChildSubreaper, 1);
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { }
    }

    [DllImport("libc", SetLastError = true)]
    static extern int prctl(int option, int arg2);

    [DllImport("libc", SetLastError = true)]
    static extern int kill(int pid, int signal);

    [DllImport("libc")]
    static extern uint getuid();

    [DllImport("libc")]
    static extern uint getgid();
}
