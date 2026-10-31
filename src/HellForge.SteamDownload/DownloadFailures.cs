using HellForge.Core;

namespace HellForge.SteamDownload;

// What setup does after the Steam download stopped. Retry: Check again signs in and continues (files already written are
// kept). FreeSpace: the drive is full; the download would need the same room again. SteamConsole: offer Steam's own
// download (download_depot in the Steam console), which runs in the Steam client under the same account.
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
            $"that signed in. The Steam client may still download build {buildId} for you: follow the steps below."),
        SteamSignInException signIn => new(DownloadFallback.Retry, signIn.Message + " Click Check again to sign in again."),
        HttpRequestException or IOException or TimeoutException or InvalidDataException => new(DownloadFallback.SteamConsole,
            $"The download from Steam stopped: {Why(error)} Click Check again to continue it (the files already downloaded are kept), " +
            "or let the Steam client download the build: follow the steps below."),
        _ => null,
    };

    // A failed request's cause, not "see the inner exception".
    static string Why(Exception error) => error is HttpRequestException { InnerException: { } inner } ? inner.GetBaseException().Message : error.Message;
}
