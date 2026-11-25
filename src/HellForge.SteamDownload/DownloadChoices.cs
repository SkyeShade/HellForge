namespace HellForge.SteamDownload;

// How the player gets the supported build when Steam has another one: three ways on setup's card, in this order.
// QrCode: HellForge signs in with a QR code scanned in the Steam app on a phone, and downloads from Steam's servers (the
// default; fully automatic). SteamCmd: no phone needed; Valve's own SteamCMD downloads it, and the player types the
// password and any Steam Guard code into SteamCMD's window, never into HellForge (SteamCmd). SteamClient: the Steam app
// on this PC downloads it through its console (SteamDepotDownload); also where setup goes when another way fails.
// DownpatchFile, last and not a Steam download: a downpatch file a player made and passed on (Downpatch), applied to
// Steam's installed game, with no network at all. HellForge never hosts or lists these files.
public enum DownloadChoice { QrCode, SteamCmd, SteamClient, DownpatchFile }

public static class DownloadChoices
{
    public const DownloadChoice Default = DownloadChoice.QrCode;

    public static IReadOnlyList<DownloadChoice> All { get; } = [DownloadChoice.QrCode, DownloadChoice.SteamCmd, DownloadChoice.SteamClient];

    // The ways setup can offer. reachesSteam: setup can reach Steam's servers itself (not under Wine, inside HellForge's
    // network namespace, which has no route out); else only the Steam app on this PC can download. steamCmd: SteamCMD
    // can be shown to the player (on Linux it needs a terminal window program) in a folder it accepts.
    // downpatchFile: Steam has HELLDIVERS 2 installed, which a downpatch file is applied to.
    public static IReadOnlyList<DownloadChoice> Offered(bool reachesSteam, bool steamCmd = true, bool downpatchFile = false)
    {
        IReadOnlyList<DownloadChoice> steam = !reachesSteam ? [DownloadChoice.SteamClient] : steamCmd ? All : [.. All.Where(choice => choice != DownloadChoice.SteamCmd)];
        return downpatchFile ? [.. steam, DownloadChoice.DownpatchFile] : steam;
    }

    // Whether HellForge signs in to Steam itself (only with the QR code; SteamCMD and the Steam app sign in on their own).
    public static bool SignsIn(DownloadChoice choice) => choice == DownloadChoice.QrCode;

    // The choice's button.
    public static string Title(DownloadChoice choice) => choice switch
    {
        DownloadChoice.QrCode => "Scan a QR code with your phone",
        DownloadChoice.SteamCmd => "No phone: sign in with SteamCMD",
        DownloadChoice.DownpatchFile => "Use a downpatch file",
        _ => "Use the Steam app",
    };

    // One line under the button, while the player chooses.
    public static string Hint(DownloadChoice choice) => choice switch
    {
        DownloadChoice.QrCode => "The quickest: approve the sign-in in the Steam app on your phone, and setup does the rest.",
        DownloadChoice.SteamCmd => "No phone needed: you type your password into Valve's own SteamCMD window, never into HellForge.",
        DownloadChoice.DownpatchFile => "No download: a file another player made with HellForge from your Steam build. HellForge doesn't host these.",
        _ => "No sign-in here at all: the Steam app on this PC downloads the build through its console.",
    };

    // The chosen way in plain words, above its steps.
    public static string Explanation(DownloadChoice choice, string buildId) => choice switch
    {
        DownloadChoice.QrCode => "Open the Steam app on your phone, tap the Steam Guard shield, then the QR code scanner, and scan this " +
            "code. Then approve the sign-in in the app. HellForge never sees your password, and nothing of the sign-in is kept.",
        DownloadChoice.SteamCmd => "No phone needed. HellForge gets SteamCMD, Valve's own command-line Steam tool, from Valve and starts " +
            "it in a window of its own, with everything filled in except your password. Type your password in the SteamCMD window; it " +
            "goes to Valve's own tool, not HellForge. If Steam asks for a Steam Guard code (by email), type it there too. SteamCMD then " +
            $"downloads build {buildId} to your own account, and HellForge checks every file against Steam's hashes and moves it into " +
            "this folder. Afterwards HellForge removes SteamCMD and what it remembered of the sign-in.",
        DownloadChoice.DownpatchFile => $"A downpatch file turns the HELLDIVERS 2 build your Steam has into build {buildId}, with no download. " +
            "A player with HellForge and the same Steam build as yours makes one in Settings > Installation and sends it to you. " +
            "HellForge doesn't host these; you make one and share it yourself. Every file it writes is checked against Steam's " +
            "hashes, and setup still checks that you own the game. The Steam download stays the recommended way.",
        _ => $"No sign-in in HellForge: the Steam app on this PC downloads build {buildId} to your own account, through its " +
            "console. HellForge then copies it into this folder, every file checked against Steam's hashes.",
    };
}
