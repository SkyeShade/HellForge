using System.IO.Compression;

namespace HellForge.Core;

// The native Linux installer's app update. The payload root is only app/ (a folder) and HellForge.exe (a file). The
// package is unpacked into <install>/temp/app-staging; app/ is moved to <install>/temp/app-previous and HellForge.exe to
// <install>/temp/app-previous-root. The staged files then take their place. All previous files stay until Commit, so any
// failure can put them back.
public static class LinuxAppInstaller
{
    const string StagingFolder = "app-staging";
    const string PreviousFolder = "app-previous";
    const string PreviousRootFolder = "app-previous-root";
    const string PayloadFolder = "payload";
    const string AppliedFolder = "applied";
    const string AppFolder = "app";
    const string LauncherProgram = "HellForge.exe";

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
            var app = Path.Combine(staging, AppFolder);
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
        // The native Linux sandbox programs. New packages require them; this keeps a hand-extracted file usable too.
        foreach (var sandbox in new[] { "hf-sandbox-probe", "hf-sandbox" })
        {
            var path = Path.Combine(scripts, sandbox);
            if (File.Exists(path)) MakeExecutable(path);
        }
        MakeExecutable(Path.Combine(app, "..", "capture-linux", "HellForge.CaptureProxy"));
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
