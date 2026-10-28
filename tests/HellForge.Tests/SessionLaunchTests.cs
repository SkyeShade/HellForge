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

        test("The sandbox is on unless turned off, and offered only by an engine built with the Steam bridge", () =>
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
            check(SessionRunner.CanSandbox(paths) && SessionRunner.SandboxByDefault(paths), "With the bridge on Windows, a session is sandboxed by default.");
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
