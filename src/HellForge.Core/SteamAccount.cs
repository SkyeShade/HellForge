using Microsoft.Win32;

namespace HellForge.Core;

// The Steam account the local Steam client is signed in to, as a SteamID64: the client keeps its 32-bit account id in
// HKCU\Software\Valve\Steam\ActiveProcess "ActiveUser" (0 while signed out). Null when Steam is signed out or not running,
// and off Windows.
public static class SteamAccount
{
    const ulong IndividualAccounts = 76561197960265728;

    public static ulong FromAccountId(uint accountId) => IndividualAccounts + accountId;

    public static ulong? Current()
    {
        if (!OperatingSystem.IsWindows()) return null;
        return Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null) is int id && id != 0
            ? FromAccountId(unchecked((uint)id)) : null;
    }
}
