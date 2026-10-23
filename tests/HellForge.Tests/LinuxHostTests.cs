using HellForge.Core;

static class LinuxHostTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        if (!OperatingSystem.IsLinux()) return;
        static void Refuses(Action action)
        {
            try { action(); }
            catch (InvalidOperationException) { return; }
            throw new Exception("Expected InvalidOperationException.");
        }

        test("Linux paths reach Windows programs as C:\\ inside the prefix and as Z:\\ elsewhere, with odd names kept", () =>
        {
            var prefix = Directory.CreateDirectory(Path.Combine(root, "lh-prefix")).FullName;
            Environment.SetEnvironmentVariable("STEAM_COMPAT_DATA_PATH", prefix);
            var drive = Path.Combine(prefix, "pfx", "drive_c");
            check(LinuxHost.WindowsPath(drive) == "C:\\", "the drive itself: " + LinuxHost.WindowsPath(drive));
            check(LinuxHost.WindowsPath(Path.Combine(drive, "users", "steamuser", "My Mods", "ä ö.dll")) == "C:\\users\\steamuser\\My Mods\\ä ö.dll", "inside C:");
            check(LinuxHost.WindowsPath(drive + "x/file") == "Z:" + (drive + "x/file").Replace('/', '\\'), "a sibling folder with the same prefix is not inside C:");
            check(LinuxHost.WindowsPath("/etc/hostname") == "Z:\\etc\\hostname", "outside the prefix");
            check(LinuxHost.WindowsPath(Path.Combine(drive, "a", "..", "..", "..", "etc")) .StartsWith("Z:"), "a path that climbs out of C: is Z:");
            Environment.SetEnvironmentVariable("STEAM_COMPAT_DATA_PATH", null);
            check(LinuxHost.WindowsPath("/home/x") == "Z:\\home\\x", "no prefix known");
        });

        test("A Windows path the session runner wrote comes back as the Linux path, and the two directions agree", () =>
        {
            var prefix = Directory.CreateDirectory(Path.Combine(root, "lh-prefix2")).FullName;
            Environment.SetEnvironmentVariable("STEAM_COMPAT_DATA_PATH", prefix);
            var drive = Path.Combine(prefix, "pfx", "drive_c");
            check(LinuxHost.NativePath("C:\\users\\steamuser\\HellForge\\logs") == Path.Combine(drive, "users/steamuser/HellForge/logs"), "C:");
            check(LinuxHost.NativePath("c:\\x") == Path.Combine(drive, "x"), "lower-case drive letter");
            check(LinuxHost.NativePath("Z:\\etc\\hostname") == "/etc/hostname", "Z:");
            check(LinuxHost.NativePath("/already/linux") == "/already/linux" && LinuxHost.NativePath("") == "" && LinuxHost.NativePath("C:") == "C:", "other text unchanged");
            check(LinuxHost.NativePath("D:\\other") == "D:\\other", "an unknown drive is left alone");
            foreach (var path in new[] { Path.Combine(drive, "users", "a b", "ü"), "/etc/hostname", Path.Combine(drive, "x") })
                check(LinuxHost.NativePath(LinuxHost.WindowsPath(path)) == path, "round trip " + path);
            Environment.SetEnvironmentVariable("STEAM_COMPAT_DATA_PATH", null);
        });

        test("Starting a Windows program through Proton fails clearly without Proton, and builds proton's waiting verb with it", () =>
        {
            Environment.SetEnvironmentVariable("PROTON", null);
            Refuses(() => LinuxHost.Windows("C:\\a.exe", []));
            Environment.SetEnvironmentVariable("PROTON", "/nonexistent/proton");
            Refuses(() => LinuxHost.Windows("C:\\a.exe", []));
            var fake = Path.Combine(root, "lh-proton");
            File.WriteAllText(fake, "#!/bin/sh\n");
            Environment.SetEnvironmentVariable("PROTON", fake);
            var start = LinuxHost.Windows("C:\\a b.exe", ["x y", "--z"], root);
            check(start.FileName == fake && start.ArgumentList.SequenceEqual(["waitforexitandrun", "C:\\a b.exe", "x y", "--z"]) && start.WorkingDirectory == root,
                "arguments stay whole: " + string.Join("|", start.ArgumentList));
            Environment.SetEnvironmentVariable("PROTON", null);
        });

        test("The Steam folder is found from the environment or the usual places, and a folder without steamapps is skipped", () =>
        {
            var steam = Directory.CreateDirectory(Path.Combine(root, "lh-steam", "steamapps")).Parent!.FullName;
            Environment.SetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam);
            check(LinuxHost.SteamRoot() == steam, "from the environment");
            Environment.SetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH", Path.Combine(root, "no-such-steam"));
            check(LinuxHost.SteamRoot() != Path.Combine(root, "no-such-steam"), "a missing folder is skipped");
            Environment.SetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH", null);
        });

        test("A native install is found from HELLFORGE_INSTALL, also from a folder inside it, and not from a plain folder", () =>
        {
            var install = Directory.CreateDirectory(Path.Combine(root, "lh-install")).FullName;
            File.WriteAllText(Path.Combine(install, InstallLayout.MarkerFile), "{}");
            Directory.CreateDirectory(Path.Combine(install, "app"));
            Environment.SetEnvironmentVariable("HELLFORGE_INSTALL", Path.Combine(install, "app"));
            check(LinuxHost.FindInstall()?.Root == install, "found from a folder inside it");
            Environment.SetEnvironmentVariable("HELLFORGE_INSTALL", Directory.CreateDirectory(Path.Combine(root, "lh-plain")).FullName);
            check(LinuxHost.FindInstall()?.Root != Path.Combine(root, "lh-plain"), "a plain folder is not an installation");
            Environment.SetEnvironmentVariable("HELLFORGE_INSTALL", null);
        });

        test("A session outside the network namespace is refused on Linux, whatever the installation says", () =>
        {
            var install = Directory.CreateDirectory(Path.Combine(root, "lh-guard")).FullName;
            Environment.SetEnvironmentVariable(LinuxNetworkBlock.EnvVar, null);
            Refuses(() => LinuxNetworkBlock.GuardSession(install));
        });
    }
}
