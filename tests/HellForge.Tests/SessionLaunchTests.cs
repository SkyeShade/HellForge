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
                """{"templates": {"live-like": {"description": "Like live", "items": []}, "starter": {"description": "New player", "items": []}, "unlock-all": {"description": "Everything", "items": [1, 2]}}}""");
            return new InstallSessionPaths(install);
        }

        test("Sessions start from the launcher's saved choices: live war, multiplayer, engine log and no crash dump unless changed", () =>
        {
            var paths = Install("launch-settings");
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarLive && ProfileStore.LoadLocalPlayFab(paths) && ProfileStore.LoadSdkShims(paths) &&
                ProfileStore.LoadEngineLog(paths) && !ProfileStore.LoadCrashDump(paths) && ProfileStore.LoadLaunchOptions(paths) == "",
                "Defaults without a settings file.");
            check(ProfileStore.LoadSteamUdp(paths), "Co-op over Steam relays is on without a settings file.");
            ProfileStore.SaveSteamUdp(paths, true);
            ProfileStore.SaveCrashDump(paths, true);
            check(ProfileStore.LoadSteamUdp(paths) && ProfileStore.LoadCrashDump(paths), "The co-op switch round-trips and another save keeps it.");
            ProfileStore.SaveSteamUdp(paths, false);
            check(!ProfileStore.LoadSteamUdp(paths), "The co-op switch turns off again.");
            check(File.ReadAllText(paths.SettingsFile).Contains("\"coopRelays\": false", StringComparison.Ordinal), "The switch is saved as coopRelays.");
            // the launchers before wrote "steamUdp": false for everyone who never chose: that key is not read, so co-op is on
            File.WriteAllText(paths.SettingsFile, """{"selectedProfile": "default", "steamUdp": false}""");
            check(ProfileStore.LoadSteamUdp(paths), "An older settings file's steamUdp: false is not read: co-op over Steam relays is on.");
            File.WriteAllText(paths.SettingsFile, """{"selectedProfile": "default", "steamUdp": true, "coopRelays": false}""");
            check(!ProfileStore.LoadSteamUdp(paths), "The player's own choice (coopRelays) is kept.");
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

        test("A profile an older launcher named with capitals is listed under its lowercase name, still selected (#260)", () =>
        {
            var paths = Install("launch-profile-case");
            _ = ProfileStore.List(paths);
            var old = Path.Combine(paths.ProfilesRoot, "Avaleine");
            Directory.CreateDirectory(old);
            File.WriteAllText(Path.Combine(old, "launcher.json"), """{"template": "starter"}""");
            File.WriteAllText(Path.Combine(old, "profile.json"), "{}");
            ProfileStore.SaveSelected(paths, "Avaleine");
            check(ProfileStore.List(paths).Select(p => p.Name).SequenceEqual(["avaleine", "default"]), "Listed as avaleine.");
            check(Directory.GetDirectories(paths.ProfilesRoot).Select(Path.GetFileName).Contains("avaleine") &&
                File.Exists(Path.Combine(paths.ProfilesRoot, "avaleine", "profile.json")), "The folder is renamed with its progress.");
            check(ProfileStore.LoadSelected(paths) == "avaleine", "The selection follows the rename.");
            check(ProfileStore.Create(paths, "avaleine", "starter") is { } taken && taken.Contains("already exists", StringComparison.Ordinal),
                "Its name is now plainly taken.");
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
            start.Environment["HELLFORGE_STEAM_UDP_PROBLEM"] = "inherited";
            SessionRunner.ConfigureLinuxSteamUdp(start, options);
            check(!start.Environment.ContainsKey("HELLFORGE_STEAM_UDP"), "An inherited switch reached the wrapper with the setting off.");
            check(!start.Environment.ContainsKey("HELLFORGE_STEAM_UDP_PROBLEM"), "An inherited problem reached the wrapper (only hellforge-offline.sh sets it).");
            SessionRunner.ConfigureLinuxSteamUdp(start, options with { SteamUdp = true });
            check(start.Environment["HELLFORGE_STEAM_UDP"] == "1", "The wrapper was not given HELLFORGE_STEAM_UDP=1.");
        });

        test("Co-op over Steam relays names what is missing (pasta or python3) with the package-manager command", () =>
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
            check(launch.Contains("(\"--steam-udp\", false, ProfileStore.SaveSteamUdp)", StringComparison.Ordinal) &&
                launch.Contains("if (options.ContainsKey(option)) save(paths, Yes(options, option, bare));", StringComparison.Ordinal),
                "settings does not validate and save --steam-udp.");
        });

        test("A co-op session without pasta starts anyway (the wrapper plays it without co-op), with its network keeper's file", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux wrapper)"); return; }
            var paths = Install(Path.Combine("co-op-missing", "pfx", "drive_c", "HellForge"));
            var wrapper = Path.Combine(paths.Install.App, "linux", "hellforge-linux.sh");
            var seen = Path.Combine(paths.Install.Root, "wrapper-environment");
            Directory.CreateDirectory(Path.GetDirectoryName(wrapper)!);
            // bash's own declare: the folder on PATH holds bash and nothing else
            File.WriteAllText(wrapper, $"#!/usr/bin/env bash\ndeclare -px > '{seen}'\nexit 0\n");
            Directory.CreateDirectory(Path.GetDirectoryName(paths.Install.NetworkBlockMarker)!);
            File.WriteAllText(paths.Install.NetworkBlockMarker, """{"emulated": "linux-netns"}""");
            var profile = ProfileStore.List(paths)[0];
            var bashOnly = Directory.CreateDirectory(Path.Combine(root, "bash-only")).FullName;
            var bash = new[] { "/usr/bin/bash", "/bin/bash" }.First(File.Exists);
            if (!File.Exists(Path.Combine(bashOnly, "bash"))) File.CreateSymbolicLink(Path.Combine(bashOnly, "bash"), bash);
            var previousPath = Environment.GetEnvironmentVariable("PATH");
            var runner = new SessionRunner();
            try
            {
                Environment.SetEnvironmentVariable("PATH", bashOnly);
                check(SessionRunner.SteamUdpProblem() != null, "This PATH has no pasta.");
                runner.Start(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, [], SteamUdp: true));
            }
            finally { Environment.SetEnvironmentVariable("PATH", previousPath); }
            var waited = System.Diagnostics.Stopwatch.StartNew();
            while (runner.IsRunning && waited.Elapsed < TimeSpan.FromSeconds(10)) Thread.Sleep(50);
            var environment = File.Exists(seen) ? File.ReadAllText(seen) : "";
            check(environment.Contains("HELLFORGE_STEAM_UDP=\"1\"", StringComparison.Ordinal),
                "The wrapper gets the switch and decides (hellforge-offline.sh plays without co-op when it cannot open): " + environment.Length);
            check(environment.Contains($"HELLFORGE_NETNS_KEEPER=\"{SessionRunner.NetnsKeeperFile(paths.Install.Root)}\"", StringComparison.Ordinal) &&
                SessionRunner.NetnsKeeperFile(paths.Install.Root) == Path.Combine(paths.Install.Root, "temp", "linux-netns-keeper"),
                "The wrapper is told where the session's network keeper goes.");
        });

        test("The Linux wrapper runs the Windows .NET programs under Wine with NLS globalization, not Proton's ICU", () =>
        {
            var wrapper = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "linux", "hellforge-linux.sh"));
            var proton = wrapper.IndexOf("\nexport PROTON\n", StringComparison.Ordinal);
            var nls = proton < 0 ? -1 : wrapper.IndexOf("\nexport DOTNET_SYSTEM_GLOBALIZATION_USENLS=1\n", proton, StringComparison.Ordinal);
            var space = nls < 0 ? -1 : wrapper.IndexOf("Entering the offline network namespace.", nls, StringComparison.Ordinal);
            check(proton >= 0 && nls > proton && space > nls,
                "Every Wine session must get DOTNET_SYSTEM_GLOBALIZATION_USENLS=1 (a top-level export with Proton's) before the namespace.");
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
                check(!SessionRunner.CanSandbox(paths) && !SessionRunner.SandboxByDefault(paths),
                    "On Linux a Steam bridge alone is not enough: the sandbox needs hf-sandbox and a current passing self-test.");
            else
                check(SessionRunner.CanSandbox(paths) && SessionRunner.SandboxByDefault(paths), "With the bridge, a session is sandboxed by default.");
            ProfileStore.SaveSandbox(paths, false);
            check(!SessionRunner.SandboxByDefault(paths), "Unless turned off.");
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
    }
}
