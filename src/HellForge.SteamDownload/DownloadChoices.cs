namespace HellForge.SteamDownload;

// How the player gets the supported build when Steam has another one: three ways on setup's card, in this order.
// QrCode: HellForge signs in with a QR code scanned in the Steam app on a phone, and downloads from Steam's servers (the
// default; fully automatic). SteamCmd: no phone needed; Valve's own SteamCMD downloads it, and the player types the
// password and any Steam Guard code into SteamCMD's window, never into HellForge (SteamCmd). SteamClient: the Steam app
// on this PC downloads it through its console (SteamDepotDownload); also where setup goes when another way fails.
public enum DownloadChoice { QrCode, SteamCmd, SteamClient }

public static class DownloadChoices
{
    public const DownloadChoice Default = DownloadChoice.QrCode;

    public static IReadOnlyList<DownloadChoice> All { get; } = [DownloadChoice.QrCode, DownloadChoice.SteamCmd, DownloadChoice.SteamClient];

    // The ways setup can offer. reachesSteam: setup can reach Steam's servers itself (not under Wine, inside HellForge's
    // network namespace, which has no route out); else only the Steam app on this PC can download. steamCmdWindow:
    // SteamCMD can get a window to ask for the password in (the native Linux launcher needs a terminal program to open
    // one, SteamCmdTerminal); else SteamCMD is not offered, and the card says why (NoTerminal).
    public static IReadOnlyList<DownloadChoice> Offered(bool reachesSteam, bool steamCmdWindow = true) =>
        !reachesSteam ? [DownloadChoice.SteamClient] : steamCmdWindow ? All : [DownloadChoice.QrCode, DownloadChoice.SteamClient];

    // Under SteamCMD's button when the PC has no terminal program to open its window (Linux).
    public const string NoTerminal = "Not on this PC: SteamCMD asks for your password in a terminal window, and HellForge found no " +
        "terminal program to open one (such as Konsole, GNOME Terminal or xterm). Install one and start setup again, or use the QR code " +
        "or the Steam app.";

    // Whether HellForge signs in to Steam itself (only with the QR code; SteamCMD and the Steam app sign in on their own).
    public static bool SignsIn(DownloadChoice choice) => choice == DownloadChoice.QrCode;

    // The choice's button.
    public static string Title(DownloadChoice choice) => choice switch
    {
        DownloadChoice.QrCode => "Scan a QR code with your phone",
        DownloadChoice.SteamCmd => "No phone: sign in with SteamCMD",
        _ => "Use the Steam app",
    };

    // One line under the button, while the player chooses.
    public static string Hint(DownloadChoice choice) => choice switch
    {
        DownloadChoice.QrCode => "The quickest: approve the sign-in in the Steam app on your phone, and setup does the rest.",
        DownloadChoice.SteamCmd => "No phone needed: you type your password into Valve's own SteamCMD window, never into HellForge.",
        _ => "No sign-in here at all: the Steam app on this PC downloads the build through its console.",
    };

    // The chosen way in plain words, above its steps.
    public static string Explanation(DownloadChoice choice, string buildId) => choice switch
    {
        DownloadChoice.QrCode => "Open the Steam app on your phone, tap the Steam Guard shield, then the QR code scanner, and scan this " +
            "code. Then approve the sign-in in the app. HellForge never sees your password, and nothing of the sign-in is kept.",
        DownloadChoice.SteamCmd => "No phone needed. HellForge gets SteamCMD, Valve's own command-line Steam tool, from Valve and starts " +
            "it in a window of its own (a terminal window on Linux), with everything filled in except your password. Type your password in the SteamCMD window; it " +
            "goes to Valve's own tool, not HellForge. If Steam asks for a Steam Guard code (by email), type it there too. SteamCMD then " +
            $"downloads build {buildId} to your own account, and HellForge checks every file against Steam's hashes and moves it into " +
            "this folder. Afterwards HellForge removes SteamCMD and what it remembered of the sign-in.",
        _ => $"No sign-in in HellForge: the Steam app on this PC downloads build {buildId} to your own account, through its " +
            "console. HellForge then copies it into this folder, every file checked against Steam's hashes.",
    };
}
