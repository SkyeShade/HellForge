using System.IO.Compression;

namespace HellForge.Core;

// The native Linux installer's app update. The payload root is only app/ (a folder) and HellForge.exe (a file). The
// package (a zip or a folder) is staged in <install>/temp/app-staging; app/ is moved to <install>/temp/app-previous and
// HellForge.exe to <install>/temp/app-previous-root. The staged files then take their place. All previous files stay
// until Commit, so any failure can put them back.
public static class LinuxAppInstaller
{
    const string StagingFolder = "app-staging";
    const string PreviousFolder = "app-previous";
    const string PreviousRootFolder = "app-previous-root";
    const string PayloadFolder = "payload";
    const string AppliedFolder = "applied";
    const string AppFolder = "app";
    const string LauncherProgram = "HellForge.exe";
    const string EngineArchive = "engine-package.zip";
    const string EngineFolder = "engine-package";

    // The payload beside the installer: HellForge-Linux.zip, or the same files unpacked in HellForge-Linux/ with the
    // engine package as a folder. Nexus Mods refuses a download with an archive inside it, so it gets the unpacked form.
    public const string PackageArchive = "HellForge-Linux.zip";
    public const string PackageFolder = "HellForge-Linux";

    // What a package must hold before the installer prepares a prefix for it.
    static readonly string[] RequiredEntries =
    [
        "app/app-build.json",
        "app/" + EngineArchive,
        "app/launcher-linux/HellForge.Launcher",
        "app/capture-linux/HellForge.CaptureProxy",
        "app/backend-linux/HellForge.LocalBackend",
        "app/linux/hellforge-linux.sh",
        "app/linux/hf-steam.sh",
        "app/linux/hf-container.sh",
        "app/linux/hf-wineserver.sh",
        "app/linux/hf-relays.sh",
        "app/linux/hf-coop-relays.sh",
        "app/linux/hf-sandbox-probe",
        "app/linux/allow-capture.sh",
        "app/linux/capture-helper.sh",
        "app/linux/capture-relay",
        "app/linux/hf-sandbox"
    ];

    enum RootEntryKind { Folder, File }

    // Exactly what scripts/Publish-HellForge.ps1 puts at the payload root. Anything else belongs to the installation.
    static readonly Dictionary<string, RootEntryKind> AllowedRootEntries = new(StringComparer.Ordinal)
    {
        [AppFolder] = RootEntryKind.Folder,
        [LauncherProgram] = RootEntryKind.File
    };

    public static string Staging(InstallLayout install) => Path.Combine(install.Temp, StagingFolder);
    public static string Previous(InstallLayout install) => Path.Combine(install.Temp, PreviousFolder);
    public static string PreviousRoot(InstallLayout install) => Path.Combine(install.Temp, PreviousRootFolder);

    // Where the installer puts HellForge in a Proton prefix: the prefix user's Documents folder.
    public static string InstallRoot(string prefix) =>
        Path.Combine(prefix, "pfx", "drive_c", "users", "steamuser", "Documents", "HellForge");

    // Why HellForge cannot be installed into a prefix folder, or null. Checked before the prefix is prepared: the
    // launcher refuses to run from inside steamapps, so an install there would be one that never starts. A link above
    // the folder is resolved first, as InstallLayout does. An installation that already runs there (one made under Wine
    // before the launcher was native) is still updated: only a new one is refused.
    public static string? PrefixProblem(string prefix)
    {
        var install = new InstallLayout(InstallRoot(prefix));
        return install.Exists || InstallLayout.LocationProblem(install.Root) == null ? null
            : "Choose a prefix folder outside Steam's steamapps folders: Steam checks, updates and deletes what is in " +
              "them, a Proton version's own folder included. This one is inside steamapps: " + prefix;
    }

    // The native launcher's first start in a folder the installer unpacked: marks it as an installation. Null, with why
    // in words for a window, when the folder cannot be one; the launcher shows that instead of ending without a word.
    public static InstallLayout? FirstStart(string packageRoot, string buildId, out string? problem)
    {
        problem = null;
        var install = new InstallLayout(packageRoot);
        try
        {
            install.Create(buildId);
            return install;
        }
        catch (InvalidDataException) when (InstallLayout.LocationProblem(install.Root) != null)
        {
            problem = "HellForge cannot run from inside Steam's steamapps folder: Steam checks, updates and deletes what " +
                "is in there.\n\nThis copy is in " + install.Root + ".\n\nRun the HellForge installer again and choose " +
                "a Proton prefix folder outside every steamapps folder, such as the installer's default or a new folder " +
                "at the top of the same drive. Then delete this copy.";
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or
            ArgumentException or NotSupportedException)
        {
            problem = "HellForge could not prepare its folder (" + install.Root + "): " + error.Message;
        }
        return null;
    }

    // The package in an installer's folder, the archive first; null when there is none.
    public static string? FindPackage(string folder)
    {
        var archive = Path.Combine(folder, PackageArchive);
        if (File.Exists(archive)) return archive;
        var unpacked = Path.Combine(folder, PackageFolder);
        return Directory.Exists(Path.Combine(unpacked, AppFolder)) ? unpacked : null;
    }

    // Why a package (archive or folder) cannot be installed, or null.
    public static string? PackageProblem(string packagePath)
    {
        Func<string, bool> has;
        if (Directory.Exists(packagePath))
            has = entry => File.Exists(Path.Combine(packagePath, entry));
        else if (File.Exists(packagePath))
        {
            using var archive = ZipFile.OpenRead(packagePath);
            var entries = archive.Entries.Select(entry => entry.FullName.Replace('\\', '/')).ToHashSet(StringComparer.Ordinal);
            has = entries.Contains;
        }
        else return "The HellForge Linux package was not found beside this installer.";
        foreach (var required in RequiredEntries)
        {
            var found = has(required) || required == "app/" + EngineArchive && has("app/" + EngineFolder + "/engine.json");
            if (!found) return "The Linux package is incomplete; missing " + required + ".";
        }
        return null;
    }

    // Refuses while this installation has a process; unpacks the package, then replaces app/ and HellForge.exe. Returns
    // the old app folder, or null for a first install. The caller commits after the new launcher has started.
    public static string? Install(InstallLayout install, string packagePath, Action installEngine)
    {
        EnsureNotRunning(install);
        var stagedApp = Stage(install, packagePath);
        try
        {
            return Replace(install, stagedApp, installEngine);
        }
        finally
        {
            TryDelete(Staging(install));
        }
    }

    // Extracts into the installation's temp folder and checks the package's app folder. A failure leaves app/ untouched.
    public static string Stage(InstallLayout install, string packagePath)
    {
        var staging = Staging(install);
        try
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Directory.CreateDirectory(staging);
            if (Directory.Exists(packagePath)) CopyPackage(packagePath, staging);
            else Extract(packagePath, staging);
            var app = Path.Combine(staging, AppFolder);
            PackEngine(app);
            var launcher = Path.Combine(app, "launcher-linux", "HellForge.Launcher");
            var scripts = Path.Combine(app, "linux");
            if (!File.Exists(launcher) || !Directory.Exists(scripts))
                throw new InvalidDataException("The package does not contain a complete app folder.");
            MakeExecutable(launcher, scripts);
            return app;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or NotSupportedException)
        {
            TryDelete(staging);
            throw new InvalidDataException("The package could not be extracted: " + error.Message + " The installed app was not changed.", error);
        }
    }

    static void Extract(string packagePath, string staging)
    {
        using var archive = ZipFile.OpenRead(packagePath);
        foreach (var entry in archive.Entries)
        {
            var path = Path.GetFullPath(Path.Combine(staging, entry.FullName.Replace('\\', '/')));
            if (!LocalInstall.Within(path, staging)) throw new InvalidDataException("The package contains an unsafe path: " + entry.FullName);
            if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\'))
            {
                Directory.CreateDirectory(path);
                continue;
            }
            if (entry.Name.Length == 0) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            entry.ExtractToFile(path);
        }
    }

    // A link is refused rather than followed, so nothing outside the package folder is copied into the installation.
    static void CopyPackage(string source, string target)
    {
        Directory.CreateDirectory(target);
        foreach (var entry in new DirectoryInfo(source).EnumerateFileSystemInfos("*", new EnumerationOptions { AttributesToSkip = 0 }))
        {
            if (entry.LinkTarget != null) throw new InvalidDataException("The package contains a link: " + entry.FullName);
            var destination = Path.Combine(target, entry.Name);
            if (entry is DirectoryInfo folder) CopyPackage(folder.FullName, destination);
            else File.Copy(entry.FullName, destination);
        }
    }

    // The launcher and setup read app/engine-package.zip, so an unpacked engine package is zipped back. Entries are named
    // with '/' and one per file, as engine.json lists them; its hashes then check every file as for the original zip.
    static void PackEngine(string app)
    {
        var folder = Path.Combine(app, EngineFolder);
        if (!Directory.Exists(folder)) return;
        var archivePath = Path.Combine(app, EngineArchive);
        if (File.Exists(archivePath))
            throw new InvalidDataException("The package holds the engine package twice, as " + EngineArchive + " and as a folder.");
        using (var archive = ZipFile.Open(archivePath, ZipArchiveMode.Create))
            foreach (var file in Directory.EnumerateFiles(folder, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 })
                .Order(StringComparer.Ordinal))
                archive.CreateEntryFromFile(file, Path.GetRelativePath(folder, file).Replace(Path.DirectorySeparatorChar, '/'),
                    CompressionLevel.Fastest);
        Directory.Delete(folder, recursive: true);
    }

    // Moves app/ and HellForge.exe aside, moves the staged ones in, then runs installEngine before anything is removed.
    // Any failure after the first move puts the previous files back.
    public static string? Replace(InstallLayout install, string stagedApp, Action installEngine)
    {
        ValidateRootEntries(stagedApp);
        EnsureNotRunning(install);
        var staging = Path.GetDirectoryName(stagedApp)!;
        Directory.CreateDirectory(install.Temp);
        var previousApp = Previous(install);
        var previousRoot = PreviousRoot(install);
        Recover(install, previousApp, previousRoot);
        var hadApp = Directory.Exists(install.App);
        var oldMoved = false;
        var newMoved = false;
        try
        {
            if (hadApp)
            {
                Directory.Move(install.App, previousApp);
                oldMoved = true;
            }
            Directory.Move(stagedApp, install.App);
            newMoved = true;
            ApplyRoot(install, staging, previousRoot);
        }
        catch (Exception error)
        {
            var rollback = TryRestore(install, previousApp, previousRoot, oldMoved, newMoved);
            if (rollback != null) throw Failed(error, rollback, install);
            throw new InvalidDataException("The app could not be replaced: " + error.Message +
                (hadApp ? " The previous installation was kept." : " Nothing was installed."), error);
        }
        try
        {
            installEngine();
        }
        catch (Exception error)
        {
            var rollback = TryRestore(install, previousApp, previousRoot, oldMoved, newMoved);
            if (rollback != null) throw Failed(error, rollback, install);
            throw new InvalidDataException("The engine could not be installed: " + error.Message +
                (hadApp ? " The previous installation was restored." : " The new app was removed."), error);
        }
        return hadApp ? previousApp : null;
    }

    // Removes the old app and top-level files after the new launcher has started. A failure leaves them in temp for the
    // next update to replace.
    public static void Commit(InstallLayout install, string? previous)
    {
        if (previous != null)
            try { if (Directory.Exists(previous)) Directory.Delete(previous, recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        TryDelete(PreviousRoot(install));
    }

    // Puts the old app and top-level files back after a later step (starting the new launcher) fails.
    public static void Rollback(InstallLayout install, string previous)
    {
        if (TryRestore(install, previous, PreviousRoot(install), oldMoved: true, newMoved: true) is { } error)
            throw new IOException("The previous installation could not be restored completely; its files are kept under " + install.Temp + ".", error);
    }

    static void EnsureNotRunning(InstallLayout install)
    {
        var running = LinuxInstallProcesses.Running(install.Root);
        if (running.Count > 0)
            throw new InvalidDataException("Close HellForge and any running session first: " + string.Join(", ", running) + ".");
    }

    // Refuses a package whose root is not exactly app/ (a folder) and HellForge.exe (a file), before app/ is touched.
    static void ValidateRootEntries(string stagedApp)
    {
        var staging = Path.GetDirectoryName(stagedApp);
        if (staging == null || !Directory.Exists(staging)) return;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in Directory.EnumerateFileSystemEntries(staging)) seen.Add(ValidateRootEntry(entry));
        foreach (var name in AllowedRootEntries.Keys)
            if (!seen.Contains(name)) throw new InvalidDataException("The package root is missing " + name + ".");
    }

    static string ValidateRootEntry(string entry)
    {
        var name = Path.GetFileName(entry);
        if (name == null || !AllowedRootEntries.TryGetValue(name, out var kind))
            throw new InvalidDataException("The package root contains an unexpected entry: " + name +
                ". Only app/ and HellForge.exe are allowed.");
        var rightType = kind == RootEntryKind.Folder ? Directory.Exists(entry) : File.Exists(entry);
        if (!rightType)
            throw new InvalidDataException("The package root entry " + name + " must be a " +
                (kind == RootEntryKind.Folder ? "folder." : "file."));
        return name;
    }

    // Installs the payload root's other allowed entry, HellForge.exe. The old one is moved to
    // temp/app-previous-root/payload and recorded in applied/ before the new one goes in, so an interrupted update can
    // be undone. app/ is not included: Replace moves it separately.
    static void ApplyRoot(InstallLayout install, string staging, string previousRoot)
    {
        var names = new List<string>();
        foreach (var entry in Directory.EnumerateFileSystemEntries(staging))
            if (Path.GetFileName(entry) is { Length: > 0 } name && name != AppFolder) names.Add(name);
        if (names.Count == 0) return;
        var applied = Path.Combine(previousRoot, AppliedFolder);
        var payload = Path.Combine(previousRoot, PayloadFolder);
        Directory.CreateDirectory(applied);
        foreach (var name in names)
        {
            var current = Path.Combine(install.Root, name);
            if (Directory.Exists(current) || File.Exists(current))
            {
                Directory.CreateDirectory(payload);
                MoveEntry(current, Path.Combine(payload, name));
            }
            File.WriteAllText(Path.Combine(applied, name), "");
            MoveEntry(Path.Combine(staging, name), current);
        }
    }

    // A previous update that stopped before Commit is undone before the next one starts: put the old app and top-level
    // files back.
    static void Recover(InstallLayout install, string previousApp, string previousRoot)
    {
        if (TryRestoreRoot(install, previousRoot) is { } rootError)
            throw new IOException("A previous app update could not be undone; its files are kept under " + previousRoot + ".", rootError);
        if (!Directory.Exists(previousApp)) return;
        if (Directory.Exists(install.App)) Directory.Delete(install.App, recursive: true);
        Directory.Move(previousApp, install.App);
    }

    // The new files are in place but not started: restore the old app and top-level entries. Returns why the restore
    // failed, or null.
    static Exception? TryRestore(InstallLayout install, string previous, string previousRoot, bool oldMoved, bool newMoved)
    {
        var rootError = TryRestoreRoot(install, previousRoot);
        try
        {
            if (newMoved && Directory.Exists(install.App)) Directory.Delete(install.App, recursive: true);
            if (oldMoved)
            {
                if (!Directory.Exists(previous))
                    throw new DirectoryNotFoundException("The previous app folder is missing: " + previous);
                Directory.Move(previous, install.App);
            }
            return rootError;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return rootError == null ? error : new AggregateException(rootError, error);
        }
    }

    // Restores temp/app-previous-root. A name is restored from payload/ when an old entry exists; otherwise applied/
    // says the new entry has no predecessor and is removed. Markers are removed before the old entry is moved back, so a
    // retry after a failure cannot delete a restored entry.
    static Exception? TryRestoreRoot(InstallLayout install, string previousRoot)
    {
        if (!Directory.Exists(previousRoot)) return null;
        try
        {
            var applied = Path.Combine(previousRoot, AppliedFolder);
            var payload = Path.Combine(previousRoot, PayloadFolder);
            foreach (var name in RootBackupNames(applied, payload)) RestoreRootEntry(install, applied, payload, name);
            Directory.Delete(previousRoot, recursive: true);
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return error; }
    }

    static HashSet<string> RootBackupNames(string applied, string payload)
    {
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var folder in new[] { applied, payload })
            if (Directory.Exists(folder))
                foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
                    if (Path.GetFileName(entry) is { Length: > 0 } name) names.Add(name);
        return names;
    }

    static void RestoreRootEntry(InstallLayout install, string applied, string payload, string name)
    {
        var current = Path.Combine(install.Root, name);
        var marker = Path.Combine(applied, name);
        var old = Path.Combine(payload, name);
        if (Directory.Exists(old) || File.Exists(old))
        {
            DeleteEntry(current);
            if (File.Exists(marker)) File.Delete(marker);
            MoveEntry(old, current);
        }
        else if (File.Exists(marker))
        {
            DeleteEntry(current);
            File.Delete(marker);
        }
    }

    static void MoveEntry(string source, string target)
    {
        if (Directory.Exists(source)) Directory.Move(source, target);
        else File.Move(source, target, overwrite: true);
    }

    static void DeleteEntry(string path)
    {
        if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
        else if (File.Exists(path)) File.Delete(path);
    }

    static Exception Failed(Exception error, Exception rollback, InstallLayout install) =>
        new IOException("The app update failed and the previous installation could not be restored; its files are kept under " +
            install.Temp + ".", new AggregateException(error, rollback));

    static void MakeExecutable(string launcher, string scripts)
    {
        if (!OperatingSystem.IsLinux()) return;
        MakeExecutable(launcher);
        // The native source-mod build agent is optional; the native Account Sync capture is part of the package.
        var app = Path.GetDirectoryName(launcher)!;
        var agent = Path.Combine(app, "..", "cli-linux", "HellForge.Cli");
        if (File.Exists(agent)) MakeExecutable(agent);
        // The native Linux sandbox programs and Sync Inventory's relay. New packages require them; this keeps a
        // hand-extracted file usable too.
        foreach (var native in new[] { "hf-sandbox-probe", "hf-sandbox", "capture-relay" })
        {
            var path = Path.Combine(scripts, native);
            if (File.Exists(path)) MakeExecutable(path);
        }
        MakeExecutable(Path.Combine(app, "..", "capture-linux", "HellForge.CaptureProxy"));
        // Account import's profile commands on native Linux; the installer's package check requires it.
        var backend = Path.Combine(app, "..", "backend-linux", "HellForge.LocalBackend");
        if (File.Exists(backend)) MakeExecutable(backend);
        foreach (var script in Directory.EnumerateFiles(scripts, "*.sh")) MakeExecutable(script);
    }

    static void MakeExecutable(string path)
    {
        if (!OperatingSystem.IsLinux()) return;
        const UnixFileMode executable = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
        File.SetUnixFileMode(path, File.GetUnixFileMode(path) | executable);
    }

    static void TryDelete(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }
}
