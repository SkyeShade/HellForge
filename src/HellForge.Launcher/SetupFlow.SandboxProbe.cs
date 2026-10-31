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

    async Task RunLinuxSandboxProbe(InstallLayout install, string? proton = null)
    {
        SetLinuxSandboxRow(null, "Running the Linux sandbox self-test...", running: true);
        var cli = Path.Combine(install.App, "cli-linux", "HellForge.Cli");
        var report = Path.Combine(install.Logs, "sandbox-self-test.txt");
        if (!File.Exists(cli))
        {
            InvalidateLinuxSandboxTest(install);
            SetLinuxSandboxRow(false, "The native Linux readiness command is missing, so the sandbox stays off.");
            ApplyLinuxSandboxSettings(install);
            return;
        }
        // Setup and Check and repair test again: a failed record for this app build and kernel is not reused (the readiness
        // command would), while a current pass is.
        if (LinuxSandboxReadiness.Read(install) is { Passed: false }) InvalidateLinuxSandboxTest(install);
        // Readiness may make the prefix case-sensitive, which rebuilds folders under the same names (new inodes); a
        // working folder inside the prefix is entered again by its name afterwards (LinuxPrefixCase).
        var workingFolder = Environment.CurrentDirectory;
        try
        {
            var start = new ProcessStartInfo(cli)
            {
                WorkingDirectory = install.Root,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            start.ArgumentList.Add("sandbox-readiness");
            start.ArgumentList.Add("--install");
            start.ArgumentList.Add(install.Root);
            if (!string.IsNullOrWhiteSpace(proton)) start.Environment["PROTON"] = proton;
            if (SteamLocator.SteamRoot(install) is { Length: > 0 } steamRoot)
                start.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = steamRoot;
            start.Environment.Remove("HELLFORGE_WRAPPER_LOG");
            using var process = Process.Start(start) ?? throw new IOException("The Linux readiness command did not start.");
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
                InvalidateLinuxSandboxTest(install);
                var reason = "the Linux sandbox self-test did not finish in 3 minutes";
                SetLinuxSandboxRow(false, char.ToUpperInvariant(reason[0]) + reason[1..] + "; the sandbox stays off.");
                ApplyLinuxSandboxSettings(install);
                return;
            }
            var errorText = await errorTask;
            var outputText = await outputTask;
            try { Directory.SetCurrentDirectory(workingFolder); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            Directory.CreateDirectory(install.Logs);
            File.WriteAllText(report, errorText + outputText);
            var readiness = LinuxSandboxReadiness.Read(install);
            var passed = process.ExitCode == 0 && readiness is { Passed: true };
            // The readiness command is the one writer of the record. A container refusal that stopped it before the
            // self-test still writes its reason to the refusal file, so Settings > Sandbox can say why.
            var recordedReason = readiness is { Passed: false } failed && !string.IsNullOrWhiteSpace(failed.Reason)
                ? failed.Reason : null;
            var result = (passed ? null : recordedReason)
                ?? (readiness == null ? "the readiness command did not record a result" : null)
                ?? SessionRunner.LinuxContainerRefusal(install.Root)
                ?? LinuxSandboxReadiness.SelfTestReason(errorText + "\n" + outputText, process.ExitCode);
            SetLinuxSandboxRow(passed, (passed ? "The Linux sandbox self-test passed. " : "The sandbox stays off: ") +
                Privacy.Scrub(result) + " Report: " + Privacy.Scrub(report));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
        {
            InvalidateLinuxSandboxTest(install);
            var reason = "the Linux sandbox self-test could not run: " + error.Message;
            SetLinuxSandboxRow(false, Privacy.Scrub(char.ToUpperInvariant(reason[0]) + reason[1..] + "; the sandbox stays off."));
        }
        ApplyLinuxSandboxSettings(install);
    }

    static void InvalidateLinuxSandboxTest(InstallLayout install)
    {
        try
        {
            File.Delete(LinuxSandboxReadiness.FileFor(install));
            File.Delete(LinuxSandboxReadiness.ModeFileFor(install));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    // The self-test runs before, and never blocks, the normal Linux setup-file checks.
    async Task<(string SteamRoot, string Wrapper, string Probe)> CheckLinuxSetupFilesWithProbe(InstallLayout install, string proton)
    {
        await RunLinuxSandboxProbe(install, proton);
        return await CheckLinuxSetupFiles(install, proton);
    }
}
