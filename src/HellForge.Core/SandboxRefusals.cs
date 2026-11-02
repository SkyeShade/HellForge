namespace HellForge.Core;

/// <summary>
/// What a sandboxed game was refused (the network gate's "denied" group, native/steam-cloud/net_gate.c): named shared
/// memory, events, mutexes, semaphores, pipes and files that Windows keeps from a low-integrity process. Overlays reach
/// their own program that way, so the session log names each module that was refused, by the overlay it is when it is
/// a known one, with how often and the first objects it asked for.
/// </summary>
public static class SandboxRefusals
{
    /// <summary>One refused call: the object asked for, the caller as "module+0xrva", and how many times (a lower bound).</summary>
    public readonly record struct Refusal(string To, string Caller, long Count);

    // The modules known to draw over games (the in-game editor's list, native/editor/editor.cpp LogOverlays), by the
    // name a player knows them by.
    static readonly Dictionary<string, string> Overlays = new(StringComparer.OrdinalIgnoreCase)
    {
        ["gameoverlayrenderer64.dll"] = "Steam's overlay",
        ["nvspcap64.dll"] = "NVIDIA's overlay",
        ["discordhook64.dll"] = "Discord's overlay",
        ["discordoverlay64.dll"] = "Discord's overlay",
        ["rtsshooks64.dll"] = "RivaTuner (MSI Afterburner)",
        ["graphics-hook64.dll"] = "OBS's game capture",
        ["ow-graphics-hook64.dll"] = "Overwolf",
        ["owexplorer.dll"] = "Overwolf",
        ["igo64.dll"] = "the EA app's overlay",
        ["overlay64.dll"] = "Ubisoft Connect's overlay",
        ["eosovh-win64-shipping.dll"] = "Epic's overlay",
        ["gclay64.dll"] = "GOG Galaxy's overlay",
        ["reshade64.dll"] = "ReShade",
        ["specialk64.dll"] = "Special K",
        ["fraps64.dll"] = "Fraps",
        ["action_x64.dll"] = "Action!",
        ["presentmon64.dll"] = "PresentMon",
    };

    const int NamesPerModule = 3;

    /// <summary>The module of a caller ("nvspcap64.dll+0x1234" is nvspcap64.dll).</summary>
    public static string ModuleOf(string caller)
    {
        var plus = caller.LastIndexOf('+');
        return plus > 0 ? caller[..plus] : caller;
    }

    /// <summary>The overlay a module is, or null when it is not a known one.</summary>
    public static string? OverlayOf(string module) => Overlays.GetValueOrDefault(module);

    /// <summary>
    /// The session log's line for the refusals, or null when there were none. total is the census's own count (it
    /// lists the most frequent refusals only); without it the listed ones are added up.
    /// </summary>
    public static string? Describe(IReadOnlyCollection<Refusal> refusals, long? total = null)
    {
        if (refusals.Count == 0) return null;
        var modules = refusals.GroupBy(r => ModuleOf(r.Caller), StringComparer.OrdinalIgnoreCase)
            .Select(g => (Module: g.Key, Count: g.Sum(r => r.Count), Names: g.OrderByDescending(r => r.Count).Select(r => r.To).Distinct().ToList()))
            .OrderBy(m => OverlayOf(m.Module) == null).ThenByDescending(m => m.Count).ThenBy(m => m.Module, StringComparer.OrdinalIgnoreCase)
            .Select(Part);
        var times = Math.Max(total ?? 0, refusals.Sum(r => r.Count));
        return $"The sandbox refused the game {times} time(s), so what asked could not reach it: " + string.Join("; ", modules) +
            ". networkCensus.deniedTargets in result.json lists each one.";
    }

    static string Part((string Module, long Count, List<string> Names) m)
    {
        var who = OverlayOf(m.Module) is { } overlay ? $"{overlay} ({m.Module})" : m.Module;
        var names = string.Join(", ", m.Names.Take(NamesPerModule)) + (m.Names.Count > NamesPerModule ? $" and {m.Names.Count - NamesPerModule} more" : "");
        return $"{who} {m.Count}x: {names}";
    }
}
