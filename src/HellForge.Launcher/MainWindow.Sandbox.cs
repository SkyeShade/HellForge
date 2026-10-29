using HellForge.Core;

namespace HellForge.Launcher;

// MainWindow's Settings > Sandbox wiring: Windows keeps its Steam-bridge condition; Linux is on by default only when this
// PC passes the self-test for this app build and kernel. A failure leaves the switch off, shows why, and Check and repair
// runs the same probe again.
public partial class MainWindow
{
    bool updatingSandboxUi;
    bool linuxSandboxCheckRunning;

    void InitializeSandboxSettings()
    {
        var steamBridge = SessionRunner.HasSteamBridge(paths);
        var linux = OperatingSystem.IsLinux();
        SandboxUnavailable.IsVisible = !linux && !steamBridge;
        SandboxLinuxNote.IsVisible = linux;
        SandboxBox.IsCheckedChanged += (_, _) =>
        {
            if (updatingSandboxUi) return;
            ProfileStore.SaveSandbox(paths, SandboxBox.IsChecked == true);
            if (linux && paths.InstallRoot != null) ApplyLinuxSandboxSettings(new InstallLayout(paths.InstallRoot));
        };
        if (linux && paths.InstallRoot != null)
        {
            ApplyLinuxSandboxSettings(new InstallLayout(paths.InstallRoot));
            Opened += async (_, _) => await CheckLinuxSandboxOnStartup(new InstallLayout(paths.InstallRoot));
        }
        else
        {
            SandboxBox.IsEnabled = SessionRunner.CanSandbox(paths);
            SandboxBox.IsChecked = SessionRunner.SandboxByDefault(paths);
        }
    }

    // Settings > Sandbox on Linux: the switch is offered only for a current pass, stays off after a failure, and the note
    // says why. The stored preference is kept, so a pass after an update turns the player's explicit on choice back on.
    void ApplyLinuxSandboxSettings(InstallLayout install)
    {
        if (!OperatingSystem.IsLinux()) return;
        var preference = ProfileStore.LoadSandbox(paths);
        var available = SessionRunner.CanSandbox(paths);
        var problem = SessionRunner.LinuxSandboxProgramProblem(paths) ??
            LinuxSandboxReadiness.Problem(install) ?? "the Linux sandbox self-test did not pass";
        updatingSandboxUi = true;
        try
        {
            SandboxUnavailable.IsVisible = false;
            SandboxLinuxNote.IsVisible = true;
            SandboxBox.IsEnabled = available;
            SandboxBox.IsChecked = available && preference;
            var note = !available
                ? "The Linux sandbox is not available: " + problem + ". The switch stays off; sessions run unsandboxed and say why. " +
                  "Run Check and repair to test it again."
                : preference
                    ? "A sandboxed session runs the game in its own Wine prefix and a read-only filesystem view. If HellForge cannot build that view, the session stops and never runs the game unsandboxed. Turn this off only to run without the sandbox."
                    : "The Linux sandbox is ready, but this switch is off. This session will run unsandboxed: the game, its mods and anything they run can write the player's files.";
            SandboxLinuxNote.Text = note;
        }
        finally { updatingSandboxUi = false; }
    }

    // The result is bound to the app build and kernel. Run it once when either changed, not on every start after a current
    // failure: Check and repair reruns it, and a player who turned the switch off is left alone.
    async Task CheckLinuxSandboxOnStartup(InstallLayout install)
    {
        if (linuxSandboxCheckRunning || setupRunning || runner.IsRunning || !ProfileStore.LoadSandbox(paths)) return;
        if (LinuxSandboxReadiness.IsCurrent(LinuxSandboxReadiness.Read(install))) return;
        linuxSandboxCheckRunning = true;
        try { await RunLinuxSandboxProbe(install); }
        finally { linuxSandboxCheckRunning = false; }
    }
}
