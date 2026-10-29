using System.Globalization;
using System.Text.Json;

namespace HellForge.Core;

// The fields the trusted runner sends across the Wine/native boundary and the sanitized launch spec keeps. Keeping one
// definition also keeps their JSON shapes together.
public abstract class SandboxWineSpec
{
    public int SchemaVersion { get; init; } = 1;
    public string RunId { get; init; } = "";
    public string InstallRootWine { get; init; } = "";
    public string GameOutWine { get; init; } = "";
    public string GameExeWine { get; init; } = "";
    public string GameDllWine { get; init; } = "";
    public string BundleDirectoryWine { get; init; } = "";
    public string WorkingDirectoryWine { get; init; } = "";
    public string AppRootWine { get; init; } = "";
    public string BackendOrigin { get; init; } = "";
    public int RelayPort { get; init; }
    public string RelayToken { get; init; } = "";
    public bool LocalPlayFab { get; init; }
    public bool NativeMods { get; init; }
    public List<string> NativeFolders { get; init; } = [];
    public List<string> GameArguments { get; init; } = [];
    public Dictionary<string, string> Environment { get; init; } = new(StringComparer.Ordinal);
}

// The trusted Wine runner writes this request; the native supervisor validates every field before it starts hf-sandbox.
// The host paths are for validation and mounts. The *Wine fields are what the domain host uses inside the overlay.
public sealed class SandboxStartRequest : SandboxWineSpec
{
    public string Nonce { get; init; } = "";
    public string InstallRoot { get; init; } = "";
    public string DomainHost { get; init; } = "";
    public string GameExe { get; init; } = "";
    public string GameOut { get; init; } = "";
    public List<string> ReadOnlyBinds { get; init; } = [];
    public int RunnerPid { get; init; }
    public string RunnerStartedUtc { get; init; } = "";
}

// The sanitized file the native supervisor leaves in gameOut for the domain host. It has no host-only fields.
public sealed class SandboxLaunchSpec : SandboxWineSpec
{
}

public sealed class SandboxStartPlan
{
    public required InstallLayout Install { get; init; }
    public required string Run { get; init; }
    public required string GameOut { get; init; }
    public required string GameExe { get; init; }
    public required string DomainHost { get; init; }
    public required string HfSandbox { get; init; }
    public required string LaunchSpec { get; init; }
    public required IReadOnlyList<string> ReadOnlyBinds { get; init; }
    public required SandboxLaunchSpec Launch { get; init; }
}

public static class SandboxProtocol
{
    public const int SchemaVersion = 1;
    public const int MaxReadOnlyBinds = 64;
    public const int MaxGameArguments = 128;
    public const int MaxEnvironmentEntries = 32;
    public const int MaxText = 4096;

    public static readonly IReadOnlySet<string> EnvironmentKeys = new HashSet<string>(StringComparer.Ordinal)
    {
        "APPDATA", "LOCALAPPDATA", "TEMP", "TMP",
        "HELLFORGE_MODLOADER", "HELLFORGE_MODLOADER_PLAN", "HELLFORGE_SHIM_LOG_DIR",
        "HELLFORGE_CLOUD_DIR", "HELLFORGE_CLOUD_LOG", "HELLFORGE_NETGATE", "HELLFORGE_NETGATE_LOG",
        "HELLFORGE_SANDBOX_STATS", "HELLFORGE_DIFFICULTY_FIELDWATCH", "HELLFORGE_PIPE",
        "SteamGameId", "SteamAppId"
    };

    static readonly char[] Control = ['\0', '\r', '\n'];

    public static string RunId(string? value)
    {
        if (value is not { Length: 32 } || value!.Any(c => !char.IsAsciiHexDigit(c))) throw new InvalidDataException("The sandbox request has no valid run id.");
        return value!.ToLowerInvariant();
    }

    public static string Nonce(string? value)
    {
        if (value is not { Length: 32 } || value!.Any(c => !char.IsAsciiHexDigit(c))) throw new InvalidDataException("The sandbox request has no valid nonce.");
        return value!.ToLowerInvariant();
    }

    public static string HostPath(string? value, string prefix, string what)
    {
        Text(value, what);
        var text = value!;
        try
        {
            var path = text.Length >= 3 && char.IsAsciiLetter(text[0]) && text[1] == ':' && text[2] is '\\' or '/'
                ? WinePath.ToHostPath(text, prefix) : text;
            return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        }
        catch (Exception error) when (error is ArgumentException or IOException or InvalidDataException or NotSupportedException)
        {
            throw new InvalidDataException("The sandbox request's " + what + " path is not usable: " + error.Message);
        }
    }

    public static void Text(string? value, string what)
    {
        if (string.IsNullOrWhiteSpace(value) || value!.Length > MaxText || value!.IndexOfAny(Control) >= 0)
            throw new InvalidDataException("The sandbox request's " + what + " is empty or not one line.");
    }

    public static void Argument(string? value, string what)
    {
        if (value is null || value.Length > MaxText || value.IndexOfAny(Control) >= 0)
            throw new InvalidDataException("The sandbox request's " + what + " is not one bounded argument.");
    }

    public static string RelativePath(string? value, string what)
    {
        Text(value, what);
        var text = value!;
        if (Path.IsPathRooted(text) || text.Split('/', '\\').Any(part => part is ".." or "."))
            throw new InvalidDataException("The sandbox request's " + what + " is not a relative path.");
        return text.Replace('\\', '/');
    }

    public static SandboxStartPlan Validate(InstallLayout install, string run, string prefix, SandboxStartRequest request)
    {
        if (request.SchemaVersion != SchemaVersion) throw new InvalidDataException("The sandbox request has an unsupported schema version.");
        var runId = RunId(request.RunId);
        Nonce(request.Nonce);
        ValidateRelay(request, "sandbox request");
        ValidateRunner(request);
        var paths = ValidatePaths(install, run, prefix, request, runId);
        var binds = ValidateBinds(install, prefix, request.ReadOnlyBinds);
        ValidateArguments(request);
        ValidateNativeFolders(install, prefix, request.NativeFolders);
        var launch = LaunchSpec(request, runId);
        return new()
        {
            Install = install, Run = paths.Run, GameOut = paths.GameOut, GameExe = paths.GameExe, DomainHost = paths.DomainHost,
            HfSandbox = paths.HfSandbox, LaunchSpec = Path.Combine(paths.GameOut, "domain", "launch.json"), ReadOnlyBinds = binds, Launch = launch
        };
    }

    sealed class Paths
    {
        public required string Run { get; init; }
        public required string GameOut { get; init; }
        public required string GameExe { get; init; }
        public required string DomainHost { get; init; }
        public required string HfSandbox { get; init; }
    }

    static void ValidateRelay(SandboxWineSpec spec, string what)
    {
        if (spec.RelayPort is < 1 or > 65535) throw new InvalidDataException("The " + what + " has no valid pipe relay port.");
        if (!SandboxPipeRelay.IsToken(spec.RelayToken)) throw new InvalidDataException("The " + what + " has no valid pipe relay token.");
    }

    static void ValidateRunner(SandboxStartRequest request)
    {
        if (request.RunnerPid <= 0 || !DateTime.TryParse(request.RunnerStartedUtc, CultureInfo.InvariantCulture,
                DateTimeStyles.RoundtripKind, out _)) throw new InvalidDataException("The sandbox request has no valid runner identity.");
    }

    static Paths ValidatePaths(InstallLayout install, string run, string prefix, SandboxStartRequest request, string runId)
    {
        var expectedRun = Path.TrimEndingDirectorySeparator(Path.Combine(install.Sessions, runId));
        run = Path.TrimEndingDirectorySeparator(Path.GetFullPath(run));
        if (!string.Equals(run, expectedRun, StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox request's run folder is not this installation's logs/sessions/<id> folder.");
        if (!Directory.Exists(run)) throw new InvalidDataException("The sandbox request's run folder is missing.");
        if (!string.Equals(HostPath(request.InstallRoot, prefix, "installation"), Path.TrimEndingDirectorySeparator(install.Root), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox request names another installation.");
        var gameOut = HostPath(request.GameOut, prefix, "gameOut");
        if (!string.Equals(gameOut, Path.Combine(run, "game"), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox request's gameOut is not <run>/game.");
        if (!Directory.Exists(gameOut)) throw new InvalidDataException("The sandbox request's gameOut is missing.");
        var exe = HostPath(request.GameExe, prefix, "game executable");
        if (!string.Equals(exe, Path.Combine(install.Game, "bin", "helldivers2.exe"), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox request's game executable is not this installation's copied game.");
        if (!File.Exists(exe)) throw new InvalidDataException("The sandbox request's game executable is missing.");
        var domainHost = HostPath(request.DomainHost, prefix, "domain host");
        if (!string.Equals(domainHost, Path.Combine(install.App, "session", "HellForge.Session.exe"), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox request's domain host is not the installed session runner.");
        if (!File.Exists(domainHost)) throw new InvalidDataException("The sandbox request's domain host is missing.");
        var hfSandbox = Path.Combine(install.App, "linux", "hf-sandbox");
        SandboxProgram(hfSandbox);
        return new() { Run = run, GameOut = gameOut, GameExe = exe, DomainHost = domainHost, HfSandbox = hfSandbox };
    }

    static void SandboxProgram(string path)
    {
        if (!File.Exists(path)) throw new InvalidDataException("The Linux sandbox program is missing: " + path);
        if (!OperatingSystem.IsLinux()) throw new InvalidDataException("The native sandbox supervisor needs Linux.");
        if ((File.GetUnixFileMode(path) & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) == 0)
            throw new InvalidDataException("The Linux sandbox program is not executable: " + path);
    }

    static List<string> ValidateBinds(InstallLayout install, string prefix, List<string>? values)
    {
        if (values == null || values.Count is 0 or > MaxReadOnlyBinds)
            throw new InvalidDataException("The sandbox request has no bounded read-only bind list.");
        var binds = new List<string>();
        foreach (var value in values)
        {
            var path = HostPath(value, prefix, "read-only bind");
            if (binds.Contains(path, StringComparer.Ordinal)) throw new InvalidDataException("The sandbox request names a read-only bind twice: " + path);
            if (!AllowedBind(install, path)) throw new InvalidDataException("The sandbox request asks for a read-only bind outside the mod, loader and session-host folders: " + path);
            binds.Add(path);
        }
        if (!binds.Contains(Path.Combine(install.App, "session"), StringComparer.Ordinal))
            throw new InvalidDataException("The sandbox request does not expose the installed session host.");
        var loader = Path.Combine(install.App, "sdk-shims", "hellforge_modloader.dll");
        if (File.Exists(loader) && !binds.Contains(loader, StringComparer.Ordinal))
            throw new InvalidDataException("The sandbox request does not expose the installed mod loader.");
        return binds;
    }

    static void ValidateArguments(SandboxStartRequest request)
    {
        var arguments = request.GameArguments;
        if (arguments == null || arguments.Count > MaxGameArguments)
            throw new InvalidDataException("The sandbox request has too many game arguments.");
        foreach (var argument in arguments) Argument(argument, "game argument");
        var environment = request.Environment;
        if (environment == null || environment.Count > MaxEnvironmentEntries)
            throw new InvalidDataException("The sandbox request has too many environment values.");
        foreach (var (key, value) in environment)
        {
            if (!EnvironmentKeys.Contains(key)) throw new InvalidDataException("The sandbox request asks for an environment variable outside the allowlist: " + key);
            Text(value, "environment value " + key);
        }
    }

    static void ValidateNativeFolders(InstallLayout install, string prefix, List<string>? folders)
    {
        if (folders == null || folders.Count > MaxReadOnlyBinds)
            throw new InvalidDataException("The sandbox request has too many native mod folders.");
        foreach (var value in folders)
        {
            Text(value, "native mod folder");
            var path = HostPath(value, prefix, "native mod folder");
            if (!AllowedBind(install, path))
                throw new InvalidDataException("The sandbox request names a native mod folder outside the mod and build folders: " + path);
        }
    }

    static SandboxLaunchSpec LaunchSpec(SandboxStartRequest request, string runId)
    {
        foreach (var (name, value) in new[]
                 {
                     ("game executable (Wine)", request.GameExeWine), ("installation (Wine)", request.InstallRootWine),
                     ("gameOut (Wine)", request.GameOutWine), ("game.dll (Wine)", request.GameDllWine),
                     ("bundle folder (Wine)", request.BundleDirectoryWine), ("working folder (Wine)", request.WorkingDirectoryWine),
                     ("app folder (Wine)", request.AppRootWine)
                 }) Text(value, name);
        if (!Uri.TryCreate(request.BackendOrigin, UriKind.Absolute, out var backend) || backend.Scheme != "http" ||
            backend.Host != "127.0.0.1" || backend.Port is < 1 or > 65535 || backend.AbsolutePath != "/")
            throw new InvalidDataException("The sandbox request's backend origin is not a loopback HTTP origin.");
        return new()
        {
            SchemaVersion = SchemaVersion, RunId = runId, InstallRootWine = request.InstallRootWine,
            GameOutWine = request.GameOutWine, GameExeWine = request.GameExeWine, GameDllWine = request.GameDllWine,
            BundleDirectoryWine = request.BundleDirectoryWine, WorkingDirectoryWine = request.WorkingDirectoryWine,
            AppRootWine = request.AppRootWine, BackendOrigin = request.BackendOrigin.TrimEnd('/'), RelayPort = request.RelayPort,
            RelayToken = request.RelayToken, LocalPlayFab = request.LocalPlayFab,
            NativeMods = request.NativeMods, NativeFolders = [.. request.NativeFolders], GameArguments = [.. request.GameArguments],
            Environment = new(request.Environment, StringComparer.Ordinal)
        };
    }

    // Keep in step with hf_extra_bind_allowed in native/linux-sandbox/hf-sandbox-view.c: both lists must change together.
    static bool AllowedBind(InstallLayout install, string path)
    {
        try { LocalInstall.NoLinks(path, install.Root); }
        catch (Exception error) when (error is InvalidDataException or ArgumentException or IOException or UnauthorizedAccessException) { return false; }
        if (string.Equals(path, Path.Combine(install.App, "session"), StringComparison.Ordinal)) return Directory.Exists(path);
        if (string.Equals(path, Path.Combine(install.App, "sdk-shims", "hellforge_modloader.dll"), StringComparison.Ordinal)) return File.Exists(path);
        var mods = Path.TrimEndingDirectorySeparator(Path.Combine(install.Mods));
        var builds = Path.TrimEndingDirectorySeparator(Path.Combine(install.Temp, "mod-builds"));
        if (!Within(mods, path) && !Within(builds, path)) return false;
        return File.Exists(path) || Directory.Exists(path);
    }

    public static bool Within(string root, string path)
    {
        root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        return path.Equals(root, StringComparison.Ordinal) ||
            path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal);
    }

    public static SandboxLaunchSpec ReadLaunch(string path)
    {
        var launch = JsonSerializer.Deserialize(File.ReadAllText(path), SandboxJson.Default.SandboxLaunchSpec)
            ?? throw new InvalidDataException("The sandbox launch spec is empty.");
        if (launch.SchemaVersion != SchemaVersion) throw new InvalidDataException("The sandbox launch spec has an unsupported schema version.");
        RunId(launch.RunId);
        ValidateRelay(launch, "sandbox launch spec");
        foreach (var (name, value) in new[]
                 {
                     ("installation", launch.InstallRootWine), ("gameOut", launch.GameOutWine), ("game executable", launch.GameExeWine),
                     ("game.dll", launch.GameDllWine), ("bundle folder", launch.BundleDirectoryWine),
                     ("working folder", launch.WorkingDirectoryWine), ("app folder", launch.AppRootWine), ("backend origin", launch.BackendOrigin)
                 }) Text(value, name);
        var nativeFolders = launch.NativeFolders;
        if (nativeFolders == null || nativeFolders.Count > MaxReadOnlyBinds)
            throw new InvalidDataException("The sandbox launch spec has too many native mod folders.");
        foreach (var folder in nativeFolders) Text(folder, "native mod folder");
        var arguments = launch.GameArguments;
        if (arguments == null || arguments.Count > MaxGameArguments)
            throw new InvalidDataException("The sandbox launch spec has too many game arguments.");
        foreach (var argument in arguments) Argument(argument, "game argument");
        var environment = launch.Environment;
        if (environment == null || environment.Count > MaxEnvironmentEntries)
            throw new InvalidDataException("The sandbox launch spec has too many environment values.");
        foreach (var (key, value) in environment)
        {
            if (!EnvironmentKeys.Contains(key)) throw new InvalidDataException("The sandbox launch spec asks for an environment variable outside the allowlist: " + key);
            Text(value, "environment value " + key);
        }
        return launch;
    }
}
