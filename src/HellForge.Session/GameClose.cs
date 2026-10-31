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

    const uint WM_CLOSE = 0x0010, GW_OWNER = 4;

    // Posts WM_CLOSE to the process's visible, unowned top-level windows; returns how many.
    public static int Request(int processId)
    {
        var windows = new List<nint>();
        EnumWindows((window, _) =>
        {
            if (GetWindowThreadProcessId(window, out var pid) != 0 && pid == (uint)processId && IsWindowVisible(window) &&
                GetWindow(window, GW_OWNER) == 0 && windows.Count < 8)
                windows.Add(window);
            return true;
        }, 0);
        return windows.Count(window => PostMessageW(window, WM_CLOSE, 0, 0));
    }

    // Asks the game to close and waits for it, up to `timeout`; true when it exited by itself.
    public static bool CloseAndWait(Process game, TimeSpan timeout, out int windows)
    {
        windows = 0;
        game.Refresh();
        if (game.HasExited) return true;
        windows = Request(game.Id);
        if (windows == 0) return false;
        return game.WaitForExit(timeout);
    }
}
