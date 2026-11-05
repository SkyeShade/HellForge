using Avalonia.Controls;
using Avalonia.Layout;
using HellForge.SteamDownload;

namespace HellForge.Launcher;

// The asset kit's Steam sign-in (GET THEM FROM STEAM, CHECK OLDER BUILDS; docs/modding/assets.md "Signing in to Steam"):
// the first is a QR code for the Steam app, and the token Steam returns is kept for this user (SteamLogins), so the next
// needs no phone. Where the cards offer it, they say so, and Sign out of Steam deletes it. Setup's sign-in keeps nothing.
public partial class MainWindow
{
    // How the next sign-in goes, in a clause.
    static string SignInWords => SteamLogins.Default.HasOwn
        ? "you are still signed in to Steam from an earlier fetch, so no phone is needed"
        : "sign in once with the Steam app on your phone; HellForge keeps that sign-in for your user only, until you sign out";

    // Sign out of Steam, when HellForge has a saved sign-in (never while a fetch is using it).
    Button? SignOutButton()
    {
        if (!SteamLogins.Default.HasOwn) return null;
        var button = new Button { Content = "Sign out of Steam", Classes = { "flat" }, IsEnabled = assetFetch == null && !modKitBusy, HorizontalAlignment = HorizontalAlignment.Left };
        ToolTip.SetShowOnDisabled(button, true);
        ToolTip.SetTip(button, "Deletes HellForge's saved Steam sign-in (" + SteamLogins.Default.OwnPath + "). The next fetch asks for the QR code again. " +
            "Steam also lists it as \"HellForge\" among your account's authorized devices, where you can remove it.");
        button.Click += (_, _) =>
        {
            try
            {
                if (SteamLogins.Default.Forget()) Append("Signed out of Steam: HellForge's saved sign-in is deleted.", LauncherLine);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                Append("Could not delete the saved Steam sign-in: " + error.Message, Red);
            }
            RefreshMods();
        };
        return button;
    }
}
