using System.Diagnostics;
using System.Globalization;

namespace HellForge.Core;

// A session's choices, as the launcher's Settings page keeps them (launcher-settings.json, ProfileStore) or as another
// launcher passes them (HellForge.Cli play). The game options are already checked (GameLaunchOptions.Parse). ModSync:
// the next session's mods after a mod sync (a ModSyncSession file), for that one session. Sandbox: the game at low
// integrity (ProfileStore.LoadSandbox). FullOwnershipCheck: ask Steam even when a recent ownership check still holds
// (SteamOwnershipRecord), for that one session. SteamUdp: co-op over Steam relays, UDP to Valve's relays only
// (ProfileStore.LoadSteamUdp, #62).
public sealed record SessionOptions(bool LocalPlayFab, bool SdkShims, string War, bool EngineLog, bool CrashDump, IReadOnlyList<string> GameOptions,
    string? ModSync = null, bool Sandbox = false, bool FullOwnershipCheck = false, bool SteamUdp = false);

// Starts and stops the engine's session runner (HellForge.Session.exe play). It owns every safety step (verified copy,
// Steam ownership, network block, cleanup); this class only starts it and asks it to stop. It runs without a window and
// reports through the run directory's files, so it does not depend on this process staying alive to clean up.
public sealed class SessionRunner
{
    // Linux cannot take the Windows low-integrity path (docs/linux.md): native Linux and Wine both report Windows for
    // parts of the session, so Wine is detected as well. Until the Linux sandbox exists, an explicit request refuses
    // rather than entering code whose effect there is undefined.
    public const string LinuxSandboxRefusal = "A sandboxed session cannot start because the sandbox is not available on Linux yet.";
    public const string LinuxSandboxUnavailableReason = "the sandbox is not available on Linux yet";
    public const string LinuxNotSandboxed = "This session is not sandboxed because " + LinuxSandboxUnavailableReason +
        "; the game and mods can write the player's files.";

    Process? controller;
    int? exitCode;

    public static bool IsLinuxHost => OperatingSystem.IsLinux() || LinuxNetworkBlock.UnderWine;

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
        var warArguments = WarArguments(paths, options.War);
        if (warArguments.Count == 2 && warArguments[0] == "--live-war") warArguments[1] = Map(warArguments[1]);
        arguments.AddRange(warArguments);
        if (options.EngineLog) arguments.Add("--engine-log");
        // the player's launch options, for the session runner to add to the game's command line (GameLaunchOptions)
        if (options.GameOptions.Count > 0) arguments.AddRange(["--game-options", GameLaunchOptions.CommandLine(options.GameOptions)]);
        if (options.ModSync != null) arguments.AddRange(["--mod-sync", options.ModSync]);
        if (options.Sandbox) arguments.Add("--sandbox");
        if (options.FullOwnershipCheck) arguments.Add("--full-ownership-check");
        return arguments;
    }

    // The galactic war (ProfileStore.LoadWar): the live war through the mirror, or the all-planets war; the frozen war needs nothing.
    static List<string> WarArguments(ISessionPaths paths, string war)
    {
        if (war == ProfileStore.WarLive) return ["--live-war", Path.Combine(paths.HostRoot, "live-war.json")];
        if (war != ProfileStore.WarOpen) return [];
        var arguments = new List<string> { "--open-war" };
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

    // Whether the sandbox can be offered: an engine built with the Steam bridge, on Windows for now (not yet implemented
    // on Linux, native or under Wine/Proton, where the game's block is the network namespace).
    public static bool CanSandbox(ISessionPaths paths) =>
        HasSteamBridge(paths) && OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine && !LinuxNetworkBlock.Requested;

    // Whether a session is sandboxed unless told otherwise: the setting (on unless turned off), where it can be offered.
    public static bool SandboxByDefault(ISessionPaths paths) => CanSandbox(paths) && ProfileStore.LoadSandbox(paths);

    // The refusal for an explicit sandbox request on Linux. The booleans are injected so the native and Wine cases can be
    // tested on either build host; Windows returns null and keeps its existing sandbox path.
    public static string? SandboxRefusal(bool requested, bool linux, bool underWine) =>
        requested && (linux || underWine) ? LinuxSandboxRefusal : null;

    public static void RefuseSandboxOnLinux(bool requested)
    {
        if (SandboxRefusal(requested, OperatingSystem.IsLinux(), LinuxNetworkBlock.UnderWine) is { } refusal)
            throw new InvalidDataException(refusal);
    }

    // What #62's HELLFORGE_STEAM_UDP=1 path runs: pasta (the passt package) and python3 (Valve's relay list). Null when
    // both are on PATH; otherwise the refusal, with the install command for this system's package manager. onPath is
    // injectable for tests.
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
    // any inherited value too: starting the launcher from a shell that set it must not open UDP by accident.
    public static void ConfigureLinuxSteamUdp(ProcessStartInfo start, SessionOptions options)
    {
        if (OperatingSystem.IsLinux() && options.SteamUdp) start.Environment["HELLFORGE_STEAM_UDP"] = "1";
        else start.Environment.Remove("HELLFORGE_STEAM_UDP");
    }

    // Refuse before the wrapper starts: #62 cannot open UDP without pasta, and its relay list needs python3.
    static void CheckLinuxSteamUdp(bool linux, SessionOptions options)
    {
        if (linux && options.SteamUdp && SteamUdpProblem() is { } problem) throw new InvalidOperationException(problem);
    }

    // HELLFORGE_DIFFICULTY_FIELDWATCH=0 goes to the game: the wrapper and the session runner inherit it, and
    // HostBoundedProcess gives the game a copy of the session runner's environment.
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
        RefuseSandboxOnLinux(options.Sandbox);
        var linux = OperatingSystem.IsLinux();
        var prefix = linux ? WinePath.PrefixFromInstallRoot(paths.InstallRoot) : null;
        var arguments = Arguments(paths, profile, options, prefix);
        LinuxNetworkBlock.GuardSession(paths.InstallRoot!);
        CheckLinuxSteamUdp(linux, options);
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
            ConfigureLinuxSteamUdp(start, options);
            start.Environment["INSTALL"] = paths.InstallRoot!;
            start.Environment["STEAM_COMPAT_DATA_PATH"] = prefix!;
            // The wrapper has no terminal here: keep its output in logs/linux-session-wrapper.log.
            start.Environment["HELLFORGE_WRAPPER_LOG"] = "1";
            if (SteamLocator.SelectedProtonScript(new InstallLayout(paths.InstallRoot!)) is { } proton)
            {
                if (!File.Exists(proton)) throw new InvalidOperationException("The selected Proton version is no longer installed. Choose an installed version in setup.");
                start.Environment["PROTON"] = proton;
            }
        }
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        // The mod loader has HellForge.Cli write a full dump at the game's first crash (WriteCrashDump); the game inherits
        // the session runner's environment.
        if (options.CrashDump)
        {
            var crashDump = Path.Combine(paths.InstallRoot!, "app", "cli", "HellForge.Cli.exe");
            start.Environment["HELLFORGE_CRASH_DUMP"] = prefix == null ? crashDump : WinePath.ToWinePath(crashDump, prefix);
        }
        // The difficulty diagnostics' FieldWatch switch: a tester's HELLFORGE_DIFFICULTY_FIELDWATCH=0 reaches the game the
        // same way (the wrapper, the session runner, then HostBoundedProcess's copy of its environment).
        ConfigureDifficultyFieldWatch(start);
        // An installation keeps even temporary files inside its folder.
        var temp = Directory.CreateDirectory(Path.Combine(paths.InstallRoot!, "temp")).FullName;
        start.Environment["TEMP"] = start.Environment["TMP"] = temp;
        exitCode = null;
        controller = Process.Start(start) ?? throw new InvalidOperationException("The session runner did not start.");
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

    static bool IsWineDrivePath(string path) => path.Length >= 3 &&
        char.ToUpperInvariant(path[0]) is 'C' or 'Z' && path[1] == ':' && path[2] is '\\' or '/';

    // The session runner stops the game and the backend, then exits.
    public static void SignalStop(string runDirectory) =>
        File.WriteAllText(Path.Combine(runDirectory, "stop-observation.signal"), "stop");

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
