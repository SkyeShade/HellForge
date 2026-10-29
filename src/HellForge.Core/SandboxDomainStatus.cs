using System.Text.Json;

namespace HellForge.Core;

public sealed record SandboxWindowSample(string Title, IReadOnlyList<string> Controls);

// The untrusted domain's evidence: the trusted runner reads it, but never treats its paths or text as authority.
public sealed record SandboxDomainStatus
{
    public int SchemaVersion { get; init; } = 1;
    public string RunId { get; init; } = "";
    public int DomainPid { get; init; }
    public int GamePid { get; init; }
    public string State { get; init; } = "starting";
    public int? ExitCode { get; init; }
    public bool JobEmpty { get; init; }
    public string? Failure { get; init; }
    public string? Matchmaking { get; init; }
    public string? PlayFabOutcome { get; init; }
    public IReadOnlyList<SandboxWindowSample> Windows { get; init; } = [];

    public static SandboxDomainStatus? Read(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            var status = JsonSerializer.Deserialize(File.ReadAllText(path), SandboxJson.Default.SandboxDomainStatus);
            if (status is not { SchemaVersion: 1 }) return null;
            var windows = status.Windows;
            if (windows == null) return status with { Windows = [] };
            return windows.Any(window => window is null || window.Controls is null)
                ? status with { Windows = windows.Where(window => window is not null && window.Controls is not null).ToList() }
                : status;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    public static void Write(string path, SandboxDomainStatus status)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var partial = path + ".partial";
        File.WriteAllText(partial, JsonSerializer.Serialize(status, SandboxJson.Default.SandboxDomainStatus) + "\n");
        File.Move(partial, path, overwrite: true);
    }
}
