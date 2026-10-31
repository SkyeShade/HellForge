using System.Globalization;

namespace HellForge.Core;

// Linux process identity for the native supervisor and its tests: a pid names a host process and its start time
// distinguishes a reused pid. Nothing here opens the process or needs ptrace. The Wine runner's heartbeat is its liveness
// signal because Wine reports Windows pids, not these host pids.
public static class LinuxProcessIdentity
{
    public sealed record ProcessStat(int Pid, int ParentPid, char State, DateTime StartedUtc);

    public static ProcessStat? Read(int pid)
    {
        if (!OperatingSystem.IsLinux() || pid <= 0) return null;
        try
        {
            var stat = File.ReadAllText($"/proc/{pid}/stat");
            var close = stat.LastIndexOf(')');
            if (close < 0 || close + 2 >= stat.Length) return null;
            var fields = stat[(close + 2)..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length <= 19 || !int.TryParse(fields[1], out var parent) || fields[0].Length == 0) return null;
            if (!ulong.TryParse(fields[19], NumberStyles.None, CultureInfo.InvariantCulture, out var ticks)) return null;
            var btime = BootTime();
            if (btime == null) return null;
            var seconds = ticks / 100d;
            return new(pid, parent, fields[0][0], btime.Value.AddSeconds(seconds));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or FormatException) { return null; }
    }

    public static bool Alive(int pid) => Read(pid) is { State: not 'Z' };

    public static bool ParentAlive(int pid) => Read(pid) is { State: not 'Z' };

    public static bool Matches(int pid, DateTime startedUtc) =>
        Read(pid) is { State: not 'Z' } stat && Math.Abs((stat.StartedUtc - startedUtc.ToUniversalTime()).TotalSeconds) <= 5;

    static DateTime? BootTime()
    {
        try
        {
            foreach (var line in File.ReadLines("/proc/stat"))
            {
                if (!line.StartsWith("btime ", StringComparison.Ordinal)) continue;
                return long.TryParse(line[6..].Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var seconds)
                    ? DateTime.UnixEpoch.AddSeconds(seconds) : null;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return null;
    }
}
