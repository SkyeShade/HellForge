using System.Globalization;

namespace HellForge.Core;

// The MOD SYNC offer of one session run, for a launcher that shows the prompt outside the game (HellForge.Cli
// mod-sync-offer): whether that run's game shows the prompt itself, whether the run is the one running, and whether an
// answer may be sent. Launchers poll this, so it reads the run's own small files and never loads the mods.
public static class ModSyncRun
{
    public const string EditorId = "hellforge.editor";

    // result.json's status once a run is over (HellForge.Session).
    static readonly string[] Ended = ["observed", "failed", "cleanup_failed"];

    // Whether the run's game shows the prompt itself: the in-game editor is one of the mods the session runs
    // (<run>/session-mods.json, written when the session stages its mods and again on a live change). A session keeps
    // the mods it started with, so the mods folder as it is now is not asked. Before the run has staged its mods, or
    // with no run, this is the editor's switch on the Mods page (hellforge-mods.json), which the next session starts
    // from.
    public static bool InGamePrompt(string? run, string modsFolder)
    {
        if (run != null && File.Exists(Path.Combine(run, SessionMods.FileName)))
        {
            try { return SessionMods.Read(Path.Combine(run, SessionMods.FileName)).Mods.Any(m => m.Id == EditorId); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException or InvalidOperationException) { }
        }
        return !ModSettings.Disabled(modsFolder).Contains(EditorId);
    }

    // Whether that run is the session running now. sessionRunning: a session runner runs on this PC, in any
    // installation. A run that ended (result.json's status) is not it, whatever else runs, and neither is one whose
    // game is gone: a run that crashed keeps its last status and its mod-sync.json. gameAlive: whether a process id is a
    // running game, or null where the run's gamePid cannot be asked about (under Wine it is not the host's).
    public static bool Running(string run, bool sessionRunning, Func<int, bool>? gameAlive = null)
    {
        if (!sessionRunning) return false;
        if (SessionFiles.ReadJson(Path.Combine(run, "result.json")) is not { } result) return true; /* being written: the runner says */
        if (Ended.Contains(SessionFiles.Text(result, "status"))) return false;
        return gameAlive == null || !int.TryParse(SessionFiles.Text(result, "gamePid"), NumberStyles.None, CultureInfo.InvariantCulture, out var pid) || gameAlive(pid);
    }

    // The offer waiting in that run, or null: none, a closed one, or a run that is not running.
    public static ModSyncOffer? Waiting(string? run, bool sessionRunning, Func<int, bool>? gameAlive = null) =>
        run != null && Running(run, sessionRunning, gameAlive) && ModSyncOffer.Read(Path.Combine(run, "backend")) is { State: not ("none" or "") } offer ? offer : null;

    // Checks an answer before it is written: the offer still waits and is the version the player was shown, the game
    // does not show the prompt itself (two prompts would both answer), and the answer is one the session takes.
    // version: --version as typed. ArgumentException: the answer is wrong. InvalidDataException: it cannot be sent now.
    public static int CheckAnswer(ModSyncOffer? offer, bool inGamePrompt, string answer, string? version, string? modId)
    {
        if (!ModSyncOffer.IsAnswer(answer, modId))
            throw new ArgumentException($"{answer}{(modId == null ? "" : " " + modId)} is not an answer to a mod sync offer: accept, decline and restart stand alone; trust, block and open name a mod with --mod.");
        if (!int.TryParse(version, NumberStyles.None, CultureInfo.InvariantCulture, out var number))
            throw new ArgumentException("--answer needs the offer's --version (mod-sync-offer prints it).");
        if (offer == null) throw new InvalidDataException("No mod sync offer is waiting.");
        if (inGamePrompt) throw new InvalidDataException("This session's game shows the MOD SYNC prompt itself (the in-game editor is on): answer it in the game.");
        if (number != offer.Version) throw new InvalidDataException($"The offer has changed (it is version {offer.Version} now): read it again before answering.");
        if (modId != null && offer.Mods.All(m => m.Id != modId)) throw new ArgumentException($"{modId} is not one of the offer's mods.");
        return number;
    }
}
