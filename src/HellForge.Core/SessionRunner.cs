using System.Diagnostics;
using System.Globalization;

namespace HellForge.Core;

// A session's choices, as the launcher's Settings page keeps them (launcher-settings.json, ProfileStore) or as another
// launcher passes them (HellForge.Cli play). The game options are already checked (GameLaunchOptions.Parse). ModSync:
// the next session's mods after a mod sync (a ModSyncSession file), for that one session. Sandbox: the game at low
// integrity (ProfileStore.LoadSandbox). FullOwnershipCheck: ask Steam even when a recent ownership check still holds
// (SteamOwnershipRecord), for that one session.
public sealed record SessionOptions(bool LocalPlayFab, bool SdkShims, string War, bool EngineLog, bool CrashDump, IReadOnlyList<string> GameOptions,
    string? ModSync = null, bool Sandbox = false, bool FullOwnershipCheck = false);

// Starts and stops the engine's session runner (HellForge.Session.exe play). It owns every safety step (verified copy,
// Steam ownership, network block, cleanup); this class only starts it and asks it to stop. It runs without a window and
// reports through the run directory's files, so it does not depend on this process staying alive to clean up.
public sealed class SessionRunner
{
    Process? controller;

    public static bool IsSupported => OperatingSystem.IsWindows();

    public static string UnsupportedMessage => OperatingSystem.IsLinux()
        ? "Linux sessions are not supported yet."
        : "Sessions are only supported on Windows for now.";

    public bool IsRunning => controller is { HasExited: false };
    public int? ProcessId => controller?.Id;
    public int? ExitCode => controller is { HasExited: true } ? controller.ExitCode : null;

    // The session runner's command line (after HellForge.Session.exe). The template only matters when the backend
    // creates the profile on first use.
    public static List<string> Arguments(ISessionPaths paths, LocalProfile profile, SessionOptions options)
    {
        if (paths.InstallRoot == null) throw new InvalidOperationException("Sessions run in installations made with HellForge-Setup.");
        var arguments = new List<string> { "play", "--install", Path.TrimEndingDirectorySeparator(paths.InstallRoot),
            "--profile", Path.TrimEndingDirectorySeparator(Path.GetFullPath(profile.Folder)), "--template", profile.Template };
        if (options.LocalPlayFab) arguments.Add("--local-playfab");
        if (options.SdkShims) arguments.Add("--multiplayer");
        arguments.AddRange(WarArguments(paths, options.War));
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

    // Whether the sandbox can be offered: an engine built with the Steam bridge, on Windows (under Wine/Proton the game's
    // block is the network namespace, and the sandbox is untested there).
    public static bool CanSandbox(ISessionPaths paths) => HasSteamBridge(paths) && !LinuxNetworkBlock.UnderWine && !LinuxNetworkBlock.Requested;

    // Whether a session is sandboxed unless told otherwise: the setting (on unless turned off), where it can be offered.
    public static bool SandboxByDefault(ISessionPaths paths) => CanSandbox(paths) && ProfileStore.LoadSandbox(paths);

    public void Start(ISessionPaths paths, LocalProfile profile, SessionOptions options)
    {
        if (!IsSupported) throw new PlatformNotSupportedException(UnsupportedMessage);
        if (IsRunning) return;
        var arguments = Arguments(paths, profile, options);
        LinuxNetworkBlock.GuardSession(paths.InstallRoot!);
        var start = new ProcessStartInfo(paths.Session)
        {
            WorkingDirectory = paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        // The mod loader has HellForge.Cli write a full dump at the game's first crash (WriteCrashDump); the game inherits
        // the session runner's environment.
        if (options.CrashDump)
            start.Environment["HELLFORGE_CRASH_DUMP"] = Path.Combine(paths.InstallRoot!, "app", "cli", "HellForge.Cli.exe");
        // An installation keeps even temporary files inside its folder.
        var temp = Directory.CreateDirectory(Path.Combine(paths.InstallRoot!, "temp")).FullName;
        start.Environment["TEMP"] = start.Environment["TMP"] = temp;
        controller = Process.Start(start) ?? throw new InvalidOperationException("The session runner did not start.");
    }

    // The run folder the session runner made for a session started at `startedUtc` (it names its newest run in
    // logs/play-session.json), or null while it has not made one yet.
    public static string? RunSince(ISessionPaths paths, DateTime startedUtc)
    {
        if (SessionFiles.ReadJson(paths.SessionFile) is not { } session) return null;
        var run = SessionFiles.Text(session, "run");
        return run.Length > 0 &&
            DateTime.TryParse(SessionFiles.Text(session, "startedUtc"), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal, out var started) &&
            started >= startedUtc.AddSeconds(-5) ? run : null;
    }

    // The newest run folder, whatever started it, or null.
    public static string? LatestRun(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SessionFile) is { } session && SessionFiles.Text(session, "run") is { Length: > 0 } run ? run : null;

    // The session runner stops the game and the backend, then exits.
    public static void SignalStop(string runDirectory) =>
        File.WriteAllText(Path.Combine(runDirectory, "stop-observation.signal"), "stop");

    // Only before a run directory exists: the session runner has not started the game yet.
    public void Kill()
    {
        if (IsRunning) controller!.Kill();
    }

    // True once, when the controller has exited; releases it.
    public bool CollectExited()
    {
        if (controller is not { HasExited: true }) return false;
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
