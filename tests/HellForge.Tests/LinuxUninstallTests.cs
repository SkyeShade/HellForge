using HellForge.Core;

// The path and ownership decisions behind Linux uninstall: AppArmor's exact commands, the removed shell installer's paths
// and content checks, Steam's content folders and state patches, the prefix marker and deletion offer, and that removal
// refuses links and keeps going.
static class LinuxUninstallTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux uninstall gives plain sudo commands for exactly the AppArmor paths that exist", () =>
        {
            check(LinuxUninstall.AppArmorPaths(false, false).Count == 0, "No paths expected.");
            check(LinuxUninstall.AppArmorPaths(true, false).SequenceEqual([LinuxUninstall.AppArmorProfile]), "Profile path missing.");
            check(LinuxUninstall.AppArmorPaths(false, true).SequenceEqual([LinuxUninstall.AppArmorUnshare]), "Unshare path missing.");
            check(LinuxUninstall.AppArmorPaths(true, true).SequenceEqual([LinuxUninstall.AppArmorProfile, LinuxUninstall.AppArmorUnshare]),
                "Both paths missing.");
            check(LinuxUninstall.AppArmorCommands(false, false).Count == 0, "Commands for no leftovers.");
            check(LinuxUninstall.AppArmorCommands(true, false).SequenceEqual([
                "sudo apparmor_parser -R /etc/apparmor.d/hellforge-unshare 2>/dev/null || true",
                "sudo rm -f -- /etc/apparmor.d/hellforge-unshare"]), "Profile commands differ.");
            check(LinuxUninstall.AppArmorCommands(false, true).SequenceEqual([
                "sudo rm -f -- /usr/local/lib/hellforge/unshare",
                "sudo rmdir /usr/local/lib/hellforge 2>/dev/null || true"]), "Unshare commands differ.");
            check(LinuxUninstall.AppArmorCommands(true, true).SequenceEqual([
                "sudo apparmor_parser -R /etc/apparmor.d/hellforge-unshare 2>/dev/null || true",
                "sudo rm -f -- /etc/apparmor.d/hellforge-unshare /usr/local/lib/hellforge/unshare",
                "sudo rmdir /usr/local/lib/hellforge 2>/dev/null || true"]), "Both commands differ.");
        });

        test("Linux uninstall finds the removed shell installer's paths under XDG folders", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux uninstall)"); return; }
            var defaults = new LinuxUninstallLocations("/home/me");
            check(LinuxUninstall.OldConfigPath(defaults) == Path.Combine("/home/me/.config", "hellforge-linux", "config"),
                "Default config path.");
            check(LinuxUninstall.OldCommandPath(defaults) == Path.Combine("/home/me/.local/bin", "hellforge-linux"),
                "Default command path.");
            check(LinuxUninstall.OldMenuEntryPath(defaults) == Path.Combine("/home/me/.local/share/applications", "hellforge-linux.desktop"),
                "Default menu path.");
            var custom = new LinuxUninstallLocations("/home/me", "/cfg", "/data");
            check(LinuxUninstall.OldConfigPath(custom) == Path.Combine("/cfg", "hellforge-linux", "config"), "XDG config path.");
            check(LinuxUninstall.OldMenuEntryPath(custom) == Path.Combine("/data/applications", "hellforge-linux.desktop"), "XDG data path.");
        });

        test("The old settings belong to this installation only when their PFX is its prefix", () =>
        {
            var header = LinuxUninstall.OldConfigHeader;
            check(LinuxUninstall.OldConfigBelongs(header + "\nPFX=/srv/prefix\n", "/srv/prefix"), "Bare PFX rejected.");
            check(LinuxUninstall.OldConfigBelongs(header + "\nPFX='/srv/prefix'\n", "/srv/prefix"), "Quoted PFX rejected.");
            check(LinuxUninstall.OldConfigBelongs(header + "\nPFX=$'/srv/prefix'\n", "/srv/prefix"), "ANSI quoted PFX rejected.");
            check(LinuxUninstall.OldConfigBelongs(header + "\nPFX=/srv/with\\ space\n", "/srv/with space"), "Escaped space rejected.");
            check(!LinuxUninstall.OldConfigBelongs(header + "\nPFX=/srv/other\n", "/srv/prefix"), "Another prefix accepted.");
            check(!LinuxUninstall.OldConfigBelongs("PFX=/srv/prefix\n", "/srv/prefix"), "Missing header accepted.");
            check(!LinuxUninstall.OldConfigBelongs(header + "\nPROTON=/srv/proton\n", "/srv/prefix"), "Missing PFX accepted.");
        });

        test("The old command and app-menu entry are recognised by their content and exact Exec path", () =>
        {
            var script = "#!/usr/bin/env bash\n" + LinuxUninstall.OldScriptHeader + "\nset -euo pipefail\n";
            check(LinuxUninstall.OldCommandBelongs(script), "Old script header rejected.");
            check(!LinuxUninstall.OldCommandBelongs("#!/bin/sh\n# a different script\n"), "Other script accepted.");
            var command = "/home/me/.local/bin/hellforge-linux";
            var menu = "[Desktop Entry]\nType=Application\nName=HellForge (Linux)\nExec=\"" + command + "\" run\nTerminal=true\n";
            check(LinuxUninstall.OldMenuEntryBelongs(menu, command), "Old menu entry rejected.");
            check(!LinuxUninstall.OldMenuEntryBelongs(menu.Replace("HellForge (Linux)", "Another app"), command), "Other menu name accepted.");
            check(!LinuxUninstall.OldMenuEntryBelongs(menu.Replace("\" run", "\" status"), command), "Other Exec accepted.");
        });

        test("Steam's uninstall paths cover its four content folders and its state patch names", () =>
        {
            var build = SupportedBuild.Current;
            var steam = Path.Combine(root, "linux-uninstall-steam-paths");
            var folders = SteamDepotDownload.ContentFolders(steam, build);
            check(folders.Count == 4, "Wrong number of content folders.");
            check(folders[0] == Path.Combine(steam, "steamapps", "content", "app_" + build.AppId), "Usual folder.");
            check(folders[1] == Path.Combine(steam, "ubuntu12_32", "steamapps", "content", "app_" + build.AppId), "ubuntu12_32 folder.");
            check(folders[2] == Path.Combine(steam, "ubuntu12_64", "steamapps", "content", "app_" + build.AppId), "ubuntu12_64 folder.");
            check(folders[3] == Path.Combine(steam, "steamrt64", "steamapps", "content", "app_" + build.AppId), "steamrt64 folder.");
            check(SteamDepotDownload.SteamAppsFolders(steam).Count == 4, "Wrong number of steamapps folders.");
            check(LinuxUninstall.StatePatchName(Path.Combine(steam, "state_" + build.AppId + "_1.patch"), build.AppId), "State patch rejected.");
            check(!LinuxUninstall.StatePatchName(Path.Combine(steam, "state_1_" + build.AppId + ".patch"), build.AppId), "Other app accepted.");
            check(LinuxUninstall.Within(steam, Path.Combine(steam, "steamapps", "content")), "Inside Steam rejected.");
            check(!LinuxUninstall.Within(steam, Path.Combine(steam, "..", "outside")), "Outside Steam accepted.");
        });

        test("Linux uninstall plans and removes Steam's download and nearby state files, but not the prefix", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux uninstall)"); return; }
            var sandbox = Path.Combine(root, "linux-uninstall-plan");
            var home = Path.Combine(sandbox, "home");
            var prefix = Path.Combine(home, ".local", "share", "hellforge-prefix");
            var install = new InstallLayout(Path.Combine(prefix, "pfx", "drive_c", "HellForge"));
            install.Create(SupportedBuild.Current.BuildId);
            var locations = new LinuxUninstallLocations(home);
            var build = SupportedBuild.Current;
            var steam = Path.Combine(home, ".local", "share", "Steam");
            var content = SteamDepotDownload.ContentFolder(steam, build);
            foreach (var depot in build.Depots) Directory.CreateDirectory(Path.Combine(content, "depot_" + depot.Id));
            var patch = Path.Combine(content, "state_" + build.AppId + "_1.patch");
            File.WriteAllText(patch, "state");
            var nearby = Path.Combine(Path.GetDirectoryName(content)!, "state_" + build.AppId + "_2.patch");
            File.WriteAllText(nearby, "state");
            var keep = Path.Combine(steam, "steamapps", "common", "keep.txt");
            Directory.CreateDirectory(Path.GetDirectoryName(keep)!);
            File.WriteAllText(keep, "keep");
            var command = LinuxUninstall.OldCommandPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(command)!);
            File.WriteAllText(command, "#!/usr/bin/env bash\n" + LinuxUninstall.OldScriptHeader + "\nset -euo pipefail\n");
            var menu = LinuxUninstall.OldMenuEntryPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(menu)!);
            File.WriteAllText(menu, "[Desktop Entry]\nType=Application\nName=HellForge (Linux)\nExec=\"" + command + "\" run\n");
            var config = LinuxUninstall.OldConfigPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(config)!);
            File.WriteAllText(config, LinuxUninstall.OldConfigHeader + "\nPFX=" + prefix + "\n");

            var plan = LinuxUninstall.Plan(install, build, locations, steam, Path.Combine(sandbox, "uninstall.log"));
            check(plan.Prefix == prefix, "Wrong prefix.");
            check(plan.PrefixDeletion is { Offered: false }, "A prefix without a marker was offered.");
            check(plan.Steam != null && plan.Steam.Depots.Count == build.Depots.Count, "Steam depots not planned.");
            check(plan.Steam!.Patches.Count == 2, "Steam's state patches were not planned.");
            check(plan.Steam.AppFolders.Count == 1, "The empty download folder was not planned.");
            check(plan.OldScript.Count == 3, "Old installer files not planned: " + string.Join(", ", plan.OldScript.Select(item => item.Path)));

            var result = LinuxUninstall.Remove(plan);
            check(result.Failed.Count == 0, "Removal failed: " + string.Join("; ", result.Failed));
            check(!Directory.Exists(content), "Steam's download folder remains.");
            check(!File.Exists(patch) && !File.Exists(nearby) && !File.Exists(config) && !File.Exists(command) && !File.Exists(menu),
                "A planned file remains.");
            check(File.Exists(keep), "A file outside the download was removed.");
            check(Directory.Exists(install.Root) && Directory.Exists(prefix), "The installation or prefix was touched.");
        });

        test("Steam's own state files away from the depot download folder are left alone", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux uninstall)"); return; }
            var sandbox = Path.Combine(root, "linux-uninstall-own-state");
            var home = Path.Combine(sandbox, "home");
            var prefix = Path.Combine(home, ".local", "share", "hellforge-prefix");
            var install = new InstallLayout(Path.Combine(prefix, "pfx", "drive_c", "HellForge"));
            install.Create(SupportedBuild.Current.BuildId);
            var locations = new LinuxUninstallLocations(home);
            var build = SupportedBuild.Current;
            var steam = Path.Combine(home, ".local", "share", "Steam");
            var downloading = Path.Combine(steam, "steamapps", "downloading");
            var content = Path.Combine(steam, "steamapps", "content");
            Directory.CreateDirectory(downloading);
            Directory.CreateDirectory(content);
            var inDownloading = Path.Combine(downloading, "state_" + build.AppId + "_1.patch");
            var inSteamApps = Path.Combine(steam, "steamapps", "state_" + build.AppId + "_2.patch");
            var nextToMissingApp = Path.Combine(content, "state_" + build.AppId + "_3.patch");
            File.WriteAllText(inDownloading, "steam's own state");
            File.WriteAllText(inSteamApps, "steam's own state");
            File.WriteAllText(nextToMissingApp, "steam's own state");

            var plan = LinuxUninstall.Plan(install, build, locations, steam, Path.Combine(sandbox, "uninstall.log"));
            check(plan.Steam != null && plan.Steam.Patches.Count == 0, "Steam's own state files were planned.");
            var result = LinuxUninstall.Remove(plan);
            check(result.Failed.Count == 0, "Removal failed: " + string.Join("; ", result.Failed));
            check(File.Exists(inDownloading) && File.Exists(inSteamApps) && File.Exists(nextToMissingApp),
                "Steam's own state file was removed.");
        });

        test("Old installer files that name another prefix are left alone", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux uninstall)"); return; }
            var sandbox = Path.Combine(root, "linux-uninstall-other-prefix");
            var home = Path.Combine(sandbox, "home");
            var prefix = Path.Combine(home, ".local", "share", "hellforge-prefix");
            var install = new InstallLayout(Path.Combine(prefix, "pfx", "drive_c", "HellForge"));
            install.Create(SupportedBuild.Current.BuildId);
            var locations = new LinuxUninstallLocations(home);
            var command = LinuxUninstall.OldCommandPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(command)!);
            File.WriteAllText(command, "#!/usr/bin/env bash\n" + LinuxUninstall.OldScriptHeader + "\n");
            var menu = LinuxUninstall.OldMenuEntryPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(menu)!);
            File.WriteAllText(menu, "[Desktop Entry]\nName=HellForge (Linux)\nExec=\"" + command + "\" run\n");
            var config = LinuxUninstall.OldConfigPath(locations);
            Directory.CreateDirectory(Path.GetDirectoryName(config)!);
            File.WriteAllText(config, LinuxUninstall.OldConfigHeader + "\nPFX=/some/other-prefix\n");

            var plan = LinuxUninstall.Plan(install, SupportedBuild.Current, locations, null, Path.Combine(sandbox, "uninstall.log"));
            check(plan.OldScript.Count == 0, "Another prefix's files were planned.");
            check(plan.Left.Any(note => note.Contains(config, StringComparison.Ordinal)), "The other prefix's settings were not reported.");
            check(LinuxUninstall.Remove(plan).Failed.Count == 0, "Nothing should have been attempted.");
            check(File.Exists(config) && File.Exists(command) && File.Exists(menu), "Another prefix's old files were removed.");
        });

        test("Removal refuses a linked depot, reports failures and still removes the other paths", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux uninstall)"); return; }
            var sandbox = Path.Combine(root, "linux-uninstall-links");
            var home = Path.Combine(sandbox, "home");
            var prefix = Path.Combine(home, ".local", "share", "hellforge-prefix");
            var install = new InstallLayout(Path.Combine(prefix, "pfx", "drive_c", "HellForge"));
            install.Create(SupportedBuild.Current.BuildId);
            var locations = new LinuxUninstallLocations(home);
            var build = SupportedBuild.Current;
            var steam = Path.Combine(home, ".local", "share", "Steam");
            var content = SteamDepotDownload.ContentFolder(steam, build);
            Directory.CreateDirectory(content);
            var outside = Path.Combine(sandbox, "outside");
            Directory.CreateDirectory(outside);
            File.WriteAllText(Path.Combine(outside, "keep.txt"), "keep");
            var linkedDepot = Path.Combine(content, "depot_" + build.Depots[0].Id);
            Directory.CreateSymbolicLink(linkedDepot, outside);
            var first = Path.Combine(content, "state_" + build.AppId + "_1.patch");
            var second = Path.Combine(content, "state_" + build.AppId + "_2.patch");
            File.WriteAllText(first, "first");
            File.WriteAllText(second, "second");

            var plan = LinuxUninstall.Plan(install, build, locations, steam, Path.Combine(sandbox, "uninstall.log"));
            check(plan.Steam != null && plan.Steam.Depots.Count == 0, "A linked depot was planned.");
            check(plan.Left.Any(note => note.Contains(linkedDepot, StringComparison.Ordinal)), "The linked depot was not reported.");
            check(plan.Steam!.Patches.Count == 2, "The state patches were not planned.");
            File.Delete(first);
            Directory.CreateDirectory(first);
            var result = LinuxUninstall.Remove(plan);
            check(result.Failed.Any(problem => problem.Contains(first, StringComparison.Ordinal)), "The failed patch was not reported.");
            check(result.Failed.Any(problem => problem.Contains(linkedDepot, StringComparison.Ordinal)), "The linked depot was not reported.");
            check(!File.Exists(second) && !Directory.Exists(second), "A later patch was not removed after a failure.");
            check(Directory.Exists(outside) && File.Exists(Path.Combine(outside, "keep.txt")), "A link target was removed.");
            check(Directory.Exists(linkedDepot) && new DirectoryInfo(linkedDepot).LinkTarget != null, "The link itself was removed.");
        });

        test("Linux uninstall writes the prefix marker only for a new or empty prefix", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux prefix marker)"); return; }
            var sandbox = Path.Combine(root, "linux-prefix-marker");
            var created = Path.Combine(sandbox, "created");
            Directory.CreateDirectory(created);
            check(LinuxUninstall.WritePrefixMarker(created, existed: false, empty: true), "A new prefix was not marked.");
            check(File.ReadAllText(LinuxUninstall.PrefixMarkerPath(created)).Trim() == LinuxUninstall.PrefixMarkerText(created),
                "The marker does not hold the prefix's full path.");
            var shared = Path.Combine(sandbox, "shared");
            Directory.CreateDirectory(shared);
            File.WriteAllText(Path.Combine(shared, "keep.txt"), "keep");
            check(!LinuxUninstall.WritePrefixMarker(shared, existed: true, empty: false), "A shared prefix was marked.");
            check(!File.Exists(LinuxUninstall.PrefixMarkerPath(shared)), "A marker was written into a non-empty prefix.");
            var empty = Path.Combine(sandbox, "empty");
            Directory.CreateDirectory(empty);
            check(LinuxUninstall.WritePrefixMarker(empty, existed: true, empty: true), "An empty existing prefix was not marked.");
            check(LinuxUninstall.PrefixWasCreated(false, false) && LinuxUninstall.PrefixWasCreated(true, true) &&
                !LinuxUninstall.PrefixWasCreated(true, false), "The new-or-empty decision is wrong.");
        });

        test("Linux uninstall offers a prefix only with its own marker and the installation inside it", () =>
        {
            var prefix = "/srv/hellforge-prefix";
            var installRoot = Path.Combine(prefix, "pfx", "drive_c", "HellForge");
            var good = new LinuxPrefixObservation(true, true, true, LinuxUninstall.PrefixMarkerText(prefix) + "\n",
                false, false);
            check(LinuxUninstall.DecidePrefixOffer(prefix, installRoot, "/home/me", "/home/me/Steam", good).Offered,
                "A matching marker was not offered.");
            check(!LinuxUninstall.DecidePrefixOffer(prefix, installRoot, "/home/me", "/home/me/Steam",
                good with { PrefixLink = true }).Offered, "A prefix that is itself a link was offered.");
            check(!LinuxUninstall.DecidePrefixOffer(prefix, installRoot, "/home/me", "/home/me/Steam",
                good with { MarkerExists = false }).Offered, "A missing marker was offered.");
            check(!LinuxUninstall.DecidePrefixOffer(prefix, installRoot, "/home/me", "/home/me/Steam",
                good with { MarkerPlainFile = false }).Offered, "A marker that is not a plain file was offered.");
            check(!LinuxUninstall.DecidePrefixOffer(prefix, installRoot, "/home/me", "/home/me/Steam",
                good with { MarkerText = "/srv/other-prefix\n" }).Offered, "Another prefix's marker was offered.");
            check(!LinuxUninstall.DecidePrefixOffer(prefix, "/srv/other/pfx/drive_c/HellForge", "/home/me",
                "/home/me/Steam", good).Offered, "A prefix without the installation was offered.");
            check(LinuxUninstall.PrefixContainsInstall(prefix, installRoot) &&
                !LinuxUninstall.PrefixContainsInstall(prefix, "/srv/elsewhere"),
                "The installation-inside-prefix check is wrong.");
        });

        test("Linux uninstall refuses the home folder, the root, Steam's folder and compatdata", () =>
        {
            var home = "/home/me";
            var steam = Path.Combine(home, ".local", "share", "Steam");
            var forbidden = new[]
            {
                home, "/", steam,
                Path.Combine(steam, "steamapps", "compatdata", "553850"),
                "/games/SteamLibrary/steamapps/compatdata/553850"
            };
            foreach (var prefix in forbidden)
            {
                var installRoot = Path.Combine(prefix, "pfx", "drive_c", "HellForge");
                var observation = new LinuxPrefixObservation(true, true, true,
                    LinuxUninstall.PrefixMarkerText(prefix) + "\n", false, false);
                var offer = LinuxUninstall.DecidePrefixOffer(prefix, installRoot, home, steam, observation);
                check(!offer.Offered, "A forbidden prefix was offered: " + prefix);
            }
        });

        test("Linux uninstall offers a prefix with Proton links inside or a linked parent and refuses a linked prefix or marker", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux prefix links)"); return; }
            var sandbox = Path.Combine(root, "linux-prefix-links");
            var home = Path.Combine(sandbox, "home");
            var locations = new LinuxUninstallLocations(home);

            var proton = Path.Combine(sandbox, "steamapps", "common", "Proton - Experimental", "files", "lib", "wine", "x86_64-windows");
            Directory.CreateDirectory(proton);
            File.WriteAllText(Path.Combine(proton, "msado15.dll"), "proton");
            var protonPrefix = Path.Combine(sandbox, "proton-prefix");
            var protonInstall = new InstallLayout(Path.Combine(protonPrefix, "pfx", "drive_c", "HellForge"));
            protonInstall.Create(SupportedBuild.Current.BuildId);
            LinuxUninstall.WritePrefixMarker(protonPrefix, existed: true, empty: true);
            var ado = Path.Combine(protonPrefix, "pfx", "drive_c", "Program Files", "Common Files", "System", "ADO");
            Directory.CreateDirectory(ado);
            File.CreateSymbolicLink(Path.Combine(ado, "msado15.dll"), Path.Combine(proton, "msado15.dll"));
            var protonOffer = LinuxUninstall.CheckPrefixDeletion(protonInstall, protonPrefix, locations, null);
            check(protonOffer.Offered, "A prefix with an absolute Proton link inside was refused: " + protonOffer.Reason);

            var markerPrefix = Path.Combine(sandbox, "marker-prefix");
            var markerInstall = new InstallLayout(Path.Combine(markerPrefix, "pfx", "drive_c", "HellForge"));
            markerInstall.Create(SupportedBuild.Current.BuildId);
            var markerTarget = Path.Combine(sandbox, "marker-target");
            File.WriteAllText(markerTarget, LinuxUninstall.PrefixMarkerText(markerPrefix) + "\n");
            File.CreateSymbolicLink(LinuxUninstall.PrefixMarkerPath(markerPrefix), markerTarget);
            var markerOffer = LinuxUninstall.CheckPrefixDeletion(markerInstall, markerPrefix, locations, null);
            check(!markerOffer.Offered && markerOffer.Reason.Contains("marker", StringComparison.Ordinal),
                "A symlinked marker was accepted: " + markerOffer.Reason);

            var realPrefix = Path.Combine(sandbox, "real-prefix");
            var linkedPrefix = Path.Combine(sandbox, "linked-prefix");
            var realInstall = new InstallLayout(Path.Combine(realPrefix, "pfx", "drive_c", "HellForge"));
            realInstall.Create(SupportedBuild.Current.BuildId);
            Directory.CreateSymbolicLink(linkedPrefix, realPrefix);
            LinuxUninstall.WritePrefixMarker(linkedPrefix, existed: true, empty: true);
            var linkedInstall = new InstallLayout(Path.Combine(linkedPrefix, "pfx", "drive_c", "HellForge"));
            var linkedOffer = LinuxUninstall.CheckPrefixDeletion(linkedInstall, linkedPrefix, locations, null);
            check(!linkedOffer.Offered && linkedOffer.Reason.Contains("link", StringComparison.Ordinal),
                "A symlinked prefix was accepted: " + linkedOffer.Reason);

            // The test's home is a link to var/home, like Fedora Atomic's /home -> /var/home.
            var realHome = Path.Combine(sandbox, "var", "home");
            var homePrefix = Path.Combine(realHome, "prefix");
            var homeInstall = new InstallLayout(Path.Combine(homePrefix, "pfx", "drive_c", "HellForge"));
            homeInstall.Create(SupportedBuild.Current.BuildId);
            var linkedHome = Path.Combine(sandbox, "home");
            Directory.CreateSymbolicLink(linkedHome, realHome);
            check(LinuxUninstall.PrefixForbidden(realHome, linkedHome, null),
                "The home reached through a link was not refused.");
            var parentPrefix = Path.Combine(linkedHome, "prefix");
            LinuxUninstall.WritePrefixMarker(parentPrefix, existed: true, empty: true);
            var parentInstall = new InstallLayout(Path.Combine(parentPrefix, "pfx", "drive_c", "HellForge"));
            var parentOffer = LinuxUninstall.CheckPrefixDeletion(parentInstall, parentPrefix, locations, null);
            check(parentOffer.Offered, "A prefix under a linked folder was refused: " + parentOffer.Reason);
            check(parentOffer.Prefix == homePrefix, "The offer did not name the resolved prefix: " + parentOffer.Prefix);
        });

        test("Linux uninstall treats WINEPREFIX or a path inside the prefix as a process using it", () =>
        {
            var prefix = "/srv/hellforge-prefix";
            var pfx = Path.Combine(prefix, "pfx");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("wineserver", "/usr/bin/wineserver", [],
                ["WINEPREFIX=" + pfx], null), prefix), "WINEPREFIX was ignored.");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("game", Path.Combine(pfx, "drive_c", "game.exe"),
                [], [], null), prefix), "An executable inside the prefix was ignored.");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("shell", "/usr/bin/bash", [],
                [], Path.Combine(pfx, "drive_c", "users", "steamuser")), prefix), "A working folder inside the prefix was ignored.");
            check(!LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("bash", "/usr/bin/bash", [],
                ["HOME=/home/me"], "/tmp"), prefix), "An unrelated process was treated as using the prefix.");
        });

        test("Linux prefix process check does not resolve relative WINEPREFIX or paths against its own folder", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux processes)"); return; }
            var prefix = Path.Combine(root, "linux-prefix-relative");
            var pfx = Path.Combine(prefix, "pfx");
            Directory.CreateDirectory(pfx);
            var originalDirectory = Environment.CurrentDirectory;
            try
            {
                Directory.SetCurrentDirectory(prefix);
                foreach (var value in new[] { "pfx", "./pfx", "pfx/.", "pfx/drive_c/.." })
                    check(!LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("wineserver", "/usr/bin/wineserver", [],
                        ["WINEPREFIX=" + value], null), prefix), $"A relative WINEPREFIX ({value}) was resolved.");
                check(!LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("game", "pfx/drive_c/game.exe", [], [], null), prefix),
                    "A relative executable was resolved.");
                check(!LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("shell", "/usr/bin/bash", [], [], "pfx/drive_c"), prefix),
                    "A relative working folder was resolved.");
                foreach (var argument in new[] { "bash", "-c", "--no-daemon", ".", "app" })
                    check(!LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("bash", "/usr/bin/bash", [argument], [], "/tmp"), prefix),
                        $"A relative argument ({argument}) was resolved.");
            }
            finally { Directory.SetCurrentDirectory(originalDirectory); }

            var game = Path.Combine(pfx, "drive_c", "game.exe");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("game", game, [], [], null), prefix),
                "An absolute executable inside the prefix was ignored.");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("game", @"Z:" + game.Replace('/', '\\'), [], [], null), prefix),
                "A Z: executable inside the prefix was ignored.");
            check(LinuxInstallProcesses.UsesPrefix(new LinuxProcessInfo("wineserver", "/usr/bin/wineserver", [],
                ["WINEPREFIX=" + @"Z:" + pfx.Replace('/', '\\')], null), prefix), "A Z: WINEPREFIX was ignored.");
        });
    }
}
