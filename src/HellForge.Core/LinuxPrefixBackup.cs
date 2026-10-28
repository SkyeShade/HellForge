using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The compressor a prefix archive uses: zstd when the system has it, gzip otherwise. The file name says which, and both
// are ordinary tar streams that standard tools can read.
public enum LinuxPrefixCompression { Zstd, Gzip }

// One entry as GNU tar lists it: the first character of the mode, the size, the name, and for a link the target. The
// listing is read with --quoting-style=escape, so a name with a newline or a backslash stays on one line.
public sealed record LinuxArchiveEntry(char Kind, long Size, string Name, string? Target = null);

// Whether a listing may be restored: every entry is under one top-level folder, no path is absolute or contains "..",
// and there is no device or other special file.
public sealed record LinuxArchiveCheck(bool Safe, string Reason, string TopLevel, long UncompressedBytes);

public sealed record LinuxPrefixBackupResult(string Archive, long Bytes, LinuxPrefixCompression Compression);

// A checked archive already unpacked into StagingRoot; StagedFolder is the one top-level folder a helper moves over
// Prefix after the launcher exits, keeping OldFolder until the move is complete.
public sealed record LinuxPrefixRestorePlan(string Archive, string Prefix, string StagedFolder, string StagingRoot,
    string OldFolder, long UncompressedBytes, string TopLevel);

// Prefix backups on Linux: the old shell installer's hellforge-prefix-<date>.tar.zst or .tar.gz in ~/hellforge-backups,
// written with the system's GNU tar relative to the prefix's parent. Restores are listed and checked before anything is
// unpacked, then unpacked beside the prefix; the launcher cannot replace the prefix it runs from, so a helper does the
// swap after it exits. The decisions are pure so they can be tested without a prefix or an archive.
public static class LinuxPrefixBackup
{
    public const string DefaultFolderName = "hellforge-backups";
    public const string ArchivePrefix = "hellforge-prefix";

    static readonly Regex ListingLine = new(
        @"^(?<kind>[-dlhcbpsD])\S{9}\s+\S+\s+(?<size>[0-9]+)(?:,\s*[0-9]+)?\s+\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}\s(?<name>.*)$",
        RegexOptions.CultureInvariant);

    public static string DefaultFolder(string home) => Path.Combine(home, DefaultFolderName);

    public static LinuxPrefixCompression ChooseCompression(bool zstdInstalled) =>
        zstdInstalled ? LinuxPrefixCompression.Zstd : LinuxPrefixCompression.Gzip;

    public static string Extension(LinuxPrefixCompression compression) =>
        compression == LinuxPrefixCompression.Zstd ? ".tar.zst" : ".tar.gz";

    public static string ArchiveName(DateTime timestamp, LinuxPrefixCompression compression, int attempt = 0) =>
        $"{ArchivePrefix}-{timestamp.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}" +
        (attempt > 0 ? "-" + attempt.ToString(CultureInfo.InvariantCulture) : "") + Extension(compression);

    public static string ArchivePath(string folder, DateTime timestamp, LinuxPrefixCompression compression,
        int attempt = 0) =>
        Path.Combine(LocalInstall.FullPath(folder), ArchiveName(timestamp, compression, attempt));

    // A timestamp can repeat within one second; -2, -3 ... keeps the earlier archive instead of overwriting it.
    public static string NextArchive(string folder, DateTime timestamp, LinuxPrefixCompression compression)
    {
        var archive = ArchivePath(folder, timestamp, compression);
        for (var attempt = 2; File.Exists(archive); attempt++) archive = ArchivePath(folder, timestamp, compression, attempt);
        return archive;
    }

    public static bool TryCompression(string archive, out LinuxPrefixCompression compression)
    {
        if (archive.EndsWith(".tar.zst", StringComparison.OrdinalIgnoreCase))
        {
            compression = LinuxPrefixCompression.Zstd;
            return true;
        }
        if (archive.EndsWith(".tar.gz", StringComparison.OrdinalIgnoreCase))
        {
            compression = LinuxPrefixCompression.Gzip;
            return true;
        }
        compression = default;
        return false;
    }

    public static string PrefixName(string prefix) => Path.GetFileName(LocalInstall.FullPath(prefix));

    // Staging and the old prefix are siblings of the prefix, so moving either is a rename on the same file system.
    public static string StagingFolder(string prefix, string token) =>
        Path.Combine(Path.GetDirectoryName(LocalInstall.FullPath(prefix))!,
            "." + PrefixName(prefix) + ".hellforge-restore-" + token);

    public static string OldFolder(string prefix, string token) =>
        Path.Combine(Path.GetDirectoryName(LocalInstall.FullPath(prefix))!,
            "." + PrefixName(prefix) + ".hellforge-old-" + token);

    public static long BackupRequiredBytes(long prefixBytes) => Math.Max(prefixBytes, 0) + DiskSpace.Margin;
    public static long RestoreRequiredBytes(long uncompressedBytes) => Math.Max(uncompressedBytes, 0) + DiskSpace.Margin;

    public static string? BackupSpaceProblem(long? free, long prefixBytes) =>
        SpaceProblem("backup", free, BackupRequiredBytes(prefixBytes));

    public static string? RestoreSpaceProblem(long? free, long uncompressedBytes) =>
        SpaceProblem("restore", free, RestoreRequiredBytes(uncompressedBytes));

    static string? SpaceProblem(string action, long? free, long needed)
    {
        if (free is not { } available || available >= needed) return null;
        return $"Not enough free space for the {action}: {DiskSpace.GB(needed - available)} more is needed " +
            $"({DiskSpace.GB(needed)} to write, {DiskSpace.GB(available)} free).";
    }

    public static long? FreeBytes(string folder) => DiskSpace.ForFolder(folder).FreeBytes;

    public static bool ZstdInstalled() => OnPath("zstd");

    static bool OnPath(string program) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
            .Any(folder => File.Exists(Path.Combine(folder, program)));

    // Sums the prefix's regular files without following links: Wine's dosdevices entries are links, and tar stores them
    // as links too. Directory entries and their blocks are left out; DiskSpace.Margin covers the difference.
    public static long EstimateBytes(string prefix)
    {
        var total = 0L;
        var pending = new Stack<string>();
        pending.Push(LocalInstall.FullPath(prefix));
        while (pending.Count > 0)
        {
            foreach (var entry in new DirectoryInfo(pending.Pop()).EnumerateFileSystemInfos())
            {
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) continue;
                if ((entry.Attributes & FileAttributes.Directory) != 0) pending.Push(entry.FullName);
                else total += ((FileInfo)entry).Length;
            }
        }
        return total;
    }

    // The prefix itself must exist, be a plain folder with no links on the path to it, not be a place that is never
    // touched (the same rule uninstall uses), and nothing may be using it.
    public static void CheckBackupSource(string prefix, LinuxUninstallLocations locations, string? steamRoot,
        string installRoot)
    {
        prefix = LocalInstall.FullPath(prefix);
        ForbidLocation(prefix, locations, steamRoot);
        if (!Directory.Exists(prefix)) throw new InvalidDataException("The Proton prefix does not exist: " + prefix);
        LocalInstall.NoLinks(prefix);
        RequireIdle(prefix, installRoot);
    }

    // The archive goes outside the prefix: one written inside it would include itself while tar reads the folder.
    public static void CheckBackupFolder(string prefix, string folder)
    {
        prefix = LocalInstall.FullPath(prefix);
        folder = LocalInstall.FullPath(folder);
        if (LocalInstall.Within(folder, prefix))
            throw new InvalidDataException("Choose a backup folder outside the Proton prefix: an archive inside it would include itself.");
        if (LinuxUninstall.Exists(folder) && !Directory.Exists(folder))
            throw new InvalidDataException("The backup path is not a folder: " + folder);
        Directory.CreateDirectory(folder);
        LocalInstall.NoLinks(folder);
    }

    public static void CheckRestoreTarget(string prefix, LinuxUninstallLocations locations, string? steamRoot,
        string installRoot)
    {
        prefix = LocalInstall.FullPath(prefix);
        ForbidLocation(prefix, locations, steamRoot);
        if (PrefixName(prefix).Length == 0) throw new InvalidDataException("Choose a folder for the Proton prefix.");
        if (LinuxUninstall.Exists(prefix) && !Directory.Exists(prefix))
            throw new InvalidDataException("The Proton prefix path is not a folder: " + prefix);
        LocalInstall.NoLinks(prefix);
        RequireIdle(prefix, installRoot);
    }

    static void ForbidLocation(string prefix, LinuxUninstallLocations locations, string? steamRoot)
    {
        if (LinuxUninstall.PrefixForbidden(prefix, locations.Home, steamRoot))
            throw new InvalidDataException("This folder is not used for backup or restore: it is the home folder, the " +
                "filesystem root, Steam's folder or under Steam's compatibility data.");
    }

    // RunningInPrefix also sees the launcher, but ignores this process and its parents; another launcher of this
    // installation, a session, or any process using the prefix refuses both operations.
    public static void RequireIdle(string prefix, string installRoot)
    {
        var names = LinuxInstallProcesses.RunningInPrefix(prefix)
            .Concat(LinuxInstallProcesses.Running(installRoot))
            .Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        if (names.Length > 0)
            throw new InvalidDataException("Close HellForge and anything else using the prefix first: " +
                string.Join(", ", names) + " is still running.");
    }

    // The listing decides the top-level folder and the unpacked size before anything is written. Device files are
    // refused; hard links and other special entries are refused too because they can point outside the top-level folder.
    // Links are checked after the whole listing is read: an entry below a link from the same archive could otherwise be
    // written through it (Wine's z: link points at /), whichever order the two entries appear in.
    public static LinuxArchiveCheck CheckEntries(IEnumerable<LinuxArchiveEntry> entries)
    {
        var list = entries.ToList();
        if (list.Count == 0) return Refuse("the archive is empty");
        string? top = null;
        var bytes = 0L;
        foreach (var entry in list)
        {
            var problem = CheckEntry(entry, ref top, ref bytes);
            if (problem != null) return Refuse(problem);
        }
        foreach (var entry in list)
            if (entry.Kind == 'l' && CheckLink(entry, top!) is { } linkProblem) return Refuse(linkProblem);
        var links = list.Where(entry => entry.Kind == 'l').ToList();
        foreach (var entry in list)
            foreach (var link in links)
                if (!ReferenceEquals(entry, link) && SameOrUnder(entry.Name, link.Name))
                    return Refuse("an entry is under a link in the same archive: " + entry.Name);
        return new(true, "", top!, bytes);
    }

    static string? CheckEntry(LinuxArchiveEntry entry, ref string? top, ref long bytes)
    {
        var name = entry.Name;
        if (name.Length == 0) return "an entry has an empty name";
        if (name.IndexOf('\0') >= 0) return "an entry name contains a null character";
        if (name[0] == '/') return "an entry is an absolute path: " + name;
        var parts = name.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Any(part => part == "..")) return "an entry contains '..': " + name;
        if (parts.Length == 0) return "an entry has no path: " + name;
        if (parts[0] == ".") return "an entry is not inside one top-level folder: " + name;
        top ??= parts[0];
        if (!parts[0].Equals(top, StringComparison.Ordinal))
            return "entries are not all inside one top-level folder: " + name;
        if (entry.Kind is 'c' or 'b') return "an entry is a device file: " + name;
        if (entry.Kind is not ('-' or 'd' or 'l')) return "an entry is a special file: " + name;
        if (parts.Length == 1 && entry.Kind != 'd') return "the top-level entry is not a folder: " + name;
        if (entry.Kind != '-') return null;
        if (entry.Size < 0) return "an entry has a negative size: " + name;
        if (entry.Size > long.MaxValue - bytes) return "the archive's size overflows a 64-bit count";
        bytes += entry.Size;
        return null;
    }

    // A link may stay inside the prefix, or be one of the standard Wine dosdevices links: z: to /, and com/lpt ports
    // to /dev. Nothing else may point outside the top-level folder.
    static string? CheckLink(LinuxArchiveEntry entry, string top)
    {
        if (entry.Target is not { Length: > 0 } target) return "a link entry has no target: " + entry.Name;
        if (target.IndexOf('\0') >= 0) return "a link target contains a null character: " + entry.Name;
        if (target[0] == '/')
            return WineAbsoluteTargetAllowed(entry.Name, top, target) ? null
                : "a link points outside the top-level folder: " + entry.Name + " -> " + target;
        return LinkStaysInside(entry.Name, target, top) ? null
            : "a link escapes the top-level folder: " + entry.Name + " -> " + target;
    }

    static bool LinkStaysInside(string link, string target, string top)
    {
        var parts = NormalizedParts(link);
        var stack = parts.Take(parts.Count - 1).ToList();
        foreach (var part in target.Split('/'))
        {
            if (part.Length == 0 || part == ".") continue;
            if (part == "..")
            {
                if (stack.Count == 0) return false;
                stack.RemoveAt(stack.Count - 1);
                continue;
            }
            stack.Add(part);
        }
        return stack.Count > 0 && stack[0].Equals(top, StringComparison.Ordinal);
    }

    static bool WineAbsoluteTargetAllowed(string name, string top, string target)
    {
        var parts = NormalizedParts(name);
        if (parts.Count != 4 || !parts[0].Equals(top, StringComparison.Ordinal) ||
            !parts[1].Equals("pfx", StringComparison.Ordinal) ||
            !parts[2].Equals("dosdevices", StringComparison.Ordinal)) return false;
        var device = parts[3];
        if (device.Equals("z:", StringComparison.OrdinalIgnoreCase)) return target == "/";
        if (!NumberedDevice(device, "com") && !NumberedDevice(device, "lpt")) return false;
        return target.StartsWith("/dev/", StringComparison.Ordinal) &&
            !target.Split('/').Any(part => part == "..");
    }

    static bool NumberedDevice(string name, string prefix) =>
        name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && name.Length > prefix.Length &&
        name[prefix.Length] is >= '1' and <= '9' && name[(prefix.Length + 1)..].All(char.IsAsciiDigit);

    static bool SameOrUnder(string name, string link)
    {
        var path = string.Join('/', NormalizedParts(name));
        var root = string.Join('/', NormalizedParts(link));
        return path.Equals(root, StringComparison.Ordinal) || path.StartsWith(root + "/", StringComparison.Ordinal);
    }

    static IReadOnlyList<string> NormalizedParts(string name) =>
        name.Split('/', StringSplitOptions.RemoveEmptyEntries).Where(part => part != ".").ToArray();

    static LinuxArchiveCheck Refuse(string reason) => new(false, reason, "", 0);

    // Parses the verbose GNU tar listing made with --quoting-style=escape. The name is the rest of the line, so spaces
    // and escaped newlines do not split it; a link's " -> " and a hard link's " link to " annotation become Target.
    public static IReadOnlyList<LinuxArchiveEntry> ParseListing(IEnumerable<string> lines)
    {
        var entries = new List<LinuxArchiveEntry>();
        foreach (var raw in lines)
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            var match = ListingLine.Match(line);
            if (!match.Success) throw new InvalidDataException("The archive listing could not be read: " + line);
            if (!long.TryParse(match.Groups["size"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var size))
                throw new InvalidDataException("The archive listing has an invalid size: " + line);
            var kind = match.Groups["kind"].Value[0];
            var name = match.Groups["name"].Value;
            var target = SplitTarget(kind, ref name, line);
            entries.Add(new(kind, size, name, target));
        }
        return entries;
    }

    static string? SplitTarget(char kind, ref string name, string line)
    {
        var marker = kind switch
        {
            'l' => " -> ",
            'h' => " link to ",
            _ => null
        };
        if (marker == null) return null;
        var at = name.IndexOf(marker, StringComparison.Ordinal);
        if (at < 0) throw new InvalidDataException("A link entry has no target: " + line);
        var target = name[(at + marker.Length)..];
        name = name[..at];
        if (target.Length == 0 || target.Contains(marker, StringComparison.Ordinal))
            throw new InvalidDataException("The archive listing could not be read: " + line);
        return target;
    }

    public static LinuxArchiveCheck ListArchive(string archive, LinuxPrefixCompression compression)
    {
        RequireTool(compression);
        var script = compression == LinuxPrefixCompression.Zstd
            ? "set -o pipefail\nzstd -dc -- \"$1\" | tar --quoting-style=escape --numeric-owner -tvf -"
            : "set -o pipefail\ngzip -dc -- \"$1\" | tar --quoting-style=escape --numeric-owner -tvf -";
        var (code, output, error) = RunBash(script, archive);
        if (code != 0) throw new InvalidDataException("The backup could not be read: " + Describe(error, code));
        return CheckEntries(ParseListing(output.Split('\n')));
    }

    // GNU tar archives the prefix by name relative to its parent, so the archive has exactly one top-level folder and
    // no absolute path. Links are stored as links (tar's default), never followed.
    public static LinuxPrefixBackupResult Backup(string prefix, string archive, LinuxPrefixCompression compression,
        Action<long>? progress, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        prefix = LocalInstall.FullPath(prefix);
        archive = LocalInstall.FullPath(archive);
        var folder = Path.GetDirectoryName(archive) ?? throw new InvalidDataException("The archive needs a folder: " + archive);
        Directory.CreateDirectory(folder);
        var part = archive + ".part";
        Delete(part);
        var script = compression == LinuxPrefixCompression.Zstd
            ? "set -o pipefail\ntar -C \"$1\" -cf - -- \"$2\" | zstd -T0 -q -o \"$3\""
            : "set -o pipefail\ntar -C \"$1\" -cf - -- \"$2\" | gzip -c > \"$3\"";
        using var process = Start(script, Path.GetDirectoryName(prefix)!, PrefixName(prefix), part);
        var error = process.StandardError.ReadToEndAsync();
        try
        {
            while (!process.WaitForExit(250))
            {
                cancellationToken.ThrowIfCancellationRequested();
                progress?.Invoke(Length(part));
            }
            var message = error.GetAwaiter().GetResult();
            cancellationToken.ThrowIfCancellationRequested();
            if (process.ExitCode != 0) throw new InvalidDataException("The backup failed: " + Describe(message, process.ExitCode));
            File.Move(part, archive);
            return new(archive, new FileInfo(archive).Length, compression);
        }
        catch
        {
            Stop(process);
            Delete(part);
            throw;
        }
    }

    // Lists and checks the archive, reserves the unpacked size (the old prefix stays until the swap), then unpacks it
    // into a staging folder beside the prefix. Nothing under the prefix is touched here.
    public static LinuxPrefixRestorePlan PrepareRestore(string archive, string prefix, string token,
        LinuxUninstallLocations locations, string? steamRoot, string installRoot)
    {
        archive = LocalInstall.FullPath(archive);
        prefix = LocalInstall.FullPath(prefix);
        if (!File.Exists(archive)) throw new InvalidDataException("The backup was not found: " + archive);
        if (!TryCompression(archive, out var compression))
            throw new InvalidDataException("Not a prefix backup (.tar.zst or .tar.gz): " + archive);
        CheckRestoreTarget(prefix, locations, steamRoot, installRoot);
        var check = ListArchive(archive, compression);
        if (!check.Safe) throw new InvalidDataException("The backup was refused: " + check.Reason + ".");
        var parent = Path.GetDirectoryName(prefix) ?? throw new InvalidDataException("The prefix needs a parent folder: " + prefix);
        if (RestoreSpaceProblem(FreeBytes(parent), check.UncompressedBytes) is { } space)
            throw new NotEnoughSpaceException(space);
        var staging = StagingFolder(prefix, token);
        var old = OldFolder(prefix, token);
        if (LinuxUninstall.Exists(staging)) throw new InvalidDataException("The staging folder already exists: " + staging);
        if (LinuxUninstall.Exists(old)) throw new InvalidDataException("The old-prefix folder already exists: " + old);
        Directory.CreateDirectory(staging);
        try
        {
            LocalInstall.NoLinks(staging);
            Extract(archive, compression, staging);
            var staged = Path.Combine(staging, check.TopLevel);
            if (!Directory.Exists(staged))
                throw new InvalidDataException("The backup has no top-level folder after unpacking: " + check.TopLevel);
            LocalInstall.NoLinks(staged);
            RequireIdle(prefix, installRoot);
            return new(archive, prefix, staged, staging, old, check.UncompressedBytes, check.TopLevel);
        }
        catch
        {
            DiscardRestore(staging);
            throw;
        }
    }

    public static void Extract(string archive, LinuxPrefixCompression compression, string staging)
    {
        RequireTool(compression);
        var script = compression == LinuxPrefixCompression.Zstd
            ? "set -o pipefail\nzstd -dc -- \"$1\" | tar --no-same-owner -xf - -C \"$2\""
            : "set -o pipefail\ngzip -dc -- \"$1\" | tar --no-same-owner -xf - -C \"$2\"";
        var (code, _, error) = RunBash(script, archive, staging);
        if (code != 0) throw new InvalidDataException("The backup could not be unpacked: " + Describe(error, code));
    }

    // Removes a staging folder after a refused or failed restore. It walks the tree itself and deletes a link instead of
    // descending into it, because a partly unpacked Wine prefix can contain z: -> /.
    public static void DiscardRestore(string staging)
    {
        try { DeleteTree(staging); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static void DeleteTree(string path)
    {
        if (LinuxUninstall.IsLink(path)) { File.Delete(path); return; }
        if (!Directory.Exists(path)) return;
        foreach (var entry in new DirectoryInfo(path).EnumerateFileSystemInfos())
        {
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) { File.Delete(entry.FullName); continue; }
            if ((entry.Attributes & FileAttributes.Directory) != 0) DeleteTree(entry.FullName);
            else entry.Delete();
        }
        Directory.Delete(path);
    }

    // Run by bash after the launcher exits: wait for anything still using the prefix to finish, move the old prefix
    // aside, move the staged prefix in, then remove the old one. If the second move fails, the old prefix is put back.
    // $6 is the launcher PID, or 0 when no wait is needed; $7 is the launcher to start in the restored prefix, or empty.
    public const string RestoreHelperTemplate = """
        #!/usr/bin/env bash
        set -u
        prefix=$1
        staged=$2
        staging=$3
        old=$4
        log=$5
        launcher_pid=$6
        relaunch=$7
        say() { printf '%s\n' "$*" >>"$log" 2>/dev/null || true; }
        fail() { say "failed: $*"; exit 1; }
        @PREFIX_IN_USE@
        if [ "$launcher_pid" != 0 ]; then
          while kill -0 "$launcher_pid" 2>/dev/null; do sleep 1; done
        fi
        [ -d "$staged" ] || fail "$staged (the staged prefix is missing)"
        [ ! -L "$staged" ] || fail "$staged (link; refused)"
        [ ! -e "$old" ] && [ ! -L "$old" ] || fail "$old (already exists)"
        deadline=$((SECONDS + 30))
        while prefix_in_use; do
          if [ "$SECONDS" -ge "$deadline" ]; then
            say "failed: $prefix (still in use by $pfx_in_use_names)"
            rm -rf --one-file-system -- "$staging" 2>/dev/null || true
            if [ -n "$relaunch" ] && [ -x "$relaunch" ]; then
              ( cd "$prefix" && nohup "$relaunch" >/dev/null 2>&1 </dev/null & )
            fi
            rm -f -- "$0" 2>/dev/null || true
            exit 1
          fi
          sleep 1
        done
        moved=0
        if [ -e "$prefix" ] || [ -L "$prefix" ]; then
          [ -d "$prefix" ] || fail "$prefix (not a folder)"
          [ ! -L "$prefix" ] || fail "$prefix (link; refused)"
          mv -- "$prefix" "$old" || fail "could not move $prefix aside"
          moved=1
        fi
        if mv -- "$staged" "$prefix"; then
          if [ "$moved" = 1 ]; then
            rm -rf --one-file-system -- "$old" 2>/dev/null || true
            if [ -e "$old" ] || [ -L "$old" ]; then say "left: $old (could not be removed)"
            else say "removed: $old"; fi
          fi
          rm -rf --one-file-system -- "$staging" 2>/dev/null || true
          say "Restore finished: $prefix"
          if [ -n "$relaunch" ] && [ -x "$relaunch" ]; then
            ( cd "$prefix" && nohup "$relaunch" >/dev/null 2>&1 </dev/null & )
          fi
          rm -f -- "$0" 2>/dev/null || true
          exit 0
        fi
        say "failed: could not move the staged prefix into place"
        if [ "$moved" = 1 ]; then
          if mv -- "$old" "$prefix"; then say "put back: $prefix"
          else say "failed: the old prefix could not be put back: $old"; fi
        fi
        exit 1
        """;

    public static string RestoreHelperScript => LinuxHelperScripts.IncludePrefixInUse(RestoreHelperTemplate);

    static void RequireTool(LinuxPrefixCompression compression)
    {
        var program = compression == LinuxPrefixCompression.Zstd ? "zstd" : "gzip";
        if (!OnPath(program))
            throw new InvalidDataException($"'{program}' is needed to read a {Extension(compression)} backup but was not found.");
    }

    static (int Code, string Output, string Error) RunBash(string script, params string[] arguments)
    {
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetTempPath()
        };
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add(script);
        start.ArgumentList.Add("bash");
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        start.Environment["LC_ALL"] = "C";
        Process process;
        try { process = Process.Start(start) ?? throw new IOException("bash did not start."); }
        catch (Win32Exception error) { throw new InvalidDataException("The system could not run bash: " + error.Message, error); }
        using (process)
        {
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            return (process.ExitCode, output.GetAwaiter().GetResult(), error.GetAwaiter().GetResult());
        }
    }

    static Process Start(string script, params string[] arguments)
    {
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetTempPath()
        };
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add(script);
        start.ArgumentList.Add("bash");
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        start.Environment["LC_ALL"] = "C";
        try { return Process.Start(start) ?? throw new IOException("bash did not start."); }
        catch (Win32Exception error) { throw new InvalidDataException("The system could not run bash: " + error.Message, error); }
    }

    static long Length(string path)
    {
        try { return new FileInfo(path).Length; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return 0; }
    }

    static void Stop(Process process)
    {
        try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception or NotSupportedException) { }
        try { process.WaitForExit(5000); }
        catch (InvalidOperationException) { }
    }

    static void Delete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static string Describe(string error, int code) =>
        error.Trim() is { Length: > 0 } text ? text.Split('\n')[0].Trim() : "tar or its decompressor exited with code " + code.ToString(CultureInfo.InvariantCulture) + ".";
}
