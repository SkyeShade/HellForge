using HellForge.Core;

static class ModKitTests
{
    // A build test runs with this host's pinned compiler: tools/zig-linux/zig on Linux, tools/zig/zig.exe on Windows.
    static bool ZigReady(ModSdk sdk) => File.Exists(OperatingSystem.IsLinux() ? sdk.NativeZig : sdk.Zig);

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The mod SDK is found in the repository and in an installation, with its template and tool pins", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory());
            check(repository != null && repository.Folder.EndsWith(Path.Combine("native", "mod-sdk"), StringComparison.Ordinal) &&
                Directory.Exists(repository.Template) && File.Exists(Path.Combine(repository.Template, "hellforge-mod.json")), "Repository SDK.");
            var tools = ModKit.Tools(repository!);
            check(tools.Count == 5 && tools.All(t => t.Platform is "windows-x64" or "linux-x64") &&
                tools.Single(t => t.Name == "zig") is { Ide: false, Version: "0.17.0", Platform: "windows-x64" } &&
                tools.Single(t => t.Name == "zig-linux") is { Ide: false, Version: "0.17.0", Platform: "linux-x64", Exe: "zig", Bytes: 57332648L,
                    Sha256: "1CBE9DF9F27E6B78D14CCBCA43B6703A404EF79EF1C463DE901D7F088D4E2026",
                    Url: "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz" } &&
                tools.Single(t => t.Name == "hpatchz-linux") is { Ide: false, Version: "5.1.3", Platform: "linux-x64", Exe: "linux64/hpatchz" } &&
                tools.All(t => t.Sha256.Length == 64), "Tool pins, including the native Linux Zig and hpatchz.");
            var placeholder = new ModTool("native-patch", "5.1.3", "https://example.invalid/tool.tar.gz", new string('0', 64),
                "hpatchz", false, "linux-x64");
            check(!ModKit.HasPinnedArchive(placeholder) && !ModKit.HasCompletePin(placeholder),
                "A zero-filled placeholder must not be downloaded or run.");

            var install = Path.Combine(root, "mdk-install");
            var sdk = Directory.CreateDirectory(Path.Combine(install, "app", "sdk")).FullName;
            File.WriteAllText(Path.Combine(sdk, "hellforge_mod.h"), "/* fixture */");
            Directory.CreateDirectory(Path.Combine(install, "mods", "some-mod"));
            var installed = ModSdk.Find(Path.Combine(install, "mods", "some-mod"));
            check(installed != null && installed.Folder == sdk && installed.Tools == Path.Combine(install, "tools") &&
                installed.Template == Path.Combine(sdk, "templates", "c-mod"), "Installation SDK: app/sdk, tools beside app.");
        });

        test("Linux Build now runs the native build agent while Windows keeps the pinned build", () =>
        {
            var page = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "src", "HellForge.Launcher", "ModKitPage.cs"));
            var helper = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "linux", "hf-build-mod.sh"));
            var linuxBuild = page.IndexOf("if (OperatingSystem.IsLinux())", StringComparison.Ordinal);
            var commandArg = linuxBuild < 0 ? -1 : page.IndexOf("start.ArgumentList.Add(\"build-mod\");", linuxBuild, StringComparison.Ordinal);
            var modArg = commandArg < 0 ? -1 : page.IndexOf("start.ArgumentList.Add(mod.Folder);", commandArg, StringComparison.Ordinal);
            var windowsBuild = modArg < 0 ? -1 : page.IndexOf("ModBuilds.Ensure(install, sdk, mod", modArg, StringComparison.Ordinal);
            check(linuxBuild >= 0 && commandArg > linuxBuild && modArg > commandArg && windowsBuild > modArg &&
                page.Contains("\"cli-linux\", \"HellForge.Cli\"", StringComparison.Ordinal) &&
                !page.Contains("hf-build-mod.sh", StringComparison.Ordinal) &&
                !page.Contains("start.Environment[\"PFX\"]", StringComparison.Ordinal) &&
                !page.Contains("start.ArgumentList.Add(\"build\");", StringComparison.Ordinal) &&
                helper.Contains("tools/zig-linux/zig", StringComparison.Ordinal) &&
                helper.Contains("-O2", StringComparison.Ordinal) && helper.Contains("build-mod --mod", StringComparison.Ordinal),
                "Linux invokes the native build agent for one mod; Windows retains ModBuilds; hf-build-mod.sh uses the pinned Zig and -O2.");
        });

        test("Native Linux Zig is found where hf-build-mod.sh finds it", () =>
        {
            var home = Path.Combine(root, "zig-home");
            var configured = Path.Combine(root, "zig-configured");
            var local = Path.Combine(home, ".local", "share", "zig-native", "zig");
            var first = Path.Combine(root, "zig-path-1");
            var second = Path.Combine(root, "zig-path-2");
            var fromPath = Path.Combine(second, "zig");
            var executable = new HashSet<string>(StringComparer.Ordinal) { configured, local, fromPath };
            var searchPath = first + Path.PathSeparator + second;
            string? Find(string? zig, string? path) => ModKit.FindNativeLinuxZig(zig, home, path, executable.Contains);
            check(Find(configured, searchPath) == configured, "ZIG wins.");
            check(Find(null, searchPath) == local, "The default wins over PATH.");
            executable.Remove(local);
            check(Find(null, searchPath) == fromPath, "PATH is the fallback.");
            check(Find(Path.Combine(root, "zig-not-executable"), first) == null, "A Zig that is not executable is not used.");
            check(Find(null, first) == null, "No executable Zig means none.");
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
            if (!ZigReady(repository)) { Console.WriteLine("  (skipped: no Zig for this host; run scripts/Setup-ModTools.ps1 or install the native Zig)"); return; }
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
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: Windows file-sharing semantics)"); return; }
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
            if (!ZigReady(repository)) { Console.WriteLine("  (skipped: no Zig for this host; run scripts/Setup-ModTools.ps1 or install the native Zig)"); return; }
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

        test("The source cache key names the compiler's identity on both hosts, and a Wine session is refused zig.exe", () =>
        {
            var mod = Directory.CreateDirectory(Path.Combine(root, "key-mod")).FullName;
            File.WriteAllText(Path.Combine(mod, ModCatalog.ManifestName), """{"format":1,"id":"test.key","name":"Key","version":"1"}""");
            Directory.CreateDirectory(Path.Combine(mod, "src"));
            File.WriteAllText(Path.Combine(mod, "src", "key.c"), "int key(void) { return 1; }\n");

            string Pin(string name, string version)
            {
                var platform = name == "zig" ? "windows-x64" : "linux-x64";
                var exe = name == "zig" ? "zig.exe" : "zig";
                var sha = name == "zig" ? new string('B', 64) : new string('A', 64);
                return $$"""
                    {"name":"{{name}}","version":"{{version}}","url":"https://example.invalid/{{name}}","sha256":"{{sha}}","exe":"{{exe}}","ide":false,"platform":"{{platform}}"}
                    """;
            }
            ModSdk Fixture(string folder, string pins)
            {
                var sdkFolder = Directory.CreateDirectory(folder).FullName;
                File.WriteAllText(Path.Combine(sdkFolder, "hellforge_mod.h"), "/* shared header */\n");
                File.WriteAllText(Path.Combine(sdkFolder, "tools.json"), "{\"format\":1,\"tools\":[" + pins + "]}");
                return new ModSdk(sdkFolder, Path.Combine(folder, "tools"), Path.Combine(sdkFolder, "templates", "c-mod"), Path.Combine(sdkFolder, "docs"));
            }

            var windows = Fixture(Path.Combine(root, "key-windows"), Pin("zig", "0.17.0") + "," + Pin("zig-linux", "0.17.0"));
            var linuxOnly = Fixture(Path.Combine(root, "key-linux"), Pin("zig-linux", "0.17.0"));
            var older = Fixture(Path.Combine(root, "key-older"), Pin("zig-linux", "0.16.0"));
            var key = ModBuilds.Key(mod, windows, "25480438");
            check(key == ModBuilds.Key(mod, linuxOnly, "25480438"),
                "The same sources and Zig version share one key whether the pin is the Windows or the Linux one.");
            check(key != ModBuilds.Key(mod, older, "25480438"), "Another Zig version is another key.");
            check(ModKit.Compiler(windows, linuxHost: false, nativeOnly: false) == windows.Zig, "Windows keeps tools/zig/zig.exe.");
            reject(() => ModKit.Compiler(windows, linuxHost: false, nativeOnly: true));
            check(ModKit.ToolsForThisPlatform(windows).Single().Name == (OperatingSystem.IsLinux() ? "zig-linux" : "zig"),
                "Mod tools are selected for the host platform.");
        });

        test("A native Linux build record is found by the session, which starts neither zig-linux nor zig.exe", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux compiler)"); return; }
            var install = new InstallLayout(Directory.CreateDirectory(Path.Combine(root, "native-build-install")).FullName);
            install.Create("25480438");
            var sdkFolder = Directory.CreateDirectory(Path.Combine(install.App, "sdk")).FullName;
            File.WriteAllText(Path.Combine(sdkFolder, "hellforge_mod.h"), "/* fixture */\n");
            var tools = Path.Combine(install.Root, "tools");
            var nativeZig = Path.Combine(tools, "zig-linux", "zig");
            Directory.CreateDirectory(Path.GetDirectoryName(nativeZig)!);
            var marker = Path.Combine(root, "native-zig-ran.txt");
            File.WriteAllText(nativeZig, $"""
                #!/bin/sh
                out=""
                previous=""
                for argument in "$@"; do
                  if [ "$previous" = "-o" ]; then out="$argument"; fi
                  previous="$argument"
                done
                echo run >> "{marker}"
                printf 'native build' > "$out"
                """);
            File.SetUnixFileMode(nativeZig, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var windowsZig = Path.Combine(tools, "zig", "zig.exe");
            Directory.CreateDirectory(Path.GetDirectoryName(windowsZig)!);
            var wineMarker = Path.Combine(root, "wine-zig-ran.txt");
            File.WriteAllText(windowsZig, $"""
                #!/bin/sh
                echo run >> "{wineMarker}"
                """);
            File.SetUnixFileMode(windowsZig, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            File.WriteAllText(Path.Combine(sdkFolder, "tools.json"), """
                {"format":1,"tools":[
                  {"name":"zig","version":"0.17.0","url":"https://example.invalid/zig.zip","sha256":"B5663F69581DCF391293FBF16C06CB80D81D806545CE618B4D0BAB7F0EB8C428","exe":"zig.exe","ide":false,"platform":"windows-x64"},
                  {"name":"zig-linux","version":"0.17.0","url":"https://example.invalid/zig.tar.xz","sha256":"1CBE9DF9F27E6B78D14CCBCA43B6703A404EF79EF1C463DE901D7F088D4E2026","bytes":57332648,"exe":"zig","ide":false,"platform":"linux-x64"}
                ]}
                """);
            var sdk = new ModSdk(sdkFolder, tools, Path.Combine(sdkFolder, "templates", "c-mod"), Path.Combine(sdkFolder, "docs"));
            var folder = Directory.CreateDirectory(Path.Combine(install.Mods, "native-mod")).FullName;
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName),
                "{\"format\":1,\"id\":\"test.native\",\"name\":\"Native\",\"version\":\"1\",\"code\":{\"dll\":\"native.dll\",\"sdk\":" +
                ModCatalog.SdkVersion + "}}");
            Directory.CreateDirectory(Path.Combine(folder, "src"));
            File.WriteAllText(Path.Combine(folder, "src", "native.c"), "int native(void) { return 1; }\n");
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            check(ModKit.Compiler(sdk, linuxHost: true, nativeOnly: false) == nativeZig, "Linux runs the pinned tools/zig-linux/zig.");

            var dll = ModBuilds.Ensure(install, sdk, mod, "25480438");
            var record = ModBuilds.Record(install, mod.Id)!;
            check(File.Exists(dll) && File.Exists(marker) && record.CompilerHost == "linux-x64" &&
                record.Compiler == "zig 0.17.0 / x86_64-windows-gnu" && record.DllSha256 != null &&
                record.Key == ModBuilds.Key(folder, sdk, "25480438"), "The native build records its compiler and host.");

            // The session finds the record with no compiler installed at all, and starts neither compiler.
            File.Delete(nativeZig);
            File.Delete(marker);
            var fallback = Path.Combine(root, "native-zig-fallback");
            File.WriteAllText(fallback, $"""
                #!/bin/sh
                echo run >> "{marker}"
                """);
            File.SetUnixFileMode(fallback, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var previousZig = Environment.GetEnvironmentVariable("ZIG");
            var previousMarker = Environment.GetEnvironmentVariable(ModKit.NativeBuildsVariable);
            Environment.SetEnvironmentVariable("ZIG", fallback);
            Environment.SetEnvironmentVariable(ModKit.NativeBuildsVariable, "1");
            try
            {
                check(ModKit.NativeBuildOnly, "The wrapper's native-build marker reaches ModBuilds.");
                var (ready, problems) = ModBuilds.Prepare(install, sdk, [mod], "25480438", installTools: false);
                check(problems.Count == 0 && ready.Count == 1 && ready[0].BuiltDll == dll &&
                    !File.Exists(marker) && !File.Exists(wineMarker),
                    "The session reused the native record without starting zig-linux or zig.exe: " + string.Join(" | ", problems));
            }
            finally
            {
                Environment.SetEnvironmentVariable("ZIG", previousZig);
                Environment.SetEnvironmentVariable(ModKit.NativeBuildsVariable, previousMarker);
            }
            reject(() => ModKit.Compiler(sdk, linuxHost: false, nativeOnly: true));
        });

        test("The Linux wrapper builds source mods natively before the namespace and marks the session", () =>
        {
            var wrapper = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "linux", "hellforge-linux.sh"));
            var session = wrapper.IndexOf("if session_command \"$@\"; then", StringComparison.Ordinal);
            var build = session < 0 ? -1 : wrapper.IndexOf("build-source-mods --install", session, StringComparison.Ordinal);
            var marker = build < 0 ? -1 : wrapper.IndexOf("export HELLFORGE_NATIVE_MOD_BUILDS=1", build, StringComparison.Ordinal);
            var space = marker < 0 ? -1 : wrapper.IndexOf("Entering the offline network namespace.", marker, StringComparison.Ordinal);
            check(session >= 0 && build > session && marker > build && space > marker,
                "The wrapper must build source mods before the namespace and tell the session they were built natively.");
        });
    }
}
