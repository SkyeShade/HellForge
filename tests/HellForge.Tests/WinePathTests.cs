using HellForge.Core;

static class WinePathTests
{
    static void RefusesInvalidData(Action action, string expected, string reason)
    {
        try { action(); }
        catch (InvalidDataException error) when (error.Message.Contains(expected, StringComparison.Ordinal)) { return; }
        throw new Exception(reason);
    }

    static void RefusesPrefix(Action action, string reason)
    {
        try { action(); }
        catch (InvalidOperationException error) when (error.Message.Contains("pfx/drive_c", StringComparison.Ordinal)) { return; }
        throw new Exception(reason);
    }

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

        test("Wine paths keep spaces and non-ASCII names in both directions", () =>
        {
            var prefix = Path.Combine(root, "Wine Prefix 日本語");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            var install = Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge Mods", "保存データ");
            Directory.CreateDirectory(install);
            var external = Path.Combine(Path.GetPathRoot(Path.GetFullPath(prefix))!, "tmp", "HellForge データ");
            check(WinePath.PrefixFromInstallRoot(install) == prefix, "Found a prefix whose path has spaces and non-ASCII names.");
            check(WinePath.ToWinePath(install, prefix) == @"C:\users\steamuser\Documents\HellForge Mods\保存データ",
                "Mapped a C: path with spaces and non-ASCII names.");
            check(WinePath.ToHostPath(@"C:\users\steamuser\Documents\HellForge Mods\保存データ", prefix) == install,
                "Mapped back a C: path with spaces and non-ASCII names.");
            check(WinePath.ToWinePath(external, prefix) == @"Z:\tmp\HellForge データ",
                "Mapped an external path with spaces and non-ASCII names to Z:.");
            check(WinePath.ToHostPath(@"Z:\tmp\HellForge データ", prefix) == external,
                "Mapped back a Z: path with spaces and non-ASCII names.");
        });

        test("Wine paths at the C: root and at the prefix root", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix-root");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            var install = Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge");
            Directory.CreateDirectory(install);
            check(WinePath.PrefixFromInstallRoot(driveC) == prefix, "Derived the prefix from the drive_c root itself.");
            check(WinePath.ToWinePath(driveC, prefix) == @"C:\", "Mapped the drive_c root to C:\\.");
            check(WinePath.ToHostPath(@"C:\", prefix) == driveC, "Mapped C:\\ to the drive_c root.");
            var prefixWine = WinePath.ToWinePath(prefix, prefix);
            check(prefixWine.StartsWith(@"Z:\", StringComparison.Ordinal), "The prefix itself is outside drive_c, so it maps through Z:.");
            check(WinePath.ToHostPath(prefixWine, prefix) == prefix, "The prefix root round-trips through Z:.");
            if (OperatingSystem.IsLinux())
                check(WinePath.ToHostPath(prefix, prefix) == prefix, "Left an already-native prefix root unchanged.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(prefix), "Accepted the prefix root as an installation root.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(Path.Combine(prefix, "pfx")), "Accepted pfx without drive_c.");
        });

        test("Wine paths with trailing separators", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix-trailing");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            var install = Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge");
            Directory.CreateDirectory(install);
            var separator = Path.DirectorySeparatorChar;
            var hostRoot = Path.GetPathRoot(Path.GetFullPath(prefix))!;
            var external = Path.Combine(hostRoot, "var", "tmp", "HellForge");
            check(WinePath.PrefixFromInstallRoot(install + separator) == prefix, "Derived the prefix from an installation with a trailing separator.");
            check(WinePath.PrefixFromInstallRoot(driveC + separator) == prefix, "Derived the prefix from a drive_c root with a trailing separator.");
            check(WinePath.ToWinePath(driveC + separator, prefix) == @"C:\", "The drive_c root with a trailing separator is still C:\\.");
            check(WinePath.ToWinePath(install + separator, prefix).TrimEnd('\\', '/') ==
                @"C:\users\steamuser\Documents\HellForge", "Mapped a C: path with a trailing separator.");
            var host = WinePath.ToHostPath(@"C:\users\steamuser\Documents\HellForge\", prefix);
            check(host.TrimEnd(separator, '/') == install, "Mapped back a C: path with a trailing separator.");
            check(WinePath.ToWinePath(external + separator, prefix).TrimEnd('\\', '/') == @"Z:\var\tmp\HellForge",
                "Mapped an external path with a trailing separator to Z:.");
            var externalHost = WinePath.ToHostPath(@"Z:\var\tmp\HellForge\", prefix);
            check(externalHost.TrimEnd(separator, '/') == external, "Mapped back a Z: path with a trailing separator.");
            check(WinePath.ToHostPath("C:" + separator + separator, prefix) == driveC,
                "A repeated root separator still maps to drive_c.");
        });

        test("Wine Z: paths map host paths outside the prefix", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix-z");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            Directory.CreateDirectory(driveC);
            var hostRoot = Path.GetPathRoot(Path.GetFullPath(prefix))!;
            var external = Path.Combine(hostRoot, "var", "tmp", "HellForge");
            check(WinePath.ToHostPath(@"Z:\", prefix) == hostRoot, "Z:\\ is the host filesystem root.");
            check(WinePath.ToWinePath(hostRoot, prefix) == @"Z:\", "The host filesystem root is Z:\\.");
            check(WinePath.ToHostPath(@"Z:\var\tmp\HellForge", prefix) == external, "Z: reaches outside the prefix.");
            check(WinePath.ToWinePath(external, prefix) == @"Z:\var\tmp\HellForge", "A host path outside the prefix maps through Z:.");
            check(WinePath.ToHostPath(@"Z:\var\..\etc", prefix) == Path.Combine(hostRoot, "etc"),
                "Z: paths are normalized against the host root.");
        });

        test("Wine paths pass native paths through and refuse other Windows drives", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix-drives");
            var relative = Path.Combine("profiles", "default");
            check(WinePath.ToHostPath(relative, prefix) == relative, "A relative native path is unchanged.");
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: drive-letter refusal is Linux-only)"); return; }
            RefusesInvalidData(() => WinePath.ToHostPath(@"D:\Games\HellForge", prefix), "D:",
                "ToHostPath returned a path on drive D: instead of refusing it.");
            RefusesInvalidData(() => WinePath.ToHostPath("D:", prefix), "D:",
                "ToHostPath returned a bare D: path instead of refusing it.");
            RefusesInvalidData(() => WinePath.ToWinePath(@"D:\Games\HellForge", prefix), "D:",
                "ToWinePath mapped an already-Windows path on drive D:.");
            RefusesInvalidData(() => WinePath.ToWinePath(@"C:\users\steamuser", prefix), "C:",
                "ToWinePath mapped an already-Windows C: path instead of refusing it.");
        });

        test("Wine prefix detection refuses paths that are not inside pfx/drive_c", () =>
        {
            var prefix = Path.Combine(root, "wine-prefix-detection");
            var driveC = Path.Combine(prefix, "pfx", "drive_c");
            var install = Path.Combine(driveC, "users", "steamuser", "Documents", "HellForge");
            Directory.CreateDirectory(install);
            check(WinePath.PrefixFromInstallRoot(install) == prefix, "Derived the prefix from a nested installation.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(prefix), "Accepted a path outside pfx/drive_c.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(Path.Combine(prefix, "pfx")), "Accepted pfx without drive_c.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(Path.Combine(driveC, "..")), "Accepted drive_c's parent.");
            RefusesPrefix(() => WinePath.PrefixFromInstallRoot(Path.Combine(root, "ordinary-folder")), "Accepted an ordinary host folder.");
        });
    }
}
