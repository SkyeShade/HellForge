namespace HellForge.SteamDownload;

// How the player gets the supported build when Steam has another one: three equal choices on setup's card.
// QrCode: sign in by scanning a QR code with the Steam app on a phone (the default). Password: no phone needed; account
// name and password, and the code Steam emails when the account has Steam Guard by email. SteamClient: no sign-in in
// HellForge at all; the Steam client on this PC downloads the build through its console and HellForge copies it.
public enum DownloadChoice { QrCode, Password, SteamClient }

public static class DownloadChoices
{
    public const DownloadChoice Default = DownloadChoice.QrCode;

    public static IReadOnlyList<DownloadChoice> All { get; } = [DownloadChoice.QrCode, DownloadChoice.Password, DownloadChoice.SteamClient];

    // Whether HellForge signs in to Steam itself for this choice (the Steam client choice never does).
    public static bool SignsIn(DownloadChoice choice) => choice != DownloadChoice.SteamClient;

    // The choice's button.
    public static string Title(DownloadChoice choice) => choice switch
    {
        DownloadChoice.QrCode => "Scan a QR code with the Steam app",
        DownloadChoice.Password => "No phone? Account name and password",
        _ => "Download with the Steam client",
    };

    // The choice in plain words, under the buttons.
    public static string Explanation(DownloadChoice choice, string buildId) => choice switch
    {
        DownloadChoice.QrCode => "Open the Steam app on your phone, tap the Steam Guard shield, then the QR code scanner, and scan this " +
            "code. Then approve the sign-in in the app. HellForge never sees your password.",
        DownloadChoice.Password => "No phone needed: sign in with your Steam account name and password. If your account has Steam Guard by " +
            "email, Steam emails you a code to enter here. Your password goes only to Steam, through Steam's own sign-in, and HellForge " +
            "never keeps it.",
        _ => $"No sign-in in HellForge: the Steam client on this PC downloads build {buildId} to your own account, through its " +
            "console. Paste the commands below into the console, one at a time. HellForge watches for the download, then copies it " +
            "into this folder, every file checked against Steam's hashes. This needs about 24 GB more free space while both copies exist.",
    };

    // The sign-in to start for a choice: the QR code at once; the password sign-in once both are typed; none for the Steam
    // client (null).
    public static SignInRequest? Request(DownloadChoice choice, string? account, string? password) => choice switch
    {
        DownloadChoice.QrCode => new QrSignIn(),
        DownloadChoice.Password when !string.IsNullOrWhiteSpace(account) && !string.IsNullOrEmpty(password) =>
            new PasswordSignIn(account.Trim(), password),
        _ => null,
    };
}
