using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace HellForge.Core;

public enum InstallerKind { WindowsSetup, LinuxPackage }

// A kept installer: the file, the version its name gives, its size, and the SHA-256 its .sha256 file records (null when
// that file is missing or cannot be read).
public sealed record KeptInstallerInfo(InstallerKind Kind, string Path, string Version, long Bytes, string? Sha256)
{
    public string KindName => Kind == InstallerKind.WindowsSetup ? "windows-setup" : "linux-package";
}

// What an installation has: its kept installer, or none; and why the last setup could not keep one, if it said.
public sealed record KeptInstallerState(KeptInstallerInfo? Kept, string? NotKept);

// A copy written for a friend: the file or installer folder, the checked file in it, its SHA-256 and size.
public sealed record KeptInstallerCopy(string Path, string File, string Sha256, long Bytes);

// Where a copy is: checking the kept file, writing the copy, or reading the copy back.
public sealed record KeptInstallerProgress(string Stage, long Done, long Total);

// The installer an installation keeps of itself in <installation>/installer, so HellForge can be handed to a friend or
// installed on a PC with no internet (docs/offline-kit.md). Windows keeps HellForge-Setup-<version>.exe, the Linux
// installer its package as HellForge-Linux-<version>.zip. A <file>.sha256 beside it (sha256sum's format) records its
// hash when kept; a copy is checked against it, and read back once written. Every install and update replaces it.
public static partial class KeptInstaller
{
    public const string FolderName = "installer";
    public const string HashSuffix = ".sha256";
    public const string NotKeptFile = "not-kept.txt";
    // The Linux copy for a friend: the installer's launcher files beside the package, as the release's installer has them.
    public const string LinuxInstallerFolder = "HellForge-Linux-Installer-";
    const string SetupPrefix = "HellForge-Setup-", SetupSuffix = ".exe";
    const string LinuxPrefix = "HellForge-Linux-", LinuxSuffix = ".zip";
    const string LauncherEntries = "app/launcher-linux/";
    // Free space left on a friend's drive beyond the copy; a USB stick needs no more.
    const long CopyMargin = 16L << 20;

    public static string Folder(InstallLayout install) => Path.Combine(install.Root, FolderName);

    // "0.5.0-beta (abc1234)" is kept as 0.5.0-beta: the commit is not part of the name, and only [0-9A-Za-z.-] are, so a
    // version can never make a path.
    public static string VersionForName(string appVersion)
    {
        var plain = appVersion.Split(' ', 2)[0];
        var safe = new string(plain.Where(c => c is (>= '0' and <= '9') or (>= 'A' and <= 'Z') or (>= 'a' and <= 'z') or '.' or '-').ToArray()).Trim('.');
        return safe.Length == 0 ? "unknown" : safe;
    }

    public static string FileName(InstallerKind kind, string version) => kind == InstallerKind.WindowsSetup
        ? SetupPrefix + VersionForName(version) + SetupSuffix : LinuxPrefix + VersionForName(version) + LinuxSuffix;

    // A setup program holds the app it installs, so the real HellForge-Setup.exe is larger than its payload. A program
    // that is not (an apphost beside its DLL, dotnet running a build) is not the file a player downloaded.
    public static bool IsWholeSetup(string? program, long payloadBytes) =>
        program != null && File.Exists(program) && new FileInfo(program).Length > payloadBytes && payloadBytes > 0 &&
        program.EndsWith(SetupSuffix, StringComparison.OrdinalIgnoreCase);

    [GeneratedRegex(@"^HellForge-(Setup|Linux)-([0-9A-Za-z.\-]+)\.(exe|zip)$", RegexOptions.CultureInvariant)]
    private static partial Regex KeptName();

    [GeneratedRegex(@"^([0-9A-Fa-f]{64}) [ *](.+)$", RegexOptions.CultureInvariant)]
    private static partial Regex HashLine();

    public static KeptInstallerState Find(InstallLayout install)
    {
        var folder = Folder(install);
        string? notKept = null;
        try { notKept = File.Exists(Path.Combine(folder, NotKeptFile)) ? File.ReadAllText(Path.Combine(folder, NotKeptFile)).Trim() : null; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var kept = Kept(folder).OrderByDescending(file => file.LastWriteTimeUtc).Select(Info).FirstOrDefault();
        return new(kept, string.IsNullOrEmpty(notKept) ? null : notKept);
    }

    // The installer files in the folder, links skipped: a name HellForge would write, and nothing else.
    static List<FileInfo> Kept(string folder)
    {
        if (!Directory.Exists(folder)) return [];
        return new DirectoryInfo(folder).EnumerateFiles("HellForge-*", new EnumerationOptions { AttributesToSkip = 0 })
            .Where(file => file.LinkTarget == null && KeptName().Match(file.Name) is { Success: true } match &&
                (match.Groups[1].Value == "Setup") == (match.Groups[3].Value == "exe"))
            .ToList();
    }

    static KeptInstallerInfo Info(FileInfo file)
    {
        var match = KeptName().Match(file.Name);
        var kind = match.Groups[1].Value == "Setup" ? InstallerKind.WindowsSetup : InstallerKind.LinuxPackage;
        return new(kind, file.FullName, match.Groups[2].Value, file.Length, RecordedHash(file.FullName));
    }

    // The hash in <file>.sha256, for this file's name; null when there is none that reads right.
    static string? RecordedHash(string file)
    {
        try
        {
            var line = File.ReadLines(file + HashSuffix).FirstOrDefault()?.Trim();
            var match = line == null ? null : HashLine().Match(line);
            return match is { Success: true } && match.Groups[2].Value == Path.GetFileName(file) ? match.Groups[1].Value.ToUpperInvariant() : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static void WriteHash(string file, string sha256)
    {
        var path = file + HashSuffix;
        File.WriteAllText(path + ".partial", sha256.ToLowerInvariant() + " *" + Path.GetFileName(file) + "\n");
        File.Move(path + ".partial", path, overwrite: true);
    }

    // Setup's own record that it kept no copy this time, which the launcher shows; the next install or update tries again.
    public static void RecordNotKept(InstallLayout install, string why)
    {
        try
        {
            Directory.CreateDirectory(Folder(install));
            File.WriteAllText(Path.Combine(Folder(install), NotKeptFile), Privacy.Scrub(why) + "\n");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    // Keeps source (Setup's own file or the Linux package archive) as this version's installer and removes every older one.
    // A source that is already the kept file is only hashed again. space: the tests' stand-in for the drive's free space.
    public static KeptInstallerInfo Keep(InstallLayout install, string source, InstallerKind kind, string appVersion,
        Func<string, DiskSpaceInfo>? space = null)
    {
        var file = new FileInfo(LocalInstall.FullPath(source));
        if (!file.Exists) throw new FileNotFoundException("The installer to keep was not found: " + Privacy.Scrub(file.FullName));
        if (file.LinkTarget != null) throw new InvalidDataException("The installer to keep is a link: " + Privacy.Scrub(file.FullName));
        return Store(install, kind, appVersion, file.Length, space, partial =>
        {
            using var input = new HashingStream(new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1 << 20), HashAlgorithmName.SHA256);
            using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                input.CopyTo(output, 1 << 20);
                output.Flush(flushToDisk: true);
            }
            return input.Hex();
        }, samePath: file.FullName);
    }

    // The Linux installer's package when it came as the HellForge-Linux folder (Nexus Mods' zip): zipped as it is, which
    // the installer takes as it takes the archive.
    public static KeptInstallerInfo KeepFolder(InstallLayout install, string folder, string appVersion, Func<string, DiskSpaceInfo>? space = null)
    {
        var full = LocalInstall.FullPath(folder);
        if (!Directory.Exists(full)) throw new DirectoryNotFoundException("The package folder to keep was not found: " + Privacy.Scrub(full));
        var bytes = Directory.EnumerateFiles(full, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 })
            .Sum(path => new FileInfo(path).Length);
        return Store(install, InstallerKind.LinuxPackage, appVersion, bytes, space, partial =>
        {
            ZipFile.CreateFromDirectory(full, partial, CompressionLevel.Fastest, includeBaseDirectory: false);
            return null;
        }, samePath: null);
    }

    // The Linux installer's step once the app is installed: its package (the archive or the folder) is kept. A failure is
    // recorded for the launcher and returned, never thrown, since the install itself has succeeded.
    public static string? KeepLinuxPackage(InstallLayout install, string package, string appVersion)
    {
        try
        {
            if (Directory.Exists(package)) KeepFolder(install, package, appVersion);
            else Keep(install, package, InstallerKind.LinuxPackage, appVersion);
            return null;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            RecordNotKept(install, error.Message);
            return error.Message;
        }
    }

    // write fills the .partial file and returns the hash of what it read, or null; the .partial is read back either way.
    static KeptInstallerInfo Store(InstallLayout install, InstallerKind kind, string appVersion, long bytes, Func<string, DiskSpaceInfo>? space,
        Func<string, string?> write, string? samePath)
    {
        var folder = Folder(install);
        Directory.CreateDirectory(folder);
        folder = LocalInstall.NoLinks(folder, install.Root);
        var target = Path.Combine(folder, FileName(kind, appVersion));
        var others = Kept(folder).Where(old => !old.FullName.Equals(target, StringComparison.OrdinalIgnoreCase)).ToList();
        if (samePath != null && LocalInstall.FullPath(samePath).Equals(target, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            return Finish(install, target, Sha256Of(target, null, default), others);
        var freeable = others.Sum(old => old.Length) + (File.Exists(target) ? new FileInfo(target).Length : 0);
        if (NeedsRoomFirst(space ?? DiskSpace.ForFolder, folder, bytes, freeable))
        {
            foreach (var old in others) Remove(old.FullName);
            Remove(target);
            others.Clear();
        }
        var partial = target + ".partial";
        try
        {
            var read = write(partial);
            var written = Sha256Of(partial, null, default);
            if (read != null && read != written)
                throw new InvalidDataException("The kept copy of the installer read back differently from the installer; it was not kept.");
            File.Move(partial, target, overwrite: true);
            return Finish(install, target, written, others);
        }
        finally { if (File.Exists(partial)) File.Delete(partial); }
    }

    // Whether the older installers must go before the new one fits; throws when it does not fit even then.
    static bool NeedsRoomFirst(Func<string, DiskSpaceInfo> space, string folder, long bytes, long freeable)
    {
        var drive = space(folder);
        if (drive.FreeBytes is not { } free) return false;
        var needed = bytes + DiskSpace.Margin;
        if (free >= needed) return false;
        if (free + freeable < needed)
            throw new NotEnoughSpaceException($"Not enough free space on {drive.Name} to keep a copy of the installer: {MB(needed)} is needed, {MB(free)} is free.");
        return true;
    }

    static KeptInstallerInfo Finish(InstallLayout install, string target, string sha256, List<FileInfo> others)
    {
        WriteHash(target, sha256);
        // an older copy that is running (Setup started from it) stays until the next update; the new one is kept
        foreach (var old in others)
            try { Remove(old.FullName); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        Remove(Path.Combine(Folder(install), NotKeptFile));
        return Info(new FileInfo(target));
    }

    static void Remove(string file)
    {
        if (File.Exists(file)) File.Delete(file);
        if (File.Exists(file + HashSuffix) && !file.EndsWith(HashSuffix, StringComparison.Ordinal)) File.Delete(file + HashSuffix);
    }

    static string Sha256Of(string file, Action<long>? passed, CancellationToken cancel)
    {
        using var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1 << 20);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[1 << 20];
        long done = 0;
        for (int read; (read = input.Read(buffer, 0, buffer.Length)) > 0;)
        {
            cancel.ThrowIfCancellationRequested();
            hash.AppendData(buffer, 0, read);
            passed?.Invoke(done += read);
        }
        return Convert.ToHexString(hash.GetCurrentHash());
    }

    static string MB(long bytes) => $"{bytes / 1e6:N0} MB";

    public const string NoHash = "The kept installer has no SHA-256 file beside it, so a damaged copy could not be told from a good one. " +
        "Run HellForge-Setup again to keep a fresh copy with its hash.";

    // Copies the kept installer into destination, a folder the player chose, as the offline kit's first half. The kept
    // file is checked against its recorded hash before anything is written, the copy as it is written, and the copy is
    // read back from the destination; a .sha256 goes beside it. Windows: the setup program. Linux: a
    // HellForge-Linux-Installer-<version> folder with the launcher files and HellForge-Linux.zip, as the release has it.
    public static KeptInstallerCopy CopyTo(KeptInstallerInfo kept, string destination, Action<KeptInstallerProgress>? progress = null,
        CancellationToken cancel = default, Func<string, DiskSpaceInfo>? space = null)
    {
        if (kept.Sha256 == null) throw new InvalidDataException(NoHash);
        var folder = LocalInstall.FullPath(destination);
        if (!Directory.Exists(folder)) throw new DirectoryNotFoundException("Choose a folder that exists: " + Privacy.Scrub(folder));
        if (LocalInstall.Within(folder, Path.GetDirectoryName(kept.Path)!))
            throw new InvalidDataException("Choose a folder outside the installation's installer folder.");
        var check = Sha256Of(kept.Path, done => progress?.Invoke(new("Checking the kept installer", done, kept.Bytes)), cancel);
        if (check != kept.Sha256) throw Damaged(kept, check);
        var linux = kept.Kind == InstallerKind.LinuxPackage;
        var target = linux ? Path.Combine(folder, LinuxInstallerFolder + kept.Version) : folder;
        var extra = linux ? LauncherBytes(kept.Path) : 0;
        var drive = (space ?? DiskSpace.ForFolder)(folder);
        if (drive.FreeBytes is { } free && free < kept.Bytes + extra + CopyMargin)
            throw new NotEnoughSpaceException($"Not enough free space on {drive.Name}: the installer needs {MB(kept.Bytes + extra + CopyMargin)}, {MB(free)} is free.");
        Directory.CreateDirectory(target);
        var file = Path.Combine(target, linux ? LinuxAppInstaller.PackageArchive : Path.GetFileName(kept.Path));
        CopyChecked(kept, file, progress, cancel);
        WriteHash(file, kept.Sha256);
        if (linux) ExtractLauncher(file, target, cancel);
        return new(target, file, kept.Sha256, kept.Bytes);
    }

    static InvalidDataException Damaged(KeptInstallerInfo kept, string actual) =>
        new($"The kept installer is damaged: its SHA-256 is {actual}, but {kept.Sha256} was recorded when it was kept. Nothing was copied. " +
            "Run HellForge-Setup again (this version or a newer one) to keep a good copy.");

    // Written to a .partial beside the target, hashed as it is written, then read back from the destination before it
    // takes the target's name: a failing drive or stick is caught there, not on a friend's PC.
    static void CopyChecked(KeptInstallerInfo kept, string file, Action<KeptInstallerProgress>? progress, CancellationToken cancel)
    {
        var partial = file + ".partial";
        try
        {
            string copied;
            using (var input = new HashingStream(new FileStream(kept.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20), HashAlgorithmName.SHA256))
            using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                var buffer = new byte[1 << 20];
                for (int read; (read = input.Read(buffer, 0, buffer.Length)) > 0;)
                {
                    cancel.ThrowIfCancellationRequested();
                    output.Write(buffer, 0, read);
                    progress?.Invoke(new("Copying", input.Bytes, kept.Bytes));
                }
                output.Flush(flushToDisk: true);
                copied = input.Hex();
            }
            if (copied != kept.Sha256) throw Damaged(kept, copied);
            var back = Sha256Of(partial, done => progress?.Invoke(new("Reading the copy back", done, kept.Bytes)), cancel);
            if (back != kept.Sha256)
                throw new IOException($"The copy in {Privacy.Scrub(Path.GetDirectoryName(file)!)} reads back with SHA-256 {back}, not {kept.Sha256}: " +
                    "the drive may be failing. It was deleted; try another folder or drive.");
            File.Move(partial, file, overwrite: true);
        }
        finally { if (File.Exists(partial)) File.Delete(partial); }
    }

    // The bytes the launcher files take once extracted from the package.
    static long LauncherBytes(string package)
    {
        try
        {
            using var archive = ZipFile.OpenRead(package);
            return archive.Entries.Where(entry => Launcher(entry) != null).Sum(entry => entry.Length);
        }
        catch (InvalidDataException error) { throw new InvalidDataException("The kept Linux package cannot be read as a zip: " + error.Message, error); }
    }

    // The path under app/launcher-linux/ an entry goes to in the installer folder, or null for every other entry.
    static string? Launcher(ZipArchiveEntry entry)
    {
        var name = entry.FullName.Replace('\\', '/');
        return name.StartsWith(LauncherEntries, StringComparison.Ordinal) && entry.Name.Length > 0 ? name[LauncherEntries.Length..] : null;
    }

    // The Linux installer is the native launcher's files beside the package (linux-package.yml copies app/launcher-linux);
    // the launcher, when its folder has a package, installs instead of starting.
    static void ExtractLauncher(string package, string target, CancellationToken cancel)
    {
        using var archive = ZipFile.OpenRead(package);
        foreach (var entry in archive.Entries)
        {
            if (Launcher(entry) is not { } relative) continue;
            cancel.ThrowIfCancellationRequested();
            var path = Path.GetFullPath(Path.Combine(target, relative));
            if (!LocalInstall.Within(path, target))
                throw new InvalidDataException("The kept Linux package contains an unsafe path: " + entry.FullName);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            entry.ExtractToFile(path + ".partial", overwrite: true);
            File.Move(path + ".partial", path, overwrite: true);
        }
        var launcher = Path.Combine(target, "HellForge.Launcher");
        if (OperatingSystem.IsLinux() && File.Exists(launcher))
            File.SetUnixFileMode(launcher, File.GetUnixFileMode(launcher) | UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);
    }
}
