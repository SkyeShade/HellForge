using System.Runtime.InteropServices;
using System.Text.Json;

namespace HellForge.Core;

// The Linux sandbox self-test's last result. Schema 2 adds the mode and the exact runtime it tested: a host record must
// not count for a container session, and a runtime update changes RuntimeIdentity and makes the record stale.
public sealed record LinuxSandboxSelfTest(
    int SchemaVersion, string AppBuild, string KernelRelease,
    string? Mode, string? RuntimeName, string? RuntimeVersion, string? RuntimeIdentity,
    bool Passed, string Reason, DateTime CheckedUtc)
{
    public const int CurrentSchemaVersion = 2;
}

// temp/sandbox-self-test-mode.json, written by hellforge-linux.sh after the self-test in the layering it actually used.
public sealed record LinuxSandboxSelfTestMode(
    int SchemaVersion, string Mode, string RuntimeName, string RuntimeVersion, string RuntimeIdentity);

public static class LinuxSandboxReadiness
{
    public const string FileName = "sandbox-self-test.json";
    public const string ModeFileName = "sandbox-self-test-mode.json";
    public const int ModeSchemaVersion = 1;

    public static string FileFor(InstallLayout install) => Path.Combine(install.Temp, FileName);
    public static string ModeFileFor(InstallLayout install) => Path.Combine(install.Temp, ModeFileName);

    public static string KernelRelease()
    {
        try
        {
            var path = LinuxNetworkBlock.UnderWine ? @"Z:\proc\sys\kernel\osrelease" : "/proc/sys/kernel/osrelease";
            var release = File.ReadAllText(path).Trim();
            if (release.Length > 0) return release;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return RuntimeInformation.OSDescription;
    }

    public static string AppBuild() => AppVersion.Text;

    // The build string the installed app itself reports: version and commit from the package's app-build.json. The
    // native CLI can be from a different publish than the Wine runner, so LinuxSandboxReadiness.AppBuild() is not used
    // here.
    public static string InstalledAppBuild(InstallLayout install)
    {
        var path = Path.Combine(install.App, "app-build.json");
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            var version = root.ValueKind == JsonValueKind.Object && root.TryGetProperty("version", out var value) &&
                value.ValueKind == JsonValueKind.String ? value.GetString()?.Trim() : null;
            if (string.IsNullOrEmpty(version))
                throw new InvalidDataException("The installed app-build.json has no version; reinstall HellForge.");
            var commit = root.TryGetProperty("commit", out var revision) && revision.ValueKind == JsonValueKind.String
                ? revision.GetString() ?? "" : "";
            return AppVersion.FromParts(version!, commit);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            throw new InvalidDataException("The installed app-build.json could not be read: " + error.Message, error);
        }
    }

    // The mode this session will use. The wrapper exports it after its one resolution; a caller outside the wrapper
    // resolves the selected Proton and Steam folder with the same C# resolver.
    public static LinuxSandboxSelfTestMode ExpectedMode(InstallLayout install)
    {
        var mode = Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_MODE");
        if (mode == "host") return HostMode();
        if (mode == "container")
        {
            var identity = Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_IDENTITY") ?? "";
            return new(ModeSchemaVersion, "container",
                Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_NAME") ?? "",
                Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME_VERSION") ?? "",
                identity.Length == 0 ? "container" : identity);
        }
        try
        {
            var proton = Environment.GetEnvironmentVariable("PROTON");
            if (string.IsNullOrWhiteSpace(proton)) proton = SteamLocator.SelectedProtonScript(install) ?? SteamLocator.ProtonScript();
            var steam = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH");
            if (string.IsNullOrWhiteSpace(steam)) steam = SteamLocator.SteamRoot(install);
            if (string.IsNullOrWhiteSpace(proton) || string.IsNullOrWhiteSpace(steam)) return HostMode();
            return FromRuntime(SteamRuntimeLocator.Resolve(proton!, steam!));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException)
        {
            return HostMode();
        }
    }

    static LinuxSandboxSelfTestMode HostMode() => new(ModeSchemaVersion, "host", "", "", "host");

    static LinuxSandboxSelfTestMode FromRuntime(SteamRuntime runtime) => runtime.Mode == SteamRuntimeMode.Container
        ? new(ModeSchemaVersion, "container", runtime.Name ?? "", runtime.Version ?? "unknown", runtime.Identity)
        : HostMode();

    public static bool IsCurrent(InstallLayout install, LinuxSandboxSelfTest? test)
    {
        var expected = ExpectedMode(install);
        return IsCurrent(test, AppBuild(), KernelRelease(), expected.Mode, expected.RuntimeIdentity);
    }

    public static bool IsCurrent(LinuxSandboxSelfTest? test, string appBuild, string kernelRelease, string mode,
        string runtimeIdentity) =>
        test is { SchemaVersion: LinuxSandboxSelfTest.CurrentSchemaVersion } current &&
        current.AppBuild == appBuild && current.KernelRelease == kernelRelease &&
        current.Mode == mode && current.RuntimeIdentity == runtimeIdentity;

    // The launcher and the native readiness command show the same first reason: the last hf-sandbox line, else the last
    // output line, else the exit code.
    public static string SelfTestReason(string output, int exitCode)
    {
        var lines = output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return lines.LastOrDefault(line => line.StartsWith("hf-sandbox:", StringComparison.Ordinal))
            ?? lines.LastOrDefault() ?? $"exit code {exitCode}";
    }

    public static LinuxSandboxSelfTest? Read(InstallLayout install)
    {
        try
        {
            var path = FileFor(install);
            if (!File.Exists(path)) return null;
            return JsonSerializer.Deserialize(File.ReadAllText(path), SandboxJson.Default.LinuxSandboxSelfTest);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // The mode file is written by the wrapper after the self-test. Missing or unreadable is not a host pass.
    public static LinuxSandboxSelfTestMode? ReadMode(InstallLayout install)
    {
        try
        {
            var path = ModeFileFor(install);
            if (!File.Exists(path)) return null;
            var mode = JsonSerializer.Deserialize(File.ReadAllText(path), SandboxJson.Default.LinuxSandboxSelfTestMode);
            if (mode == null || mode.SchemaVersion != ModeSchemaVersion || mode.Mode is not ("host" or "container")) return null;
            return mode with { RuntimeName = mode.RuntimeName ?? "", RuntimeVersion = mode.RuntimeVersion ?? "",
                RuntimeIdentity = mode.RuntimeIdentity ?? "" };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // The CLI turns the self-test's exit status and the mode file into the one record. A wrapper that exits 0 without a
    // mode file is a failure; so is a container mode whose runtime cannot be identified (no readable VERSIONS.txt, so no
    // hash). A readable VERSIONS.txt whose version did not parse is "unknown" and still counts: the hash identifies it.
    public static (LinuxSandboxSelfTestMode Mode, bool Passed, string Reason) EvaluateSelfTest(InstallLayout install,
        int exitCode, string reason)
    {
        var expected = ExpectedMode(install);
        var metadata = ReadMode(install);
        if (metadata == null) return (expected, false, "the self-test did not report its runtime mode");
        if (metadata.Mode != expected.Mode || metadata.RuntimeIdentity != expected.RuntimeIdentity)
            return (metadata, false, expected.Mode == "container"
                ? "the self-test ran in host mode, but this session uses the Steam Linux Runtime container"
                : "the self-test ran in container mode, but this session uses host mode");
        if (metadata.Mode == "container" && RuntimeUnidentified(metadata.RuntimeIdentity))
            return (metadata, false, "the Steam Linux Runtime self-test could not identify the runtime (VERSIONS.txt is missing or unreadable)");
        if (exitCode != 0) return (metadata, false, reason);
        return (metadata, true, "");
    }

    public static void Save(InstallLayout install, bool passed, string reason, DateTime? checkedUtc = null,
        string? appBuild = null, string? kernelRelease = null, LinuxSandboxSelfTestMode? mode = null)
    {
        Directory.CreateDirectory(install.Temp);
        var chosen = mode ?? HostMode();
        var value = new LinuxSandboxSelfTest(LinuxSandboxSelfTest.CurrentSchemaVersion, appBuild ?? AppBuild(),
            kernelRelease ?? KernelRelease(), chosen.Mode, chosen.RuntimeName, chosen.RuntimeVersion,
            chosen.RuntimeIdentity, passed, reason.Trim(), (checkedUtc ?? DateTime.UtcNow).ToUniversalTime());
        var path = FileFor(install);
        var partial = path + ".partial";
        File.WriteAllText(partial, JsonSerializer.Serialize(value, SandboxJson.Default.LinuxSandboxSelfTest) + "\n");
        File.Move(partial, path, overwrite: true);
    }

    // Null when the self-test passed for this build, kernel, mode and runtime; otherwise the sentence Settings > Sandbox
    // and a refused session show. The launcher and the session runner both use this.
    public static string? Problem(InstallLayout install) => Problem(Read(install), ExpectedMode(install));

    public static string? Problem(InstallLayout install, LinuxSandboxSelfTestMode expected) => Problem(Read(install), expected);

    // SteamRuntime.Identity is "<folder>|<VERSIONS.txt SHA-256>", or "<folder>|no-hash" when the file could not be read.
    static bool RuntimeUnidentified(string? identity) => string.IsNullOrWhiteSpace(identity) ||
        identity.EndsWith("|no-hash", StringComparison.Ordinal);

    public static string? Problem(LinuxSandboxSelfTest? test, LinuxSandboxSelfTestMode expected,
        string? appBuild = null, string? kernelRelease = null)
    {
        if (test == null) return "the Linux sandbox self-test has not run for this build and kernel";
        if (test.SchemaVersion != LinuxSandboxSelfTest.CurrentSchemaVersion)
            return "the Linux sandbox self-test record uses an old schema; run the self-test again";
        if (test.AppBuild != (appBuild ?? AppBuild()) || test.KernelRelease != (kernelRelease ?? KernelRelease()))
            return "the Linux sandbox self-test was run for another HellForge build or kernel";
        if (test.Mode != expected.Mode)
            return expected.Mode == "container"
                ? "the Linux sandbox self-test was run in host mode, but this session uses the Steam Linux Runtime container"
                : "the Linux sandbox self-test was run in container mode, but this session uses host mode";
        if (test.RuntimeIdentity != expected.RuntimeIdentity)
            return "the Linux sandbox self-test belongs to another Steam Linux Runtime; run the self-test again";
        if (test.Mode == "container" && RuntimeUnidentified(test.RuntimeIdentity))
            return "the Linux sandbox self-test could not identify the Steam Linux Runtime (VERSIONS.txt is missing or unreadable)";
        return test.Passed ? null : string.IsNullOrWhiteSpace(test.Reason) ? "the Linux sandbox self-test failed" : test.Reason;
    }

    public static string ProgramProblem(InstallLayout install)
    {
        var sandbox = Path.Combine(install.App, "linux", "hf-sandbox");
        if (!File.Exists(sandbox)) return "the Linux sandbox program is missing (" + sandbox + ")";
        if (!OperatingSystem.IsLinux()) return "the Linux sandbox needs Linux";
        UnixFileMode mode;
        try { mode = File.GetUnixFileMode(sandbox); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "the Linux sandbox program could not be read (" + sandbox + ")";
        }
        return (mode & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) != 0
            ? "" : "the Linux sandbox program is not executable (" + sandbox + ")";
    }
}
