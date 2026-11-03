namespace HellForge.Core;

// Which source Linux setup copies the supported build from, in the order Windows setup tries them: Steam's installed
// supported build, Steam's depot download, then Steam's depot download to be fetched.
public enum SetupSourceKind { SteamGame, DepotDownload, SteamDownload }

public static class SetupSourceChoice
{
    public static SetupSourceKind Linux(bool steamGameSupported, bool depotDownloadComplete) =>
        steamGameSupported ? SetupSourceKind.SteamGame
        : depotDownloadComplete ? SetupSourceKind.DepotDownload
        : SetupSourceKind.SteamDownload;
}
