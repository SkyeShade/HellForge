using HellForge.Core;

// Which source Linux setup copies the supported build from: Windows' order (installed build, depot download, else Steam's
// download).
static class SetupSourceTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux setup picks the build's source in Windows' order", () =>
        {
            check(SetupSourceChoice.Linux(steamGameSupported: true, depotDownloadComplete: false) == SetupSourceKind.SteamGame,
                "Steam's supported build was not used.");
            check(SetupSourceChoice.Linux(steamGameSupported: true, depotDownloadComplete: true) == SetupSourceKind.SteamGame,
                "A depot download came before Steam's supported build.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: true) == SetupSourceKind.DepotDownload,
                "Steam's depot download was not used.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: false) == SetupSourceKind.SteamDownload,
                "Without the build, Steam's download was not offered.");
        });
    }
}
