using System.Globalization;
using System.Runtime.InteropServices;
using HellForge.Core;

namespace HellForge.Session;

// The facts a session's log opens with (SessionFacts): read once, before anything is started, never from the game.
static class SessionHeader
{
    public static SessionFacts Facts(PlayOptions options, string engine) =>
        SessionFacts.Collect(engine, new SessionSettings(options.Sandbox, War(options), options.LocalPlayFab, options.Multiplayer, options.EngineLog,
            Environment.GetEnvironmentVariable("HELLFORGE_CRASH_DUMP") is { Length: > 0 }, options.GameOptions), WineVersion());

    // live, frozen, or open with its seed and what it deals
    static string War(PlayOptions options)
    {
        if (options.LiveWar != null) return ProfileStore.WarLive;
        if (!options.OpenWar) return ProfileStore.WarFrozen;
        return $"{ProfileStore.WarOpen} (seed {options.WarSeed.ToString(CultureInfo.InvariantCulture)}, subfactions {(options.DealSubfactions ? "yes" : "no")}, " +
            $"SEAF {(options.DealSeaf ? "yes" : "no")})";
    }

    // Wine's own version under Wine or Proton (ntdll's wine_get_version), else null.
    static unsafe string? WineVersion()
    {
        if (!LinuxNetworkBlock.UnderWine) return null;
        try
        {
            if (!NativeLibrary.TryLoad("ntdll.dll", out var ntdll) || !NativeLibrary.TryGetExport(ntdll, "wine_get_version", out var export)) return "unknown";
            return Marshal.PtrToStringAnsi(((delegate* unmanaged<nint>)export)()) ?? "unknown";
        }
        catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return "unknown"; }
    }
}
