namespace HellForge.Core;

// HellForge added to Steam as a non-Steam game and started from there: Steam gives the launcher the shortcut's own
// identity in SteamAppId, SteamGameId and SteamOverlayGameId, and every program HellForge starts inherits it. Steam's API
// takes SteamAppId before steam_appid.txt, so the ownership probe and the game then sign in to Steam as the shortcut
// instead of HELLDIVERS 2, and HellForge reports Steam as not detected (#434). On Windows the launcher and the CLI take
// the shortcut's values out of their own environment as they start: their programs fall back to the steam_appid.txt
// HellForge writes (553850), as when HellForge is started outside Steam. Linux keeps its own handling: hellforge-linux.sh
// keeps an inherited SteamGameId for Steam's runtime, and the Proton calls drop SteamAppId (LinuxProtonInvocation).
public static class SteamShortcutEnvironment
{
    public static readonly IReadOnlyList<string> Names = ["SteamAppId", "SteamGameId", "SteamOverlayGameId"];

    // The variables that name another app than HELLDIVERS 2 (empty ones too), given how to read one.
    public static IReadOnlyList<string> Foreign(Func<string, string?> read) =>
        [.. Names.Where(name => read(name) is { } value && value.Trim() != LocalInstall.AppId)];

    // Removes them from this process's environment, on Windows. Returns what it removed.
    public static IReadOnlyList<string> Forget()
    {
        if (!OperatingSystem.IsWindows()) return [];
        var foreign = Foreign(Environment.GetEnvironmentVariable);
        foreach (var name in foreign) Environment.SetEnvironmentVariable(name, null);
        return foreign;
    }
}
