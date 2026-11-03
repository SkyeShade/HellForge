using System.Text.RegularExpressions;

namespace HellForge.Core;

/// <summary>
/// What the Steam broker's log (its standard error, native/steam-cloud/hellforge_steam_broker.c) says about a sandboxed
/// session's start (docs/steam-bridge.md, "The rest of a low-integrity session"). The session runner resumes the game
/// only once the broker is ready: Steam started in the broker, the snapshot filled and the pipe waiting for the game.
/// A broker that ends first is the session's failure, in the player's words, instead of a game that shows STEAM NOT
/// DETECTED.
/// </summary>
public static partial class SteamBrokerLog
{
    /// <summary>How long the session waits for the broker: an antivirus may hold a new program for a cloud check first.</summary>
    public static readonly TimeSpan StartLimit = TimeSpan.FromSeconds(60);

    /// <summary>The broker waits for the game: Steam is started in it and the game may connect.</summary>
    public static bool IsReady(string line) => line.StartsWith("broker: waiting for the game", StringComparison.Ordinal);

    /// <summary>The game connected to the broker and passed its process check.</summary>
    public static bool IsConnected(string line) => line == "broker: game connected";

    /// <summary>
    /// The broker ended because the game is done with Steam: the game said so (SteamAPI_Shutdown, as it quits) or exited.
    /// A pipe that closed under a running game is not that.
    /// </summary>
    public static bool EndedWithGame(IReadOnlyList<string> lines) =>
        lines.Any(l => l.StartsWith("broker: shutdown;", StringComparison.Ordinal) || l.StartsWith("broker: the game exited", StringComparison.Ordinal));

    /// <summary>The session's failure for a broker that ended before it was ready, from its log lines and exit code.</summary>
    public static string Failure(IReadOnlyList<string> lines, int? exitCode)
    {
        if (lines.FirstOrDefault(l => l.StartsWith("broker: SteamAPI_Init", StringComparison.Ordinal)) is { } init)
            return "Steam did not answer HellForge's Steam broker, so the sandboxed game could not see Steam (" + Detail(init) +
                "). Make sure Steam is running and signed in, and if Steam runs as administrator, start it normally; then press PLAY again.";
        if (lines.FirstOrDefault(l => l.StartsWith("broker: cannot load Valve's", StringComparison.Ordinal)) is { } load)
            return "HellForge's Steam broker could not load the game's steam_api64.dll" + WindowsError(load) + ". Run Check and repair, then press PLAY again.";
        if (lines.Any(l => l.StartsWith("broker: Valve's DLL lacks manual dispatch", StringComparison.Ordinal)))
            return "The game's steam_api64.dll is not the one HellForge's Steam broker was built for. Run Check and repair, then press PLAY again.";
        if (lines.FirstOrDefault(l => l.StartsWith("broker: cannot create the channel", StringComparison.Ordinal)) is { } channel)
            return "HellForge's Steam broker could not open its channel to the game" + WindowsError(channel) +
                ". Close any other HellForge session, then press PLAY again.";
        var last = lines.LastOrDefault(l => l.StartsWith("broker: ", StringComparison.Ordinal));
        return "HellForge's Steam broker stopped before the game started" + (exitCode is { } code ? $" (exit code {code})" : "") +
            (last != null ? ": " + last["broker: ".Length..] : "") + ". Press PLAY again, or turn off Settings > Sandbox to play without it.";
    }

    /// <summary>The session's failure for a broker still not ready after StartLimit.</summary>
    public static string NotReady() =>
        $"HellForge's Steam broker was not ready after {StartLimit.TotalSeconds:0} s, so the sandboxed game could not see Steam. " +
        "An antivirus may be checking app\\sdk-shims\\hellforge_steam_broker.exe: allow it, then press PLAY again.";

    // "broker: SteamAPI_Init -> false" -> "SteamAPI_Init -> false"
    static string Detail(string line) => line["broker: ".Length..].Trim();

    // " (Windows error 5)" from "... (5)" at the end of a broker line, or "".
    static string WindowsError(string line) => TrailingCode().Match(line) is { Success: true } m ? $" (Windows error {m.Groups[1].Value})" : "";

    [GeneratedRegex(@"\((\d+)\)\s*$")]
    private static partial Regex TrailingCode();
}
