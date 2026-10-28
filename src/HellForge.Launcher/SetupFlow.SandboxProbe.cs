using System.ComponentModel;
using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using HellForge.Core;

namespace HellForge.Launcher;

// The Linux sandbox probe's Setup row and its one-line, information-only result. It is a partial of MainWindow (like
// SetupFlow.cs) so the setup flow can call it without carrying the process and UI code in that already large file.
public partial class MainWindow
{
    // The phase-1 sandbox probe is information only, not a SetupStep: it never refuses a session or changes a setting.
    (Border Dot, TextBlock Detail)? linuxSandboxRow;

    void InitializeLinuxSandboxProbeRow()
    {
        if (!OperatingSystem.IsLinux()) return;
        if (linuxSandboxRow is not null) { SetLinuxSandboxRow(null, ""); return; }
        var dot = new Border { Classes = { "dot" }, Background = Pending, VerticalAlignment = VerticalAlignment.Top, Margin = new(0, 5, 14, 0) };
        var title = new TextBlock { Text = "Run the Linux sandbox probe (information only)", FontWeight = FontWeight.SemiBold };
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

    // Phase 1 only: run the native probe and show one line. A missing probe, a failed item or a timeout is information,
    // not a setup failure; no sandbox is enabled and no session is refused from this result.
    async Task RunLinuxSandboxProbe(InstallLayout install)
    {
        SetLinuxSandboxRow(null, "Running the native Linux sandbox probe (information only)...", running: true);
        var wrapper = Path.Combine(install.App, "linux", "hellforge-linux.sh");
        var report = Path.Combine(install.Logs, "sandbox-probe.json");
        if (!File.Exists(wrapper))
        {
            SetLinuxSandboxRow(false, "Information only: app/linux/hellforge-linux.sh is missing.");
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
            start.ArgumentList.Add("--sandbox-probe");
            start.Environment["INSTALL"] = install.Root;
            // Keep stdout for this row even when the launcher usually sends wrapper output to its log.
            start.Environment.Remove("HELLFORGE_WRAPPER_LOG");
            using var process = Process.Start(start) ?? throw new IOException("The Linux sandbox probe did not start.");
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
                SetLinuxSandboxRow(false, "Information only: the Linux sandbox probe did not finish in 3 minutes.");
                return;
            }
            var errorText = await errorTask;
            var outputText = await outputTask;
            var lines = (errorText + "\n" + outputText).Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            var result = lines.LastOrDefault(line => line.StartsWith("hf-sandbox-probe:", StringComparison.Ordinal))
                ?? lines.LastOrDefault()
                ?? $"exit code {process.ExitCode}";
            SetLinuxSandboxRow(process.ExitCode == 0,
                $"Information only: {Privacy.Scrub(result)} Report: {Privacy.Scrub(report)}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
        {
            SetLinuxSandboxRow(false, "Information only: the Linux sandbox probe could not run: " + Privacy.Scrub(error.Message));
        }
    }

    // The probe runs before, and never blocks, the normal Linux setup-file checks.
    async Task<(string SteamRoot, string Wrapper, string Probe)> CheckLinuxSetupFilesWithProbe(InstallLayout install, string proton)
    {
        await RunLinuxSandboxProbe(install);
        return await CheckLinuxSetupFiles(install, proton);
    }
}
