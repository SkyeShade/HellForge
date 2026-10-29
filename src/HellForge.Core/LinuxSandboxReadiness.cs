using System.Runtime.InteropServices;
using System.Text.Json;

namespace HellForge.Core;

// The Linux sandbox self-test's last result. The test is bound to the HellForge build and the kernel release: an update
// of either makes the result stale, so the launcher runs the test again before it offers the switch.
public sealed record LinuxSandboxSelfTest(int SchemaVersion, string AppBuild, string KernelRelease, bool Passed, string Reason, DateTime CheckedUtc)
{
    public const int CurrentSchemaVersion = 1;
}

public static class LinuxSandboxReadiness
{
    public const string FileName = "sandbox-self-test.json";

    public static string FileFor(InstallLayout install) => Path.Combine(install.Temp, FileName);

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

    public static bool IsCurrent(LinuxSandboxSelfTest? test) => IsCurrent(test, AppBuild(), KernelRelease());

    public static bool IsCurrent(LinuxSandboxSelfTest? test, string appBuild, string kernelRelease) =>
        test is { SchemaVersion: LinuxSandboxSelfTest.CurrentSchemaVersion } current &&
        current.AppBuild == appBuild && current.KernelRelease == kernelRelease;

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

    public static void Save(InstallLayout install, bool passed, string reason, DateTime? checkedUtc = null,
        string? appBuild = null, string? kernelRelease = null)
    {
        Directory.CreateDirectory(install.Temp);
        var value = new LinuxSandboxSelfTest(LinuxSandboxSelfTest.CurrentSchemaVersion, appBuild ?? AppBuild(),
            kernelRelease ?? KernelRelease(), passed, reason.Trim(), (checkedUtc ?? DateTime.UtcNow).ToUniversalTime());
        var path = FileFor(install);
        var partial = path + ".partial";
        File.WriteAllText(partial, JsonSerializer.Serialize(value, SandboxJson.Default.LinuxSandboxSelfTest) + "\n");
        File.Move(partial, path, overwrite: true);
    }

    // Null when the self-test passed for this build and kernel; otherwise the sentence Settings > Sandbox shows. The
    // launcher and the session runner both use this, so a stale result never counts as a pass.
    public static string? Problem(InstallLayout install) => Problem(Read(install));

    public static string? Problem(LinuxSandboxSelfTest? test)
    {
        if (test == null) return "the Linux sandbox self-test has not run for this build and kernel";
        if (!IsCurrent(test)) return "the Linux sandbox self-test was run for another HellForge build or kernel";
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
