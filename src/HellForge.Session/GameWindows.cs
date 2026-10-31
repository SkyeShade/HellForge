using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace HellForge.Session;

// What the session can see of a game that does not close (QuitWatch): its visible top-level windows, whether one is hung,
// and a cheap picture of what is still running (its threads by where they started and what they wait on, Windows Error
// Reporting and the game's crash reporter, which hold a crashed game open while they write their report).
static class GameWindows
{
    delegate bool EnumCallback(nint window, nint value);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, nint value);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(nint window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] static extern bool IsHungAppWindow(nint window);

    // The game's visible top-level windows (a minimised one counts), and whether any of them is hung.
    public static (int Visible, bool Hung) Visible(int processId)
    {
        int visible = 0;
        var hung = false;
        EnumWindows((window, _) =>
        {
            if (GetWindowThreadProcessId(window, out var pid) == 0 || pid != (uint)processId || !IsWindowVisible(window)) return true;
            visible++;
            hung |= IsHungAppWindow(window);
            return true;
        }, 0);
        return (visible, hung);
    }

    // Programs that keep a crashed or quitting game open, by name (Windows Error Reporting, the game's crash reporter).
    static readonly string[] Holders = ["WerFault", "WerFaultSecure", "crs-handler", "GameGuard", "GameMon", "GameMon64"];

    // The game's threads, grouped by the module and offset they started at, with their states; and the holders running
    // since the game started. Everything here is read without stopping the game.
    public static JsonObject Snapshot(Process game, DateTime gameStartedUtc)
    {
        var snapshot = new JsonObject { ["utc"] = DateTime.UtcNow.ToString("o"), ["pid"] = game.Id };
        try
        {
            game.Refresh();
            snapshot["hasExited"] = game.HasExited;
            snapshot["threadCount"] = game.Threads.Count;
            snapshot["threads"] = Threads(game);
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        { snapshot["threadsError"] = error.Message; }
        snapshot["holders"] = HoldersSince(gameStartedUtc);
        return snapshot;
    }

    // The game's threads by where they started (the 40 commonest), each with how many are in which state.
    static JsonArray Threads(Process game)
    {
        var modules = Modules(game);
        var groups = new Dictionary<string, (int Count, Dictionary<string, int> States)>(StringComparer.Ordinal);
        foreach (ProcessThread thread in game.Threads)
            using (thread)
            {
                var start = Where(thread, modules);
                var state = State(thread);
                if (!groups.TryGetValue(start, out var group)) group = (0, new Dictionary<string, int>(StringComparer.Ordinal));
                group.States[state] = group.States.GetValueOrDefault(state) + 1;
                groups[start] = (group.Count + 1, group.States);
            }
        return new JsonArray([.. groups.OrderByDescending(g => g.Value.Count).Take(40).Select(g => (JsonNode?)new JsonObject
        {
            ["start"] = g.Key,
            ["count"] = g.Value.Count,
            ["states"] = string.Join(", ", g.Value.States.OrderByDescending(s => s.Value).Select(s => $"{s.Key} x{s.Value}"))
        })]);
    }

    static string State(ProcessThread thread)
    {
        try { return thread.ThreadState == System.Diagnostics.ThreadState.Wait ? "Wait:" + thread.WaitReason : thread.ThreadState.ToString(); }
        catch (Exception error) when (error is InvalidOperationException or NotSupportedException) { return "?"; }
    }

    // The holders started since the game did (or whose start cannot be read).
    static JsonArray HoldersSince(DateTime gameStartedUtc)
    {
        var holders = new JsonArray();
        foreach (var name in Holders)
            foreach (var process in Process.GetProcessesByName(name))
                using (process)
                {
                    var started = StartedUtc(process);
                    if (started is { } s && s < gameStartedUtc.AddSeconds(-5)) continue;
                    holders.Add((JsonNode?)new JsonObject { ["name"] = name, ["pid"] = process.Id, ["startedUtc"] = started?.ToString("o") });
                }
        return holders;
    }

    static DateTime? StartedUtc(Process process)
    {
        try { return process.StartTime.ToUniversalTime(); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException) { return null; }
    }

    // The game's modules' ranges (name, base, size); empty when they cannot be read.
    static List<(string Name, long Base, long Size)> Modules(Process game)
    {
        var modules = new List<(string, long, long)>();
        try
        {
            foreach (ProcessModule module in game.Modules)
                using (module) modules.Add((module.ModuleName, (long)module.BaseAddress, module.ModuleMemorySize));
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException) { }
        return modules;
    }

    // Where a thread started: "module+0xoffset", or its raw address.
    static string Where(ProcessThread thread, List<(string Name, long Base, long Size)> modules)
    {
        long at;
        try { at = (long)thread.StartAddress; }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException) { return "?"; }
        foreach (var (name, start, size) in modules)
            if (at >= start && at < start + size) return $"{name}+0x{at - start:x}";
        return $"0x{at:x}";
    }
}
