using HellForge.Core;

// Which source Linux setup copies the supported build from: Windows' order (installed build, depot download, build
// patch, else Steam's download), with a patch that failed earlier skipped.
static class SetupSourceTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var build = SupportedBuild.Current.BuildId;
        var pack = new DownpatchPack("26088801", build, $"https://files.hf-mod.com/downpatch/downpatch-26088801-to-{build}.zip",
            new string('A', 64), 1);

        test("Linux setup picks the build's source in Windows' order and skips a failed patch", () =>
        {
            check(SetupSourceChoice.Linux(steamGameSupported: true, depotDownloadComplete: false, pack: null, patchFailed: false) ==
                SetupSourceKind.SteamGame, "Steam's supported build was not used.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: true, pack: null, patchFailed: false) ==
                SetupSourceKind.DepotDownload, "Steam's depot download was not used.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: false, pack: pack, patchFailed: false) ==
                SetupSourceKind.BuildPatch, "The published build patch was not used.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: false, pack: pack, patchFailed: true) ==
                SetupSourceKind.SteamDownload, "A failed patch was tried again instead of offering Steam's download.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: false, pack: null, patchFailed: false) ==
                SetupSourceKind.SteamDownload, "A build without a patch did not offer Steam's download.");
            check(SetupSourceChoice.Linux(steamGameSupported: false, depotDownloadComplete: true, pack: pack, patchFailed: false) ==
                SetupSourceKind.DepotDownload, "A complete depot download did not come before the build patch.");
        });
    }
}
