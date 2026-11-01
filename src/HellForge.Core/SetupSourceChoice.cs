namespace HellForge.Core;

// Which source Linux setup copies the supported build from, in the order Windows setup tries them: Steam's installed
// supported build, Steam's complete depot download, else the download from Steam for the player's account (the setup
// card's three ways: the QR code, SteamCMD, or the Steam app's console), which copies what Steam's installed build holds.
public enum SetupSourceKind { SteamGame, DepotDownload, SteamDownload }

public static class SetupSourceChoice
{
    public static SetupSourceKind Linux(bool steamGameSupported, bool depotDownloadComplete) =>
        steamGameSupported ? SetupSourceKind.SteamGame
        : depotDownloadComplete ? SetupSourceKind.DepotDownload
        : SetupSourceKind.SteamDownload;
}
