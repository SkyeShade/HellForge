using System.IO.Compression;
using HellForge.Core;
using HellForge.Core.GameData;

static class CosmeticPacksTests
{
    // A pack in the community's format: an option with sub-options, a plain option, and its folders of patches.
    static string Pack(string root, string name, string guid)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, name)).FullName;
        File.WriteAllText(Path.Combine(folder, CosmeticPacks.ManifestName), $$"""
            {
              "Version": 1, "Guid": "{{guid}}", "Name": "{{name}} Pack", "Description": "Skins.", "IconPath": ".jpgs/icon.png",
              "Options": [
                { "Name": "Rifle", "Description": "A rifle skin.", "Image": ".jpgs/rifle.jpg",
                  "SubOptions": [ { "Name": "Rifle/A", "Include": ["Rifle A"] }, { "Name": "Rifle/B", "Include": ["Rifle B"] } ], },
                { "Name": "Pistol", "Include": ["Pistol"] }
              ]
            }
            """);
        foreach (var variant in new[] { "Rifle A", "Rifle B", "Pistol" })
        {
            var at = Directory.CreateDirectory(Path.Combine(folder, variant)).FullName;
            foreach (var suffix in new[] { "", ".gpu_resources", ".stream" })
                File.WriteAllText(Path.Combine(at, "9ba626afa44a3aa3.patch_0" + suffix), variant + suffix);
        }
        Directory.CreateDirectory(Path.Combine(folder, ".jpgs"));
        File.WriteAllText(Path.Combine(folder, ".jpgs", "icon.png"), "png");
        return folder;
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Cosmetic packs: options and sub-options choose the folders staged, renumbered after the game's own patches", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "pack-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var data = Directory.CreateDirectory(Path.Combine(install.Game, "data")).FullName;
            File.WriteAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_0"), "the player's own"); // a patch HellForge did not stage
            var folder = Pack(install.Mods, "kinetic", "0967dcdf-f9c8-4e3c-b8ac-184d2ba16f2a");

            var (mods, problems) = ModCatalog.Load(install.Mods);
            var pack = mods.Single();
            check(problems.Count == 0 && pack is { Id: "pack.0967dcdf-f9c8-4e3c-b8ac-184d2ba16f2a", Name: "kinetic Pack", Cosmetic: true } &&
                pack.Pack!.Options.Count == 2 && pack.Pack.Icon == Path.Combine(folder, ".jpgs", "icon.png"), "Read as a cosmetic mod: " + string.Join(" | ", problems));
            check(pack.PackFolders.Select(Path.GetFileName).SequenceEqual(["Rifle A", "Pistol"]), "By default: every option, each with its first sub-option.");

            ModSettings.SetChoice(install.Mods, pack.Id, new(new HashSet<string> { "Pistol" }, new Dictionary<string, string> { ["Rifle"] = "Rifle/B" }));
            ModSettings.SetEnabled(install.Mods, "some.other-mod", false);
            pack = ModCatalog.Load(install.Mods).Mods.Single();
            check(pack.PackFolders.Select(Path.GetFileName).SequenceEqual(["Rifle B"]), "The choice is kept: Pistol off, Rifle/B.");
            check(ModSettings.Read(install.Mods).Choices.ContainsKey(pack.Id), "Switching another mod off keeps the pack's choice.");

            var staged = ModStaging.Stage(install, [pack]);
            check(staged.Order().SequenceEqual(["9ba626afa44a3aa3.patch_1", "9ba626afa44a3aa3.patch_1.gpu_resources", "9ba626afa44a3aa3.patch_1.stream"]) &&
                File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_1")) == "Rifle B", "Staged after the game's own patch_0: " + string.Join(", ", staged));
            ModStaging.Unstage(install);
            check(File.Exists(Path.Combine(data, "9ba626afa44a3aa3.patch_0")) && !File.Exists(Path.Combine(data, "9ba626afa44a3aa3.patch_1")), "Unstaged; the player's own patch stays.");

            check(ModCatalog.Digest([pack]) == "none", "Friends do not need the same cosmetic packs.");
        });

        test("Cosmetic packs: an empty Include (a VANILLA choice) stages nothing; a folder outside the pack is still refused", () =>
        {
            var mods = Directory.CreateDirectory(Path.Combine(root, "vanilla-mods")).FullName;
            var folder = Pack(mods, "reticles", "22222222-2222-3333-4444-555555555555");
            var manifest = Path.Combine(folder, CosmeticPacks.ManifestName);
            File.WriteAllText(manifest, File.ReadAllText(manifest).Replace("""{ "Name": "Rifle/A", "Include": ["Rifle A"] }""", """{ "Name": "VANILLA", "Include": [""] }"""));
            var (loaded, problems) = ModCatalog.Load(mods);
            check(problems.Count == 0 && loaded.Single().PackFolders.Select(Path.GetFileName).SequenceEqual(["Pistol"]),
                "Loads; VANILLA (the default sub-option) adds no folder: " + string.Join(" | ", problems));

            File.WriteAllText(manifest, File.ReadAllText(manifest).Replace("""["Pistol"]""", """["../elsewhere"]"""));
            (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Count == 0 && problems.Single().Contains("Pistol includes a folder outside the pack"), "Refused: " + string.Join(" | ", problems));
        });

        test("The Mods page's order: gameplay then cosmetic, saved, new mods on top; sessions load it bottom first", () =>
        {
            var mods = Directory.CreateDirectory(Path.Combine(root, "order-mods")).FullName;
            foreach (var id in new[] { "a.one", "a.two", "a.three" })
            {
                var folder = Directory.CreateDirectory(Path.Combine(mods, id)).FullName;
                File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), $$"""{"format":1,"id":"{{id}}","name":"{{id}}","version":"1","gameplay":true}""");
                Directory.CreateDirectory(Path.Combine(folder, "data"));
                File.WriteAllText(Path.Combine(folder, "data", "9ba626afa44a3aa3.patch_0"), id);
            }
            File.WriteAllText(Path.Combine(mods, "a.one", "icon.png"), "png");
            File.WriteAllText(Path.Combine(mods, "a.one", ModCatalog.ManifestName), File.ReadAllText(Path.Combine(mods, "a.one", ModCatalog.ManifestName)).Replace("\"gameplay\"", "\"icon\":\"icon.png\",\"gameplay\""));
            File.WriteAllText(Path.Combine(mods, "a.two", ModCatalog.ManifestName), File.ReadAllText(Path.Combine(mods, "a.two", ModCatalog.ManifestName)).Replace("\"gameplay\"", "\"icon\":\"../a.one/icon.png\",\"gameplay\""));
            Pack(mods, "skins", "11111111-2222-3333-4444-555555555555");
            ModSettings.SetOrder(mods, ["pack.11111111-2222-3333-4444-555555555555", "a.three", "a.one"]);
            var (listed, problems) = ModCatalog.Load(mods);
            check(problems.Count == 0 && listed.Select(m => m.Id).SequenceEqual(["a.two", "a.three", "a.one", "pack.11111111-2222-3333-4444-555555555555"]),
                "Gameplay first (the new a.two on top), then the cosmetic group: " + string.Join(", ", listed.Select(m => m.Id)) + " " + string.Join(" | ", problems));
            check(ModCatalog.LoadOrder(listed).Select(m => m.Id).First() == "pack.11111111-2222-3333-4444-555555555555", "The bottom mod loads first.");
            check(listed.Single(m => m.Id == "a.one").Icon == Path.Combine(mods, "a.one", "icon.png") && listed.Single(m => m.Id == "a.two").Icon == null,
                "A manifest's icon is a file in its own folder, never outside it.");
            var gameplay = listed.Where(m => m.Gameplay).ToList();
            check(ModCatalog.Digest(gameplay) == ModCatalog.Digest(Enumerable.Reverse(gameplay)), "The modlist code does not depend on the gameplay order.");
            check(ModCatalog.Digest(gameplay) != ModCatalog.Digest(gameplay.Skip(1)), "It still needs the same gameplay mods.");
            check(ModCatalog.Digest(listed) == ModCatalog.Digest(listed.Where(m => m.Gameplay)), "Cosmetic packs are not in it, wherever they sit.");
        });

        test("Lua script mods load, marked Lua and outside the modlist code: a pack, a zip of one, or a HellForge mod's data with a lua resource", () =>
        {
            static void Archive(string path, ulong type)
            {
                var (archive, gpu, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(type, 0x1234)] = new([1, 2, 3, 4], [], []) });
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllBytes(path, archive);
                File.WriteAllBytes(path + ".gpu_resources", gpu);
            }
            var install = new InstallLayout(Path.Combine(root, "lua-install"));
            install.Create("25480438");
            var mods = Directory.CreateDirectory(install.Mods).FullName;
            // as the community ships them: manifest.json with no options, the script patch at the root
            var lua = Path.Combine(mods, "frv-swap");
            Directory.CreateDirectory(lua);
            File.WriteAllText(Path.Combine(lua, CosmeticPacks.ManifestName), """{"Version":1,"Guid":"7a41fd46-5db0-4f09-9a96-22a832e5bb62","Name":"FRV Weapon Swap","Options":[]}""");
            Archive(Path.Combine(lua, "9ba626afa44a3aa3.patch_0"), ResourceTypes.Lua);
            var skin = Path.Combine(mods, "skin");
            Directory.CreateDirectory(skin);
            Archive(Path.Combine(skin, "9ba626afa44a3aa3.patch_0"), ResourceTypes.Texture);
            var coded = Path.Combine(mods, "wrapped");
            File.WriteAllText(Path.Combine(Directory.CreateDirectory(coded).FullName, ModCatalog.ManifestName), """{"format":1,"id":"a.wrapped","name":"Wrapped","version":"1"}""");
            Archive(Path.Combine(coded, "data", "9ba626afa44a3aa3.patch_0"), ResourceTypes.Lua);

            var (loaded, problems) = ModCatalog.Load(mods);
            check(problems.Count == 1 && problems[0].Contains("need Bingus Shared Loader") && loaded.Count == 3,
                "All three load (their Lua needs Bingus Shared Loader, not here): " + string.Join(", ", loaded.Select(m => m.Id)) + " | " + string.Join(" | ", problems));
            var frv = loaded.Single(m => m.Id == "pack.7a41fd46-5db0-4f09-9a96-22a832e5bb62");
            var wrapped = loaded.Single(m => m.Id == "a.wrapped");
            check(frv.Lua && wrapped.Lua && !loaded.Single(m => m.Id == "pack.skin").Lua, "The Lua ones are marked Lua, the skin is not.");
            check(ModCatalog.Digest(loaded) == ModCatalog.Digest(loaded.Where(m => !m.Lua)), "Lua mods are not in the modlist code.");

            var zip = Path.Combine(root, "lua-source", "frv.zip");
            Directory.CreateDirectory(Path.GetDirectoryName(zip)!);
            ZipFile.CreateFromDirectory(lua, zip);
            Directory.Delete(lua, recursive: true);
            var (candidates, refused) = ModInstaller.Inspect(install, [zip]);
            check(candidates.Count == 1 && refused.Count == 0, "Add mods takes its zip: " + string.Join(" | ", refused));
        });

        test("Lua runtimes: a Lua mod without Bingus Shared Loader, an HD2Runtime mod without HD2Runtime, an HD2Runtime without the bridge", () =>
        {
            var mods = Directory.CreateDirectory(Path.Combine(root, "lua-runtimes")).FullName;
            string LuaMod(string name, string guid, string? runtimeSpec = null, bool bingus = false)
            {
                var folder = Directory.CreateDirectory(Path.Combine(mods, name)).FullName;
                File.WriteAllText(Path.Combine(folder, CosmeticPacks.ManifestName), $$"""{"Version":1,"Guid":"{{guid}}","Name":"{{name}}","Options":[]}""");
                var (archive, gpu, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(ResourceTypes.Lua, 0x1234)] = new([1, 2, 3, 4], [], []) });
                File.WriteAllBytes(Path.Combine(folder, "9ba626afa44a3aa3.patch_0"), archive);
                File.WriteAllBytes(Path.Combine(folder, "9ba626afa44a3aa3.patch_0.gpu_resources"), gpu);
                if (runtimeSpec != null) File.WriteAllText(Path.Combine(folder, "hd2runtime.json"), runtimeSpec);
                if (bingus) File.WriteAllText(Path.Combine(folder, "BingusSharedLoader-manifest.json"), """{"name":"Bingus Shared Loader","provides":{"shared_loader_api":1}}""");
                return folder;
            }
            List<string> Problems() => ModCatalog.Load(mods).Problems;
            LuaMod("Railgun", "11111111-0000-0000-0000-000000000001",
                """{"format":1,"resource":"mods/azura/railgun","requires":{"bingus":{"api":1},"hd2runtime":{"module":"mods/skyeshade/hd2runtime"}}}""");
            var problems = Problems();
            check(problems.Count == 2 && problems.Any(p => p.StartsWith("Railgun needs Bingus Shared Loader")) && problems.Any(p => p.StartsWith("Railgun needs HD2Runtime")),
                "Both runtimes missing: " + string.Join(" | ", problems));
            LuaMod("Bingus", "11111111-0000-0000-0000-000000000002", bingus: true);
            var runtime = LuaMod("HD2Runtime", "11111111-0000-0000-0000-000000000003",
                """{"format":1,"name":"HD2Runtime","resource":"mods/skyeshade/hd2runtime","requires":{"bingus":{"api":1}}}""");
            problems = Problems();
            check(problems.Count == 1 && problems[0].Contains("has no HellForge bridge"), "An HD2Runtime from before the bridge: " + string.Join(" | ", problems));
            File.WriteAllText(Path.Combine(runtime, "hd2runtime.json"),
                """{"format":1,"name":"HD2Runtime","resource":"mods/skyeshade/hd2runtime","requires":{"bingus":{"api":1}},"hellforge":{"bridge":1}}""");
            check(Problems().Single().Contains("was made for HellForge's bridge 1"), "An older bridge.");
            File.WriteAllText(Path.Combine(runtime, "hd2runtime.json"),
                """{"format":1,"name":"HD2Runtime","resource":"mods/skyeshade/hd2runtime","requires":{"bingus":{"api":1}},"hellforge":{"bridge":""" + LuaRuntimes.Bridge + "}}");
            check(Problems().Count == 0, "With both, and this bridge: " + string.Join(" | ", Problems()));
            // a disabled runtime counts as absent
            ModSettings.SetEnabled(mods, "pack.11111111-0000-0000-0000-000000000002", false);
            check(Problems().Single().StartsWith("Railgun, HD2Runtime need Bingus Shared Loader"), "Bingus turned off: " + string.Join(" | ", Problems()));
        });

        test("A cosmetic pack zipped loads like a folder, and Add mods takes it", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "pack-zip-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var source = Directory.CreateDirectory(Path.Combine(root, "pack-zip-source")).FullName;
            var folder = Pack(source, "zipped", "99999999-2222-3333-4444-555555555555");
            var zip = Path.Combine(source, "zipped.zip");
            ZipFile.CreateFromDirectory(folder, zip, CompressionLevel.Fastest, includeBaseDirectory: true);
            var (candidates, problems) = ModInstaller.Inspect(install, [zip, Path.Combine(folder, CosmeticPacks.ManifestName)]);
            check(problems.Count == 1 && candidates.Single() is { Id: "pack.99999999-2222-3333-4444-555555555555", IsZip: true },
                "The zip is a pack; the same pack again (its folder) is refused as chosen twice: " + string.Join(" | ", problems));
            ModInstaller.Add(install, candidates.Single());
            var (mods, loadProblems) = ModCatalog.Load(install.Mods);
            check(loadProblems.Count == 0 && mods.Single() is { Cosmetic: true, Archive: "zipped.zip" } p && p.PackFolders.Count == 2,
                "Loaded from the zip: " + string.Join(" | ", loadProblems));
        });

        test("A .7z (as a .rar) is unpacked by Windows' tar and added as a folder: a pack with its files at the root, a mod in a folder", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "pack-7z-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var source = Directory.CreateDirectory(Path.Combine(root, "pack-7z-source")).FullName;
            var pack = Pack(source, "sevenzip", "33333333-2222-3333-4444-555555555555");
            var mod = Directory.CreateDirectory(Path.Combine(source, "a-mod")).FullName;
            File.WriteAllText(Path.Combine(mod, ModCatalog.ManifestName), """{"format":1,"id":"a.sevenzip","name":"Seven","version":"1","gameplay":true}""");
            File.WriteAllText(Path.Combine(Directory.CreateDirectory(Path.Combine(mod, "data")).FullName, "9ba626afa44a3aa3.patch_0"), "mod");
            static void SevenZip(string from, string what, string archive)
            {
                var tar = OperatingSystem.IsLinux() ? "bsdtar" : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
                using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(tar) { ArgumentList = { "--format", "7zip", "-cf", archive, "-C", from, what }, UseShellExecute = false, CreateNoWindow = true })!;
                process.WaitForExit();
                if (process.ExitCode != 0) throw new InvalidOperationException("tar could not write " + archive);
            }
            var packArchive = Path.Combine(source, "Pack Download.7z");
            var modArchive = Path.Combine(source, "a-mod.7z");
            SevenZip(pack, ".", packArchive);
            SevenZip(source, "a-mod", modArchive);

            var (candidates, problems) = ModInstaller.Inspect(install, [packArchive, modArchive]);
            check(problems.Count == 0 && candidates.Select(c => c.Id).SequenceEqual(["pack.33333333-2222-3333-4444-555555555555", "a.sevenzip"]) && candidates.All(c => !c.IsZip),
                "Both are read: " + string.Join(" | ", problems));
            foreach (var candidate in candidates) ModInstaller.Add(install, candidate);
            var (mods, loadProblems) = ModCatalog.Load(install.Mods);
            check(loadProblems.Count == 0 && Directory.Exists(Path.Combine(install.Mods, "Pack Download")) && Directory.Exists(Path.Combine(install.Mods, "a-mod")) &&
                mods.Single(m => m.Cosmetic).PackFolders.Count == 2 && mods.Any(m => m.Id == "a.sevenzip"),
                "Copied as folders (the pack named after its archive) and loaded: " + string.Join(" | ", loadProblems));
            check(!Directory.EnumerateDirectories(install.Temp, "mod-unpacked-*").Any(), "The unpacked copies are gone.");
        });
    }
}
