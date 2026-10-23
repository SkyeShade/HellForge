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
            var token = Path.Combine(root, "war-token.txt");
            var server = SessionRunner.Arguments(paths, profile, new(true, false, ProfileStore.WarServer, false, false, [],
                new WarServerOptions("https://War.Example:8443/", null, token)));
            check(server.SequenceEqual(["play", "--install", paths.Install.Root, "--profile", profile.Folder, "--template", profile.Template,
                "--local-playfab", "--war-server", "https://war.example:8443", "--war-server-token", token]), "War server: " + string.Join(" ", server));
            var refused = 0;
            foreach (var missing in new WarServerOptions?[] { null, new("http://war.example", null, null), new("https://war.example/api", null, null) })
                try { SessionRunner.Arguments(paths, profile, new(true, false, ProfileStore.WarServer, false, false, [], missing)); }
                catch (ArgumentException) { refused++; }
            check(refused == 3, "A war server session without an https origin was given a command line.");
        });

        test("A war server is chosen for one session, never saved as the launcher's war", () =>
        {
            var paths = Install("launch-war-server");
            var refused = false;
            try { ProfileStore.SaveWar(paths, ProfileStore.WarServer); }
            catch (ArgumentException) { refused = true; }
            check(refused && ProfileStore.LoadWar(paths) == ProfileStore.WarLive, "A war server was saved as the launcher's war.");
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
