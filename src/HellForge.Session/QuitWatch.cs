namespace HellForge.Session;

// The game that closed its window but never exited (a player's "the game refuses to close, I end it in Task Manager"):
// once the game has shown a window and then has had none for Grace, it is stuck quitting, and the session ends it as
// the launcher's Stop does, so the clean-up (and a sandbox's copy-back) still runs. A window that is only hung (not
// responding) is noted once it has been hung for HungNote, and never ended for it: a long load hangs the window too.
// Fed once a second by PlaySession.Observe with the game's visible top-level windows (GameWindows.Visible).
public sealed class QuitWatch(int graceMilliseconds = QuitWatch.GraceMilliseconds, int hungNoteMilliseconds = QuitWatch.HungNoteMilliseconds)
{
    public const int GraceMilliseconds = 30000, HungNoteMilliseconds = 15000;

    public enum Verdict { Running, HungNote, StuckQuitting }

    bool sawWindow, hungNoted;
    long? goneSince, hungSince;

    // Whether the game has shown a window this session.
    public bool SawWindow => sawWindow;
    // How long the game has had no window (since it last had one), or 0.
    public long WindowlessMilliseconds(long now) => goneSince is { } since ? now - since : 0;

    // One look at the game: its visible windows and whether one of them is hung. StuckQuitting once (the caller ends the
    // game); HungNote once per hang (the caller logs what the game's threads are doing).
    public Verdict Update(long now, int visibleWindows, bool hung)
    {
        if (visibleWindows > 0)
        {
            sawWindow = true;
            goneSince = null;
            if (!hung) { hungSince = null; hungNoted = false; return Verdict.Running; }
            hungSince ??= now;
            if (hungNoted || now - hungSince < hungNoteMilliseconds) return Verdict.Running;
            hungNoted = true;
            return Verdict.HungNote;
        }
        hungSince = null;
        hungNoted = false;
        if (!sawWindow) return Verdict.Running; // still starting: no window yet
        goneSince ??= now;
        return now - goneSince >= graceMilliseconds ? Verdict.StuckQuitting : Verdict.Running;
    }
}
