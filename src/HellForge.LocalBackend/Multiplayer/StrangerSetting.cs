using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// "Play with strangers" (ModSettings.PlayWithStrangers, off by default), asked for every frame a stranger sends: the
// settings file is read again only when its time stamp changed, and the time stamp is looked at once per Recheck, so a
// flood of frames costs no disk reads. A turn on or off in the launcher reaches a running backend within Recheck.
public sealed class StrangerSetting(string? modsFolder, Func<DateTime>? clock = null)
{
    public static readonly TimeSpan Recheck = TimeSpan.FromSeconds(2);

    readonly Func<DateTime> now = clock ?? (() => DateTime.UtcNow);
    readonly object gate = new();
    DateTime checkedAt = DateTime.MinValue, stamp = DateTime.MinValue;
    bool allowed, read;

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
                allowed = ModSettings.PlayWithStrangers(modsFolder);
                read = true;
                Reads++;
                return allowed;
            }
        }
    }

    static DateTime Stamp(string modsFolder)
    {
        var path = Path.Combine(modsFolder, ModSettings.FileName);
        if (!File.Exists(path)) path = Path.Combine(modsFolder, ModSettings.LegacyFileName);
        return File.Exists(path) ? File.GetLastWriteTimeUtc(path) : DateTime.MinValue;
    }
}
