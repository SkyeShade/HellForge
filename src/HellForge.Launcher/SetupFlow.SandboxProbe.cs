using System.ComponentModel;
using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// The Linux sandbox self-test's Setup row. It records a pass or the first reason against this app build and kernel; the
// result is what lets Settings > Sandbox offer the switch. A failure does not stop setup, but it leaves the switch off.
public partial class MainWindow
{
    (Border Dot, TextBlock Detail)? linuxSandboxRow;

    void InitializeLinuxSandboxProbeRow()
    {
        if (!OperatingSystem.IsLinux()) return;
        if (linuxSandboxRow is not null) { SetLinuxSandboxRow(null, ""); return; }
        var dot = new Border { Classes = { "dot" }, Background = Pending, VerticalAlignment = VerticalAlignment.Top, Margin = new(0, 5, 14, 0) };
        var title = new TextBlock { Text = "Check the Linux sandbox", FontWeight = FontWeight.SemiBold };
        var detail = new TextBlock { Classes = { "muted" }, FontSize = 12, TextWrapping = TextWrapping.Wrap };
        var row = new DockPanel();
        DockPanel.SetDock(dot, Dock.Left);
        row.Children.Add(dot);
        row.Children.Add(new StackPanel { Spacing = 2, Children = { title, detail } });
        SetupSteps.Children.Add(row);
        linuxSandboxRow = (dot, detail);
    }

    void SetLinuxSandboxRow(bool? success, string detail, bool running = false)
    {
        if (linuxSandboxRow is not { } row) return;
        row.Dot.Background = running ? Accent : success switch { true => Green, false => Red, null => Pending };
        row.Dot.Classes.Set("pulse", running);
        row.Detail.Text = detail;
        row.Detail.Foreground = success == false ? Red : Grey;
    }

    async Task RunLinuxSandboxProbe(InstallLayout install)
    {
        SetLinuxSandboxRow(null, "Running the Linux sandbox self-test...", running: true);
        var wrapper = Path.Combine(install.App, "linux", "hellforge-linux.sh");
        var report = Path.Combine(install.Logs, "sandbox-self-test.txt");
        if (!File.Exists(wrapper))
        {
            RecordLinuxSandboxTest(install, false, "app/linux/hellforge-linux.sh is missing");
            SetLinuxSandboxRow(false, "app/linux/hellforge-linux.sh is missing, so the sandbox stays off.");
            ApplyLinuxSandboxSettings(install);
            return;
        }
        try
        {
            var start = new ProcessStartInfo("bash")
            {
                WorkingDirectory = install.Root,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            start.ArgumentList.Add(wrapper);
            start.ArgumentList.Add("--sandbox-self-test");
            start.Environment["INSTALL"] = install.Root;
            start.Environment.Remove("HELLFORGE_WRAPPER_LOG");
            using var process = Process.Start(start) ?? throw new IOException("The Linux sandbox self-test did not start.");
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(3));
            try
            {
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                try { process.Kill(entireProcessTree: true); }
                catch (InvalidOperationException) { }
                var reason = "the Linux sandbox self-test did not finish in 3 minutes";
                RecordLinuxSandboxTest(install, false, reason);
                SetLinuxSandboxRow(false, char.ToUpperInvariant(reason[0]) + reason[1..] + "; the sandbox stays off.");
                ApplyLinuxSandboxSettings(install);
                return;
            }
            var errorText = await errorTask;
            var outputText = await outputTask;
            Directory.CreateDirectory(install.Logs);
            File.WriteAllText(report, errorText + outputText);
            var passed = process.ExitCode == 0;
            // A container refusal stops the wrapper before hf-sandbox runs, so its written reason is the result: the
            // readiness record and Settings > Sandbox then say why, not a generic self-test failure.
            var result = (passed ? null : SessionRunner.LinuxContainerRefusal(install.Root))
                ?? LinuxSandboxReadiness.SelfTestReason(errorText + "\n" + outputText, process.ExitCode);
            RecordLinuxSandboxTest(install, passed, passed ? "" : result);
            SetLinuxSandboxRow(passed, (passed ? "The Linux sandbox self-test passed. " : "The sandbox stays off: ") +
                Privacy.Scrub(result) + " Report: " + Privacy.Scrub(report));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
        {
            var reason = "the Linux sandbox self-test could not run: " + error.Message;
            RecordLinuxSandboxTest(install, false, reason);
            SetLinuxSandboxRow(false, Privacy.Scrub(char.ToUpperInvariant(reason[0]) + reason[1..] + "; the sandbox stays off."));
        }
        ApplyLinuxSandboxSettings(install);
    }

    static void RecordLinuxSandboxTest(InstallLayout install, bool passed, string reason)
    {
        try { LinuxSandboxReadiness.Save(install, passed, reason); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    // The self-test runs before, and never blocks, the normal Linux setup-file checks.
    async Task<(string SteamRoot, string Wrapper, string Probe)> CheckLinuxSetupFilesWithProbe(InstallLayout install, string proton)
    {
        await RunLinuxSandboxProbe(install);
        return await CheckLinuxSetupFiles(install, proton);
    }
}
