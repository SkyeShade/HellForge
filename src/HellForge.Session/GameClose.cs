using System.Diagnostics;
using System.Runtime.InteropServices;

namespace HellForge.Session;

// Asks the game to close as its window's close button does (WM_CLOSE to its top-level windows), so it writes what it
// keeps only on a clean exit: its shader cache above all (ShaderCache). A process at the player's integrity may post to
// a low-integrity game's windows; the game cannot post back.
static class GameClose
{
    delegate bool EnumCallback(nint window, nint value);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, nint value);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(nint window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessageW(nint window, uint message, nint wparam, nint lparam);
    [DllImport("user32.dll")] static extern bool IsHungAppWindow(nint window);

    const uint WM_CLOSE = 0x0010, GW_OWNER = 4;

    // How a close request went. NoWindow and Hung are never waited for: a game without a window (stuck quitting, or still
    // starting) or with a window that does not respond would not read the request, so the session ends it at once.
    public enum Outcome { AlreadyExited, NoWindow, Hung, ClosedByItself, TimedOut }

    // The process's visible, unowned top-level windows (at most 8), and whether one is hung.
    static (List<nint> Windows, bool Hung) Windows(int processId)
    {
        var windows = new List<nint>();
        var hung = false;
        EnumWindows((window, _) =>
        {
            if (GetWindowThreadProcessId(window, out var pid) != 0 && pid == (uint)processId && IsWindowVisible(window) &&
                GetWindow(window, GW_OWNER) == 0 && windows.Count < 8)
            {
                windows.Add(window);
                hung |= IsHungAppWindow(window);
            }
            return true;
        }, 0);
        return (windows, hung);
    }

    // Asks the game to close (WM_CLOSE to each of its windows) and waits for it, up to `timeout`.
    public static Outcome CloseAndWait(Process game, TimeSpan timeout, out int windows)
    {
        windows = 0;
        game.Refresh();
        if (game.HasExited) return Outcome.AlreadyExited;
        var (found, hung) = Windows(game.Id);
        windows = found.Count;
        if (found.Count == 0) return Outcome.NoWindow;
        if (hung) return Outcome.Hung;
        if (found.Count(window => PostMessageW(window, WM_CLOSE, 0, 0)) == 0) return Outcome.NoWindow;
        return game.WaitForExit(timeout) ? Outcome.ClosedByItself : Outcome.TimedOut;
    }
}
