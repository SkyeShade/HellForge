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

    static string? RepositoryRoot()
    {
        for (var folder = Directory.GetCurrentDirectory(); folder != null; folder = Path.GetDirectoryName(folder))
            if (File.Exists(Path.Combine(folder, "linux", "hellforge-linux.sh"))) return folder;
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
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: Windows WFP marker)"); return; }
            var install = new InstallLayout(Path.Combine(root, "port-inplace"));
            check(!LinuxNetworkBlock.InPlace(install.Root), "No marker: not in place.");
            Marker(install, """{"block":"wfp","provider":"{x}","filters":8,"programs":1}""");
            check(!LinuxNetworkBlock.InPlace(install.Root), "A block from before the narrowing (no port) is not in place.");
            Marker(install, JsonSerializer.Serialize(new { block = "wfp", port = install.ComputedBackendPort + 1 }));
            check(!LinuxNetworkBlock.InPlace(install.Root), "A block for another port is not in place.");
            Marker(install, JsonSerializer.Serialize(new { block = "wfp", port = install.ComputedBackendPort }));
            check(LinuxNetworkBlock.InPlace(install.Root), "A block for this installation's port is in place.");
        });

        test("Network block: the native Linux session guard needs the wrapper and an emulated marker", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux guard)"); return; }
            var install = new InstallLayout(Path.Combine(root, "guard-session"));
            string? Problem()
            {
                try { LinuxNetworkBlock.GuardSession(install.Root); return null; }
                catch (InvalidOperationException error) { return error.Message; }
            }
            check(Problem()?.Contains("wrapper is missing", StringComparison.Ordinal) == true, "A missing wrapper passed the guard.");
            Directory.CreateDirectory(Path.Combine(install.App, "linux"));
            File.WriteAllText(Path.Combine(install.App, "linux", "hellforge-linux.sh"), "");
            check(Problem()?.Contains("network block is not set up", StringComparison.Ordinal) == true, "A missing marker passed the guard.");
            Marker(install, """{"block":"wfp","provider":"{x}","filters":8,"programs":1}""");
            check(Problem()?.Contains("network block is not set up", StringComparison.Ordinal) == true,
                "A Windows marker passed the native Linux guard.");
            Marker(install, """{"block":"wfp","provider":"linux-netns","emulated":"linux-netns"}""");
            var problem = Problem();
            check(problem == null, "An emulated marker was refused: " + problem);
        });

        test("Network block: setup reports the offline self-test's own reason", () =>
        {
            check(LinuxNetworkBlock.SelfTestProblem(null) == null, "No stderr reported a reason.");
            check(LinuxNetworkBlock.SelfTestProblem(" \n") == null, "Blank stderr reported a reason.");
            check(LinuxNetworkBlock.SelfTestProblem("need iproute2 (ip)\n") == "need iproute2 (ip)", "A missing program was not reported.");
            check(LinuxNetworkBlock.SelfTestProblem("This system only lets allowed programs make the network namespace HellForge runs in.\n" +
                "Run this once: sudo bash allow-namespace.sh\n")!.StartsWith("This system only lets allowed programs", StringComparison.Ordinal),
                "The AppArmor refusal was not reported.");
            check(LinuxNetworkBlock.SelfTestProblem("unshare: unshare failed: Operation not permitted\n") ==
                "unshare: unshare failed: Operation not permitted", "unshare's refusal was not reported.");
            check(LinuxNetworkBlock.SelfTestProblem("hellforge-offline: could not fetch Valve's relay list, using the last one\n" +
                "hellforge-offline: no relay list for UDP; refusing to open it\n") ==
                "hellforge-offline: no relay list for UDP; refusing to open it", "The last refusal was not reported.");
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

        test("Linux Steam ownership confirmation does not trigger the optional Zig download", () =>
        {
            if (RepositoryRoot() is not { } repository) { Console.WriteLine("  (skipped: no repository root above the test)"); return; }
            var wrapper = File.ReadAllText(Path.Combine(repository, "linux", "hellforge-linux.sh"));
            var ownershipCheck = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Core", "SteamOwnershipCheck.cs"));
            var setup = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Launcher", "SetupFlow.cs"));
            var skip = wrapper.IndexOf("if [ \"${HELLFORGE_SKIP_MOD_TOOLS:-}\" = 1 ]; then", StringComparison.Ordinal);
            var alternative = skip < 0 ? -1 : wrapper.IndexOf("else", skip, StringComparison.Ordinal);
            var download = wrapper.IndexOf("HellForge.Cli.exe\" mod-tools", StringComparison.Ordinal);
            var end = skip < 0 ? -1 : wrapper.IndexOf("\n  fi", skip, StringComparison.Ordinal);
            check(skip >= 0 && alternative > skip && download > alternative && end > download &&
                wrapper.Contains("Skipping the mod-tools download for the Steam ownership check.", StringComparison.Ordinal),
                "The wrapper does not guard the mod-tools download during ownership checks.");
            check(ownershipCheck.Contains("start.Environment[\"HELLFORGE_SKIP_MOD_TOOLS\"] = \"1\";", StringComparison.Ordinal),
                "The ownership probe does not set the wrapper's mod-tools skip flag.");
            var probeExecutable = ownershipCheck.IndexOf(
                "start.ArgumentList.Add(WinePath.ToWinePath(request.Probe, request.Prefix));", StringComparison.Ordinal);
            var probeArguments = ownershipCheck.IndexOf(
                "AddProbeArguments(start, prepared, useWinePaths: true, prefix: request.Prefix, request.PrivateResult);", StringComparison.Ordinal);
            check(probeExecutable >= 0 && probeArguments > probeExecutable,
                "The Linux wrapper receives the mapped Steam probe executable before its DLL, hash, and result arguments.");
            var prepareOnly = wrapper.IndexOf("HELLFORGE_PREPARE_MOD_TOOLS_ONLY", StringComparison.Ordinal);
            var namespaceLaunch = wrapper.IndexOf("Entering the offline network namespace.", StringComparison.Ordinal);
            var ownershipFailure = setup.IndexOf("if (exit != 0)", StringComparison.Ordinal);
            var prepareCall = setup.IndexOf("PrepareLinuxModTools(wrapper, proton, install, steamRoot, token)", StringComparison.Ordinal);
            check(prepareOnly >= 0 && namespaceLaunch > prepareOnly &&
                setup.Contains("start.Environment[\"HELLFORGE_PREPARE_MOD_TOOLS_ONLY\"] = \"1\";", StringComparison.Ordinal) &&
                ownershipFailure >= 0 && prepareCall > ownershipFailure &&
                setup.IndexOf("SetStep(SetupStep.Copy);", prepareCall, StringComparison.Ordinal) > prepareCall,
                "Setup does not request the mod-tools download after ownership confirmation and before game copying.");
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
            check(written.RootElement.TryGetProperty("externalIpv4RouteError", out var route) && route.ValueKind == JsonValueKind.Number,
                "The report carries whether Windows has a route out (ticket 0352).");
            check(written.RootElement.TryGetProperty("internetConnectivity", out var connectivity) && connectivity.ValueKind == JsonValueKind.Number,
                "The report carries Windows' connectivity flags (ticket 0352).");
            Probe("41000", report);
            check(!File.ReadAllText(report).Contains("otherLoopbackTcpError", StringComparison.Ordinal), "Without it the report is as before (the session's probe).");
        });

        test("Network block probe: a report passes only as the session always required, and a failure says which part failed", () =>
        {
            string? Problem(string json, bool narrowed = false) => NetworkBlockProbe.Problem(System.Text.Json.Nodes.JsonNode.Parse(json), narrowed);
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":10013,"externalIpv6TcpError":10013}""") == null, "A holding block failed.");
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":10013,"externalIpv6TcpError":10051,"loopbackTcpError":0,"otherLoopbackTcpError":10013}""", true) == null,
                "A holding narrowed block failed (IPv6 is not required).");
            // the 0.2.0 tickets (0050, 0072, 0087, 0091): the filters no longer applied, the outside connect waited
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":-1,"externalIpv6TcpError":10051}""") is { } waited &&
                waited.Contains("not stopping the game's connections", StringComparison.Ordinal), "A connect let out passed or was not named.");
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":0,"externalIpv6TcpError":10051}""") is { } answered && answered.Contains("error 0", StringComparison.Ordinal),
                "A connect something answered passed.");
            // ticket 0069: an older HellForge's backend on a random port behind a block narrowed by a newer one
            check(Problem("""{"loopbackHttpPassed":false,"externalIpv4TcpError":10013,"externalIpv6TcpError":10013}""") is { } own &&
                own.Contains("stops the game reaching HellForge's own backend", StringComparison.Ordinal), "A refused backend passed or was not named.");
            check(Problem("""{"loopbackHttpPassed":false,"externalIpv4TcpError":-1}""") is { } unreachable && unreachable.Contains("cannot reach", StringComparison.Ordinal),
                "An unreachable backend passed.");
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":10013,"loopbackTcpError":0,"otherLoopbackTcpError":10061}""", true) is { } wide &&
                wide.Contains("other programs on this PC", StringComparison.Ordinal), "A narrowed block letting other loopback ports through passed.");
            check(Problem("""{"loopbackHttpPassed":true,"externalIpv4TcpError":10013}""", true) != null, "A narrowed block's report without the other port passed.");
            check(NetworkBlockProbe.Problem(null, false) != null && Problem("[]") != null && Problem("""{"loopbackHttpPassed":"yes","externalIpv4TcpError":"10013"}""") != null,
                "No report, or one of another shape, passed.");
        });

        // ticket 0352: with no route out, Windows fails the outside connect (10065) before WFP judges it
        test("Network block probe: with no network, a narrowed block holds on its other loopback port alone", () =>
        {
            static System.Text.Json.Nodes.JsonNode Report(int outside, int? route, int other = 10013, int? connectivity = null)
            {
                var report = new System.Text.Json.Nodes.JsonObject
                {
                    ["loopbackHttpPassed"] = true, ["externalIpv4TcpError"] = outside, ["externalIpv6TcpError"] = 10065,
                    ["loopbackTcpError"] = 0, ["otherLoopbackTcpError"] = other
                };
                if (route != null) report["externalIpv4RouteError"] = route;
                if (connectivity != null) report["internetConnectivity"] = connectivity;
                return report;
            }
            foreach (var error in new[] { 10050, 10051, 10065 })
                check(NetworkBlockProbe.Problem(Report(error, 1231), true) == null && NetworkBlockProbe.Offline(Report(error, 1231)),
                    $"No route and error {error}: a narrowed block that refuses the other loopback port failed, or was not offline.");
            check(NetworkBlockProbe.Problem(Report(10065, 1231, 10061), true) is { } stale && stale.Contains("other programs on this PC", StringComparison.Ordinal),
                "No route, and filters that no longer apply (the other port reached the stack): passed.");
            check(NetworkBlockProbe.Problem(Report(10065, 1231), false) is { } wide && wide.Contains("no internet connection", StringComparison.Ordinal),
                "No route and a block that is not narrowed: passed, or did not say the PC is offline.");
            // a route out and no connectivity reading (a probe from before it): the connect may have reached the network (a
            // router's ICMP unreachable), so the block did not refuse it
            check(NetworkBlockProbe.Problem(Report(10065, 0), true) is { } routed && routed.Contains("error 10065", StringComparison.Ordinal) &&
                !NetworkBlockProbe.Offline(Report(10065, 0)), "A route out, no connectivity and error 10065: passed.");
            // a route out while Windows sees the internet, on either protocol, or could not say: still fails
            foreach (var online in new[] { 0x40, 0x400, 0x440, 0x60, -1 })
                check(NetworkBlockProbe.Problem(Report(10065, 0, connectivity: online), true) != null && !NetworkBlockProbe.Offline(Report(10065, 0, connectivity: online)),
                    $"A route out, connectivity 0x{online:x} and error 10065: passed as offline.");
            // a route out on a network with no way on (ticket 0352's PC): offline, proved by the other loopback port
            foreach (var local in new[] { 0x0, 0x1, 0x10, 0x20, 0x31, 0x220 })
                foreach (var error in new[] { 10050, 10051, 10065 })
                    check(NetworkBlockProbe.Problem(Report(error, 0, connectivity: local), true) == null && NetworkBlockProbe.Offline(Report(error, 0, connectivity: local)),
                        $"A route out, no internet (0x{local:x}) and error {error}: failed, or was not offline.");
            check(NetworkBlockProbe.Problem(Report(10065, 0, 10061, 0x20), true) != null, "No internet, and filters that no longer apply: passed.");
            check(NetworkBlockProbe.Problem(Report(-1, 0, connectivity: 0x20), true) != null && NetworkBlockProbe.Problem(Report(0, 0, connectivity: 0x20), true) != null,
                "No internet, but a connect that waited or was answered: passed.");
            check(NetworkBlockProbe.Problem(Report(10065, 0, connectivity: 0x20), false) is { } wideLocal && wideLocal.Contains("no internet connection", StringComparison.Ordinal),
                "No internet and a block that is not narrowed: passed, or did not say the PC is offline.");
            // the ticket's own report (HellForge-logs-20261107-234437, run 9a50d976): the probe from before the connectivity check
            var ticket = System.Text.Json.Nodes.JsonNode.Parse("""{"loopbackHttpPassed":true,"externalIpv4TcpError":10065,"externalIpv4RouteError":0,"externalIpv6TcpError":10051,"loopbackTcpError":0,"otherLoopbackTcpError":10013}""");
            check(NetworkBlockProbe.Problem(ticket, true) != null && !NetworkBlockProbe.Offline(ticket),
                "The ticket's report, which has no connectivity reading, passed: an old probe must be rebuilt (by the repair) first.");
            ticket!["internetConnectivity"] = 0x20;
            check(NetworkBlockProbe.Problem(ticket, true) == null && NetworkBlockProbe.Offline(ticket),
                "The ticket's report with Windows seeing no internet failed.");
            // a probe from before the route check
            check(NetworkBlockProbe.Problem(Report(10065, null), true) != null && !NetworkBlockProbe.Offline(Report(10065, null)),
                "A report without the route: passed as offline.");
            check(NetworkBlockProbe.Problem(Report(-1, 1231), true) != null && NetworkBlockProbe.Problem(Report(0, 1231), true) != null,
                "No route, but a connect that waited or was answered: passed.");
            check(!NetworkBlockProbe.Offline(Report(10013, 1231)) && NetworkBlockProbe.Problem(Report(10013, 1231), true) == null,
                "A refused connect is the online proof, route or not.");
        });

        test("Network block probe: the probe, the session and the install script share the offline rule", () =>
        {
            if (Scripts() is not { } scripts) { Console.WriteLine("  (skipped: no scripts folder)"); return; }
            var probe = File.ReadAllText(Path.Combine(scripts, "OfflineControlNetworkProbe.cs"));
            var install = File.ReadAllText(Path.Combine(scripts, "Install-WfpBlock.ps1"));
            check(probe.Contains("error != 10050 && error != 10051 && error != 10065", StringComparison.Ordinal) &&
                  probe.Contains("route != 0 || (connectivity >= 0 && (connectivity & Internet) == 0)", StringComparison.Ordinal) &&
                  probe.Contains("const int Internet = 0x40 | 0x400;", StringComparison.Ordinal) &&
                  probe.Contains("(other > 0 && Offline(route, connectivity, v4))", StringComparison.Ordinal) &&
                  probe.Contains("\\\"externalIpv4RouteError\\\":", StringComparison.Ordinal) &&
                  probe.Contains("\\\"internetConnectivity\\\":", StringComparison.Ordinal), "The probe's offline rule changed.");
            check(install.Contains("'externalIpv4RouteError'", StringComparison.Ordinal) && install.Contains("'internetConnectivity'", StringComparison.Ordinal) &&
                  install.Contains("$egress.internetConnectivity -ge 0 -and ($egress.internetConnectivity -band 0x440) -eq 0", StringComparison.Ordinal) &&
                  install.Contains("($noRoute -or $noInternet) -and $egress.externalIpv4TcpError -in @(10050,10051,10065)", StringComparison.Ordinal) &&
                  install.Contains("-ne 10013 -and -not $offline", StringComparison.Ordinal), "Install-WfpBlock.ps1's offline rule changed.");
        });
    }
}
