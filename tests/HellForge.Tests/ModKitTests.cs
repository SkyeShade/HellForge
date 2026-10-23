using HellForge.Core;

static class ModKitTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The mod SDK is found in the repository and in an installation, with its template and tool pins", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory());
            check(repository != null && repository.Folder.EndsWith(Path.Combine("native", "mod-sdk"), StringComparison.Ordinal) &&
                Directory.Exists(repository.Template) && File.Exists(Path.Combine(repository.Template, "hellforge-mod.json")), "Repository SDK.");
            var tools = ModKit.Tools(repository!);
            check(tools.Count == 3 && tools.Single(t => t.Name == "zig") is { Ide: false, Version: "0.17.0" } && tools.All(t => t.Sha256.Length == 64), "Tool pins.");

            var install = Path.Combine(root, "mdk-install");
            var sdk = Directory.CreateDirectory(Path.Combine(install, "app", "sdk")).FullName;
            File.WriteAllText(Path.Combine(sdk, "hellforge_mod.h"), "/* fixture */");
            Directory.CreateDirectory(Path.Combine(install, "mods", "some-mod"));
            var installed = ModSdk.Find(Path.Combine(install, "mods", "some-mod"));
            check(installed != null && installed.Folder == sdk && installed.Tools == Path.Combine(install, "tools") &&
                installed.Template == Path.Combine(sdk, "templates", "c-mod"), "Installation SDK: app/sdk, tools beside app.");
        });

        test("New mods are made from the template with every placeholder filled, and bad ids or existing folders are refused", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var parent = Directory.CreateDirectory(Path.Combine(root, "mdk-new")).FullName;
            var folder = ModKit.Create(repository, parent, "tester.first-mod", "First Mod");
            check(folder == Path.Combine(parent, "first-mod"), "Folder from the id's mod name.");
            var files = Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).ToList();
            check(files.Any(f => f.EndsWith(Path.Combine("src", "first_mod.c"), StringComparison.Ordinal)) && files.All(f => !f.Contains("{{")) &&
                files.All(f => !File.ReadAllText(f).Contains("{{")), "Every placeholder filled: " + string.Join(", ", files.Select(Path.GetFileName)));
            var manifest = File.ReadAllText(Path.Combine(folder, ModCatalog.ManifestName));
            check(manifest.Contains("\"id\": \"tester.first-mod\"") && manifest.Contains($"\"dll\": \"first_mod.dll\", \"sdk\": {ModCatalog.SdkVersion}"), "Manifest.");
            var include = File.ReadAllLines(Path.Combine(folder, "CMakeLists.txt")).Single(l => l.StartsWith("include(", StringComparison.Ordinal));
            var cmake = Path.GetFullPath(Path.Combine(folder, include["include(".Length..^1]));
            check(cmake == Path.Combine(repository.Folder, "cmake", "HellForgeMod.cmake"), "The CMake include reaches the SDK: " + cmake);
            foreach (var id in new[] { "Bad.Id", "noowner", "a.b.c", "owner.", ".mod", "owner.with space" })
                reject(() => ModKit.Create(repository, parent, id, "x"));
            reject(() => ModKit.Create(repository, parent, "tester.second", "Bad \"name\""));
            reject(() => ModKit.Create(repository, parent, "tester.first-mod", "Again"));
        });

        test("A mod made from the template builds with the pinned Zig and then loads as a code mod", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            if (!File.Exists(repository.Zig)) { Console.WriteLine("  (skipped: no Zig in tools/; run scripts/Setup-ModTools.ps1)"); return; }
            var parent = Directory.CreateDirectory(Path.Combine(root, "mdk-build")).FullName;
            var folder = ModKit.Create(repository, parent, "tester.built-mod", "Built Mod");
            var dll = ModKit.Build(folder, repository);
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            check(File.Exists(dll) && mod.HasCode && mod.CodeDll == "built_mod.dll" && mod.Id == "tester.built-mod", "Built and loadable.");
            File.WriteAllText(Path.Combine(folder, "src", "built_mod.c"), "int broken(void) { return missing; }");
            reject(() => ModKit.Build(folder, repository));
        });

        test("Mods are added from zips or folders, replace the same id after it is flagged, and are removed only from mods", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var install = new InstallLayout(Path.Combine(root, "add-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var outside = Directory.CreateDirectory(Path.Combine(root, "add-sources")).FullName;
            var zipped = ModKit.Create(repository, outside, "tester.zipped", "Zipped Mod");
            var zip = ModPackage.Create(ModCatalog.Parse(zipped, EntityProfile.Current), Path.Combine(outside, "zips"));
            var folder = ModKit.Create(repository, outside, "tester.folder", "Folder Mod");
            File.WriteAllText(Path.Combine(outside, "notes.zip"), "not a zip");

            var (first, problems) = ModInstaller.Inspect(install, [zip, Path.Combine(folder, ModCatalog.ManifestName), Path.Combine(outside, "notes.zip")]);
            check(first.Count == 2 && first.All(m => m.Replaces == null) && problems.Count == 1 && problems[0].StartsWith("notes.zip", StringComparison.Ordinal),
                "Two mods, nothing replaced; the broken zip is a problem: " + string.Join(" | ", problems));
            foreach (var mod in first) ModInstaller.Add(install, mod);
            var (loaded, loadProblems) = ModCatalog.Load(install.Mods);
            check(loadProblems.Count == 0 && loaded.Select(m => m.Id).Order().SequenceEqual(["tester.folder", "tester.zipped"]) &&
                File.Exists(Path.Combine(install.Mods, Path.GetFileName(zip))) && Directory.Exists(Path.Combine(install.Mods, "folder", "src")),
                "Both load: the zip as a zip, the folder copied.");

            // A newer version of the folder mod replaces the installed one, flagged first.
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), File.ReadAllText(Path.Combine(folder, ModCatalog.ManifestName)).Replace("0.1.0", "0.2.0"));
            var (update, _) = ModInstaller.Inspect(install, [Path.Combine(folder, ModCatalog.ManifestName)]);
            check(update.Single() is { Version: "0.2.0" } u && u.Replaces == Path.Combine(install.Mods, "folder"), "The update names what it replaces.");
            ModInstaller.Add(install, update.Single());
            (loaded, _) = ModCatalog.Load(install.Mods);
            check(loaded.Count == 2 && loaded.Single(m => m.Id == "tester.folder").Version == "0.2.0", "Replaced, not duplicated.");
            var (again, againProblems) = ModInstaller.Inspect(install, [Path.Combine(install.Mods, "folder", ModCatalog.ManifestName)]);
            check(again.Count == 0 && againProblems.Single().Contains("mods folder already"), "A mod already in the mods folder is not added again.");

            foreach (var mod in loaded) ModInstaller.Remove(install, mod);
            check(ModCatalog.Load(install.Mods).Mods.Count == 0 && Directory.GetFileSystemEntries(install.Mods).All(e => Path.GetFileName(e).StartsWith('.')),
                "Removed: the zip and the folder are gone.");
            reject(() => ModInstaller.Remove(install, ModCatalog.Parse(zipped, EntityProfile.Current)));
            check(Directory.Exists(zipped), "A mod outside the mods folder is never deleted.");
        });

        test("Unpacked tools are moved into place even while an antivirus still holds a file open", () =>
        {
            if (!OperatingSystem.IsWindows()) return; // exclusive file locks are a Windows behaviour
            var staging = Directory.CreateDirectory(Path.Combine(root, "held-tools", "zig-staging")).FullName;
            var target = Path.Combine(root, "held-tools", "zig");
            var exe = Path.Combine(staging, "zig.exe");
            File.WriteAllText(exe, "exe");
            var lines = new List<string>();
            var scan = File.Open(exe, FileMode.Open, FileAccess.Read, FileShare.None); // what a scanner does
            var release = Task.Delay(1200).ContinueWith(_ => scan.Dispose());
            ModKit.RetryWhileHeld(() => Directory.Move(staging, target), lines.Add).GetAwaiter().GetResult();
            release.Wait();
            check(File.Exists(Path.Combine(target, "zig.exe")) && !Directory.Exists(staging) && lines.Count == 1 && lines[0].Contains("antivirus"),
                "Moved once the file was released, saying why it waited: " + string.Join(" | ", lines));
        });

        test("A mod is packed to share: its sources and files, no DLL or IDE leftovers, the same zip every time, never into mods", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var install = new InstallLayout(Path.Combine(root, "package-install"));
            install.Create("25480438");
            var folder = ModKit.Create(repository, Directory.CreateDirectory(install.Mods).FullName, "tester.packed", "Packed Mod");
            Directory.CreateDirectory(Path.Combine(folder, "images"));
            File.WriteAllText(Path.Combine(folder, "images", "icon.png"), "png");
            File.WriteAllText(Path.Combine(folder, "packed.dll"), "an IDE's build");
            Directory.CreateDirectory(Path.Combine(folder, ".idea"));
            File.WriteAllText(Path.Combine(folder, ".idea", "workspace.xml"), "<x/>");
            Directory.CreateDirectory(Path.Combine(folder, "build"));
            File.WriteAllText(Path.Combine(folder, "build", "out.o"), "o");
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            var zip = ModPackage.Create(mod, install.Exports);
            using (var archive = System.IO.Compression.ZipFile.OpenRead(zip))
            {
                var names = archive.Entries.Select(e => e.FullName).ToList();
                check(Path.GetFileName(zip) == "packed-0.1.0.zip" && names.Contains("packed/" + ModCatalog.ManifestName) &&
                    names.Contains("packed/images/icon.png") && names.Any(n => n.StartsWith("packed/src/", StringComparison.Ordinal)) &&
                    !names.Any(n => n.EndsWith(".dll", StringComparison.Ordinal) || n.Contains("/.idea/") || n.Contains("/build/")),
                    "Sources and files, in the mod's folder; no DLL, .idea or build: " + string.Join(", ", names));
            }
            var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(zip)));
            check(hash == Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(ModPackage.Create(mod, install.Exports)))),
                "The same mod makes the same zip.");
            reject(() => ModPackage.Create(mod, install.Mods));
            reject(() => ModPackage.Create(mod, Path.Combine(install.Mods, "zips")));

            // Dropped into another player's mods folder, the zip loads as the same mod.
            var friend = new InstallLayout(Path.Combine(root, "package-friend"));
            friend.Create("25480438");
            File.Copy(zip, Path.Combine(Directory.CreateDirectory(friend.Mods).FullName, Path.GetFileName(zip)));
            var (loaded, problems) = ModCatalog.Load(friend.Mods);
            check(problems.Count == 0 && loaded.Single() is { Id: "tester.packed", HasSource: true }, "The zip loads in a mods folder: " + string.Join(" | ", problems));
        });

        test("Source mods are built by HellForge before a session, cached, checked and reused; one that does not compile stays off", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            if (!File.Exists(repository.Zig)) { Console.WriteLine("  (skipped: no Zig in tools/; run scripts/Setup-ModTools.ps1)"); return; }
            var install = new InstallLayout(Directory.CreateDirectory(Path.Combine(root, "source-install")).FullName);
            var mods = Directory.CreateDirectory(Path.Combine(install.Root, "mods")).FullName;
            var folder = ModKit.Create(repository, mods, "tester.source-mod", "Source Mod");
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            check(mod.HasSource && mod.HasCode && mod.DllPath == null, "A source mod loads without a DLL: HellForge builds it.");
            const string build = "25480438";
            var dll = ModBuilds.Ensure(install, repository, mod, build);
            var record = ModBuilds.Record(install, mod.Id)!;
            check(File.Exists(dll) && dll.StartsWith(ModBuilds.Folder(install), StringComparison.OrdinalIgnoreCase) && record.Error == null &&
                record.Key == ModBuilds.Key(folder, repository, build), "Built into the installation's build cache, with its record.");
            check(ModBuilds.Ensure(install, repository, mod, build) == dll && ModBuilds.Record(install, mod.Id)!.Built == record.Built, "The same sources reuse the build.");
            File.AppendAllText(dll, "x");
            ModBuilds.Ensure(install, repository, mod, build);
            check(ModBuilds.Record(install, mod.Id)!.Built > record.Built, "A DLL that no longer matches its record is built again.");
            check(ModBuilds.Key(folder, repository, "1") != ModBuilds.Key(folder, repository, build), "Another game build is another key.");
            var plan = ModLoaderPlan.Text([mod with { BuiltDll = dll }], root, EntityProfile.Current, build);
            check(plan.Contains("mod\ttester.source-mod\t" + folder + "\t" + dll + "\n"), "The loader plan loads the cached build.");
            var digest = ModCatalog.Digest([mod]);
            File.WriteAllText(Path.Combine(folder, "source_mod.dll"), "an IDE's build");
            check(ModCatalog.Digest([ModCatalog.Parse(folder, EntityProfile.Current)]) == digest, "Friends compare a source mod by its sources, not a DLL in its folder.");
            File.AppendAllText(Path.Combine(folder, "src", "source_mod.c"), "\nint broken(void) { return missing; }\n");
            check(ModCatalog.Digest([ModCatalog.Parse(folder, EntityProfile.Current)]) != digest, "Changed sources change the modlist digest.");
            var steps = new List<ModBuildProgress>();
            var (ready, problems) = ModBuilds.Prepare(install, repository, [ModCatalog.Parse(folder, EntityProfile.Current)], build, progress: steps.Add);
            check(steps.Count == 2 && steps[0] is { Stage: "compiling", Done: 0, Total: 1, Mod: "Source Mod" } && steps[1] is { Stage: "done", Done: 1 },
                "The launcher's progress: the mod being compiled, then done.");
            var failed = ModBuilds.Record(install, mod.Id)!;
            check(ready.Count == 0 && problems.Count == 1 && problems[0].Contains("did not compile") && problems[0].Contains("source_mod.c") &&
                failed.Error != null && failed.Error.Contains("missing"), "A mod that does not compile stays off, its error (file and line) kept: " + string.Join(" | ", problems));
            (ready, problems) = ModBuilds.Prepare(install, repository, [ModCatalog.Parse(folder, EntityProfile.Current)], build);
            check(problems.Count == 1 && problems[0].Contains("unchanged") && ModBuilds.Record(install, mod.Id)!.Built == failed.Built,
                "The same broken sources are not compiled again.");
            var prebuilt = Directory.CreateDirectory(Path.Combine(root, "prebuilt-mod")).FullName;
            File.WriteAllText(Path.Combine(prebuilt, "mod.dll"), "dll");
            File.WriteAllText(Path.Combine(prebuilt, ModCatalog.ManifestName),
                """{"format":1,"id":"test.prebuilt","name":"Prebuilt","version":"1","code":{"dll":"mod.dll","sdk":6,"game":"1"}}""");
            reject(() => ModCatalog.Parse(prebuilt, EntityProfile.Current));
            File.WriteAllText(Path.Combine(prebuilt, ModCatalog.ManifestName),
                """{"format":1,"id":"test.prebuilt","name":"Prebuilt","version":"1","code":{"dll":"mod.dll","sdk":6,"game":"25480438"}}""");
            check(ModCatalog.Parse(prebuilt, EntityProfile.Current) is { HasSource: false } p && p.DllPath == Path.Combine(prebuilt, "mod.dll"),
                "A prebuilt DLL for this game build loads from its folder; one for another build is refused.");
        });
    }
}
