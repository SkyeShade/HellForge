using HellForge.Core;

// The versions an update keeps (KeptVersions): which are kept and dropped, room on the drive, going back and forward with
// each version's own data, refusals, a failure or a kill mid-switch, and where it may delete.
static class KeptVersionsTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        static string App(string folder, string version, string commit, int hour, string build = "25480438")
        {
            Directory.CreateDirectory(Path.Combine(folder, "launcher"));
            Directory.CreateDirectory(Path.Combine(folder, "cli"));
            Directory.CreateDirectory(Path.Combine(folder, "launcher-linux"));
            File.WriteAllText(Path.Combine(folder, "launcher", "HellForge.Launcher.exe"), "launcher " + commit);
            File.WriteAllText(Path.Combine(folder, "cli", "HellForge.Cli.exe"), "cli " + commit);
            File.WriteAllText(Path.Combine(folder, "launcher-linux", "HellForge.Launcher"), "linux " + commit);
            File.WriteAllText(Path.Combine(folder, "app-build.json"),
                $$"""{ "builtUtc": "2026-11-22T{{hour:00}}:00:00Z", "version": "{{version}}", "commit": "{{commit}}" }""");
            File.WriteAllText(Path.Combine(folder, "engine.json"), $$"""{ "version": "0.1.53", "build": "{{build}}" }""");
            return folder;
        }
        // What a version writes once it runs: its starter and its own data.
        static void Runs(InstallLayout install, string commit)
        {
            File.WriteAllText(Path.Combine(install.Root, "HellForge.exe"), "stub " + commit);
            File.WriteAllText(Path.Combine(install.Root, "launcher-settings.json"), "settings " + commit);
            Directory.CreateDirectory(Path.Combine(install.Profiles, "p"));
            File.WriteAllText(Path.Combine(install.Profiles, "p", "profile.json"), "profile " + commit);
            Directory.CreateDirectory(Path.Combine(install.Root, "account-snapshot", "p"));
            File.WriteAllText(Path.Combine(install.Root, "account-snapshot", "p", "seal.json"), "seal " + commit);
            Directory.CreateDirectory(install.Mods);
            File.WriteAllText(Path.Combine(install.Mods, "hellforge-mods.json"), "mods " + commit);
        }
        static InstallLayout Fresh(string root, string name)
        {
            var install = new InstallLayout(Path.Combine(root, name));
            install.Create("25480438");
            Directory.Delete(install.App);
            App(install.App, "0.4.0-beta", "aaaaaaa", 1);
            Runs(install, "aaaaaaa");
            return install;
        }
        // An update as Setup makes it: the new app staged, the old one kept, the new one moved in, then it runs.
        static KeepResult Update(InstallLayout install, string version, string commit, int hour, Func<string, long?>? free = null)
        {
            var staged = App(Path.Combine(install.Temp, "setup-staging-" + commit), version, commit, hour);
            var result = KeptVersions.KeepCurrent(install, staged, free ?? (_ => 100L << 30));
            if (Directory.Exists(install.App)) Directory.Delete(install.App, recursive: true);
            Directory.Move(staged, install.App);
            Runs(install, commit);
            return result;
        }
        static string Read(InstallLayout install, string path) => File.ReadAllText(Path.Combine(install.Root, path));
        // Every file under the installation but logs, with its text: the state a refused or failed switch must leave.
        static string Tree(InstallLayout install) => string.Join("\n", Directory.GetFiles(install.Root, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(install.Root, f)).Where(f => !f.StartsWith("logs", StringComparison.Ordinal)).Order(StringComparer.Ordinal)
            .Select(f => f + "=" + File.ReadAllText(Path.Combine(install.Root, f))));
        Func<IReadOnlyList<string>> Nothing = () => [];

        test("An update keeps the app it replaces with its HellForge.exe and data; the last 2 are kept, the oldest dropped", () =>
        {
            var install = Fresh(root, "kept-update");
            var first = Update(install, "0.5.0-beta", "bbbbbbb", 2);
            check(first.Kept && first.Name == "0.4.0-beta-aaaaaaa", "The replaced app was not kept: " + first.Message);
            var kept = KeptVersions.List(install).Single();
            check(File.ReadAllText(Path.Combine(kept.App, "launcher", "HellForge.Launcher.exe")) == "launcher aaaaaaa", "The kept app is not the replaced one.");
            check(File.ReadAllText(Path.Combine(kept.Folder, "HellForge.exe")) == "stub aaaaaaa", "The replaced HellForge.exe was not kept.");
            check(File.ReadAllText(Path.Combine(kept.Folder, "data", "profiles", "p", "profile.json")) == "profile aaaaaaa", "The profiles were not snapshotted.");
            check(File.ReadAllText(Path.Combine(kept.Folder, "data", "account-snapshot", "p", "seal.json")) == "seal aaaaaaa", "The seals were not snapshotted.");
            check(kept.Data.Contains("mods/hellforge-mods.json") && !kept.Data.Contains("catalogue"), "The snapshot's entries are wrong: " + string.Join(",", kept.Data));
            check(!OperatingSystem.IsWindows() || !Directory.Exists(Path.Combine(kept.App, "launcher-linux")), "A Windows kept copy still has the Linux build.");
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb", "The new app is not installed.");
            Update(install, "0.5.0-beta", "ccccccc", 3);
            var third = Update(install, "0.5.1-beta", "ddddddd", 4);
            var names = KeptVersions.List(install).Select(k => k.Name).ToList();
            check(names.SequenceEqual(["0.5.0-beta-ccccccc", "0.5.0-beta-bbbbbbb"]), "The kept versions are " + string.Join(", ", names));
            check(third.Dropped.Count == 1 && third.Dropped[0].Contains("aaaaaaa"), "The oldest was not dropped: " + third.Message);
            var again = Update(install, "0.5.1-beta", "ddddddd", 4);
            check(!again.Kept && KeptVersions.List(install).Count == 2, "Installing the same version again kept a copy of it.");
            check(File.ReadAllText(KeptVersions.LogFile(install)).Contains("Kept 0.5.0-beta (ccccccc)", StringComparison.Ordinal), "No log line for the keep.");
        });

        test("Without room on the drive the update keeps nothing, says so and still installs", () =>
        {
            var install = Fresh(root, "kept-no-room");
            var result = Update(install, "0.5.0-beta", "bbbbbbb", 2, _ => 1L << 30);
            check(!result.Kept && result.Message.Contains("free", StringComparison.Ordinal), "Kept without room: " + result.Message);
            check(KeptVersions.List(install).Count == 0 && (!Directory.Exists(KeptVersions.Root(install)) || !Directory.EnumerateFileSystemEntries(KeptVersions.Root(install)).Any()), "Something was left in app-versions.");
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb", "The update did not install.");
            check(File.ReadAllText(KeptVersions.LogFile(install)).Contains("was not kept", StringComparison.Ordinal), "No log line for the skipped keep.");
        });

        test("Going back swaps in the kept app, starter and data, keeps the current version the same way, and going forward undoes it", () =>
        {
            var install = Fresh(root, "kept-switch");
            Update(install, "0.5.0-beta", "bbbbbbb", 2);
            Directory.CreateDirectory(Path.Combine(install.Root, "catalogue"));
            File.WriteAllText(Path.Combine(install.Root, "catalogue", "item-catalog.json"), "catalogue bbbbbbb");
            Directory.CreateDirectory(Path.Combine(install.Game, "data"));
            File.WriteAllText(Path.Combine(install.Game, "data", "9ba626afa44a3aa3.patch_0"), "staged");
            File.WriteAllText(Path.Combine(install.Temp, "staged-mods.json"), """["9ba626afa44a3aa3.patch_0"]""");
            File.WriteAllText(Path.Combine(install.Mods, "my-mod.txt"), "a mod");
            var back = KeptVersions.Switch(install, "0.4.0-beta-aaaaaaa", Nothing);
            check(back.From == "0.5.0-beta (bbbbbbb)" && back.To == "0.4.0-beta (aaaaaaa)", $"Switched {back.From} -> {back.To}.");
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher aaaaaaa" && Read(install, "HellForge.exe") == "stub aaaaaaa", "The old app is not back.");
            check(Read(install, "profiles/p/profile.json") == "profile aaaaaaa" && Read(install, "launcher-settings.json") == "settings aaaaaaa" &&
                Read(install, "mods/hellforge-mods.json") == "mods aaaaaaa", "The old version's data is not back.");
            check(!Directory.Exists(Path.Combine(install.Root, "catalogue")), "A catalogue the old version never had is still there.");
            check(Read(install, "mods/my-mod.txt") == "a mod", "A mod went with the switch.");
            check(!File.Exists(Path.Combine(install.Game, "data", "9ba626afa44a3aa3.patch_0")), "The newer version's staged file is still in game/data.");
            var kept = KeptVersions.List(install).Single();
            check(kept.Name == "0.5.0-beta-bbbbbbb" && File.ReadAllText(Path.Combine(kept.Folder, "data", "profiles", "p", "profile.json")) == "profile bbbbbbb",
                "The version left is not kept with its data.");
            check(kept.Identity.IsNewerThan(KeptVersions.Identify(install.App)), "The version left is not newer.");
            KeptVersions.Switch(install, "0.5.0-beta (bbbbbbb)", Nothing);
            check(Read(install, "app/launcher/HellForge.Launcher.exe") == "launcher bbbbbbb" && Read(install, "profiles/p/profile.json") == "profile bbbbbbb" &&
                Read(install, "catalogue/item-catalog.json") == "catalogue bbbbbbb" && Read(install, "HellForge.exe") == "stub bbbbbbb", "Going forward did not bring it all back.");
            check(KeptVersions.List(install).Single().Name == "0.4.0-beta-aaaaaaa" && Read(install, "app-versions/0.4.0-beta-aaaaaaa/data/profiles/p/profile.json") == "profile aaaaaaa",
                "The old version is not kept again with its data.");
            check(File.ReadAllText(KeptVersions.LogFile(install)).Contains("Switched from 0.4.0-beta (aaaaaaa) to 0.5.0-beta (bbbbbbb)", StringComparison.Ordinal), "No log line for the switch.");
        });

        test("A switch is refused while the game or a HellForge program runs, and changes nothing", () =>
        {
            var install = Fresh(root, "kept-refused");
            Update(install, "0.5.0-beta", "bbbbbbb", 2);
            var before = Tree(install);
            reject(() => KeptVersions.Switch(install, "0.4.0-beta-aaaaaaa", () => ["HELLDIVERS 2"]));
            reject(() => KeptVersions.Switch(install, "0.3.0-beta-none", Nothing));
            check(Tree(install) == before, "A refused switch changed the installation.");
        });

        test("A failure at any step of a switch puts everything back as it was", () =>
        {
            var install = Fresh(root, "kept-failure");
            Update(install, "0.5.0-beta", "bbbbbbb", 2);
            var before = Tree(install);
            foreach (var failAt in new[] { "app", "starter", "data", "record", "kept" })
            {
                var calls = 0;
                reject(() => KeptVersions.Switch(install, "0.4.0-beta-aaaaaaa", Nothing, step =>
                {
                    if (step == failAt && ++calls == (failAt == "data" ? 3 : 1)) throw new IOException("disk said no at " + step);
                }));
                check(Tree(install) == before, "A failure at " + failAt + " left the installation changed.");
            }
            check(File.ReadAllText(KeptVersions.LogFile(install)).Contains("were put back as they were", StringComparison.Ordinal), "No log line for the rollback.");
        });

        test("A switch killed half-way is put back by the next update or switch", () =>
        {
            var install = Fresh(root, "kept-killed");
            Update(install, "0.5.0-beta", "bbbbbbb", 2);
            var before = Tree(install);
            try
            {
                KeptVersions.Switch(install, "0.4.0-beta-aaaaaaa", Nothing, step => { if (step == "data") throw new OperationCanceledException("killed"); });
                throw new Exception("The kill did not stop the switch.");
            }
            catch (OperationCanceledException) { }
            check(!Directory.Exists(Path.Combine(install.App, "launcher")) || Read(install, "app/launcher/HellForge.Launcher.exe") != "launcher bbbbbbb", "The kill came too late to test.");
            KeptVersions.Recover(install);
            check(Tree(install) == before, "Recover did not put the half-done switch back.");
        });

        test("Kept versions are deleted only inside app-versions, by their own names", () =>
        {
            var install = Fresh(root, "kept-paths");
            Update(install, "0.5.0-beta", "bbbbbbb", 2);
            check(KeptVersions.FolderNameOf("../../evil", "a\\b/c") is var name && !name.Contains('/') && !name.Contains('\\') && !name.StartsWith('.'),
                "An unsafe name came through: " + KeptVersions.FolderNameOf("../../evil", "a\\b/c"));
            Directory.CreateDirectory(Path.Combine(KeptVersions.Root(install), "deeper", "x"));
            foreach (var path in new[] { install.Root, install.App, install.Profiles, KeptVersions.Root(install), Path.Combine(KeptVersions.Root(install), ".."),
                Path.Combine(KeptVersions.Root(install), "deeper", "x"), Path.Combine(root, "elsewhere") })
                reject(() => KeptVersions.DeleteKept(install, path));
            check(Directory.Exists(install.App) && Directory.Exists(install.Profiles), "A refused delete deleted something.");
            File.WriteAllText(Path.Combine(KeptVersions.Root(install), ".switch-journal.json"),
                $$"""[{ "from": {{System.Text.Json.JsonSerializer.Serialize(Path.Combine(root, "outside"))}}, "to": {{System.Text.Json.JsonSerializer.Serialize(install.App)}} }]""");
            reject(() => KeptVersions.Recover(install));
            File.Delete(Path.Combine(KeptVersions.Root(install), ".switch-journal.json"));
            var outside = Directory.CreateDirectory(Path.Combine(root, "kept-paths-outside")).FullName;
            File.WriteAllText(Path.Combine(outside, "keep-me.txt"), "mine");
            var link = Path.Combine(KeptVersions.Root(install), "linked");
            try { Directory.CreateSymbolicLink(link, outside); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException && OperatingSystem.IsWindows())
            {
                // without the symlink privilege a junction, which any user may make
                using var mklink = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{outside}\"") { CreateNoWindow = true, UseShellExecute = false });
                mklink?.WaitForExit();
                if (!Directory.Exists(link)) { Console.WriteLine("  (link part skipped: " + error.Message + ")"); return; }
            }
            reject(() => KeptVersions.DeleteKept(install, link));
            check(File.Exists(Path.Combine(outside, "keep-me.txt")), "A delete followed a link out of app-versions.");
        });
    }
}
