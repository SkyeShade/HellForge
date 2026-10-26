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

        test("Sessions start from the launcher's saved choices: live war, multiplayer, engine log, no crash dump and no DSS everywhere unless changed", () =>
        {
            var paths = Install("launch-settings");
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarLive && ProfileStore.LoadLocalPlayFab(paths) && ProfileStore.LoadSdkShims(paths) &&
                ProfileStore.LoadEngineLog(paths) && !ProfileStore.LoadCrashDump(paths) && !ProfileStore.LoadDssEverywhere(paths) &&
                ProfileStore.LoadLaunchOptions(paths) == "", "Defaults without a settings file.");
            ProfileStore.SaveWar(paths, ProfileStore.WarOpen);
            ProfileStore.SaveCrashDump(paths, true);
            ProfileStore.SaveDssEverywhere(paths, true);
            check(ProfileStore.LoadWar(paths) == ProfileStore.WarOpen && ProfileStore.LoadCrashDump(paths) && ProfileStore.LoadDssEverywhere(paths),
                "Saved choices are read back.");
            ProfileStore.SaveWar(paths, ProfileStore.WarFrozen);
            check(ProfileStore.LoadDssEverywhere(paths) && ProfileStore.LoadCrashDump(paths), "Saving one choice keeps the others.");
        });

        test("DSS effects on every planet: every effect is picked until some are unticked, and the session gets them only when on", () =>
        {
            var paths = Install("launch-dss");
            check(ProfileStore.LoadDssEffects(paths).SequenceEqual(DssEffects.Keys) && DssEffects.Keys.Contains(DssEffects.Current), "Every effect by default.");
            ProfileStore.SaveDssEffects(paths, ["orbital-blockade", "eagle-storm"]);
            check(ProfileStore.LoadDssEffects(paths).SequenceEqual(["eagle-storm", "orbital-blockade"]), "The picked ones, in the list's order.");
            ProfileStore.SaveDssEverywhere(paths, true);
            check(ProfileStore.LoadDssEffects(paths).Count == 2, "Switching it on keeps the picks.");
            ProfileStore.SaveDssEffects(paths, []);
            check(ProfileStore.LoadDssEffects(paths).Count == 0, "None picked stays none.");
            File.WriteAllText(paths.SettingsFile, """{"dssEverywhere": true, "dssEffects": ["eagle-storm", "from-a-later-version", 1209]}""");
            check(ProfileStore.LoadDssEverywhere(paths) && ProfileStore.LoadDssEffects(paths).SequenceEqual(["eagle-storm"]), "Unknown keys are left out.");
            check(ProfileStore.DssSessionEffects(false, DssEffects.Keys).Count == 0 && ProfileStore.DssSessionEffects(true, ["eagle-storm"]).SequenceEqual(["eagle-storm"]),
                "The session gets the picks only when on.");
            check(DssEffects.Parse("all").SequenceEqual(DssEffects.Keys) && DssEffects.Parse("").Count == 0 &&
                DssEffects.Parse(" orbital-blockade, eagle-storm,eagle-storm ").SequenceEqual(["eagle-storm", "orbital-blockade"]), "Keys parse in the list's order.");
            var refused = false;
            try { DssEffects.Parse("eagle-storm,nope"); } catch (ArgumentException) { refused = true; }
            check(refused, "An unknown effect is refused.");
            check(DssEffects.Ids(["eagle-storm", "exosuit-reserves-patriot", DssEffects.Current]).SequenceEqual([1425, 1212, 1216, 1261]), "Each effect's ids.");
            check(DssEffects.All.Where(e => e.Key != DssEffects.Current).All(e => e.Ids.Count > 0) &&
                DssEffects.All.SelectMany(e => e.Ids).Distinct().Count() == DssEffects.All.Sum(e => e.Ids.Count), "Every listed effect has ids of its own.");
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
            var live = SessionRunner.Arguments(paths, profile, new(true, true, ProfileStore.WarLive, [], true, false, []));
            check(live.SequenceEqual(["play", "--install", paths.Install.Root, "--profile", profile.Folder, "--template", profile.Template,
                "--local-playfab", "--multiplayer", "--live-war", paths.LiveWarCache, "--engine-log"]), "Live war: " + string.Join(" ", live));
            var open = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarOpen, ["eagle-storm", DssEffects.Current], false, true, GameLaunchOptions.Parse("-windowed")));
            check(open.SequenceEqual(["play", "--install", paths.Install.Root, "--profile", profile.Folder, "--template", profile.Template,
                "--open-war", "--dss-everywhere", "eagle-storm,current", "--game-options", "-windowed"]), "Open war, DSS effects, launch options: " + string.Join(" ", open));
            var frozen = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarFrozen, [], false, false, []));
            check(!frozen.Contains("--live-war") && !frozen.Contains("--open-war") && !frozen.Contains("--dss-everywhere"), "The frozen war needs nothing.");
        });

        test("A session's run folder is the one the session runner named after it started, not an earlier one", () =>
        {
            var paths = Install("launch-run");
            var started = DateTime.UtcNow;
            check(SessionRunner.RunSince(paths, started) == null && SessionRunner.LatestRun(paths) == null, "No run yet.");
            var earlier = Path.Combine(paths.RunsFolder, "earlier");
            Directory.CreateDirectory(paths.Install.Logs);
            File.WriteAllText(paths.SessionFile, $$"""{"runId": "earlier", "run": {{System.Text.Json.JsonSerializer.Serialize(earlier)}}, "startedUtc": "{{started.AddMinutes(-3):o}}"}""");
            check(SessionRunner.RunSince(paths, started) == null && SessionRunner.LatestRun(paths) == earlier, "An earlier run is not this session's.");
            var run = Path.Combine(paths.RunsFolder, "now");
            File.WriteAllText(paths.SessionFile, $$"""{"runId": "now", "run": {{System.Text.Json.JsonSerializer.Serialize(run)}}, "startedUtc": "{{DateTime.UtcNow:o}}"}""");
            check(SessionRunner.RunSince(paths, started) == run, "This session's run.");
        });
    }
}
