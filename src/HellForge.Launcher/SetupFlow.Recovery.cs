using HellForge.Core;

namespace HellForge.Launcher;

// Setup after a session cut short: the SDK stand-ins it left in game/bin go back first, a copy still not complete opens
// Check and repair with its first problem once setup has finished here, and a copy that verifies needs nothing from Steam.
public partial class MainWindow
{
    // A copy that is not complete: Check and repair with its first problem named once setup has finished here, otherwise
    // first setup.
    void EnterSetupFor(InstallLayout install, string problem, string? recoveryError = null)
    {
        if (SetupReceipt.FinishedBefore(install) is not { } finished) { EnterSetup(repair: false); return; }
        var stuck = recoveryError == null ? ""
            : $" The SDK files an interrupted session left could not be put back ({Privacy.Scrub(recoveryError)}): close HELLDIVERS 2 if it is still running, then restart HellForge.";
        EnterSetup(repair: true, reason: $"{finished} {problem}.{stuck} Check and repair the game copy; completed files are kept.");
    }

    // This installation's own copy, checked before Steam: once an interrupted session's SDK stand-ins are back, a copy
    // that verifies needs nothing from Steam, whose installed game may have moved on to a newer build (Linux:
    // FindLinuxGame). Every file is hashed: Check and repair does not trust the size and time cache.
    async Task<bool> ExistingCopyVerifies(InstallLayout install, SupportedBuild build)
    {
        if (!build.IsComplete(install.Game))
        {
            if (!Directory.Exists(Path.Combine(install.Temp, "sdk-shim-originals"))) return false; // no session has run here
            try { SdkShimRecovery.Recover(install); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                throw new SetupStopped("The SDK files an interrupted session left could not be put back: " + error.Message +
                    " Close HELLDIVERS 2 if it is still running, then start setup again.");
            }
            if (!build.IsComplete(install.Game)) return false;
        }
        SetRow(SetupStep.Find, null, "Checking the existing HellForge game folder...", running: true);
        return await Task.Run(() => build.Verify(install.Game).Count == 0);
    }
}
