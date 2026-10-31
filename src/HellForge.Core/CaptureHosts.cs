namespace HellForge.Core;

// The capture's line in the hosts file: a marker, then the API host mapped to loopback. Remove drops every line that holds
// the marker, so Add starts a new line first when the file does not end with one; otherwise the marker would join the
// player's last entry and cleanup would delete that entry with it; the marker says so, and cleanup leaves that last line
// without a newline again.
// A hosts file the player made read-only has the attribute cleared for each write and set again straight after, so it is
// read-only for the whole sync. The marker line records that (and a hosts file Add had to create), so a cleanup after a
// crash, even a crash between the write and the attribute, leaves the file as it was. A file another program holds open is
// retried for a few seconds; what still fails is a HostsFileException with a plain reason (HostsFailure) for the player.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class CaptureHosts
{
    public const string Marker = "# HellForge-CaptureProxy";
    // Words after the marker: what Remove restores.
    public const string ReadOnlyFlag = "was-read-only", CreatedFlag = "was-missing", NoNewlineFlag = "no-final-newline";
    // The capture's reason a hosts write failed, in its sync folder, for the launcher's message.
    public const string FailureFile = "hosts-failure.txt";

    // How long a hosts file another program holds open is retried.
    public static TimeSpan LockedRetry { get; set; } = TimeSpan.FromSeconds(6);

    public static void Add(string hostsPath, string host)
    {
        var existed = File.Exists(hostsPath);
        var readOnly = existed && File.GetAttributes(hostsPath).HasFlag(FileAttributes.ReadOnly);
        var midLine = existed && Retry(hostsPath, "read", () => EndsMidLine(hostsPath));
        var marker = Marker + (readOnly ? " " + ReadOnlyFlag : "") + (existed ? "" : " " + CreatedFlag) + (midLine ? " " + NoNewlineFlag : "");
        var text = marker + Environment.NewLine + Mapping(host) + Environment.NewLine;
        if (!existed) Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(hostsPath))!);
        Write(hostsPath, "add the capture's line", () => File.AppendAllText(hostsPath, midLine ? Environment.NewLine + text : text));
        // Security software that undoes the change at once leaves nothing to redirect the game.
        if (!Retry(hostsPath, "read", () => File.ReadAllLines(hostsPath)).Any(line => line.Trim().Equals(Mapping(host), StringComparison.OrdinalIgnoreCase)))
            throw new HostsFileException(HostsFailure.Reverted, hostsPath, null);
    }

    // True when it removed anything.
    public static bool Remove(string hostsPath, string host)
    {
        if (!File.Exists(hostsPath)) return false;
        var lines = Retry(hostsPath, "read", () => File.ReadAllLines(hostsPath));
        var markers = lines.Where(line => line.Contains(Marker, StringComparison.OrdinalIgnoreCase)).ToArray();
        var kept = lines.Where(line => !line.Contains(Marker, StringComparison.OrdinalIgnoreCase) &&
            !line.Trim().Equals(Mapping(host), StringComparison.OrdinalIgnoreCase)).ToArray();
        if (kept.Length == lines.Length) return false;
        if (markers.Any(line => HasFlag(line, CreatedFlag)) && kept.All(string.IsNullOrWhiteSpace))
        {
            Write(hostsPath, "remove the hosts file it created", () => File.Delete(hostsPath));
            return true;
        }
        // The player's lines, without a final newline when the file had none before Add (Add's own newline goes with
        // the capture's lines).
        var rest = string.Join(Environment.NewLine, kept);
        if (kept.Length > 0 && !markers.Any(line => HasFlag(line, NoNewlineFlag))) rest += Environment.NewLine;
        Write(hostsPath, "remove the capture's line", () => File.WriteAllText(hostsPath, rest));
        // A crash between the write and the attribute in Add left it cleared.
        if (markers.Any(line => HasFlag(line, ReadOnlyFlag)))
            File.SetAttributes(hostsPath, File.GetAttributes(hostsPath) | FileAttributes.ReadOnly);
        return true;
    }

    // The reason a capture in syncDir gave for a failed hosts write, or null.
    public static string? ReadFailure(string syncDir)
    {
        try
        {
            var text = File.ReadAllText(Path.Combine(syncDir, FailureFile)).Trim();
            return text.Length > 0 ? text : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static void WriteFailure(string syncDir, string message) =>
        File.WriteAllText(Path.Combine(syncDir, FailureFile), message + Environment.NewLine);

    static string Mapping(string host) => "127.0.0.1 " + host;

    static bool HasFlag(string markerLine, string flag) =>
        markerLine.Split(' ', StringSplitOptions.RemoveEmptyEntries).Contains(flag, StringComparer.OrdinalIgnoreCase);

    static bool EndsMidLine(string path)
    {
        using var file = File.OpenRead(path);
        if (file.Length == 0) return false;
        file.Seek(-1, SeekOrigin.End);
        return file.ReadByte() != '\n';
    }

    // One write with the read-only attribute cleared for it and set again after (other attributes kept).
    static void Write(string hostsPath, string what, Action write)
    {
        var attributes = File.Exists(hostsPath) ? File.GetAttributes(hostsPath) : (FileAttributes?)null;
        var readOnly = attributes is { } a && a.HasFlag(FileAttributes.ReadOnly);
        try
        {
            if (readOnly) Denied(hostsPath, () => File.SetAttributes(hostsPath, attributes!.Value & ~FileAttributes.ReadOnly), HostsFailure.ReadOnly);
            Retry(hostsPath, what, () => { write(); return true; });
        }
        finally
        {
            if (readOnly && File.Exists(hostsPath)) File.SetAttributes(hostsPath, attributes!.Value);
        }
    }

    static T Retry<T>(string hostsPath, string what, Func<T> action)
    {
        var deadline = DateTime.UtcNow + LockedRetry;
        while (true)
        {
            try
            {
                return action();
            }
            catch (IOException error) when (IsLocked(error))
            {
                if (DateTime.UtcNow >= deadline) throw new HostsFileException(HostsFailure.InUse, hostsPath, error);
                Thread.Sleep(250);
            }
            catch (UnauthorizedAccessException error)
            {
                throw new HostsFileException(HostsFailure.Blocked, hostsPath, error);
            }
        }
    }

    static void Denied(string hostsPath, Action action, HostsFailure failure)
    {
        try { action(); }
        catch (UnauthorizedAccessException error) { throw new HostsFileException(failure, hostsPath, error); }
    }

    // ERROR_SHARING_VIOLATION and ERROR_LOCK_VIOLATION.
    static bool IsLocked(IOException error) => (error.HResult & 0xFFFF) is 32 or 33;
}

public enum HostsFailure { ReadOnly, InUse, Blocked, Reverted }

// A hosts write that failed for a reason the player can be told; Message is that plain reason.
public sealed class HostsFileException(HostsFailure failure, string path, Exception? inner)
    : IOException(Describe(failure, path), inner)
{
    public HostsFailure Failure { get; } = failure;

    public static string Describe(HostsFailure failure, string path) => failure switch
    {
        HostsFailure.ReadOnly => $"Windows would not let the sync clear the read-only setting on the hosts file ({path}). " +
            "Security software that protects the hosts file usually does this.",
        HostsFailure.InUse => $"Another program kept the hosts file ({path}) open, so the sync could not change it. " +
            "This is usually antivirus scanning it or a hosts editor; close that program, then sync again.",
        HostsFailure.Blocked => $"Windows refused the change to the hosts file ({path}) even with administrator rights. " +
            "Security software that protects the hosts file usually does this: Microsoft Defender reports it as " +
            "SettingsModifier:Win32/HostsFileHijack, other antivirus calls it hosts file protection.",
        _ => $"Something undid the sync's change to the hosts file ({path}) as soon as it was made. " +
            "Security software that protects the hosts file usually does this: Microsoft Defender reports it as " +
            "SettingsModifier:Win32/HostsFileHijack, other antivirus calls it hosts file protection.",
    };
}
