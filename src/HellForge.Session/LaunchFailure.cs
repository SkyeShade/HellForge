namespace HellForge.Session;

// What the player is told when a launch edit (PlaySession.ApplyEarlyConstructor, ApplyRouting) was not made. 0.3.0-beta
// said "The game could not be connected to the local backend." for a game that had crashed while starting, before
// HellForge could connect it (#203, #113): result.json had the crash (gameExitCode 0xC0000005), the message did not. A
// game that closed now says so, with its exit code and where Windows recorded the crash (WindowsCrash); one still running
// says which part of the connect step failed.
static class LaunchFailure
{
    // The two launch edits, as the messages word them.
    public const string Prepare = "prepare it for", Connect = "connect it to";

    // The game closed during `step`, with exit code `code`; crash: Windows' record of the crash (or null), whose module
    // `owner` is. A module of another program is named as the thing to close, since it crashed the game. sandboxed: the
    // session ran the game in the sandbox; sandboxHint: SandboxRefusals.CrashHint, a program known to crash it there,
    // which comes first (its crash is often recorded in Windows' own code: #369, ntdll.dll).
    public static string Closed(string step, int code, WindowsCrash.Record? crash, WindowsCrash.Owner owner, bool sandboxed = false, string? sandboxHint = null)
    {
        var meaning = Tools.ExitMeaning(code);
        var how = $"0x{code:X8}" + (meaning.Length > 0 ? ": " + meaning : "") + (crash != null ? $"; Windows recorded the crash in {crash.Module}+{crash.Offset}" : "");
        return $"The game closed while starting, before HellForge could {step} the local backend (exit code {how}). " + (
            sandboxHint != null ? sandboxHint
            : crash != null && owner == WindowsCrash.Owner.Other
            ? $"{crash.Module} is not part of the game, HellForge or Windows: another program put it into the game, such as an overlay, a recorder, or RGB or " +
              "security software. Close that program, turn off its in-game overlay or add the HellForge folder to its exclusions, then press PLAY."
            : sandboxed && WindowsCrash.IsCrash(code) ? HellForge.Core.SandboxRefusals.CrashAdvice
            : "Press PLAY to try again. If the game closes again, save the crash report on the Play page and attach it to a GitHub issue.");
    }

    // The game still runs, and the connect step's edit was not made; outcome: HellForgeLocalRoutingPatch.Apply's.
    public static string NotConnected(string outcome) => "The game could not be connected to the local backend: " + outcome switch
    {
        "module_not_observed" => "it had not loaded its code (game.dll) after 45 seconds",
        "decoded_bytes_not_observed" or "decoded_bytes_changed" => "its code in memory was not what HellForge expects",
        "patch_failed" => "HellForge's change to its memory did not go through",
        _ => "the edit ended as " + outcome
    } + ". Press PLAY to try again. If it fails again, save the logs (Save logs to Desktop on the Console page) and report it.";
}
