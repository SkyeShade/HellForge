using System.Text.RegularExpressions;

namespace HellForge.Core;

// Who may read a sync folder. The capture (HellForge.CaptureProxy) makes it private, but on a standard Windows account
// the UAC prompt runs the capture as the administrator who answered it, so the launcher passes its own account
// (--grant-sid) and the capture grants both. The capture compiles this file in as well, so it uses nothing else from
// HellForge.Core.
public static class CaptureAccess
{
    // A SID string (S-1-5-21-...), and nothing else: it goes onto icacls' command line.
    public static bool IsSid(string value) => Regex.IsMatch(value, @"^S-1-[0-9]{1,13}(-[0-9]{1,10}){1,15}\z");

    // icacls arguments that make the sync folder private: inheritance removed, full control for the capture's own account
    // and, when it differs, the launcher's. SIDs, not names: a name icacls cannot resolve fails the whole call.
    public static string SyncFolderAclArguments(string folder, string captureSid, string? launcherSid)
    {
        var grants = "*" + captureSid + ":(OI)(CI)F";
        if (launcherSid != null && !string.Equals(launcherSid, captureSid, StringComparison.OrdinalIgnoreCase)) grants += " *" + launcherSid + ":(OI)(CI)F";
        return $"\"{folder}\" /inheritance:r /grant:r {grants}";
    }
}
