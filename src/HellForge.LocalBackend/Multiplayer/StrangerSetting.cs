using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// "Play with strangers" (ModSettings.PlayWithStrangers, never chosen by default), asked for every frame a stranger sends:
// the settings file is read again only when its time stamp changed, and the time stamp is looked at once per Recheck, so a
// flood of frames costs no disk reads. A turn on or off in the launcher reaches a running backend within Recheck. Without a
// choice, a sandboxed game takes strangers and an unsandboxed one does not (ModSettings.StrangersEffective); the first
// answer says which applies and why.
public sealed class StrangerSetting(string? modsFolder, Func<DateTime>? clock = null, bool sandboxed = false, Action<string>? log = null)
{
    public static readonly TimeSpan Recheck = TimeSpan.FromSeconds(2);

    readonly Func<DateTime> now = clock ?? (() => DateTime.UtcNow);
    readonly object gate = new();
    DateTime checkedAt = DateTime.MinValue, stamp = DateTime.MinValue;
    bool allowed, read, logged;

    // How many times the file was read (tests).
    public int Reads { get; private set; }

    public bool Allowed
    {
        get
        {
            if (modsFolder == null) return false;
            lock (gate)
            {
                var time = now();
                if (read && time - checkedAt < Recheck) return allowed;
                checkedAt = time;
                var current = Stamp(modsFolder);
                if (read && current == stamp) return allowed;
                stamp = current;
                var chosen = ModSettings.PlayWithStrangers(modsFolder);
                allowed = ModSettings.StrangersEffective(chosen, sandboxed);
                read = true;
                Reads++;
                if (!logged)
                {
                    logged = true;
                    log?.Invoke(Reason(chosen));
                }
                return allowed;
            }
        }
    }

    string Reason(bool? chosen) => chosen switch
    {
        true => "Play with strangers is on: the player turned it on.",
        false => "Play with strangers is off: the player turned it off.",
        _ when sandboxed => "Play with strangers is on: not chosen, and this game is sandboxed.",
        _ => "Play with strangers is off: not chosen, and this game is not sandboxed."
    };

    static DateTime Stamp(string modsFolder)
    {
        var path = Path.Combine(modsFolder, ModSettings.FileName);
        if (!File.Exists(path)) path = Path.Combine(modsFolder, ModSettings.LegacyFileName);
        return File.Exists(path) ? File.GetLastWriteTimeUtc(path) : DateTime.MinValue;
    }
}
