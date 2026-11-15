namespace HellForge.Core;

/// <summary>
/// What a sandboxed game was refused (the network gate's "denied" group, native/steam-cloud/net_gate.c): named shared
/// memory, events, mutexes, semaphores, pipes and files that Windows keeps from a low-integrity process (a folder opened
/// as a file or a read-only file opened for writing, refused at any level, are left out there). Overlays reach
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
        ["nahimicosd.dll"] = "Nahimic's audio overlay",
        ["nahimicmsiosd.dll"] = "Nahimic's audio overlay",
        ["audiodevprops2.dll"] = "Nahimic's audio overlay",
    };

    // Programs that crash a sandboxed game as it starts when they are refused what they ask for, and how a player turns
    // them off. Nahimic (A-Volute's audio software, bundled with MSI, Dell, Lenovo and ASUS audio drivers) crashed every
    // sandboxed start it was refused in, in the bug reports (#369, ticket 0402); other refused overlays never did.
    const string TurnOffNahimic = "Turn Nahimic off (stop and disable the Nahimic service in Windows' Services app, or uninstall Nahimic)";
    static readonly Dictionary<string, (string Name, string TurnOff)> Crashers = new(StringComparer.OrdinalIgnoreCase)
    {
        ["nahimicosd.dll"] = ("Nahimic's audio overlay", TurnOffNahimic),
        ["nahimicmsiosd.dll"] = ("Nahimic's audio overlay", TurnOffNahimic),
        ["audiodevprops2.dll"] = ("Nahimic's audio overlay", TurnOffNahimic),
    };

    /// <summary>
    /// For a sandboxed game that crashed as it started: what to do when a program known to crash it there was refused,
    /// or null.
    /// </summary>
    public static string? CrashHint(IEnumerable<Refusal> refusals)
    {
        var modules = refusals.Select(r => ModuleOf(r.Caller)).Where(Crashers.ContainsKey).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        if (modules.Count == 0) return null;
        var (name, turnOff) = Crashers[modules[0]];
        return $"{name} ({string.Join(", ", modules)}) was in the game, and the sandbox refused it what it asked for, which crashes the game as it " +
            $"starts. {turnOff} and press PLAY, or turn the sandbox off in Settings > Sandbox.";
    }

    /// <summary>What a sandboxed game that crashed as it started, with no known cause, is worth trying.</summary>
    public const string CrashAdvice = "Press PLAY to try again. If it closes again, press PLAY once with the sandbox off (Settings > Sandbox): if the " +
        "game then starts, a program on this PC breaks in the sandbox. Either way, save the crash report on the Play page and attach it to a GitHub issue.";

    const int NamesPerModule = 3;

    /// <summary>The module of a caller ("nvspcap64.dll+0x1234" is nvspcap64.dll).</summary>
    public static string ModuleOf(string caller)
    {
        var plus = caller.LastIndexOf('+');
        return plus > 0 ? caller[..plus] : caller;
    }

    /// <summary>The modules known to be overlays (the in-game editor's list must be among them; SandboxSessionTests).</summary>
    public static IEnumerable<string> OverlayModules => Overlays.Keys;

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
        return $"Windows refused the sandboxed game {times} time(s), so what asked could not reach it: " + string.Join("; ", modules) +
            ". networkCensus.deniedTargets in result.json lists each one.";
    }

    /// <summary>
    /// The session log's line for the tablet gate (native/steam-cloud/tablet_gate.c), or null when it had nothing to do.
    /// refused counts the refused loads; loaded is a Wintab32.dll that got past it.
    /// </summary>
    public static string? DescribeTablet(long refused, bool loaded) =>
        loaded ? "The tablet driver (Wintab32.dll) loaded in the sandboxed game although the sandbox refuses it, so its \"OpenMem failed\" box can still " +
            "show on alt-tab. networkCensus.tabletLoaded in result.json records it."
        : refused > 0 ? $"The sandboxed game was kept from the tablet driver (Wintab32.dll) {refused} time(s). The game has no use for it, and at low " +
            "integrity some drivers show an \"OpenMem failed\" box on every alt-tab. A pen still works as a mouse."
        : null;

    static string Part((string Module, long Count, List<string> Names) m)
    {
        var who = OverlayOf(m.Module) is { } overlay ? $"{overlay} ({m.Module})" : m.Module;
        var names = string.Join(", ", m.Names.Take(NamesPerModule)) + (m.Names.Count > NamesPerModule ? $" and {m.Names.Count - NamesPerModule} more" : "");
        return $"{who} {m.Count}x: {names}";
    }
}
