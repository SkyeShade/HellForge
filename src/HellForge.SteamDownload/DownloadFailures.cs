using HellForge.Core;

namespace HellForge.SteamDownload;

// What setup does after the Steam download stopped. Retry: Check again signs in and continues (files already written are
// kept). FreeSpace: the drive is full; the download would need the same room again. SteamConsole: offer the Steam app's
// own download (download_depot in its console), which runs in the Steam client under the player's account.
public enum DownloadFallback { Retry, FreeSpace, SteamConsole }

public sealed record DownloadFailure(DownloadFallback Fallback, string Message);

public static class DownloadFailures
{
    // null: not a failure setup handles (a bug, which setup shows as it is).
    public static DownloadFailure? Describe(Exception error, string buildId) => error switch
    {
        NotEnoughSpaceException space => new(DownloadFallback.FreeSpace,
            $"{space.Message} Free some space on that drive, then click Check again: the files already downloaded are kept."),
        DepotAccessDeniedException denied => new(DownloadFallback.SteamConsole,
            $"{denied.Message} This happens when HELLDIVERS 2 is borrowed through Steam Family Sharing rather than owned by the account " +
            $"that signed in. The Steam app may still download build {buildId} for you: follow the steps below."),
        SteamSignInException signIn => new(DownloadFallback.Retry, signIn.Message + " Click Check again to sign in again."),
        SteamCmdException steamCmd => new(DownloadFallback.SteamConsole,
            $"SteamCMD cannot be used: {steamCmd.Message} The Steam app can download build {buildId} for you instead: follow the steps below."),
        HttpRequestException or IOException or TimeoutException or InvalidDataException => new(DownloadFallback.SteamConsole,
            $"The download from Steam stopped: {Why(error)} The Steam app can download the build instead: follow the steps below. Or " +
            "choose another way to try again; the files already downloaded are kept."),
        _ => null,
    };

    // A failed request's cause, not "see the inner exception".
    static string Why(Exception error) => error is HttpRequestException { InnerException: { } inner } ? inner.GetBaseException().Message : error.Message;
}
