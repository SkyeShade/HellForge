using HellForge.Core;

// Starting a session the way HellForge.exe's PLAY does, for HellForge.Cli play and other launchers (docs/external-launchers.md).
static class SessionLaunchTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        InstallSessionPaths Install(string name)
        {
            var install = new InstallLayout(Path.Combine(root, name));
            install.Create(SupportedBuild.Current.BuildId);
            Directory.CreateDirectory(install.App);
            File.WriteAllText(Path.Combine(install.App, "profile-templates.json"),
                """{"templates": {"starter": {"description": "New player", "items": []}, "unlock-all": {"description": "Everything", "items": [1, 2]}}}""");
            return new InstallSessionPaths(install);
        }

        test("Sessions start from the launcher's saved choices: live war, multiplayer, engine log and no crash dump unless changed", () =>
        {
            var paths = Install("launch-settings");
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarLive && ProfileStore.LoadLocalPlayFab(paths) && ProfileStore.LoadSdkShims(paths) &&
                ProfileStore.LoadEngineLog(paths) && !ProfileStore.LoadCrashDump(paths) && ProfileStore.LoadLaunchOptions(paths) == "",
                "Defaults without a settings file.");
            check(!ProfileStore.LoadSteamUdp(paths), "Co-op over Steam relays is off without a settings file.");
            ProfileStore.SaveSteamUdp(paths, true);
            ProfileStore.SaveCrashDump(paths, true);
            check(ProfileStore.LoadSteamUdp(paths) && ProfileStore.LoadCrashDump(paths), "The co-op switch round-trips and another save keeps it.");
            ProfileStore.SaveSteamUdp(paths, false);
            check(!ProfileStore.LoadSteamUdp(paths), "The co-op switch turns off again.");
            ProfileStore.SaveWar(paths, ProfileStore.WarOpen);
            ProfileStore.SaveCrashDump(paths, true);
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarOpen && ProfileStore.LoadCrashDump(paths), "Saved choices are read back.");
            File.WriteAllText(paths.SettingsFile, """{"selectedProfile": "default", "liveWar": false}""");
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarFrozen, "A settings file from before the war choice keeps its live-war switch.");
        });

        test("A fresh installation gets a default profile; profiles are made only from listed templates and plain names", () =>
        {
            var paths = Install("launch-profiles");
            var profiles = ProfileStore.List(paths);
            check(profiles.Count == 1 && profiles[0].Name == ProfileStore.DefaultName && profiles[0].Template == ProfileStore.FirstProfileTemplate,
                "The first profile uses the development template.");
            check(ProfileStore.Create(paths, "friends", "starter") == null, "A new profile.");
            check(ProfileStore.Create(paths, "friends", "starter") != null, "The same name twice.");
            check(ProfileStore.Create(paths, "Bad Name", "starter") != null && ProfileStore.Create(paths, "x", "two words") != null, "Plain names only.");
            check(ProfileStore.List(paths).Select(p => p.Name).SequenceEqual(["default", "friends"]), "Both listed, by name.");
        });

        test("The session runner gets exactly the launcher's command line for each choice", () =>
        {
            var paths = Install("launch-arguments");
            var profile = ProfileStore.List(paths)[0];
            var live = SessionRunner.Arguments(paths, profile, new(true, true, ProfileStore.WarLive, true, false, []));
            check(live.SequenceEqual(["play", "--install", paths.Install.Root, "--profile", profile.Folder, "--template", profile.Template,
                "--local-playfab", "--multiplayer", "--live-war", paths.LiveWarCache, "--engine-log"]), "Live war: " + string.Join(" ", live));
            var open = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarOpen, false, true, GameLaunchOptions.Parse("-windowed")));
            check(open.SequenceEqual(["play", "--install", paths.Install.Root, "--profile", profile.Folder, "--template", profile.Template,
                "--open-war", "--game-options", "-windowed"]), "Open war, launch options: " + string.Join(" ", open));
            var frozen = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, []));
            check(!frozen.Contains("--live-war") && !frozen.Contains("--open-war"), "The frozen war needs nothing.");
            var sandboxed = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, [], Sandbox: true));
            check(sandboxed[^1] == "--sandbox" && !frozen.Contains("--sandbox"), "Sandboxed only when chosen: " + string.Join(" ", sandboxed));
            // the all-planets war's deals: on unless turned off (and then said), each kept through the other's and the seed's saves
            check(ProfileStore.LoadDealSubfactions(paths) && ProfileStore.LoadDealSeaf(paths), "The all-planets war deals subfactions and SEAF allies by default.");
            ProfileStore.SaveDealSeaf(paths, false);
            ProfileStore.SaveSubfactionSeed(paths, 77);
            check(ProfileStore.LoadDealSubfactions(paths) && !ProfileStore.LoadDealSeaf(paths) && ProfileStore.LoadSubfactionSeed(paths) == 77,
                "Turning one deal off keeps the other and the seed.");
            var dealt = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarOpen, false, false, []));
            check(dealt.Contains("--war-seed") && dealt[dealt.IndexOf("--war-seed") + 1] == "77" && dealt.Contains("--war-seaf") &&
                dealt[dealt.IndexOf("--war-seaf") + 1] == "no" && !dealt.Contains("--war-subfactions"), "Only what differs from the default is passed: " + string.Join(" ", dealt));
        });

        test("SessionRunner gives the wrapper HELLFORGE_STEAM_UDP only when co-op over Steam relays is on", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux wrapper environment)"); return; }
            var options = new SessionOptions(false, false, ProfileStore.WarFrozen, false, false, []);
            var start = new System.Diagnostics.ProcessStartInfo();
            start.Environment["HELLFORGE_STEAM_UDP"] = "1";
            SessionRunner.ConfigureLinuxSteamUdp(start, options);
            check(!start.Environment.ContainsKey("HELLFORGE_STEAM_UDP"), "An inherited switch reached the wrapper with the setting off.");
            SessionRunner.ConfigureLinuxSteamUdp(start, options with { SteamUdp = true });
            check(start.Environment["HELLFORGE_STEAM_UDP"] == "1", "The wrapper was not given HELLFORGE_STEAM_UDP=1.");
        });

        test("Co-op over Steam relays refuses to start without pasta or python3 and names the package-manager command", () =>
        {
            foreach (var (manager, command) in new[]
                     {
                         ("pacman", "sudo pacman -S --needed passt"), ("apt-get", "sudo apt install passt"),
                         ("dnf", "sudo dnf install passt"), ("zypper", "sudo zypper install passt")
                     })
            {
                var problem = SessionRunner.SteamUdpProblem(program => program == manager || program == "python3");
                check(problem != null && problem.Contains("pasta (the passt package)", StringComparison.Ordinal) &&
                    problem.Contains(command, StringComparison.Ordinal), manager + ": " + problem);
            }
            var python = SessionRunner.SteamUdpProblem(program => program is "pasta" or "dnf");
            check(python != null && python.Contains("python3", StringComparison.Ordinal) &&
                python.Contains("sudo dnf install python3", StringComparison.Ordinal), "python3: " + python);
            check(SessionRunner.SteamUdpProblem(program => program is "pasta" or "python3") == null, "Both programs present was refused.");
        });

        test("The CLI accepts --steam-udp for play and settings, with play defaulting to the saved setting", () =>
        {
            var source = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../src"));
            var program = File.ReadAllText(Path.Combine(source, "HellForge.Cli", "Program.cs"));
            var launch = File.ReadAllText(Path.Combine(source, "HellForge.Cli", "Launch.cs"));
            var playOptions = System.Text.RegularExpressions.Regex.Match(program, "\"play\" => \\[(.*?)\\]").Groups[1].Value;
            var settingsOptions = System.Text.RegularExpressions.Regex.Match(program, "\"settings\" => \\[(.*?)\\]").Groups[1].Value;
            check(playOptions.Contains("\"--steam-udp\"", StringComparison.Ordinal) &&
                settingsOptions.Contains("\"--steam-udp\"", StringComparison.Ordinal),
                "The CLI command lines do not accept --steam-udp for play and settings.");
            check(launch.Contains("SteamUdp: Yes(options, \"--steam-udp\", ProfileStore.LoadSteamUdp(paths))", StringComparison.Ordinal),
                "play does not validate --steam-udp or default it to the saved setting.");
            check(launch.Contains("if (options.ContainsKey(\"--steam-udp\")) ProfileStore.SaveSteamUdp(paths, Yes(options, \"--steam-udp\", false));",
                    StringComparison.Ordinal), "settings does not validate and save --steam-udp.");
        });

        test("A co-op session refuses before it starts when pasta is missing", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux wrapper)"); return; }
            var paths = Install(Path.Combine("co-op-missing", "pfx", "drive_c", "HellForge"));
            var wrapper = Path.Combine(paths.Install.App, "linux", "hellforge-linux.sh");
            Directory.CreateDirectory(Path.GetDirectoryName(wrapper)!);
            File.WriteAllText(wrapper, "#!/usr/bin/env bash\nexit 0\n");
            Directory.CreateDirectory(Path.GetDirectoryName(paths.Install.NetworkBlockMarker)!);
            File.WriteAllText(paths.Install.NetworkBlockMarker, """{"emulated": "linux-netns"}""");
            var profile = ProfileStore.List(paths)[0];
            var emptyPath = Directory.CreateDirectory(Path.Combine(root, "no-programs")).FullName;
            var previousPath = Environment.GetEnvironmentVariable("PATH");
            try
            {
                Environment.SetEnvironmentVariable("PATH", emptyPath);
                try
                {
                    new SessionRunner().Start(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, [], SteamUdp: true));
                    check(false, "A session without pasta was started.");
                }
                catch (InvalidOperationException error)
                {
                    check(error.Message.Contains("pasta", StringComparison.Ordinal) && error.Message.Contains("python3", StringComparison.Ordinal),
                        "The refusal does not name what is missing: " + error.Message);
                }
            }
            finally { Environment.SetEnvironmentVariable("PATH", previousPath); }
        });

        test("The sandbox is on unless turned off, and offered only by an engine built with the Steam bridge on Windows", () =>
        {
            var paths = Install("launch-sandbox");
            check(ProfileStore.LoadSandbox(paths), "On without a settings file.");
            ProfileStore.SaveCrashDump(paths, true);
            check(ProfileStore.LoadSandbox(paths), "Saving another choice keeps it on.");
            ProfileStore.SaveSandbox(paths, false);
            ProfileStore.SaveWar(paths, ProfileStore.WarOpen);
            check(!ProfileStore.LoadSandbox(paths) && ProfileStore.LoadCrashDump(paths), "Turned off, it stays off through other choices.");
            ProfileStore.SaveSandbox(paths, true);
            check(ProfileStore.LoadSandbox(paths), "Switched on again.");
            // an earlier beta's file says "sandbox": false for everyone who never chose: not read, so it starts sandboxed once
            File.WriteAllText(paths.SettingsFile, """{"selectedProfile": "default", "war": "frozen", "sandbox": false}""");
            check(ProfileStore.LoadSandbox(paths), "An earlier beta's \"sandbox\": false is not read.");
            var shims = Directory.CreateDirectory(Path.Combine(paths.Install.App, "sdk-shims")).FullName;
            check(!SessionRunner.HasSteamBridge(paths), "No stand-ins' build.json: no bridge.");
            File.WriteAllText(Path.Combine(shims, "build.json"), """{"standIns": {}}""");
            check(!SessionRunner.HasSteamBridge(paths), "Built without the Steamworks SDK: no bridge.");
            check(ProfileStore.LoadSandbox(paths) && !SessionRunner.SandboxByDefault(paths), "On, but a session is not sandboxed without the bridge.");
            File.WriteAllText(Path.Combine(shims, "build.json"), """{"standIns": {}, "steamBridge": "AB12"}""");
            check(SessionRunner.HasSteamBridge(paths), "Built with it.");
            if (OperatingSystem.IsLinux())
                check(!SessionRunner.CanSandbox(paths) && !SessionRunner.SandboxByDefault(paths), "On Linux, the sandbox is Windows-only.");
            else
                check(SessionRunner.CanSandbox(paths) && SessionRunner.SandboxByDefault(paths), "With the bridge, a session is sandboxed by default.");
            ProfileStore.SaveSandbox(paths, false);
            check(!SessionRunner.SandboxByDefault(paths), "Unless turned off.");
        });

        test("An explicit sandbox request is refused on Linux, before the Windows low-integrity path", () =>
        {
            check(SessionRunner.SandboxRefusal(true, linux: true, underWine: false) == SessionRunner.LinuxSandboxRefusal,
                "Native Linux did not refuse the sandbox.");
            check(SessionRunner.SandboxRefusal(true, linux: false, underWine: true) == SessionRunner.LinuxSandboxRefusal,
                "A Windows build under Wine did not refuse the sandbox.");
            check(SessionRunner.SandboxRefusal(true, linux: false, underWine: false) == null,
                "Windows lost its existing sandbox path.");
            check(SessionRunner.SandboxRefusal(false, linux: true, underWine: false) == null,
                "A Linux session that did not ask for the sandbox was refused.");

            var source = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../src"));
            var launch = File.ReadAllText(Path.Combine(source, "HellForge.Cli", "Launch.cs"));
            var playSession = File.ReadAllText(Path.Combine(source, "HellForge.Session", "PlaySession.cs"));
            check(launch.Contains("SessionRunner.RefuseSandboxOnLinux(requestedSandbox == true);", StringComparison.Ordinal),
                "The CLI parses --sandbox but does not refuse an explicit yes on Linux.");
            check(playSession.Contains("SessionRunner.RefuseSandboxOnLinux(options.Sandbox);", StringComparison.Ordinal),
                "PlaySession does not refuse --sandbox on Linux before the Windows code.");

            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux refusal)"); return; }
            var paths = Install("launch-sandbox-refusal");
            var profile = ProfileStore.List(paths)[0];
            try
            {
                new SessionRunner().Start(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, [], Sandbox: true));
                check(false, "SessionRunner started a sandboxed session on Linux.");
            }
            catch (InvalidDataException error)
            {
                check(error.Message == SessionRunner.LinuxSandboxRefusal, "The refusal was not the clear sandbox message: " + error.Message);
            }
        });

        test("A saved-on sandbox is announced as unavailable on Linux without changing the setting", () =>
        {
            var paths = Install("launch-sandbox-notice");
            check(ProfileStore.LoadSandbox(paths), "The saved setting is not on by default.");
            ProfileStore.SaveSandbox(paths, true);
            if (OperatingSystem.IsLinux())
                check(ProfileStore.LoadSandbox(paths) && !SessionRunner.SandboxByDefault(paths),
                    "Linux did not keep the setting on while leaving this session unsandboxed.");

            var source = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../src"));
            var launcher = File.ReadAllText(Path.Combine(source, "HellForge.Launcher", "MainWindow.axaml.cs"));
            var playSession = File.ReadAllText(Path.Combine(source, "HellForge.Session", "PlaySession.cs"));
            check(launcher.Contains("if (OperatingSystem.IsLinux() && ProfileStore.LoadSandbox(paths))", StringComparison.Ordinal) &&
                launcher.Contains("Append(SessionRunner.LinuxNotSandboxed, LauncherLine);", StringComparison.Ordinal),
                "The launcher console does not announce the Linux sandbox's absence.");
            check(playSession.Contains("var linuxSandboxUnavailable = !options.Sandbox && SessionRunner.IsLinuxHost", StringComparison.Ordinal) &&
                playSession.Contains("facts.Lines(linuxSandboxUnavailable)", StringComparison.Ordinal),
                "The session log does not get the Linux sandbox note.");
        });

        test("A session's run folder is the one the session runner named after it started, not an earlier one", () =>
        {
            var paths = Install("launch-run");
            check(SessionRunner.RunSince(paths, null) == null && SessionRunner.LatestRun(paths) == null, "No run yet.");
            var earlier = Path.Combine(paths.RunsFolder, "earlier");
            Directory.CreateDirectory(paths.Install.Logs);
            File.WriteAllText(paths.SessionFile, $$"""{"runId": "earlier", "run": {{System.Text.Json.JsonSerializer.Serialize(earlier)}}}""");
            check(SessionRunner.RunSince(paths, "earlier") == null && SessionRunner.LatestRun(paths) == earlier, "An earlier run is not this session's.");
            var run = Path.Combine(paths.RunsFolder, "now");
            File.WriteAllText(paths.SessionFile, $$"""{"runId": "now", "run": {{System.Text.Json.JsonSerializer.Serialize(run)}}, "startedUtc": "not a timestamp"}""");
            check(SessionRunner.RunSince(paths, "earlier") == run, "The run ID changed even though its clock timestamp was unusable.");
            check(SessionRunner.RunSince(paths, "now") == null, "The same run ID is not a new run.");
        });

        test("A PC without the Visual C++ 2012 or 2015-2022 x64 runtime is told which to install before the game starts", () =>
        {
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: Windows system runtimes)"); return; }
            var system = Directory.CreateDirectory(Path.Combine(root, "vc-runtimes")).FullName;
            var none = VisualCppRuntimes.Problem(system, "press Play again.");
            check(none != null && none.Contains("2012") && none.Contains("2015-2022") && none.EndsWith("press Play again."), "Both missing: " + none);
            File.WriteAllText(Path.Combine(system, "msvcr110.dll"), "synthetic");
            File.WriteAllText(Path.Combine(system, "vcruntime140.dll"), "synthetic");
            var partial = VisualCppRuntimes.Problem(system, "start setup again.");
            check(partial != null && !partial.Contains("2012") && partial.Contains("2015-2022") && partial.Contains("a Microsoft runtime"),
                "msvcp140.dll missing: " + partial);
            File.WriteAllText(Path.Combine(system, "msvcp140.dll"), "synthetic");
            check(VisualCppRuntimes.Problem(system, "start setup again.") == null, "Every runtime is there.");
            check(File.Exists(Path.Combine(VisualCppRuntimes.SystemFolder, "kernel32.dll")), "Not the 64-bit system folder: " + VisualCppRuntimes.SystemFolder);
        });
    }
}
