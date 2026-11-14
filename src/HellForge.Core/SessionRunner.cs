using System.Diagnostics;
using System.Globalization;

namespace HellForge.Core;

// A session's choices, as the launcher's Settings page keeps them (launcher-settings.json, ProfileStore) or as another
// launcher passes them (HellForge.Cli play). The game options are already checked (GameLaunchOptions.Parse). ModSync:
// the next session's mods after a mod sync (a ModSyncSession file), for that one session. Sandbox: low integrity on
// Windows, or the native Linux domain (ProfileStore.LoadSandbox). FullOwnershipCheck: ask Steam even when a recent ownership check still holds
// (SteamOwnershipRecord), for that one session. SteamUdp: co-op over Steam relays, UDP to Valve's relays only
// (ProfileStore.LoadSteamUdp, #62). WarServer: the HellForge war's server for that one session (else
// ProfileStore.LoadWarServer).
public sealed record SessionOptions(bool LocalPlayFab, bool SdkShims, string War, bool EngineLog, bool CrashDump, IReadOnlyList<string> GameOptions,
    string? ModSync = null, bool Sandbox = false, bool FullOwnershipCheck = false, bool SteamUdp = false, string? WarServer = null);

// Starts and stops the engine's session runner (HellForge.Session.exe play). It owns every safety step (verified copy,
// Steam ownership, network block, cleanup); this class only starts it and asks it to stop. It runs without a window and
// reports through the run directory's files, so it does not depend on this process staying alive to clean up.
public sealed class SessionRunner
{
    Process? controller;
    int? exitCode;

    public static bool IsSupported => OperatingSystem.IsWindows() || OperatingSystem.IsLinux();

    public static string UnsupportedMessage => "Sessions are supported on Windows and experimental on Linux.";

    public bool IsRunning => controller is { HasExited: false };
    public int? ProcessId => controller?.Id;
    public int? ExitCode => controller is { HasExited: true } ? controller.ExitCode : exitCode;

    // The session runner's command line (after HellForge.Session.exe). The template only matters when the backend
    // creates the profile on first use.
    public static List<string> Arguments(ISessionPaths paths, LocalProfile profile, SessionOptions options, string? winePrefix = null)
    {
        if (paths.InstallRoot == null) throw new InvalidOperationException("Sessions run in installations made with HellForge-Setup.");
        string Map(string path) => winePrefix == null ? path : WinePath.ToWinePath(path, winePrefix);
        var arguments = new List<string> { "play", "--install", Map(Path.TrimEndingDirectorySeparator(paths.InstallRoot)),
            "--profile", Map(Path.TrimEndingDirectorySeparator(Path.GetFullPath(profile.Folder))), "--template", profile.Template };
        if (options.LocalPlayFab) arguments.Add("--local-playfab");
        if (options.SdkShims) arguments.Add("--multiplayer");
        var warArguments = WarArguments(paths, options.War, options.WarServer);
        for (var i = 0; i + 1 < warArguments.Count; i++)
            if (warArguments[i] is "--live-war" or "--hellforge-war") warArguments[i + 1] = Map(warArguments[i + 1]);
        arguments.AddRange(warArguments);
        if (options.EngineLog) arguments.Add("--engine-log");
        // the player's launch options, for the session runner to add to the game's command line (GameLaunchOptions)
        if (options.GameOptions.Count > 0) arguments.AddRange(["--game-options", GameLaunchOptions.CommandLine(options.GameOptions)]);
        if (options.ModSync != null) arguments.AddRange(["--mod-sync", options.ModSync]);
        if (options.Sandbox) arguments.Add("--sandbox");
        if (options.FullOwnershipCheck) arguments.Add("--full-ownership-check");
        return arguments;
    }

    // The galactic war (ProfileStore.LoadWar): the HellForge war from its server (cached per server, WarServer.CachePath),
    // the live war through the mirror, or the all-planets war; the frozen war needs nothing.
    // A war script beside it (war-script.json: major orders, dispatches, reward mail; docs/war-script.md) goes with any war.
    static List<string> WarArguments(ISessionPaths paths, string war, string? warServer)
    {
        var script = Path.Combine(paths.HostRoot, "war-script.json");
        List<string> scripted = File.Exists(script) ? ["--war-script", script] : [];
        if (war == ProfileStore.WarLive) return [.. scripted, "--live-war", Path.Combine(paths.HostRoot, "live-war.json")];
        if (war == ProfileStore.WarHellForge)
        {
            var server = warServer ?? ProfileStore.LoadWarServer(paths);
            return [.. scripted, "--hellforge-war", WarServer.CachePath(paths.HostRoot, server), "--war-server", server];
        }
        if (war != ProfileStore.WarOpen) return scripted;
        var arguments = new List<string>(scripted) { "--open-war" };
        // its seed (0 needs nothing), and whether it deals its subfactions and SEAF allies (on needs nothing)
        if (ProfileStore.LoadSubfactionSeed(paths) is var seed and not 0) arguments.AddRange(["--war-seed", seed.ToString(CultureInfo.InvariantCulture)]);
        if (!ProfileStore.LoadDealSubfactions(paths)) arguments.AddRange(["--war-subfactions", "no"]);
        if (!ProfileStore.LoadDealSeaf(paths)) arguments.AddRange(["--war-seaf", "no"]);
        return arguments;
    }

    // Whether the engine was built with the Steam bridge (the Steamworks SDK): its stand-ins' build.json pins the bridge
    // build, which a sandboxed session needs (<app>/sdk-shims, beside the session runner's folder).
    public static bool HasSteamBridge(ISessionPaths paths) =>
        Path.GetDirectoryName(Path.GetDirectoryName(paths.Session)) is { } app &&
        SessionFiles.ReadJson(Path.Combine(app, "sdk-shims", "build.json")) is { } build &&
        SessionFiles.Text(build, "steamBridge") is { Length: > 0 };

    // Whether the sandbox can be offered. Windows keeps its Steam-bridge condition. Native Linux offers the domain when
    // app/linux/hf-sandbox is executable and the self-test passed for this app build and kernel; a stale or failed test is
    // not a pass. Under Wine this is false: the trusted runner's Windows sandbox path is never used on Linux.
    public static bool CanSandbox(ISessionPaths paths) =>
        OperatingSystem.IsWindows() ? HasSteamBridge(paths) && !LinuxNetworkBlock.UnderWine && !LinuxNetworkBlock.Requested
        : OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine && paths.InstallRoot != null &&
          LinuxSandboxProgramProblem(paths) == null && LinuxSandboxReadiness.Problem(new InstallLayout(paths.InstallRoot)) == null;

    // The package-side refusal, before the wrapper runs. The wrapper reruns the real self-test; this only says whether
    // the sandbox command exists at all.
    public static string? LinuxSandboxProgramProblem(ISessionPaths paths)
    {
        if (paths.InstallRoot == null) return "this launcher is not in a HellForge installation";
        var problem = LinuxSandboxReadiness.ProgramProblem(new InstallLayout(paths.InstallRoot));
        return problem.Length > 0 ? problem : null;
    }

    // Why the Linux switch is off or unavailable, in the wording Settings > Sandbox and a refused session use.
    public static string LinuxSandboxProblem(ISessionPaths paths)
    {
        if (LinuxSandboxProgramProblem(paths) is { } program) return program;
        return LinuxSandboxReadiness.Problem(new InstallLayout(paths.InstallRoot!)) ??
            "the Linux sandbox self-test did not pass";
    }

    // Whether a session is sandboxed unless told otherwise: the setting (on unless turned off), where it can be offered.
    public static bool SandboxByDefault(ISessionPaths paths) => CanSandbox(paths) && ProfileStore.LoadSandbox(paths);

    // What #62's HELLFORGE_STEAM_UDP=1 path runs: pasta (the passt package) and python3 (Valve's relay list). Null when
    // both are on PATH; otherwise what is missing, with the install command for this system's package manager (the
    // launcher says it before a session, which then plays without co-op). onPath is injectable for tests.
    public static string? SteamUdpProblem(Func<string, bool>? onPath = null)
    {
        onPath ??= OnPath;
        var pasta = onPath("pasta");
        var python = onPath("python3");
        if (pasta && python) return null;
        var missing = (pasta, python) switch
        {
            (false, false) => "pasta (the passt package) and python3",
            (false, true) => "pasta (the passt package)",
            _ => "python3"
        };
        return $"Co-op over Steam relays needs {missing}, which could not be found on PATH. {SteamUdpInstallHint(onPath, !pasta, !python)}";
    }

    static string SteamUdpInstallHint(Func<string, bool> onPath, bool pasta, bool python)
    {
        static string Packages(bool pasta, bool python, string pythonPackage) =>
            string.Join(' ', new[] { pasta ? "passt" : "", python ? pythonPackage : "" }.Where(name => name.Length > 0));
        if (onPath("pacman")) return "Install with: sudo pacman -S --needed " + Packages(pasta, python, "python") + ".";
        if (onPath("apt-get")) return "Install with: sudo apt install " + Packages(pasta, python, "python3") + ".";
        if (onPath("dnf")) return "Install with: sudo dnf install " + Packages(pasta, python, "python3") + ".";
        if (onPath("zypper")) return "Install with: sudo zypper install " + Packages(pasta, python, "python3") + ".";
        return "Install with your package manager: passt provides pasta, and Python 3 provides python3.";
    }

    static bool OnPath(string program) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
            .Any(folder => File.Exists(Path.Combine(folder, program)));

    // HELLFORGE_STEAM_UDP=1 goes to the wrapper for a Linux session only when the switch is on. When it is off, remove
    // any inherited value too: starting the launcher from a shell that set it must not open UDP by accident. A session
    // whose switch is on but which cannot open it (no pasta, no python3, no relay list) starts without it:
    // hellforge-offline.sh sets HELLFORGE_STEAM_UDP_PROBLEM, so an inherited one goes.
    public static void ConfigureLinuxSteamUdp(ProcessStartInfo start, SessionOptions options)
    {
        if (OperatingSystem.IsLinux() && options.SteamUdp) start.Environment["HELLFORGE_STEAM_UDP"] = "1";
        else start.Environment.Remove("HELLFORGE_STEAM_UDP");
        start.Environment.Remove("HELLFORGE_STEAM_UDP_PROBLEM");
    }

    // Where a Linux session's namespace keeper is noted (HELLFORGE_NETNS_KEEPER, hellforge-offline.sh): the process that
    // lets hf-coop-relays.sh turn co-op over Steam relays on while the session runs (CoopRelaysSwitch).
    public static string NetnsKeeperFile(string installRoot) => Path.Combine(installRoot, "temp", "linux-netns-keeper");

    // HELLFORGE_DIFFICULTY_FIELDWATCH (=1 turns the diagnostic on) goes to the game: the wrapper and the session
    // runner inherit it, and HostBoundedProcess gives the game a copy of the session runner's environment.
    static void ConfigureDifficultyFieldWatch(ProcessStartInfo start)
    {
        if (Environment.GetEnvironmentVariable("HELLFORGE_DIFFICULTY_FIELDWATCH") is { Length: > 0 } fieldWatch)
            start.Environment["HELLFORGE_DIFFICULTY_FIELDWATCH"] = fieldWatch;
    }

    public void Start(ISessionPaths paths, LocalProfile profile, SessionOptions options)
    {
        if (!IsSupported) throw new PlatformNotSupportedException(UnsupportedMessage);
        if (IsRunning) return;
        if (paths.InstallRoot == null) throw new InvalidOperationException("Sessions run in installations made with HellForge-Setup.");
        var linux = OperatingSystem.IsLinux();
        var prefix = linux ? WinePath.PrefixFromInstallRoot(paths.InstallRoot) : null;
        var arguments = Arguments(paths, profile, options, prefix);
        LinuxNetworkBlock.GuardSession(paths.InstallRoot!);
        if (linux && options.Sandbox && LinuxSandboxProgramProblem(paths) is { } sandboxProblem)
            throw new InvalidOperationException("The Linux sandbox could not start: " + sandboxProblem +
                ". The game was not started. To run without the sandbox, turn off Settings > Sandbox.");
        // The Windows programs in the session cannot read the Linux client's account. Use only a fresh result the
        // launcher already has: probing here would start Proton a second time, and its wineserver would hold up the
        // session wrapper. On a cache miss PlaySession's own ownership probe gets the account under Wine.
        var steamAccount = linux ? SteamAccount.Current() : null;
        var wrapper = Path.Combine(paths.InstallRoot!, "app", "linux", "hellforge-linux.sh");
        var start = new ProcessStartInfo(linux ? "bash" : paths.Session)
        {
            WorkingDirectory = paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        if (linux)
        {
            start.ArgumentList.Add(wrapper);
            start.ArgumentList.Add(paths.Session);
            ConfigureLinuxStart(start, paths, options, prefix!, steamAccount);
        }
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        // The mod loader has HellForge.Cli write a dump at the game's first crash (WriteCrashDump); the game inherits
        // the session runner's environment.
        if (options.CrashDump)
        {
            var crashDump = Path.Combine(paths.InstallRoot!, "app", "cli", "HellForge.Cli.exe");
            start.Environment["HELLFORGE_CRASH_DUMP"] = prefix == null ? crashDump : WinePath.ToWinePath(crashDump, prefix);
        }
        // The difficulty diagnostics' FieldWatch switch: a tester's HELLFORGE_DIFFICULTY_FIELDWATCH=1 reaches the game the
        // same way (the wrapper, the session runner, then HostBoundedProcess's copy of its environment).
        ConfigureDifficultyFieldWatch(start);
        // An installation keeps even temporary files inside its folder.
        var temp = Directory.CreateDirectory(Path.Combine(paths.InstallRoot!, "temp")).FullName;
        start.Environment["TEMP"] = start.Environment["TMP"] = temp;
        exitCode = null;
        controller = Process.Start(start) ?? throw new InvalidOperationException("The session runner did not start.");
    }

    // The wrapper's environment: the installation and prefix, the native account handoff, its log, and the selected
    // Proton version. The Windows programs the wrapper starts through Proton inherit all of it.
    static void ConfigureLinuxStart(ProcessStartInfo start, ISessionPaths paths, SessionOptions options, string prefix, ulong? steamAccount)
    {
        ConfigureLinuxSteamUdp(start, options);
        start.Environment["HELLFORGE_NETNS_KEEPER"] = NetnsKeeperFile(paths.InstallRoot!);
        start.Environment["INSTALL"] = paths.InstallRoot!;
        start.Environment["STEAM_COMPAT_DATA_PATH"] = prefix;
        start.Environment["HELLFORGE_LINUX_INSTALL"] = paths.InstallRoot!;
        start.Environment["HELLFORGE_LINUX_PREFIX"] = prefix;
        if (options.Sandbox) start.Environment["HELLFORGE_SANDBOX"] = "1";
        else start.Environment.Remove("HELLFORGE_SANDBOX");
        if (steamAccount is { } account)
            start.Environment[SteamAccount.EnvironmentVariable] = SteamAccount.AccountId(account).ToString(CultureInfo.InvariantCulture);
        else
            start.Environment.Remove(SteamAccount.EnvironmentVariable);
        // The wrapper has no terminal here: keep its output in logs/linux-session-wrapper.log.
        start.Environment["HELLFORGE_WRAPPER_LOG"] = "1";
        var install = new InstallLayout(paths.InstallRoot!);
        // A Steam folder the player chose outside the usual places is saved in the installation; the wrapper and the
        // Windows programs under Wine see it in the environment even when the shell cannot find Steam by itself.
        if (SteamLocator.SteamRoot(install) is { } steamRoot)
            start.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = steamRoot;
        if (SteamLocator.SelectedProtonScript(install) is { } proton)
        {
            if (!File.Exists(proton)) throw new InvalidOperationException("The selected Proton version is no longer installed. Choose an installed version in setup.");
            start.Environment["PROTON"] = proton;
        }
    }

    // The ID of the run currently named in logs/play-session.json, or null before the first run.
    public static string? LatestRunId(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SessionFile) is { } session ? SessionFiles.Text(session, "runId") : null;

    // The run folder the session runner published after `previousRunId`, or null while no new run exists yet.
    public static string? RunSince(ISessionPaths paths, string? previousRunId)
    {
        if (SessionFiles.ReadJson(paths.SessionFile) is not { } session) return null;
        var runId = SessionFiles.Text(session, "runId");
        if (runId.Length == 0 || runId == previousRunId) return null;
        var run = SessionFiles.Text(session, "run");
        if (OperatingSystem.IsLinux() && paths.InstallRoot != null && IsWineDrivePath(run))
            run = WinePath.ToHostPath(run, WinePath.PrefixFromInstallRoot(paths.InstallRoot!));
        return run.Length > 0 ? run : null;
    }

    // The newest run folder, whatever started it, or null.
    public static string? LatestRun(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SessionFile) is { } session && SessionFiles.Text(session, "run") is { Length: > 0 } run
            ? OperatingSystem.IsLinux() && paths.InstallRoot != null && IsWineDrivePath(run)
                ? WinePath.ToHostPath(run, WinePath.PrefixFromInstallRoot(paths.InstallRoot!))
                : run
            : null;

    // hellforge-linux.sh writes the reason when it refuses to start inside Flatpak, Snap or Steam's runtime container,
    // before Steam, Proton or any namespace step. The launcher and the setup probe show this line, not a generic failure.
    public const string LinuxContainerRefusalFile = "linux-container-refusal.txt";

    public static string? LinuxContainerRefusal(string installRoot)
    {
        if (!OperatingSystem.IsLinux()) return null;
        try
        {
            var path = Path.Combine(installRoot, "logs", LinuxContainerRefusalFile);
            if (!File.Exists(path)) return null;
            var reason = File.ReadAllText(path).Trim();
            return reason.Length > 0 ? reason : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static bool IsWineDrivePath(string path) => path.Length >= 3 &&
        char.ToUpperInvariant(path[0]) is 'C' or 'Z' && path[1] == ':' && path[2] is '\\' or '/';

    // The stop signal's text that ends the game at once, without asking it to close first.
    public const string StopNowSignal = "stop-now";

    // The session runner stops the game and the backend, then exits. It first asks the game to close and gives it up to
    // 30 s, so the game saves its shader cache (ShaderCache); now: true ends the game at once.
    public static void SignalStop(string runDirectory, bool now = false) =>
        File.WriteAllText(Path.Combine(runDirectory, "stop-observation.signal"), now ? StopNowSignal : "stop");

    // Used before a run directory exists, when the session runner has not started the game yet, and on Linux when closing
    // the launcher after a stop did not finish in time. The latter skips the session runner's own cleanup.
    public void Kill()
    {
        if (!IsRunning) return;
        if (OperatingSystem.IsLinux()) controller!.Kill(entireProcessTree: true);
        else controller!.Kill();
    }

    // True once, when the controller has exited; releases it.
    public bool CollectExited()
    {
        if (controller is not { HasExited: true }) return false;
        exitCode = controller.ExitCode;
        controller.Dispose();
        controller = null;
        return true;
    }

    public async Task WaitForExitAsync(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (IsRunning && DateTime.UtcNow < deadline) await Task.Delay(250);
    }
}
