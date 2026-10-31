using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Why a Sync Inventory capture did not start, in words a player can act on. The capture (HellForge.CaptureProxy) stops
// before it listens with one of the exit codes below and writes capture-failure.json beside proxy.log with the detail
// (which program holds port 443, what Windows said about the hosts file). The launcher adds what only it sees: the
// administrator prompt declined, Windows refusing to start the file, the capture ending before it said anything, or
// not getting ready at all. Security programs are named from Windows Security Center (SecurityProducts), so a player
// with Kaspersky, Avast or the like is told which program to allow and the exact file, at the moment it happens.
// The capture compiles this file in as well, so it uses nothing else from HellForge.Core.
public enum CaptureProblem
{
    None,
    // The launcher's side.
    Declined, Blocked, Missing, Stopped, Crashed, NotReady,
    // The capture's side, each with its exit code (ExitCode).
    EvidenceFolder, LeftoverCapture, HostsBusy, HostsDenied, Offline, Certificate, PortInUse, PortDenied
}

public sealed record CaptureFailure(CaptureProblem Problem, string? Detail);

public static class CaptureStart
{
    public const string FailureFile = "capture-failure.json";
    // An unhandled .NET exception (the capture logs it as "crashed: ..." in proxy.log).
    public const int UnhandledException = unchecked((int)0xE0434352);
    const int FirstCode = 20;

    static readonly CaptureProblem[] ProxyProblems =
    [
        CaptureProblem.EvidenceFolder, CaptureProblem.LeftoverCapture, CaptureProblem.HostsBusy, CaptureProblem.HostsDenied,
        CaptureProblem.Offline, CaptureProblem.Certificate, CaptureProblem.PortInUse, CaptureProblem.PortDenied
    ];

    // 20 and up: a capture that stopped before listening. 0, 1 and 2 keep their meaning (done, nothing captured, bad arguments).
    public static int ExitCode(CaptureProblem problem) =>
        Array.IndexOf(ProxyProblems, problem) is var index and >= 0 ? FirstCode + index : 1;

    public static CaptureProblem FromExitCode(int code) =>
        code - FirstCode is var index and >= 0 && index < ProxyProblems.Length ? ProxyProblems[index] : CaptureProblem.None;

    public static void Write(string syncDir, CaptureProblem problem, string? detail)
    {
        var failure = new JsonObject { ["problem"] = problem.ToString(), ["detail"] = detail };
        File.WriteAllText(Path.Combine(syncDir, FailureFile), failure.ToJsonString() + "\n");
    }

    public static CaptureFailure? Read(string syncDir)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(syncDir, FailureFile)));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("problem", out var name) || name.ValueKind != JsonValueKind.String ||
                !Enum.TryParse<CaptureProblem>(name.GetString(), out var problem) || problem == CaptureProblem.None) return null;
            var detail = root.TryGetProperty("detail", out var text) && text.ValueKind == JsonValueKind.String ? text.GetString() : null;
            return new CaptureFailure(problem, detail);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    // Windows' answer when the launcher asked it to start the capture elevated (Win32Exception.NativeErrorCode).
    public static CaptureProblem FromStartError(int win32Error) => win32Error switch
    {
        1223 => CaptureProblem.Declined,          // ERROR_CANCELLED: the administrator prompt was answered No
        2 or 3 => CaptureProblem.Missing,         // the file is gone (a security program's quarantine)
        _ => CaptureProblem.Blocked               // 5 access denied, 225/226 virus, 4551 application control policy, ...
    };

    // A capture that ended before it listened: its own failure file, else its exit code, else how it ended.
    public static CaptureFailure FromExit(int exitCode, CaptureFailure? written, string? proxyLog)
    {
        if (written != null) return written;
        if (FromExitCode(exitCode) is var known and not CaptureProblem.None) return new CaptureFailure(known, null);
        var crashed = proxyLog?.Split('\n').Select(line => line.TrimEnd('\r')).LastOrDefault(line => line.Contains(" crashed: ", StringComparison.Ordinal));
        if (exitCode == UnhandledException || crashed != null)
            return new CaptureFailure(CaptureProblem.Crashed, crashed?[(crashed.IndexOf(" crashed: ", StringComparison.Ordinal) + 10)..]);
        // Ended without a reason of its own: stopped from outside (exit 1 from a kill, an access violation, ...) or before
        // it could write a line.
        return new CaptureFailure(CaptureProblem.Stopped, string.IsNullOrWhiteSpace(proxyLog)
            ? $"it wrote nothing (exit code 0x{exitCode:X8})" : $"exit code 0x{exitCode:X8}");
    }

    // Problems a security program commonly causes: blocking or removing the file, killing it, holding or guarding the
    // hosts file, refusing the certificate store.
    public static bool SecurityProgramLikely(CaptureProblem problem) => problem is CaptureProblem.Blocked or CaptureProblem.Missing
        or CaptureProblem.Stopped or CaptureProblem.NotReady or CaptureProblem.HostsBusy or CaptureProblem.HostsDenied
        or CaptureProblem.Certificate or CaptureProblem.PortDenied or CaptureProblem.Crashed;

    // The status line for the launcher. antivirus: third-party security programs Windows knows of (SecurityProducts);
    // proxyPath: the capture's exe, to name in an exclusion.
    public static string Message(CaptureFailure failure, IReadOnlyList<string> antivirus, string proxyPath, string proxyLog)
    {
        var detail = string.IsNullOrWhiteSpace(failure.Detail) ? "" : " (" + failure.Detail.TrimEnd('.') + ")";
        var what = failure.Problem switch
        {
            CaptureProblem.Declined => "Sync cancelled: it needs the administrator prompt.",
            CaptureProblem.Blocked => "Windows refused to start the capture" + detail + ".",
            CaptureProblem.Missing => "The capture program is missing: " + proxyPath + " was removed" + detail + ".",
            CaptureProblem.Stopped => "The capture was stopped before it could start" + detail + ".",
            CaptureProblem.Crashed => "The capture crashed while starting" + detail + ".",
            CaptureProblem.NotReady => "The capture has not got ready after a minute; still waiting.",
            CaptureProblem.EvidenceFolder => "The capture could not write its log folder" + detail + ".",
            CaptureProblem.LeftoverCapture => "An earlier sync's capture is still running and could not be stopped" + detail +
                ". Restart the PC, then sync again.",
            CaptureProblem.HostsBusy => "Another program kept the hosts file locked, so the capture could not redirect the game" + detail + ".",
            CaptureProblem.HostsDenied => "The capture was not allowed to change the hosts file" + detail + ".",
            CaptureProblem.Offline => "The capture could not reach HELLDIVERS 2's servers" + detail + ": check your internet connection, then sync again.",
            CaptureProblem.Certificate => "The capture could not add its certificate to Windows" + detail + ".",
            CaptureProblem.PortInUse => "Another program already uses port 443, which the capture needs" + detail +
                ": close it, then sync again.",
            CaptureProblem.PortDenied => "Windows did not let the capture use port 443" + detail + ".",
            _ => "The capture could not start."
        };
        if (failure.Problem != CaptureProblem.Declined && failure.Problem != CaptureProblem.NotReady && failure.Problem != CaptureProblem.Offline)
            what += " Details: " + proxyLog + ".";
        return SecurityProgramLikely(failure.Problem) ? what + " " + SecurityAdvice(antivirus, proxyPath) : what;
    }

    // Which program to allow, and the exact file.
    public static string SecurityAdvice(IReadOnlyList<string> antivirus, string proxyPath)
    {
        if (antivirus.Count == 0)
            return "A security program may have blocked it: allow " + proxyPath + " in it (in Windows Security, under Protection history), then sync again.";
        var names = antivirus.Count == 1 ? antivirus[0] : string.Join(", ", antivirus.SkipLast(1)) + " or " + antivirus[^1];
        var kaspersky = antivirus.Any(name => name.Contains("Kaspersky", StringComparison.OrdinalIgnoreCase))
            ? " In Kaspersky's exclusion for it, tick \"Do not restrict application activity\" too." : "";
        return names + " may have blocked it: add " + proxyPath + " to " + (antivirus.Count == 1 ? "its" : "that program's") +
            " exclusions, then sync again." + kaspersky;
    }
}
