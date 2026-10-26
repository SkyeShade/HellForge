using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;

// The network block's backend port (lockdown item 6): one loopback port per installation, chosen by setup, recorded in
// the WFP block's marker and read back by sessions; a block without it is placed again.
static class NetworkBlockTests
{
    static string? Scripts()
    {
        for (var folder = AppContext.BaseDirectory; folder != null; folder = Path.GetDirectoryName(folder))
            if (File.Exists(Path.Combine(folder, "scripts", "Install-WfpBlock.ps1"))) return Path.Combine(folder, "scripts");
        return null;
    }

    static void Marker(InstallLayout install, string json)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(install.NetworkBlockMarker)!);
        File.WriteAllText(install.NetworkBlockMarker, json);
        File.WriteAllText(Path.Combine(install.Root, "network-block", "NetworkProbe.exe"), "");
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Backend port: deterministic, below the dynamic range, and the same for a path written differently", () =>
        {
            var a = new InstallLayout(Path.Combine(root, "port-a"));
            var b = new InstallLayout(Path.Combine(root, "port-b"));
            check(a.ComputedBackendPort is >= InstallLayout.BackendPortFirst and < InstallLayout.BackendPortFirst + InstallLayout.BackendPortCount,
                $"Port {a.ComputedBackendPort} is in 40000-46999.");
            check(InstallLayout.BackendPortFirst + InstallLayout.BackendPortCount <= 49152, "The range ends below Windows' dynamic ports.");
            check(a.ComputedBackendPort == new InstallLayout(a.Root).ComputedBackendPort, "Same path gives the same port.");
            check(a.ComputedBackendPort == new InstallLayout(a.Root + Path.DirectorySeparatorChar).ComputedBackendPort, "A trailing separator gives the same port.");
            check(a.ComputedBackendPort == new InstallLayout(a.Root.ToUpperInvariant()).ComputedBackendPort, "Letter case gives the same port.");
            check(a.ComputedBackendPort != b.ComputedBackendPort, $"Different paths give different ports ({a.ComputedBackendPort} vs {b.ComputedBackendPort}).");
        });

        test("Backend port: sessions use the port the block's marker names, else the computed one", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "port-marker"));
            check(install.BlockedBackendPort() == null && install.BackendPort == install.ComputedBackendPort, "No marker: the computed port.");
            Marker(install, """{"block":"wfp","provider":"{x}","port":41234,"filters":8,"programs":1}""");
            check(install.BlockedBackendPort() == 41234 && install.BackendPort == 41234, "The marker's port.");
            foreach (var marker in new[] { """{"block":"wfp","filters":8}""", """{"port":0}""", """{"port":70000}""", """{"port":"41234"}""", "not json" })
            {
                Marker(install, marker);
                check(install.BlockedBackendPort() == null && install.BackendPort == install.ComputedBackendPort, "No usable port in " + marker + ": the computed one.");
            }
        });

        test("Network block: setup places it again unless its marker names this installation's port", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "port-inplace"));
            check(!LinuxNetworkBlock.InPlace(install.Root), "No marker: not in place.");
            Marker(install, """{"block":"wfp","provider":"{x}","filters":8,"programs":1}""");
            check(!LinuxNetworkBlock.InPlace(install.Root), "A block from before the narrowing (no port) is not in place.");
            Marker(install, JsonSerializer.Serialize(new { block = "wfp", port = install.ComputedBackendPort + 1 }));
            check(!LinuxNetworkBlock.InPlace(install.Root), "A block for another port is not in place.");
            Marker(install, JsonSerializer.Serialize(new { block = "wfp", port = install.ComputedBackendPort }));
            check(LinuxNetworkBlock.InPlace(install.Root), "A block for this installation's port is in place.");
        });

        test("Network block: the port has one source, setup, which the WFP script takes and records", () =>
        {
            if (Scripts() is not { } scripts) { Console.WriteLine("  (skipped: no scripts folder above the test)"); return; }
            var common = File.ReadAllText(Path.Combine(scripts, "NetworkBlock.Common.ps1"));
            var wfp = File.ReadAllText(Path.Combine(scripts, "Install-WfpBlock.ps1"));
            check(!common.Contains("BackendPort", StringComparison.Ordinal), "The common script does not compute a port of its own.");
            check(wfp.Contains("[int]$BackendPort", StringComparison.Ordinal) && wfp.Contains("port=$BackendPort", StringComparison.Ordinal),
                "Install-WfpBlock.ps1 takes -BackendPort and records it in the marker.");
        });

        test("Scripts: the ownership probe starts without Start-Process, and a link in the block's path is refused by name", () =>
        {
            if (Scripts() is not { } scripts) { Console.WriteLine("  (skipped: no scripts folder above the test)"); return; }
            // Windows PowerShell reads [ ] in Start-Process's -WorkingDirectory and redirect paths as wildcards: an installation
            // folder with brackets failed every ownership check
            check(!File.ReadAllLines(Path.Combine(scripts, "Test-SteamOwnership.ps1")).Where(l => !l.TrimStart().StartsWith('#'))
                    .Any(l => l.Contains("Start-Process", StringComparison.OrdinalIgnoreCase)),
                "Test-SteamOwnership.ps1 starts the probe through .NET.");
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (link check skipped: Windows only)"); return; }
            var folder = Directory.CreateDirectory(Path.Combine(root, "[links] test")).FullName;
            var real = Directory.CreateDirectory(Path.Combine(folder, "real", "game", "bin")).FullName;
            File.WriteAllText(Path.Combine(real, "helldivers2.exe"), "x");
            var linked = Path.Combine(folder, "linked");
            using (var junction = Process.Start(new ProcessStartInfo("cmd.exe", ["/c", "mklink", "/J", linked, Path.Combine(folder, "real")]) { UseShellExecute = false, RedirectStandardOutput = true })!)
                junction.WaitForExit();
            if (!Directory.Exists(linked)) { Console.WriteLine("  (link check skipped: could not make a junction)"); return; }
            string NoLinks(string path)
            {
                var script = $"$HellForgeInstallRoot='{folder.Replace("'", "''")}'; . '{Path.Combine(scripts, "NetworkBlock.Common.ps1").Replace("'", "''")}'; " +
                    $"try {{ No-Links '{path.Replace("'", "''")}'; 'none' }} catch {{ $_.Exception.Message }}";
                using var shell = Process.Start(new ProcessStartInfo("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script])
                    { UseShellExecute = false, RedirectStandardOutput = true })!;
                var output = shell.StandardOutput.ReadToEnd().Trim();
                shell.WaitForExit();
                return output;
            }
            check(NoLinks(Path.Combine(real, "helldivers2.exe")) == "none", "A plain path (brackets in its name included) passes.");
            var refused = NoLinks(Path.Combine(linked, "game", "bin", "helldivers2.exe"));
            check(refused.StartsWith("Linked target path: " + linked + " is a link", StringComparison.Ordinal), "A path through a junction is refused, naming the junction: " + refused);
            Directory.Delete(linked);
        });

        test("Network block probe: with a second loopback port it reports how both connects end", () =>
        {
            if (!OperatingSystem.IsWindows() || Scripts() is not { } scripts) { Console.WriteLine("  (skipped: Windows only)"); return; }
            var folder = Directory.CreateDirectory(Path.Combine(root, "probe")).FullName;
            var exe = Path.Combine(folder, "NetworkProbe.exe");
            var build = Process.Start(new ProcessStartInfo("powershell.exe", ["-NoProfile", "-Command",
                $"Add-Type -Path '{Path.Combine(scripts, "OfflineControlNetworkProbe.cs")}' -OutputAssembly '{exe}' -OutputType ConsoleApplication"])
                { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true })!;
            build.WaitForExit();
            if (!File.Exists(exe)) { Console.WriteLine("  (skipped: Windows PowerShell could not compile the probe)"); return; }
            int Probe(params string[] args)
            {
                using var probe = Process.Start(new ProcessStartInfo(exe, args) { UseShellExecute = false })!;
                probe.WaitForExit();
                return probe.ExitCode;
            }
            var report = Path.Combine(folder, "report.json");
            check(Probe("41000", report, "41000") == 2, "The second port must differ from the backend's.");
            check(Probe("41000", report, "x") == 2, "A second port that does not parse is refused.");
            Probe("41000", report, "41001");
            using var written = JsonDocument.Parse(File.ReadAllText(report));
            // No block on this PC's test run: both loopback connects reach the stack (nothing listens: 10061).
            check(written.RootElement.TryGetProperty("loopbackTcpError", out var own) && own.GetInt32() != 10013 &&
                  written.RootElement.TryGetProperty("otherLoopbackTcpError", out var other) && other.GetInt32() != 10013,
                "The report carries both loopback results.");
            Probe("41000", report);
            check(!File.ReadAllText(report).Contains("otherLoopbackTcpError", StringComparison.Ordinal), "Without it the report is as before (the session's probe).");
        });
    }
}
