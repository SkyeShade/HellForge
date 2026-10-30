using System.IO.Compression;
using HellForge.Core;

// The native Linux installer's app update: staging, the app and top-level payload swap and their rollback, the refusal
// to replace installation data, and the process check that keeps an update from touching a running installation.
static class LinuxInstallerTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux app update stages the package and keeps the old app until it is committed", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.App, "old-only.txt"), "old file");
            var package = Package(root, "linux-update.zip", "new launcher");
            var engineRan = false;
            var previous = LinuxAppInstaller.Install(install, package, () =>
            {
                engineRan = true;
                check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "new launcher",
                    "The engine step ran before the new app was in place.");
                check(File.Exists(Path.Combine(LinuxAppInstaller.Previous(install), "old-only.txt")),
                    "The old app was removed before the update finished.");
            });
            check(engineRan, "The engine step did not run.");
            check(previous == LinuxAppInstaller.Previous(install) && Directory.Exists(previous),
                "The old app was not kept for rollback.");
            check(!File.Exists(Path.Combine(install.App, "old-only.txt")) && File.Exists(Path.Combine(install.App, "new-only.txt")),
                "The new app did not replace the old files.");
            check((File.GetUnixFileMode(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) & UnixFileMode.UserExecute) != 0,
                "The new launcher is not executable.");
            check((File.GetUnixFileMode(Path.Combine(install.App, "linux", "hf-sandbox-probe")) & UnixFileMode.UserExecute) != 0,
                "The Linux sandbox probe is not executable.");
            check((File.GetUnixFileMode(Path.Combine(install.App, "capture-linux", "HellForge.CaptureProxy")) & UnixFileMode.UserExecute) != 0,
                "The native Account Sync capture is not executable.");
            check((File.GetUnixFileMode(Path.Combine(install.App, "linux", "hf-sandbox")) & UnixFileMode.UserExecute) != 0,
                "The Linux sandbox program is not executable.");
            check((File.GetUnixFileMode(Path.Combine(install.App, "linux", "capture-relay")) & UnixFileMode.UserExecute) != 0,
                "The Linux capture relay is not executable.");
            LinuxAppInstaller.Commit(install, previous);
            check(!Directory.Exists(previous) && !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) &&
                !Directory.Exists(LinuxAppInstaller.Staging(install)), "The old app or the staging folder stayed after commit.");
        });

        test("Linux app update installs app and the package's top-level program on a first install", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-first-install"));
            var package = Package(root, "linux-first-install.zip", "new launcher", topLevelProgram: "first stub");
            var previous = LinuxAppInstaller.Install(install, package, () => { });
            check(previous == null, "A first install kept an old app.");
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "new launcher",
                "The app was not installed.");
            check(File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "first stub",
                "The top-level program was not installed.");
            check(!install.Exists, "The package created the installation marker.");
            install.Create(SupportedBuild.Current.BuildId);
            check(File.Exists(install.Marker), "A first launcher start cannot create the installation marker after this install.");
            LinuxAppInstaller.Commit(install, previous);
            check(File.Exists(Path.Combine(install.Root, "HellForge.exe")), "Commit removed the new top-level program.");
        });

        test("Linux app update keeps the old app when the package cannot be extracted", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-bad-package"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            var package = Path.Combine(root, "linux-update-bad-package.zip");
            File.WriteAllText(package, "not a zip");
            reject(() => LinuxAppInstaller.Install(install, package, () => throw new InvalidDataException("The engine ran.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                !Directory.Exists(LinuxAppInstaller.Staging(install)), "An extraction failure changed the old app.");
        });

        test("Linux app update restores the old app when the new one cannot be moved in", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-no-new-app"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.App, "old-only.txt"), "old file");
            var missing = Path.Combine(LinuxAppInstaller.Staging(install), "app");
            reject(() => LinuxAppInstaller.Replace(install, missing, () => throw new InvalidDataException("The engine ran.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.Exists(Path.Combine(install.App, "old-only.txt")) && !Directory.Exists(LinuxAppInstaller.Previous(install)),
                "A failed app move did not restore the old app.");
        });

        test("Linux app update restores the old app and top-level program when the engine install fails", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-engine-fails"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.App, "old-only.txt"), "old file");
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "old stub");
            var package = Package(root, "linux-update-engine-fails.zip", "new launcher", topLevelProgram: "new stub");
            reject(() => LinuxAppInstaller.Install(install, package, () => throw new InvalidDataException("The engine broke.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.Exists(Path.Combine(install.App, "old-only.txt")) && !File.Exists(Path.Combine(install.App, "new-only.txt")) &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "old stub" &&
                !Directory.Exists(LinuxAppInstaller.Previous(install)) && !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) &&
                !Directory.Exists(LinuxAppInstaller.Staging(install)), "An engine failure did not restore the old installation.");
        });

        test("Linux app update replaces the top-level program and rollback restores the old one", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-no-start"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.App, "old-only.txt"), "old file");
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "old stub");
            File.WriteAllText(Path.Combine(install.Root, "old-root.txt"), "old root");
            var package = Package(root, "linux-update-no-start.zip", "new launcher", topLevelProgram: "new stub");
            var previous = LinuxAppInstaller.Install(install, package, () => { });
            check(previous != null, "The update did not keep the old app.");
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "new launcher" &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "new stub",
                "The update did not replace the app and the top-level program.");
            check(Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) &&
                File.ReadAllText(Path.Combine(LinuxAppInstaller.PreviousRoot(install), "payload", "HellForge.exe")) == "old stub",
                "The old top-level program was not kept for rollback.");
            LinuxAppInstaller.Rollback(install, previous!);
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.Exists(Path.Combine(install.App, "old-only.txt")) && !File.Exists(Path.Combine(install.App, "new-only.txt")) &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "old stub" &&
                File.ReadAllText(Path.Combine(install.Root, "old-root.txt")) == "old root" &&
                !Directory.Exists(previous) && !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)),
                "A launch failure did not restore the old installation.");
        });

        test("Linux app update refuses a package containing an installation-data folder", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-protected"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "old stub");
            var package = Package(root, "linux-update-protected.zip", "new launcher", topLevelProgram: "new stub", extraRootFolder: "mods");
            reject(() => LinuxAppInstaller.Install(install, package, () => throw new InvalidDataException("The engine ran.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "old stub" &&
                !Directory.Exists(Path.Combine(install.Root, "mods")) && !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) &&
                !Directory.Exists(LinuxAppInstaller.Staging(install)), "A package with mods/ changed the installation.");
        });

        test("Linux app update refuses an unexpected top-level file", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-unexpected"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "old stub");
            var package = Package(root, "linux-update-unexpected.zip", "new launcher", extraRootFile: "launcher-settings.json");
            reject(() => LinuxAppInstaller.Install(install, package, () => throw new InvalidDataException("The engine ran.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "old stub" &&
                !File.Exists(Path.Combine(install.Root, "launcher-settings.json")) &&
                !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) && !Directory.Exists(LinuxAppInstaller.Staging(install)),
                "A package with an unexpected top-level file changed the installation.");
        });

        test("Linux app update refuses a top-level entry of the wrong type", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux update)"); return; }
            var install = new InstallLayout(Path.Combine(root, "linux-update-wrong-type"));
            Directory.CreateDirectory(Path.Combine(install.App, "launcher-linux"));
            File.WriteAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"), "old launcher");
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "old stub");
            var package = Package(root, "linux-update-wrong-type.zip", "new launcher", topLevelProgram: null, extraRootFolder: "HellForge.exe");
            reject(() => LinuxAppInstaller.Install(install, package, () => throw new InvalidDataException("The engine ran.")));
            check(File.ReadAllText(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher")) == "old launcher" &&
                File.ReadAllText(Path.Combine(install.Root, "HellForge.exe")) == "old stub" &&
                !Directory.Exists(LinuxAppInstaller.PreviousRoot(install)) && !Directory.Exists(LinuxAppInstaller.Staging(install)),
                "A package with the wrong HellForge.exe type changed the installation.");
        });

        test("Linux running-install check recognises this installation's launcher and session, and no other folder", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux processes)"); return; }
            var prefix = Path.Combine(root, "linux-processes");
            var installRoot = Path.Combine(prefix, "pfx", "drive_c", "HellForge");
            var launcher = Path.Combine(installRoot, "app", "launcher-linux", "HellForge.Launcher");
            var wrapper = Path.Combine(installRoot, "app", "linux", "hellforge-linux.sh");
            Directory.CreateDirectory(installRoot);
            check(LinuxInstallProcesses.BelongsTo(new("HellForge.Launcher", launcher, [], [], null), installRoot),
                "A launcher from this app was not recognised.");
            check(!LinuxInstallProcesses.BelongsTo(new("HellForge.Launcher", Path.Combine(root, "other", "HellForge.Launcher"), [], [], null),
                installRoot), "A launcher from another folder was treated as this installation's.");
            var originalDirectory = Environment.CurrentDirectory;
            try
            {
                Directory.SetCurrentDirectory(installRoot);
                foreach (var argument in new[] { "bash", "-c", "--no-daemon", ".", "app" })
                    check(!LinuxInstallProcesses.BelongsTo(new("bash", "/usr/bin/bash", [argument], [], "/tmp"), installRoot),
                        $"A relative argument ({argument}) was resolved against the launcher's working directory.");
            }
            finally { Directory.SetCurrentDirectory(originalDirectory); }
            check(LinuxInstallProcesses.BelongsTo(new("bash", "/usr/bin/bash", [wrapper], [], null), installRoot),
                "The wrapper argument was not recognised.");
            check(LinuxInstallProcesses.BelongsTo(new("hf-inner.sh", "/usr/bin/bash", [], ["INSTALL=" + installRoot], null), installRoot),
                "The INSTALL environment entry was not recognised.");
            check(LinuxInstallProcesses.BelongsTo(new("hf-inner.sh", "/usr/bin/bash", [], [], installRoot), installRoot),
                "The installation as working directory was not recognised.");
            check(LinuxInstallProcesses.BelongsTo(new("HellForge.Session.exe", null,
                [@"C:\HellForge\app\session\HellForge.Session.exe"], [], null), installRoot), "The Wine path was not recognised.");
            check(LinuxInstallProcesses.BelongsTo(new("HellForge.Session.exe", null,
                [@"Z:" + wrapper.Replace('/', '\\')], [], null), installRoot), "A Z: path argument was not recognised.");
            check(!LinuxInstallProcesses.BelongsTo(new("bash", "/usr/bin/bash", [], ["INSTALL=" + installRoot + "-other"], null), installRoot),
                "A similar installation path was treated as this one.");
        });

        test("Linux leftover check before PLAY leaves the Steam bridge and Steam's reaper for it alone, and nothing else", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux processes)"); return; }
            var installRoot = Path.Combine(root, "linux-bridge-processes", "pfx", "drive_c", "HellForge");
            var app = Path.Combine(installRoot, "app");
            var cli = Path.Combine(app, "cli-linux", "HellForge.Cli");
            var script = Path.Combine(app, "linux", "hf-bridge-host.sh");
            Directory.CreateDirectory(installRoot);
            LinuxProcessInfo Process(string name, string? exe, string[] arguments, string[]? environment = null, string? cwd = null) =>
                new(name, exe, arguments, environment ?? [], cwd);
            // From steam-run, a process Steam started shows only its command line: no executable, environment or folder.
            var bridges = new[]
            {
                Process("HellForge.Cli", null, [cli, "steam-bridge", "host"]),
                Process("HellForge.Cli", cli, [Path.Combine(app, "linux", "..", "cli-linux", "HellForge.Cli"), "steam-bridge", "host"],
                    ["INSTALL=" + installRoot], Path.Combine(app, "linux")),
                Process("reaper", null, ["/home/player/.local/share/Steam/ubuntu12_32/reaper", "SteamLaunch", "AppId=3141592653", "--", script],
                    cwd: Path.Combine(app, "linux")),
                Process("bash", "/usr/bin/bash", ["bash", script], ["INSTALL=" + installRoot]),
            };
            foreach (var bridge in bridges)
                check(LinuxInstallProcesses.BelongsTo(bridge, installRoot) && LinuxInstallProcesses.IsSteamBridge(bridge, installRoot),
                    "Not recognised as this installation's Steam bridge: " + string.Join(" ", bridge.Arguments));
            var sessions = new[]
            {
                Process("HellForge.Cli", cli, [cli, "steam-bridge", "inner"]),
                Process("bash", "/usr/bin/bash", ["bash", Path.Combine(app, "linux", "hellforge-linux.sh")]),
                Process("reaper", null, ["reaper", "SteamLaunch", "AppId=1", "--", script, Path.Combine(app, "session", "HellForge.Session.exe")]),
                Process("HellForge.Launcher", Path.Combine(app, "launcher-linux", "HellForge.Launcher"), ["HellForge.Launcher", script]),
                Process("helldivers2.exe", null, [@"C:\HellForge\game\bin\helldivers2.exe"], cwd: Path.Combine(app, "linux")),
            };
            foreach (var session in sessions)
                check(LinuxInstallProcesses.BelongsTo(session, installRoot) && !LinuxInstallProcesses.IsSteamBridge(session, installRoot),
                    "Treated as the Steam bridge, so PLAY would not wait for it: " + string.Join(" ", session.Arguments));
            var other = Path.Combine(root, "linux-bridge-other", "app", "cli-linux", "HellForge.Cli");
            check(!LinuxInstallProcesses.IsSteamBridge(Process("HellForge.Cli", null, [other, "steam-bridge", "host"]), installRoot),
                "Another folder's bridge was treated as this installation's.");
        });
    }

    static string Package(string root, string name, string launcher, string? topLevelProgram = "stub",
        string? extraRootFolder = null, string? extraRootFile = null)
    {
        var path = Path.Combine(root, name);
        using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
        void Add(string entry, string content)
        {
            using var writer = new StreamWriter(archive.CreateEntry(entry).Open());
            writer.Write(content);
        }
        Add("app/app-build.json", "{}");
        Add("app/engine-package.zip", "engine");
        Add("app/launcher-linux/HellForge.Launcher", launcher);
        Add("app/capture-linux/HellForge.CaptureProxy", "proxy");
        Add("app/linux/hellforge-linux.sh", "#!/bin/sh\n");
        Add("app/linux/hf-steam.sh", "#!/bin/sh\n");
        Add("app/linux/hf-container.sh", "#!/bin/sh\n");
        Add("app/linux/hf-sandbox-probe", "probe");
        Add("app/linux/allow-capture.sh", "#!/bin/sh\n");
        Add("app/linux/capture-helper.sh", "#!/bin/sh\n");
        Add("app/linux/capture-relay", "relay");
        Add("app/linux/hf-sandbox", "sandbox");
        Add("app/new-only.txt", "new file");
        if (topLevelProgram != null) Add("HellForge.exe", topLevelProgram);
        if (extraRootFolder != null) Add(extraRootFolder + "/payload.txt", "payload");
        if (extraRootFile != null) Add(extraRootFile, "unexpected");
        return path;
    }
}
