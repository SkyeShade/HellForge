using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;

namespace HellForge.Session;

// The second HellForge.Session process: it runs inside hf-sandbox's overlay wineserver and is the only Windows process that
// starts, edits and watches the game. The trusted runner never opens the game process. Its status is evidence, not authority.
static class SandboxDomainHost
{
    public static int Run(string specPath)
    {
        SandboxLaunchSpec spec;
        try { spec = SandboxProtocol.ReadLaunch(specPath); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        {
            Console.Error.WriteLine("sandbox host: " + error.Message);
            return 1;
        }
        var statusPath = Path.Combine(spec.GameOutWine, "domain", "status.json");
        var stopPath = Path.Combine(spec.GameOutWine, "domain", "stop");
        var domain = Directory.CreateDirectory(Path.Combine(spec.GameOutWine, "domain")).FullName;
        var clean = new SandboxDomainStatus
        {
            RunId = spec.RunId, DomainPid = Environment.ProcessId, State = "starting", Windows = []
        };
        SandboxDomainStatus.Write(statusPath, clean);
        HostBoundedProcess? job = null;
        Thread? applier = null;
        HellForgeLocalRoutingPatch.PlayFabRedirect? playFab = null;
        SandboxPipeRelayDomain? relay = null;
        try
        {
            ApplyEnvironment(spec.Environment);
            var stats = Directory.CreateDirectory(Path.Combine(spec.GameOutWine, "sandbox-stats")).FullName;
            Environment.SetEnvironmentVariable("HELLFORGE_SANDBOX_STATS", stats);
            // These are supervisor-side diagnostics: never a way for the game to ask for an unsandboxed child.
            Environment.SetEnvironmentVariable("HELLFORGE_BRIDGE_PIPE", null);
            Environment.SetEnvironmentVariable("HELLFORGE_CRASH_DUMP", null);
            Environment.SetEnvironmentVariable("HELLFORGE_ALLOW_CHILD_PROCESSES", null);
            Environment.SetEnvironmentVariable("HELLFORGE_LINUX_INSTALL", null);
            Environment.SetEnvironmentVariable("HELLFORGE_LINUX_PREFIX", null);

            // HELLFORGE_PIPE is set only for a multiplayer session, when the backend serves the lobby/party and live
            // pipes. Solo sessions leave the loader's live pipe unavailable exactly as they do outside the sandbox;
            // serving a name with no trusted upstream would only make the loader retry it.
            if (spec.Environment.ContainsKey("HELLFORGE_PIPE"))
            {
                relay = new SandboxPipeRelayDomain(spec.RunId, spec.RelayPort, spec.RelayToken,
                    line => Console.Error.WriteLine("sandbox pipe relay: " + line));
                relay.Start();
            }
            var install = new InstallLayout(spec.InstallRootWine);
            var arguments = GameLaunchOptions.CommandLine(spec.GameArguments);
            job = new HostBoundedProcess(spec.GameExeWine, arguments, spec.WorkingDirectoryWine, Path.Combine(spec.GameOutWine, "environment"),
                0, singleProcess: true);
            clean = clean with { GamePid = job.Process.Id, State = "starting" };
            SandboxDomainStatus.Write(statusPath, clean);
            job.Resume();
            playFab = spec.LocalPlayFab
                ? HellForgeLocalRoutingPatch.StartPlayFabRedirect(job.Process.Id, spec.GameExeWine, spec.BackendOrigin + "/playfab", -1)
                : null;
            if (spec.NativeMods)
            {
                var pid = job.Process.Id;
                applier = new Thread(() =>
                {
                    try
                    {
                        NativeModApplier.Run(install, spec.NativeFolders, pid, spec.GameExeWine,
                            Path.Combine(domain, "native-mods.log"), Path.Combine(domain, "native-mods.json"));
                    }
                    catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or InvalidOperationException) { }
                }) { IsBackground = true, Name = "native mods" };
                applier.Start();
            }
            ApplyEarlyConstructor(spec, job);
            ApplyRouting(spec, job);
            clean = clean with { State = "running" };
            SandboxDomainStatus.Write(statusPath, clean);
            ObserveGame(spec, job, statusPath, stopPath, playFab, applier, ref clean);
            return 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or InvalidOperationException or
                                         System.ComponentModel.Win32Exception or ArgumentException)
        {
            try
            {
                SandboxDomainStatus.Write(statusPath, clean with { State = "failed", Failure = Privacy.Scrub(error.Message) });
            }
            catch (Exception writeError) when (writeError is IOException or UnauthorizedAccessException) { }
            Console.Error.WriteLine("sandbox host: " + error.Message);
            return 1;
        }
        finally
        {
            relay?.Stop();
            try { job?.Dispose(); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        }
    }

    static void ObserveGame(SandboxLaunchSpec spec, HostBoundedProcess job, string statusPath, string stopPath,
        HellForgeLocalRoutingPatch.PlayFabRedirect? playFab, Thread? applier, ref SandboxDomainStatus clean)
    {
        var clock = Stopwatch.StartNew();
        var windows = new List<SandboxWindowSample>();
        long nextWindows = 0;
        bool stopped = false;
        while (true)
        {
            job.Process.Refresh();
            if (job.Process.HasExited) break;
            if (File.Exists(stopPath))
            {
                stopped = true;
                clean = clean with { State = "stopping" };
                SandboxDomainStatus.Write(statusPath, clean);
                job.Stop();
                break;
            }
            var matchmaking = spec.LocalPlayFab ? null : ClearMatchmaking(job.Process.Id, spec.GameDllWine);
            if (clock.ElapsedMilliseconds >= nextWindows)
            {
                windows = HostWindowText.Read(job.Process.Id).Select(w => new SandboxWindowSample(w.Title, w.Controls)).ToList();
                nextWindows = clock.ElapsedMilliseconds + 30000;
            }
            clean = clean with
            {
                State = "running", Matchmaking = matchmaking,
                PlayFabOutcome = playFab?.Outcome, Windows = windows
            };
            SandboxDomainStatus.Write(statusPath, clean);
            Thread.Sleep(1000);
        }
        applier?.Join(5000);
        var exited = job.Process.HasExited;
        int? code = null;
        if (exited) code = job.Process.ExitCode;
        bool empty;
        try { empty = job.ActiveProcesses == 0; }
        catch (System.ComponentModel.Win32Exception) { empty = false; }
        clean = clean with
        {
            State = stopped ? "stopped" : "exited", ExitCode = code, JobEmpty = empty,
            Matchmaking = clean.Matchmaking, PlayFabOutcome = playFab?.Outcome, Windows = windows
        };
        SandboxDomainStatus.Write(statusPath, clean);
    }

    static void ApplyEnvironment(Dictionary<string, string> values)
    {
        foreach (var (key, value) in values)
        {
            if (!SandboxProtocol.EnvironmentKeys.Contains(key)) throw new InvalidDataException("The launch spec asks for an environment variable outside the allowlist: " + key);
            Environment.SetEnvironmentVariable(key, value);
        }
    }

    static void ApplyEarlyConstructor(SandboxLaunchSpec spec, HostBoundedProcess job)
    {
        HellForgeHostEarlyConstructorExperiment.TargetExe = spec.GameExeWine;
        HellForgeHostEarlyResult experiment;
        try
        {
            experiment = HellForgeHostEarlyConstructorExperiment.ApplySteamGame(job.Process.Id, job.Process.StartTime.Ticks,
                "SkipConstructorAndMainCheck");
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            throw new InvalidOperationException("The game could not be prepared for the local backend.");
        }
        var ready = experiment.Written && experiment.MainCheckWritten && experiment.LaterStatusWritten && experiment.ReadbackMatched;
        if (experiment.Outcome != "applied_at_candidate_boundary" || !ready)
            throw new InvalidOperationException("The game could not be prepared for the local backend.");
    }

    static void ApplyRouting(SandboxLaunchSpec spec, HostBoundedProcess job)
    {
        HellForgeLocalRoutingResult routing;
        try
        {
            routing = HellForgeLocalRoutingPatch.Apply(job.Process.Id, job.Process.StartTime.Ticks, spec.GameExeWine,
                spec.GameDllWine, spec.BackendOrigin, 45000, false);
        }
        catch (Exception error) when (error is InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception)
        {
            throw new InvalidOperationException("The game could not be connected to the local backend.");
        }
        if (routing.Outcome != "applied" || !routing.ReadbackMatched)
            throw new InvalidOperationException("The game could not be connected to the local backend.");
    }

    static string? ClearMatchmaking(int pid, string gameDll)
    {
        try { return HellForgeLocalRoutingPatch.ClearPlatformMatchmaking(pid, gameDll); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { return "error"; }
    }
}
