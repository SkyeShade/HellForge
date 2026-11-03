using System.ComponentModel;
using System.Diagnostics;

namespace HellForge.Core;

// The antivirus programs Windows Security Center knows of (root\SecurityCenter2 AntiVirusProduct), without Microsoft's
// own: when a capture does not start, a program like Kaspersky or Avast is named so the player knows what to allow.
// Asked through PowerShell's Get-CimInstance (no WMI package), only after something failed, and never for more than a
// few seconds. Empty off Windows, on a server edition (no Security Center), or when PowerShell does not answer.
public static class SecurityProducts
{
    public static IReadOnlyList<string> ThirdPartyAntivirus()
    {
        if (!OperatingSystem.IsWindows()) return [];
        var powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");
        var start = new ProcessStartInfo(powershell)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var argument in new[] { "-NoProfile", "-NonInteractive", "-Command",
            "Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction SilentlyContinue | ForEach-Object { $_.displayName }" })
            start.ArgumentList.Add(argument);
        try
        {
            using var process = Process.Start(start);
            if (process == null) return [];
            var output = process.StandardOutput.ReadToEndAsync();
            _ = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(8000))
            {
                try { process.Kill(); } catch (InvalidOperationException) { }
                return [];
            }
            return ThirdParty(output.Result.Split('\n'));
        }
        catch (Exception error) when (error is Win32Exception or IOException or InvalidOperationException)
        {
            return [];
        }
    }

    // Security Center's names without Microsoft Defender's, each once, in order.
    public static IReadOnlyList<string> ThirdParty(IEnumerable<string> displayNames) =>
    [
        .. displayNames.Select(name => name.Trim()).Where(name => name.Length > 0 &&
            !name.Contains("Windows Defender", StringComparison.OrdinalIgnoreCase) &&
            !name.Contains("Microsoft Defender", StringComparison.OrdinalIgnoreCase))
            .Distinct(StringComparer.OrdinalIgnoreCase)
    ];
}
