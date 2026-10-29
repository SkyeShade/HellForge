using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.Session;

// PlaySession's Linux domain: the host paths and exact bind inputs, the request-file handoff to hf-inner's native
// supervisor, and the observation/evidence of the overlay-prefix host and game. Windows keeps its low-integrity path.
sealed partial class PlaySession
{
    string SandboxUserData => Path.Combine(gameOut, "userdata");
    string hostInstall = "", hostPrefix = "", hostRun = "", hostGameOut = "";
    bool linuxSandbox;
    readonly List<string> sandboxBinds = [];
    Timer? sandboxHeartbeat;
    string sandboxNonce = "";
    SandboxPipeRelayHost? sandboxPipeRelay;

    static string HostEnvironment(params string[] names)
    {
        foreach (var name in names)
            if (Environment.GetEnvironmentVariable(name) is { } value && value.StartsWith('/'))
                return value;
        throw new SessionFailure("The Linux sandbox needs its host installation and prefix from the wrapper. Start HellForge with app/linux/hellforge-linux.sh.");
    }

    void SayLinuxSandboxNotice()
    {
        var setting = ProfileStore.LoadSandbox(new InstallSessionPaths(install));
        var programMissing = !File.Exists(Path.Combine(install.App, "linux", "hf-sandbox"));
        var reason = !setting ? "Settings > Sandbox is off"
            : programMissing ? "the Linux sandbox program is missing"
                : LinuxSandboxReadiness.Problem(install) ?? "this session was started without the Linux sandbox";
        var notice = "This session is not sandboxed (" + reason + "). The game, its mods and anything they run can write the player's files.";
        result["sandboxNotice"] = notice;
        Say(notice);
    }

    void ConfigureLinuxSandbox()
    {
        linuxSandbox = options.Sandbox && LinuxNetworkBlock.UnderWine;
        if (options.Sandbox && OperatingSystem.IsLinux() && !linuxSandbox)
            throw new SessionFailure("The Linux sandbox runs through HellForge's Linux wrapper, not as a native session. Start HellForge with app/linux/hellforge-linux.sh.");
        if (linuxSandbox)
        {
            // Host paths cross the request seam as Z: paths: the supervisor maps them back on native Linux, and the
            // overlay does not mask Z:. Keeping them in one form also makes Path.Combine and the bind allowlist agree
            // when Wine's current drive is not Z:.
            hostInstall = WinePath.ToZPath(HostEnvironment("INSTALL"));
            hostPrefix = WinePath.ToZPath(HostEnvironment("HELLFORGE_LINUX_PREFIX", "STEAM_COMPAT_DATA_PATH"));
            hostInstall = Path.TrimEndingDirectorySeparator(hostInstall);
            hostPrefix = Path.TrimEndingDirectorySeparator(hostPrefix);
        }
        fullVerify = OperatingSystem.IsLinux() && SessionIntegrity.LastEndedUnclean(install);
    }

    // The loader plan names active mod folders and built DLLs. The overlay hides the installation through C:, so rewrite
    // those two fields to their Z: host paths, copy the plan into gameOut, and expose each exact input read-only.
    void PrepareSandboxModInputs(string loaderPlan, string loader)
    {
        var hostLoader = SandboxWinePath(loader);
        var hostPlan = SandboxWinePath(loaderPlan);
        var lines = File.ReadAllLines(hostPlan);
        for (var index = 0; index < lines.Length; index++)
        {
            if (!lines[index].StartsWith("mod\t", StringComparison.Ordinal)) continue;
            var fields = lines[index].Split('\t');
            if (fields.Length != 4) throw new SessionFailure("The mod loader plan has a malformed mod entry.");
            var folder = SandboxWinePath(fields[2]);
            SandboxBind(folder);
            fields[2] = folder;
            if (fields[3] != "-")
            {
                var dll = SandboxWinePath(fields[3]);
                SandboxBind(dll);
                fields[3] = dll;
            }
            lines[index] = string.Join('\t', fields);
        }
        var loaderCopy = Path.Combine(gameOut, "loader", "modloader-plan.txt");
        Directory.CreateDirectory(Path.GetDirectoryName(loaderCopy)!);
        File.WriteAllLines(loaderCopy, lines, new UTF8Encoding(false));
        SandboxBind(Path.Combine(hostInstall, "app", "session"));
        SandboxBind(hostLoader);
        var hostLoaderCopy = WinePath.ToHostPath(loaderCopy, hostPrefix);
        Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER", WinePath.ToZPath(hostLoader));
        Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER_PLAN", WinePath.ToZPath(hostLoaderCopy));
    }

    // One exact host input for hf-sandbox's --bind-ro list. The request validator repeats these checks.
    void SandboxBind(string hostPath)
    {
        // A Unix host path is mapped textually before Wine's GetFullPath can turn it into a C: path.
        hostPath = hostPath.StartsWith("/", StringComparison.Ordinal)
            ? WinePath.ToZPath(hostPath)
            : Path.TrimEndingDirectorySeparator(Path.GetFullPath(hostPath));
        var allowed = string.Equals(hostPath, Path.Combine(hostInstall, "app", "session"), StringComparison.Ordinal) ||
            string.Equals(hostPath, Path.Combine(hostInstall, "app", "sdk-shims", "hellforge_modloader.dll"), StringComparison.Ordinal) ||
            SandboxProtocol.Within(Path.Combine(hostInstall, "mods"), hostPath) ||
            SandboxProtocol.Within(Path.Combine(hostInstall, "temp", "mod-builds"), hostPath);
        if (!allowed || !(File.Exists(hostPath) || Directory.Exists(hostPath)))
            throw new SessionFailure("The sandbox request named an input outside the mod, loader and session-host folders: " + hostPath);
        SandboxProtocol.AddBind(sandboxBinds, hostPath);
    }

    string PrepareProfileFolder()
    {
        profileFolder = Path.Combine(install.UserData, "environment");
        if (options.Sandbox)
        {
            // the game's AppData and temp: a copy in gameOut too, back at the end
            SandboxFolders.Copy(install.UserData, gameOut, ["environment"]);
            profileFolder = Path.Combine(gameOut, "environment");
        }
        foreach (var folder in new[] { "Roaming", "Local", "Temp" }) Directory.CreateDirectory(Path.Combine(profileFolder, folder));
        var requestLog = Path.Combine(run, "backend", "requests.jsonl");
        long requestOffset = 0;
        result["preLaunchBackendRequests"] = Tools.BackendRequests(requestLog, ref requestOffset).Count;
        return requestLog;
    }

    // The request-file seam: the native supervisor started by hf-inner.sh is already watching this run. Everything the
    // domain may name is fixed or checked: gameOut, the game executable, the session host, the environment allowlist and
    // the exact read-only mod/loader inputs.
    SandboxDomainStatus StartSandboxDomain()
    {
        var (request, control) = PrepareSandboxStartRequest();
        PublishSandboxStart(control, request);
        return WaitForSandboxStart(control);
    }

    (SandboxStartRequest Request, string Control) PrepareSandboxStartRequest()
    {
        // The domain host itself is a file in app/session, so that bind is required even when no mod needs the loader.
        SandboxBind(Path.Combine(hostInstall, "app", "session"));
        var hostLoader = Path.Combine(hostInstall, "app", "sdk-shims", "hellforge_modloader.dll");
        if (File.Exists(hostLoader)) SandboxBind(hostLoader);
        if (nativeFolders != null)
            foreach (var folder in nativeFolders) SandboxBind(SandboxWinePath(folder));
        var relay = StartSandboxPipeRelay((string)result["runId"]!);
        var control = Directory.CreateDirectory(Path.Combine(run, "control")).FullName;
        var heartbeat = Path.Combine(control, "runner.heartbeat");
        File.WriteAllText(heartbeat, "1");
        sandboxHeartbeat = new Timer(_ =>
        {
            try { File.WriteAllText(heartbeat, "1"); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }, null, 1000, 1000);
        sandboxNonce = Guid.NewGuid().ToString("N");
        var request = new SandboxStartRequest
        {
            SchemaVersion = SandboxProtocol.SchemaVersion,
            RunId = (string)result["runId"]!,
            Nonce = sandboxNonce,
            InstallRoot = hostInstall,
            InstallRootWine = install.Root,
            DomainHost = Path.Combine(hostInstall, "app", "session", "HellForge.Session.exe"),
            GameExe = Path.Combine(hostInstall, "game", "bin", "helldivers2.exe"),
            GameExeWine = exe,
            GameOut = hostGameOut,
            GameOutWine = gameOut,
            GameDllWine = gameDll,
            BundleDirectoryWine = bundle,
            WorkingDirectoryWine = Path.Combine(copy, "bin"),
            AppRootWine = app,
            BackendOrigin = (string)result["backendOrigin"]!,
            RelayPort = relay.Port,
            RelayToken = relay.Token,
            LocalPlayFab = options.LocalPlayFab,
            NativeMods = modNative,
            NativeFolders = nativeFolders?.Select(SandboxWinePath).ToList() ?? [],
            GameArguments = SandboxGameArguments(),
            Environment = SandboxGameEnvironment(),
            ReadOnlyBinds = [.. sandboxBinds],
            RunnerPid = Environment.ProcessId,
            RunnerStartedUtc = Process.GetCurrentProcess().StartTime.ToUniversalTime().ToString("o")
        };
        return (request, control);
    }

    // The trusted end of F.4e stays in this real-prefix process: it opens the real backend/live pipes and accepts only
    // the overlay host's loopback connections for this run and token.
    SandboxPipeRelayHost StartSandboxPipeRelay(string runId)
    {
        if (sandboxPipeRelay != null) return sandboxPipeRelay;
        var relay = new SandboxPipeRelayHost(runId, Guid.NewGuid().ToString("N"), Say);
        try { relay.Start(); }
        catch (System.Net.Sockets.SocketException error)
        {
            throw new SessionFailure("The Linux sandbox's named-pipe relay could not listen on loopback: " + error.Message +
                ". The game was not started. To run without the sandbox, turn off Settings > Sandbox.");
        }
        sandboxPipeRelay = relay;
        result["sandboxPipeRelay"] = new JsonObject
        {
            ["port"] = relay.Port, ["maxConnections"] = SandboxPipeRelay.MaxConnections,
            ["maxBytesPerSecond"] = SandboxPipeRelay.MaxBytesPerSecond, ["maxChunkBytes"] = SandboxPipeRelay.MaxChunkBytes
        };
        return relay;
    }

    void StopSandboxPipeRelay()
    {
        sandboxPipeRelay?.Stop();
        sandboxPipeRelay = null;
    }

    static void PublishSandboxStart(string control, SandboxStartRequest request)
    {
        var partial = Path.Combine(control, "start.json.partial");
        File.WriteAllText(partial, JsonSerializer.Serialize(request, SandboxJson.Default.SandboxStartRequest));
        File.Move(partial, Path.Combine(control, "start.json"), overwrite: true);
    }

    SandboxDomainStatus WaitForSandboxStart(string control)
    {
        var deadline = DateTime.UtcNow.AddSeconds(180);
        while (DateTime.UtcNow < deadline)
        {
            ThrowIfStopRequested();
            var status = ReadDomainStatus();
            if (status is { GamePid: > 0, State: "running" or "exited" or "stopped" })
            {
                RecordSandboxStart(status, control);
                return status;
            }
            if (SandboxStartFailure(control, status) is { } failure) throw failure;
            Thread.Sleep(250);
        }
        throw new SessionFailure("The Linux sandbox did not start the game within 180 seconds. The game was not started. " +
            "To run without the sandbox, turn off Settings > Sandbox.");
    }

    static SessionFailure? SandboxStartFailure(string control, SandboxDomainStatus? status)
    {
        if (status is { State: "failed" })
            return new SessionFailure("The Linux sandbox host failed: " + (status.Failure ?? "no reason was recorded") +
                ". The game was not started. To run without the sandbox, turn off Settings > Sandbox.");
        if (Tools.ReadJson(Path.Combine(control, "refused.json")) is JsonObject refused)
            return new SessionFailure("The Linux sandbox could not start: " + ((string?)refused["reason"] ?? "the supervisor refused the request") +
                ". The game was not started. To run without the sandbox, turn off Settings > Sandbox.");
        if (File.Exists(Path.Combine(control, "exit.json")))
            return new SessionFailure("The Linux sandbox ended before the game started. See " + Path.Combine(control, "domain.log") +
                " or the wrapper log. To run without the sandbox, turn off Settings > Sandbox.");
        return null;
    }

    void RecordSandboxStart(SandboxDomainStatus status, string control)
    {
        gameStarted = DateTime.UtcNow;
        result["gamePid"] = status.GamePid;
        result["sandboxDomainPid"] = status.DomainPid;
        if (Tools.ReadJson(Path.Combine(control, "domain.json")) is JsonObject domain)
        {
            if (domain["supervisorPid"] is not null) result["sandboxSupervisorPid"] = domain["supervisorPid"]!.DeepClone();
            if (domain["childPid"] is not null) result["sandboxDomainProcessPid"] = domain["childPid"]!.DeepClone();
        }
        result["gameLaunched"] = true;
        result["launchPrepared"] = true;
        Save();
    }

    List<string> SandboxGameArguments()
    {
        var arguments = new List<string> { "--bundle-dir", bundle, "--release" };
        if (options.EngineLog) arguments.Add("--log-to-file");
        arguments.AddRange(options.GameOptions);
        return arguments;
    }

    string SandboxWinePath(string value)
    {
        var host = WinePath.ToHostPath(value, hostPrefix);
        return WinePath.ToZPath(host);
    }

    Dictionary<string, string> SandboxGameEnvironment()
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["APPDATA"] = Path.Combine(profileFolder!, "Roaming"),
            ["LOCALAPPDATA"] = Path.Combine(profileFolder!, "Local"),
            ["TEMP"] = Path.Combine(profileFolder!, "Temp"),
            ["TMP"] = Path.Combine(profileFolder!, "Temp"),
            ["HELLFORGE_SANDBOX_STATS"] = Path.Combine(gameOut, "sandbox-stats")
        };
        foreach (var key in SandboxProtocol.EnvironmentKeys)
            if (!values.ContainsKey(key) && Environment.GetEnvironmentVariable(key) is { Length: > 0 } value) values[key] = value;
        // No backend pipe exists in a solo session, so never leave a stale value for the stand-ins to chase.
        if (!options.Multiplayer) values.Remove("HELLFORGE_PIPE");
        return values;
    }

    SandboxDomainStatus? ReadDomainStatus() => SandboxDomainStatus.Read(Path.Combine(gameOut, "domain", "status.json"));

    void ObserveSandbox(SandboxDomainStatus initial, string requestLog, int blockRules)
    {
        var clock = Stopwatch.StartNew();
        var observations = Path.Combine(run, "observations.jsonl");
        long nextSample = 0, nextBlockCheck = 10000, requestOffset = 0;
        var requestCount = 0;
        var status = initial;
        Say("Play session running in the Linux sandbox. Close the game or press Stop in the launcher to end it.");
        while (true)
        {
            var current = ReadDomainStatus() ?? status;
            status = current;
            if (current.State == "failed")
                throw new SessionFailure("The Linux sandbox host failed: " + (current.Failure ?? "no reason was recorded"));
            if (current.State is "exited" or "stopped")
            {
                RecordSandboxEnd(current, clock);
                break;
            }
            CheckBlock(clock, blockRules, ref nextBlockCheck);
            requestCount += Tools.BackendRequests(requestLog, ref requestOffset).Count;
            if (clock.ElapsedMilliseconds >= nextSample)
            {
                WriteSandboxSample(observations, clock.ElapsedMilliseconds, requestCount, current);
                nextSample = clock.ElapsedMilliseconds + 30000;
            }
            if (StopRequested())
            {
                result["observedSeconds"] = Math.Round(clock.Elapsed.TotalSeconds, 2);
                break;
            }
            Thread.Sleep(1000);
        }
    }

    void WriteSandboxSample(string observations, long elapsed, int requestCount, SandboxDomainStatus status)
    {
        var row = new JsonObject
        {
            ["utc"] = DateTime.UtcNow.ToString("o"), ["elapsedMilliseconds"] = elapsed,
            ["windows"] = new JsonArray(status.Windows.Select(w => (JsonNode?)new JsonObject
            {
                ["Title"] = w.Title, ["Controls"] = new JsonArray(w.Controls.Select(c => (JsonNode?)JsonValue.Create(c)).ToArray())
            }).ToArray()),
            ["backendRequests"] = requestCount, ["sampled"] = true
        };
        if (status.Matchmaking is { Length: > 0 } matchmaking) row["platformMatchmaking"] = matchmaking;
        File.AppendAllText(observations, row.ToJsonString() + "\n", new UTF8Encoding(false));
    }

    void RecordSandboxEnd(SandboxDomainStatus status, Stopwatch clock)
    {
        result["gameExitedNaturally"] = status.State == "exited";
        result["jobEmpty"] = status.JobEmpty;
        result["gamePid"] = status.GamePid;
        result["sandboxDomainPid"] = status.DomainPid;
        if (status.ExitCode is { } code) result["exitCode"] = code;
        if (status.PlayFabOutcome is { Length: > 0 } playFab) result["playFabRedirect"] = playFab;
        result["sandboxDomainState"] = status.State;
        result["observedSeconds"] = Math.Round(clock.Elapsed.TotalSeconds, 2);
    }
}
