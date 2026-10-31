namespace HellForge.Core;

// Which source Linux setup copies the supported build from, in the order Windows setup tries them: Steam's installed
// supported build, Steam's depot download, a published build patch, then Steam's depot download to be fetched. A patch
// that failed earlier is not tried again, so the download is offered as on Windows.
public enum SetupSourceKind { SteamGame, DepotDownload, BuildPatch, SteamDownload }

public static class SetupSourceChoice
{
    public static SetupSourceKind Linux(bool steamGameSupported, bool depotDownloadComplete, DownpatchPack? pack, bool patchFailed) =>
        steamGameSupported ? SetupSourceKind.SteamGame
        : depotDownloadComplete ? SetupSourceKind.DepotDownload
        : pack != null && !patchFailed ? SetupSourceKind.BuildPatch
        : SetupSourceKind.SteamDownload;
}
