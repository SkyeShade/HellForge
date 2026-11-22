using HellForge.Core;

// Playing with a kept version from the installed launcher (KeptVersions.Activate, SessionRuntime): app/'s programs and the
// data swap while app/launcher stays, refusals and roll-back, an update while a kept one is played with, the repair of an
// installation an earlier whole-app switch left with an older app, and the older versions' play command.
static class KeptRuntimesTests
{
    // The options HellForge 0.4.0-beta's and 0.3.0-beta's session runner take (src/HellForge.Session/Program.cs at
    // v0.4.0-beta and v0.3.0-beta: the same loop); anything else makes it print its usage and exit 2.
    static readonly string[] OldValued = ["--install", "--profile", "--template", "--live-war", "--war-seed", "--war-subfactions", "--war-seaf", "--game-options", "--mod-sync"];
    static readonly string[] OldFlags = ["--open-war", "--local-playfab", "--multiplayer", "--engine-log", "--sandbox", "--full-ownership-check"];

    // 0.4.0-beta's parser, as it reads a command line: the options with their values, or null where it would exit 2.
    static Dictionary<string, string>? OldParse(IReadOnlyList<string> args)
    {
        if (args.Count == 0 || args[0] != "play") return null;
        var options = new Dictionary<string, string>();
        for (var i = 1; i < args.Count; i++)
        {
            if (OldValued.Contains(args[i])) { if (i + 1 >= args.Count) return null; options[args[i]] = args[++i]; }
            else if (OldFlags.Contains(args[i])) options[args[i]] = "";
            else return null;
        }
        return options.ContainsKey("--install") ? options : null;
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        static string App(string folder, string version, string commit, int hour, string engine = "0.1.55")
        {
            foreach (var part in new[] { "launcher", "launcher-linux", "cli", "session", "backend", "sdk", BundledMods.FolderName })
                Directory.CreateDirectory(Path.Combine(folder, part));
            File.WriteAllText(Path.Combine(folder, "launcher", "HellForge.Launcher.exe"), "launcher " + commit);
            // the old whole-app switch checks the launcher this system runs
            File.WriteAllText(Path.Combine(folder, "launcher-linux", "HellForge.Launcher"), "linux " + commit);
            File.WriteAllText(Path.Combine(folder, "cli", "HellForge.Cli.exe"), "cli " + commit);
            File.WriteAllText(Path.Combine(folder, "session", "HellForge.Session.exe"), "session " + commit);
            File.WriteAllText(Path.Combine(folder, "backend", "HellForge.LocalBackend.exe"), "backend " + commit);
            File.WriteAllText(Path.Combine(folder, "sdk", "hellforge_mod.h"), $"#define HF_MOD_SDK_VERSION {(engine == "0.1.47" ? 16 : 20)}\n");
            File.WriteAllText(Path.Combine(folder, "profile-templates.json"), engine == "0.1.47"
                ? """{ "templates": { "starter": { "items": [] }, "unlock-all": { "items": [] } } }"""
                : """{ "templates": { "starter": { "items": [] }, "live-like": { "items": [] } } }""");
            File.WriteAllText(Path.Combine(folder, "app-build.json"),
                $$"""{ "builtUtc": "2026-11-22T{{hour:00}}:00:00Z", "version": "{{version}}", "commit": "{{commit}}", "engine": "{{engine}}" }""");
            File.WriteAllText(Path.Combine(folder, "engine.json"), $$"""{ "version": "{{engine}}", "build": "25480438" }""");
            return folder;
        }
        static void Runs(InstallLayout install, string commit)
        {
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "stub " + commit);
            File.WriteAllText(Path.Combine(install.Root, "launcher-settings.json"), "settings " + commit);
            Directory.CreateDirectory(Path.Combine(install.Profiles, "p"));
            File.WriteAllText(Path.Combine(install.Profiles, "p", "profile.json"), "profile " + commit);
            Directory.CreateDirectory(install.Mods);
            File.WriteAllText(Path.Combine(install.Mods, "hellforge-mods.json"), """{ "format": 1, "disabled": [] }""");
        }
        // 0.4.0-beta (its engine) installed and run, then updated to a 0.5 build: 0.4 kept.
        static InstallLayout Updated(string root, string name)
        {
            var install = new InstallLayout(Path.Combine(root, name));
            install.Create("25480438");
            Directory.Delete(install.App);
            App(install.App, "0.4.0-beta", "d8a8b46", 1, engine: "0.1.47");
            Runs(install, "d8a8b46");
            var staged = App(Path.Combine(install.Temp, "setup-staging"), "0.5.0-beta", "bbbbbbb", 2);
            KeptVersions.KeepCurrent(install, staged, _ => 100L << 30);
            Directory.Move(staged, install.App);
            Runs(install, "bbbbbbb");
            return install;
        }
        static string Read(InstallLayout install, string path) => File.ReadAllText(Path.Combine(install.Root, path));
        static string Tree(InstallLayout install) => string.Join("\n", Directory.GetFiles(install.Root, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(install.Root, f)).Where(f => !f.StartsWith("logs", StringComparison.Ordinal)).Order(StringComparer.Ordinal)
            .Select(f => f + "=" + File.ReadAllText(Path.Combine(install.Root, f))));
        Func<IReadOnlyList<string>> Nothing = () => [];
        const string Old = "0.4.0-beta-d8a8b46";

        test("Playing with a kept version swaps its programs and data into app/ while the installed launcher stays, and back", () =>
        {
            var install = Updated(root, "runtime-switch");
            var launcherBefore = Read(install, "app/launcher/HellForge.Launcher.exe");
            var to = KeptVersions.Activate(install, Old, Nothing);
            check(to.From == "0.5.0-beta (bbbbbbb)" && to.To == "0.4.0-beta (d8a8b46)", $"Switched {to.From} -> {to.To}.");
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == launcherBefore && launcherBefore == "launcher bbbbbbb", "The launcher in app/ changed.");
            check(Read(install, "app/session/HellForge.Session.exe") == "session d8a8b46" && Read(install, "app/cli/HellForge.Cli.exe") == "cli d8a8b46" &&
                Read(install, "app/backend/HellForge.LocalBackend.exe") == "backend d8a8b46", "The kept version's programs are not in app/.");
            check(Read(install, "HellForge.exe") == "stub bbbbbbb", "The starter changed: it must keep starting the installed launcher.");
            check(Read(install, "profiles/p/profile.json") == "profile d8a8b46" && Read(install, "launcher-settings.json") == "settings d8a8b46", "The kept version's data is not in place.");
            check(KeptVersions.Active(install).Name == Old && KeptVersions.Installed(install).Name == "0.5.0-beta-bbbbbbb" && KeptVersions.PlaysKept(install),
                "Active " + KeptVersions.Active(install).Name + ", installed " + KeptVersions.Installed(install).Name);
            check(KeptVersions.List(install).Single().Name == Old, "The list changed: " + string.Join(", ", KeptVersions.List(install).Select(k => k.Name)));
            check(SessionRuntime.Of(new InstallSessionPaths(install)) is { Family: "0.4", Sdk: 16, HellForgeWar: false, NeedsCatalogue: false },
                "The session runtime in app/ is not 0.4's.");
            File.WriteAllText(Path.Combine(install.Profiles, "p", "profile.json"), "profile d8a8b46, played");
            var back = KeptVersions.Activate(install, "0.5.0-beta (bbbbbbb)", Nothing);
            check(back.To == "0.5.0-beta (bbbbbbb)" && !KeptVersions.PlaysKept(install), "Not back to the installed version.");
            check(Read(install, "app/session/HellForge.Session.exe") == "session bbbbbbb" && Read(install, "profiles/p/profile.json") == "profile bbbbbbb" &&
                Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb", "The installed version's programs and data are not back.");
            check(Read(install, $"app-versions/{Old}/app/session/HellForge.Session.exe") == "session d8a8b46" &&
                Read(install, $"app-versions/{Old}/data/profiles/p/profile.json") == "profile d8a8b46, played", "The kept version did not keep its played data.");
            check(Read(install, $"app-versions/{Old}/app/launcher/HellForge.Launcher.exe") == "launcher d8a8b46", "The kept version's own launcher went.");
            check(!Directory.EnumerateDirectories(KeptVersions.Root(install), ".*").Any(), "A half folder is left: " + string.Join(", ", Directory.EnumerateDirectories(KeptVersions.Root(install), ".*")));
            check(File.ReadAllText(KeptVersions.LogFile(install)).Contains("Plays with 0.4.0-beta (d8a8b46) now", StringComparison.Ordinal), "No log line for the switch.");
        });

        test("A runtime switch is refused while the game or a program from the installation runs, and changes nothing", () =>
        {
            var install = Updated(root, "runtime-refused");
            var before = Tree(install);
            reject(() => KeptVersions.Activate(install, Old, () => ["HELLDIVERS 2"]));
            reject(() => KeptVersions.Activate(install, Old, () => ["another HellForge window"]));
            reject(() => KeptVersions.Activate(install, "0.2.0-beta-none", Nothing));
            check(Tree(install) == before, "A refused switch changed the installation.");
            KeptVersions.Activate(install, Old, Nothing);
            var playing = Tree(install);
            reject(() => KeptVersions.Activate(install, "0.5.0-beta-bbbbbbb", () => ["a HellForge session"]));
            check(Tree(install) == playing, "A refused switch back changed the installation.");
        });

        test("A failure or a kill at any step of a runtime switch puts everything back", () =>
        {
            var install = Updated(root, "runtime-failure");
            foreach (var (target, label) in new[] { (Old, "to the kept one"), ("0.5.0-beta-bbbbbbb", "back") })
            {
                var before = Tree(install);
                foreach (var failAt in new[] { "app", "data", "record", "kept" })
                {
                    var calls = 0;
                    reject(() => KeptVersions.Activate(install, target, Nothing, step =>
                    {
                        if (step == failAt && ++calls == (failAt == "app" ? 3 : 1)) throw new IOException("disk said no at " + step);
                    }));
                    check(Tree(install) == before, $"Going {label}: a failure at {failAt} left the installation changed.");
                }
                try
                {
                    KeptVersions.Activate(install, target, Nothing, step => { if (step == "data") throw new OperationCanceledException("killed"); });
                    throw new Exception("The kill did not stop the switch.");
                }
                catch (OperationCanceledException) { }
                KeptVersions.Recover(install);
                check(Tree(install) == before, $"Going {label}: Recover did not put a killed switch back.");
                KeptVersions.Activate(install, target, Nothing);
            }
        });

        test("Each version keeps its own mod list across runtime switches; a mod added meanwhile is off in the other's", () =>
        {
            var install = Updated(root, "runtime-mods");
            KeptVersions.Activate(install, Old, Nothing);
            var mod = Path.Combine(install.Mods, "only-in-04");
            Directory.CreateDirectory(Path.Combine(mod, "data"));
            File.WriteAllText(Path.Combine(mod, ModCatalog.ManifestName), """{"format":1,"id":"test.only-04","name":"x","version":"1.0.0","gameplay":false}""");
            var (archive, _, _) = HellForge.Core.GameData.ResourceArchive.Write(new Dictionary<HellForge.Core.GameData.ResourceKey, HellForge.Core.GameData.ResourceParts>
                { [new(HellForge.Core.GameData.ResourceTypes.Texture, 0x1234)] = new([1, 2, 3, 4], [], []) });
            File.WriteAllBytes(Path.Combine(mod, "data", "9ba626afa44a3aa3.patch_0"), archive);
            var back = KeptVersions.Activate(install, "0.5.0-beta-bbbbbbb", Nothing);
            check(back.SwitchedOff.SequenceEqual(["test.only-04"]) && ModSettings.Read(install.Mods).Disabled.Contains("test.only-04"),
                "A mod added while 0.4 played is on in 0.5's list: " + string.Join(",", back.SwitchedOff));
            ModSettings.SetEnabled(install.Mods, "test.only-04", true);
            KeptVersions.Activate(install, Old, Nothing);
            check(!ModSettings.Read(install.Mods).Disabled.Contains("test.only-04"), "0.4's own list (the mod on) did not come back.");
        });

        test("An update while a kept version is played with puts the installed one back first, then keeps it", () =>
        {
            var install = Updated(root, "runtime-update");
            KeptVersions.Activate(install, Old, Nothing);
            var staged = App(Path.Combine(install.Temp, "setup-staging-next"), "0.5.1-beta", "ccccccc", 3);
            var result = KeptVersions.KeepCurrent(install, staged, _ => 100L << 30);
            check(result.Kept && result.Name == "0.5.0-beta-bbbbbbb" && result.Message.Contains("again before the update", StringComparison.Ordinal), "Not kept: " + result.Message);
            check(!KeptVersions.PlaysKept(install) && Read(install, "profiles/p/profile.json") == "profile bbbbbbb", "The update kept the wrong data.");
            check(Read(install, "app-versions/0.5.0-beta-bbbbbbb/app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb" &&
                Read(install, $"app-versions/{Old}/app/session/HellForge.Session.exe") == "session d8a8b46", "The kept apps are not whole.");
        });

        test("An update repairs an installation an earlier whole-app switch left with the older app in app/", () =>
        {
            var install = Updated(root, "runtime-repair");
            // what rc3's switch to 0.4 did: the whole app, launcher and starter, swapped
            KeptVersions.Switch(install, Old, Nothing);
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher d8a8b46", "The old-style switch did not happen.");
            var staged = App(Path.Combine(install.Temp, "setup-staging-rc4"), "0.5.0-beta", "ddddddd", 4);
            var result = KeptVersions.KeepCurrent(install, staged, _ => 100L << 30);
            check(result.Kept && result.Name == "0.5.0-beta-bbbbbbb" && result.Message.Contains("back in app/", StringComparison.Ordinal), "Not repaired: " + result.Message);
            check(Read(install, "profiles/p/profile.json") == "profile bbbbbbb", "The newest version's data did not come back.");
            check(KeptVersions.List(install).Select(k => k.Name).Order().SequenceEqual([Old, "0.5.0-beta-bbbbbbb"]) &&
                Read(install, $"app-versions/{Old}/data/profiles/p/profile.json") == "profile d8a8b46", "0.4 is not kept with its data.");
            Directory.Move(staged, install.App);
            check(KeptVersions.SwitchTargets(install).Select(t => t.Name).Order().SequenceEqual([Old, "0.5.0-beta-bbbbbbb"]), "Setup's switch list is wrong.");
            // Setup's switch in the same state: the newest back, then 0.4 played with
            var repair = Updated(root, "runtime-repair-setup");
            KeptVersions.Switch(repair, Old, Nothing);
            check(KeptVersions.SwitchTargets(repair).Select(t => t.Name).SequenceEqual(["0.5.0-beta-bbbbbbb"]), "The newer app is not offered back.");
            KeptVersions.SwitchRuntime(repair, Old, Nothing);
            check(Read(repair, "app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb" && Read(repair, "app/session/HellForge.Session.exe") == "session d8a8b46" &&
                Read(repair, "profiles/p/profile.json") == "profile d8a8b46", "Setup's switch did not put the newest launcher back with 0.4 played.");
        });

        test("A switch outcome names the version played with; an outcome a later switch made stale is dropped", () =>
        {
            var install = Updated(root, "runtime-outcome");
            var done = KeptVersions.SwitchRuntime(install, Old, Nothing);
            KeptVersions.WriteResult(install, KeptVersions.Outcome(done));
            check(KeptVersions.TakeResult(install) is { Status: "done", To: "0.4.0-beta (d8a8b46)" }, "The outcome was not shown.");
            KeptVersions.WriteResult(install, KeptVersions.Outcome(done));
            KeptVersions.Activate(install, "0.5.0-beta-bbbbbbb", Nothing);
            check(KeptVersions.TakeResult(install) == null && !File.Exists(KeptVersions.ResultFile(install)), "A stale outcome was shown (B1).");
        });

        test("0.4.0-beta's session runner gets a command line its own parser takes; this version's gets everything", () =>
        {
            var install = Updated(root, "runtime-arguments");
            File.WriteAllText(Path.Combine(install.Root, "war-script.json"), "{}");
            var paths = new InstallSessionPaths(install);
            var live = new LocalProfile("p", Path.Combine(install.Profiles, "p"), "live-like");
            var starter = live with { Template = "starter" };
            SessionOptions Options(string war) => new(true, true, war, true, true, GameLaunchOptions.Parse("-windowed"), Sandbox: true, FullOwnershipCheck: true,
                WarServer: "http://localhost:5093");
            var current = SessionRunner.Arguments(paths, live, Options(ProfileStore.WarHellForge));
            check(current.Contains("--hellforge-war") && current.Contains("--war-script") && current.Contains("live-like"), "This version lost options: " + string.Join(" ", current));
            KeptVersions.Activate(install, Old, Nothing);
            var older = new InstallSessionPaths(install);
            var hellforge = SessionRunner.Arguments(older, live, Options(ProfileStore.WarHellForge));
            check(hellforge.SequenceEqual(["play", "--install", install.Root, "--profile", live.Folder, "--local-playfab", "--multiplayer", "--engine-log",
                "--game-options", "-windowed", "--sandbox", "--full-ownership-check"]), "0.4, HellForge war, a template it lacks: " + string.Join(" ", hellforge));
            check(OldParse(hellforge) is { } parsed && !parsed.ContainsKey("--template") && parsed["--install"] == install.Root, "0.4's parser refuses: " + string.Join(" ", hellforge));
            ProfileStore.SaveSubfactionSeed(older, 9);
            ProfileStore.SaveDealSeaf(older, false);
            foreach (var war in new[] { ProfileStore.WarLive, ProfileStore.WarOpen, ProfileStore.WarFrozen })
            {
                var arguments = SessionRunner.Arguments(older, starter, Options(war) with { ModSync = Path.Combine(install.Temp, "mod-sync", "session.json") });
                check(OldParse(arguments) is { } options && options["--template"] == "starter" && options.ContainsKey("--mod-sync") && !arguments.Contains("--war-script"),
                    $"0.4's parser refuses the {war} war: " + string.Join(" ", arguments));
                if (war == ProfileStore.WarOpen) check(arguments.SkipWhile(a => a != "--open-war").Take(5).SequenceEqual(["--open-war", "--war-seed", "9", "--war-seaf", "no"]), "Open war: " + string.Join(" ", arguments));
                if (war == ProfileStore.WarLive) check(arguments.SkipWhile(a => a != "--live-war").Skip(1).First() == paths.LiveWarCache, "Live war: " + string.Join(" ", arguments));
            }
            check(SessionRuntime.FamilyOf(new("0.4.0-beta", "d8a8b46", "0.1.47", "25480438")) == "0.4" && SessionRuntime.FamilyOf(new("0.3.0-beta", "b1aef77", "0.1.41", "25480438")) == "0.3" &&
                SessionRuntime.FamilyOf(new("unknown", "b1aef77", "0.1.41", "25480438")) == "0.3" && SessionRuntime.FamilyOf(new("0.4.0-beta", "82eb444", "0.1.55", "25480438")) == SessionRuntime.Current &&
                SessionRuntime.FamilyOf(new("0.5.0-beta", "889375b", "0.1.55", "25480438")) == SessionRuntime.Current && SessionRuntime.FamilyOf(new("0.2.0-beta", "x", "0.1.30", "25480438")) == null,
                "A version's line is read wrong.");
        });
    }
}
