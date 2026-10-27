using HellForge.Core;

static class WinePathTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("Wine paths map between C:, Z: and native prefix paths", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            var install = Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge");
            Directory.CreateDirectory(install);
            check(WinePath.PrefixFromInstallRoot(install) == prefix, "Derived the Proton prefix from the installation.");
            check(WinePath.ToHostPath(@"C:\users\steamuser\Documents\HellForge\logs", prefix) ==
                Path.Combine(install, "logs"), "Mapped a C: path into the prefix.");
            check(WinePath.ToHostPath(@"Z:\home\steamuser\Downloads", prefix) ==
                Path.Combine(Path.GetPathRoot(Path.GetFullPath(prefix))!, "home", "steamuser", "Downloads"), "Mapped Z: to the filesystem root.");
            check(WinePath.ToWinePath(Path.Combine(install, "logs"), prefix) == @"C:\users\steamuser\Documents\HellForge\logs",
                "Mapped a prefix path back to C:.");
            check(WinePath.ToWinePath(Path.Combine(Path.GetPathRoot(Path.GetFullPath(prefix))!, "tmp", "logs"), prefix) == @"Z:\tmp\logs",
                "Mapped an external path back to Z:.");
            check(WinePath.ToHostPath(Path.Combine(install, "logs"), prefix) == Path.Combine(install, "logs"),
                "Left an already-native path unchanged.");

            var paths = new InstallSessionPaths(new InstallLayout(install));
            var profile = new LocalProfile("default", Path.Combine(install, "profiles", "default"), "starter");
            var arguments = SessionRunner.Arguments(paths, profile,
                new(true, true, ProfileStore.WarLive, true, false, []), prefix);
            check(arguments.Contains(@"C:\users\steamuser\Documents\HellForge") &&
                arguments.Contains(@"C:\users\steamuser\Documents\HellForge\profiles\default") &&
                arguments.Contains(@"C:\users\steamuser\Documents\HellForge\live-war.json"),
                "Session arguments use Windows paths inside Proton.");

            if (OperatingSystem.IsLinux())
            {
                var runFolder = Path.Combine(install, "logs", "sessions", "run");
                Directory.CreateDirectory(Path.GetDirectoryName(paths.SessionFile)!);
                var started = DateTime.UtcNow;
                File.WriteAllText(paths.SessionFile, System.Text.Json.JsonSerializer.Serialize(new
                {
                    runId = "run",
                    run = @"C:\users\steamuser\Documents\HellForge\logs\sessions\run",
                    startedUtc = started.ToString("o")
                }));
                check(SessionRunner.LatestRun(paths) == runFolder && SessionRunner.RunSince(paths, null) == runFolder,
                    "Session run-file C: paths map back to native folders.");
            }
        });

        test("Wine C: paths cannot escape the prefix", () =>
        {
            try { WinePath.ToHostPath(@"C:\..\..\outside", Path.Combine(root, "wine-prefix")); }
            catch (InvalidDataException) { return; }
            throw new Exception("An escaping C: path was accepted.");
        });
    }
}
