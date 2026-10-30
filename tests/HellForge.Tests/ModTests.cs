using System.Buffers.Binary;
using HellForge.Core;
using HellForge.Core.GameData;

// Mods: manifests, the modlist digest, data staging and the native applier against a synthetic entity region in this
// process (the game's real region has the profile's size and framing; here only the bytes HellForge reads are filled in).
static class ModTests
{
    const string Example = """
        {"format":1,"id":"test.ammo","name":"Ammo","version":"1.0.0","gameplay":true,"native":{
          "networkFieldTypes":[{"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}],
          "componentWrites":[{"component":"DepositComponentData","resource":"0x056DE1C5E21E723E","fields":[
            {"name":"capacity","offset":0,"expect":1000,"value":10000},
            {"name":"start_amount","offset":4,"expect":1000,"value":10000},
            {"name":"refill_amount","offset":8,"expect":500,"value":5000}]}]}}
        """;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Engine name hashes and the entity profile match the reviewed build", () =>
        {
            check(EngineHash.Id32("remaining") == 0x69889752 && EngineHash.Id32("deposit_value") == 0x48B393D0, "IdString32 differs from the engine's.");
            var profile = EntityProfile.Current;
            var deposit = profile.Components["DepositComponentData"];
            check(profile.ExeSha256 == SupportedBuild.Current.ExeSha256 && profile.EntityRegionSize == 46616576 &&
                deposit.Index == 177 && deposit.RecordOffset == deposit.Indices * 16 && deposit.Stride == 152, "Entity profile not as exported.");
        });

        test("Mod manifests are validated and conflicting native edits are refused", () =>
        {
            var mods = Path.Combine(root, "mods-validation");
            void Write(string folder, string json) { Directory.CreateDirectory(Path.Combine(mods, folder)); File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), json); }
            Write("good", Example);
            Write("narrow", Example.Replace("\"test.ammo\"", "\"test.narrow\"").Replace("\"bits\":14", "\"bits\":8"));
            Write("unknown-component", Example.Replace("\"test.ammo\"", "\"test.unknown\"").Replace("DepositComponentData", "NoSuchComponentData"));
            Write("misaligned", Example.Replace("\"test.ammo\"", "\"test.misaligned\"").Replace("\"offset\":4", "\"offset\":6"));
            Write("conflict", Example.Replace("\"test.ammo\"", "\"test.conflict\""));
            Write("empty", """{"format":1,"id":"test.empty","name":"Empty","version":"1"}""");
            Write("bad-id", Example.Replace("\"test.ammo\"", "\"Test Ammo\""));
            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Count == 1 && loaded[0].Id == "test.ammo" && loaded[0].HasNative && loaded[0].ComponentWrites[0].Resource == 0x056DE1C5E21E723E,
                "Only the valid mod loads.");
            check(problems.Count == 6 && problems.Any(p => p.Contains("same native value")), "Every refusal is reported: " + string.Join(" | ", problems));
        });

        test("Code mods name a built DLL for an SDK this HellForge provides, and the loader plan lists them", () =>
        {
            var mods = Path.Combine(root, "mods-code");
            void Write(string folder, string json, bool dll = true)
            {
                Directory.CreateDirectory(Path.Combine(mods, folder));
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), json);
                if (dll) File.WriteAllText(Path.Combine(mods, folder, "mod.dll"), "synthetic dll " + folder);
            }
            Write("good", """{"format":1,"id":"test.code","name":"Code","version":"1","code":{"dll":"mod.dll","sdk":1}}""");
            Write("future", """{"format":1,"id":"test.future","name":"Future","version":"1","code":{"dll":"mod.dll","sdk":99}}""");
            Write("unbuilt", """{"format":1,"id":"test.unbuilt","name":"Unbuilt","version":"1","code":{"dll":"mod.dll","sdk":1}}""", dll: false);
            Write("escape", """{"format":1,"id":"test.escape","name":"Escape","version":"1","code":{"dll":"..\\mod.dll","sdk":1}}""");
            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Count == 1 && loaded[0].HasCode && loaded[0].CodeDll == "mod.dll", "Only the built mod for this SDK loads.");
            check(problems.Count == 3 && problems.Any(p => p.Contains($"SDK {ModCatalog.SdkVersion}")) && problems.Any(p => p.Contains("no sources")), "Refusals: " + string.Join(" | ", problems));
            var plan = ModLoaderPlan.Text(loaded, Path.Combine(root, "session"), EntityProfile.Current, "25480438").Split('\n');
            check(plan.Contains("build\t25480438") && plan.Contains("network_config\thelldivers2.exe\t1a10268") &&
                plan.Any(l => l.StartsWith("component\tDepositComponentData\t7287344\t", StringComparison.Ordinal)) &&
                plan.Contains($"mod\ttest.code\t{loaded[0].Folder}\t{Path.Combine(loaded[0].Folder, "mod.dll")}"), "The loader plan is incomplete.");
            var digest = ModCatalog.Digest(loaded);
            File.WriteAllText(Path.Combine(mods, "good", "mod.dll"), "rebuilt dll");
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) != digest, "A rebuilt DLL kept the modlist code.");
            var rebuilt = ModCatalog.Digest(ModCatalog.Load(mods).Mods);
            Directory.CreateDirectory(Path.Combine(mods, "good", ".idea"));
            File.WriteAllText(Path.Combine(mods, "good", ".idea", "workspace.xml"), "ide");
            File.WriteAllText(Path.Combine(mods, "good", "Folder.DotSettings.user"), "ide");
            Directory.CreateDirectory(Path.Combine(mods, "good", "build"));
            File.WriteAllText(Path.Combine(mods, "good", "build", "build.ninja"), "cmake");
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) == rebuilt, "Editor or build files changed the modlist code.");
        });

        test("Mods load from zips as well as folders, and a removed zip takes its mod with it", () =>
        {
            var mods = Path.Combine(root, "mods-zips");
            Directory.CreateDirectory(mods);
            const string manifest = """{"format":1,"id":"test.zipped","name":"Zipped","version":"1"}""";
            void Zip(string name, params (string Entry, string Text)[] files)
            {
                using var zip = System.IO.Compression.ZipFile.Open(Path.Combine(mods, name), System.IO.Compression.ZipArchiveMode.Create);
                foreach (var (entry, text) in files)
                    using (var writer = new StreamWriter(zip.CreateEntry(entry).Open())) writer.Write(text);
            }
            const string patch = "data/0123456789abcdef.patch_0";
            Zip("root.zip", (ModCatalog.ManifestName, manifest), (patch, "patch bytes"));
            Zip("nested.zip", ("Nested Mod/" + ModCatalog.ManifestName, manifest.Replace("test.zipped", "test.nested")), ("Nested Mod/" + patch, "nested patch"));
            Zip("escape.zip", ("../outside.txt", "no"), (ModCatalog.ManifestName, manifest.Replace("test.zipped", "test.escape")));
            Zip("empty.zip", ("readme.txt", "no manifest"));
            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Select(m => m.Id).SequenceEqual(["test.nested", "test.zipped"]), "Zipped mods: " + string.Join(", ", loaded.Select(m => m.Id)) + " problems: " + string.Join(" | ", problems));
            check(loaded.All(m => m.Archive != null && m.Folder.Contains(ModArchives.UnpackedFolder)), "A zipped mod does not name its zip or loads from outside the cache.");
            // the zips' hashes are kept for the next process (a session start does not read every zip again)
            var hashes = Path.Combine(mods, ModArchives.UnpackedFolder, ".zip-hashes.json");
            check(File.Exists(hashes) && File.ReadAllText(hashes).Contains("root.zip", StringComparison.Ordinal), "The zips' hashes were not kept.");
            check(problems.Count == 2 && problems.Any(p => p.StartsWith("escape.zip", StringComparison.Ordinal) && p.Contains("unsafe path")) &&
                problems.Any(p => p.StartsWith("empty.zip", StringComparison.Ordinal)), "Zip refusals: " + string.Join(" | ", problems));
            check(!File.Exists(Path.Combine(root, "outside.txt")) && !File.Exists(Path.Combine(mods, "outside.txt")), "An unsafe zip wrote outside its folder.");
            // The same content as a folder has the same modlist code as the zip.
            var folder = Path.Combine(root, "mods-zips-folder", "zipped");
            Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), manifest);
            Directory.CreateDirectory(Path.Combine(folder, "data"));
            File.WriteAllText(Path.Combine(folder, patch), "patch bytes");
            check(ModCatalog.Digest(ModCatalog.Load(Path.GetDirectoryName(folder)!).Mods) == ModCatalog.Digest(loaded.Where(m => m.Id == "test.zipped")),
                "A zip and a folder with the same files have different modlist codes.");
            // What a zipped mod writes into its unpacked folder as it runs (key bindings, a cache, even a shipped file
            // rewritten) is each player's own: friends still match. Its zip changing is not.
            var zippedCode = ModCatalog.Digest(loaded);
            foreach (var mod in loaded)
            {
                File.WriteAllText(Path.Combine(mod.Folder, "mod-bindings.ini"), "jump=space");
                File.WriteAllBytes(Path.Combine(Directory.CreateDirectory(Path.Combine(mod.Folder, "cache")).FullName, "preview-profile.bin"), [1, 2, 3]);
                File.WriteAllText(Path.Combine(mod.Folder, patch), "rewritten by the mod");
            }
            var (played, _) = ModCatalog.Load(mods);
            check(ModCatalog.Digest(played) == zippedCode, "Files a zipped mod wrote into its folder changed the modlist code.");
            check(ModCatalog.Digest(ModCatalog.Load(Path.GetDirectoryName(folder)!).Mods) == ModCatalog.Digest(played.Where(m => m.Id == "test.zipped")),
                "A zip and a folder with the same files differ once the zipped mod wrote files.");
            File.Delete(Path.Combine(mods, "nested.zip"));
            Zip("nested.zip", ("Nested Mod/" + ModCatalog.ManifestName, manifest.Replace("test.zipped", "test.nested")), ("Nested Mod/" + patch, "another patch"));
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) != zippedCode, "A zip with other files kept the modlist code.");
            File.Delete(Path.Combine(mods, "root.zip"));
            var (after, _) = ModCatalog.Load(mods);
            check(after.Select(m => m.Id).SequenceEqual(["test.nested"]) &&
                !Directory.GetDirectories(Path.Combine(mods, ModArchives.UnpackedFolder)).Any(d => Path.GetFileName(d).StartsWith("root-", StringComparison.Ordinal)),
                "A removed zip left its mod behind.");
        });

        test("HD2Runtime's mod template and an HD2Runtime mod's sources are refused with a message that says what they are", () =>
        {
            var mods = Path.Combine(root, "mods-not-mods");
            Directory.CreateDirectory(mods);
            void Zip(string name, params (string Entry, string Text)[] files)
            {
                using var zip = System.IO.Compression.ZipFile.Open(Path.Combine(mods, name), System.IO.Compression.ZipArchiveMode.Create);
                foreach (var (entry, text) in files)
                    using (var writer = new StreamWriter(zip.CreateEntry(entry).Open())) writer.Write(text);
            }
            Zip("HD2Runtime-ModTemplate-0.50.0.zip", ("hd2runtime.json", """{"format":1,"name":"My HD2 Mod","resource":"mods/your_name/my_hd2_mod","guid":"auto"}"""),
                ("build.cmd", "@echo off"), ("src/addon.lua", "return {}"));
            Zip("Cool Mod Source.zip", ("cool/hd2runtime.json", """{"format":1,"name":"Cool Mod","guid":"0b6c0e4e-0000-4000-8000-000000000001"}"""),
                ("cool/src/addon.lua", "return {}"));
            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Count == 0 && problems.Count == 2, "Problems: " + string.Join(" | ", problems));
            check(problems.Any(p => p.StartsWith("HD2Runtime-ModTemplate-0.50.0.zip", StringComparison.Ordinal) && p.Contains("mod template") && p.Contains("runtime zip")),
                "The template is not named as such: " + string.Join(" | ", problems));
            check(problems.Any(p => p.StartsWith("Cool Mod Source.zip", StringComparison.Ordinal) && p.Contains("source of an HD2Runtime mod (Cool Mod)")),
                "Unbuilt sources are not named as such: " + string.Join(" | ", problems));
        });

        test("Disabled mods are listed but not loaded, checked or counted", () =>
        {
            var mods = Path.Combine(root, "mods-switches");
            foreach (var (folder, id) in new[] { ("ammo", "test.ammo"), ("rival", "test.rival") })
            {
                Directory.CreateDirectory(Path.Combine(mods, folder));
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), Example.Replace("\"test.ammo\"", $"\"{id}\""));
            }
            var (both, conflict) = ModCatalog.Load(mods);
            check(both.Count == 1 && conflict.Count == 1, "Two enabled mods editing the same value both loaded.");
            ModSettings.SetEnabled(mods, "test.ammo", false);
            var (listed, problems) = ModCatalog.Load(mods);
            check(problems.Count == 0 && listed.Count == 2 && !listed.Single(m => m.Id == "test.ammo").Enabled &&
                ModCatalog.Active(listed).Select(m => m.Id).SequenceEqual(["test.rival"]), "A disabled mod still blocked or loaded.");
            var digest = ModCatalog.Digest(ModCatalog.Active(listed));
            ModSettings.SetEnabled(mods, "test.rival", false);
            check(ModCatalog.Digest(ModCatalog.Active(ModCatalog.Load(mods).Mods)) == "none" && digest != "none", "Disabled mods were counted.");
            ModSettings.SetEnabled(mods, "test.ammo", true);
            check(ModSettings.Disabled(mods).SetEquals(["test.rival"]), "Switch state not saved.");
        });

        test("The modlist digest covers gameplay mods and their exact files", () =>
        {
            var mods = Path.Combine(root, "mods-digest");
            Directory.CreateDirectory(Path.Combine(mods, "ammo"));
            File.WriteAllText(Path.Combine(mods, "ammo", ModCatalog.ManifestName), Example);
            Directory.CreateDirectory(Path.Combine(mods, "visual", "data"));
            File.WriteAllText(Path.Combine(mods, "visual", ModCatalog.ManifestName), """{"format":1,"id":"test.visual","name":"Visual","version":"1","gameplay":false}""");
            File.WriteAllText(Path.Combine(mods, "visual", "data", "9ba626afa44a3aa3.patch_0"), "texture");
            check(ModCatalog.Digest([]) == "none", "No mods digest as none.");
            var first = ModCatalog.Digest(ModCatalog.Load(mods).Mods);
            File.WriteAllText(Path.Combine(mods, "visual", "data", "9ba626afa44a3aa3.patch_0"), "other texture");
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) == first, "A cosmetic mod changed the digest.");
            File.WriteAllText(Path.Combine(mods, "ammo", ModCatalog.ManifestName), Example.Replace("\"value\":5000", "\"value\":4000"));
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) != first && first.Length == 32, "A gameplay change kept the digest.");
        });

        test("The modlist code leaves out documentation and pictures for a mod without code, and a synced mod still rehashes", () =>
        {
            var mods = Path.Combine(root, "mods-unloaded");
            var manifest = """
                {"format":1,"id":"test.docs","name":"Docs","version":"1","native":{"networkFieldTypes":[
                  {"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}]}}
                """;
            void Write(string folder, bool extras)
            {
                Directory.CreateDirectory(Path.Combine(mods, folder, "data"));
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), manifest);
                File.WriteAllText(Path.Combine(mods, folder, "data", "0123456789abcdef.patch_0"), "a unit patch");
                if (!extras) return;
                File.WriteAllText(Path.Combine(mods, folder, "README.md"), "read me");
                File.WriteAllText(Path.Combine(mods, folder, "LICENSE"), "licence");
                File.WriteAllText(Path.Combine(mods, folder, "CHANGELOG.txt"), "changes");
                Directory.CreateDirectory(Path.Combine(mods, folder, "docs"));
                File.WriteAllText(Path.Combine(mods, folder, "docs", "notes.txt"), "notes");
                File.WriteAllBytes(Path.Combine(mods, folder, "icon.png"), [1, 2, 3]);
            }
            Write("with", extras: true);
            Write("without", extras: false);
            var with = ModCatalog.Parse(Path.Combine(mods, "with"), EntityProfile.Current);
            var without = ModCatalog.Parse(Path.Combine(mods, "without"), EntityProfile.Current);
            check(ModCatalog.Digest([with]) == ModCatalog.Digest([without]), "A README, licence or picture changed the modlist code.");
            File.WriteAllText(Path.Combine(mods, "with", "data", "0123456789abcdef.patch_0"), "another unit patch");
            check(ModCatalog.Digest([ModCatalog.Parse(Path.Combine(mods, "with"), EntityProfile.Current)]) != ModCatalog.Digest([without]),
                "A change under data/ kept the modlist code.");
            File.WriteAllText(Path.Combine(mods, "with", "data", "0123456789abcdef.patch_0"), "a unit patch");
            // A data spec's PNG is an input ModDataBuilder reads: it stays in the code even though it is a picture.
            var spec = Path.Combine(mods, "spec");
            Directory.CreateDirectory(spec);
            File.WriteAllText(Path.Combine(spec, ModCatalog.ManifestName), """{"format":1,"id":"test.spec","name":"Spec","version":"1"}""");
            File.WriteAllText(Path.Combine(spec, DataSpecs.FileName), """
                {"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{"icon":"icon.png","name":"test/mod/icon"}]}]}
                """);
            File.WriteAllBytes(Path.Combine(spec, "icon.png"), [1, 2, 3]);
            var specDigest = ModCatalog.Digest([ModCatalog.Parse(spec, EntityProfile.Current)]);
            File.WriteAllBytes(Path.Combine(spec, "icon.png"), [4, 5, 6]);
            check(ModCatalog.Digest([ModCatalog.Parse(spec, EntityProfile.Current)]) != specDigest, "A data spec's own PNG input left the modlist code.");
            // Code can read any file of its folder: its README counts.
            var code = Path.Combine(mods, "code");
            Directory.CreateDirectory(code);
            File.WriteAllText(Path.Combine(code, ModCatalog.ManifestName), """{"format":1,"id":"test.code-docs","name":"Code","version":"1","code":{"dll":"mod.dll","sdk":1}}""");
            File.WriteAllText(Path.Combine(code, "mod.dll"), "synthetic dll");
            File.WriteAllText(Path.Combine(code, "README.md"), "one");
            var codeDigest = ModCatalog.Digest([ModCatalog.Parse(code, EntityProfile.Current)]);
            File.WriteAllText(Path.Combine(code, "README.md"), "two");
            check(ModCatalog.Digest([ModCatalog.Parse(code, EntityProfile.Current)]) != codeDigest, "A code mod's README left the modlist code.");
            // What a host shares is the counted files only; the joiner's copy rehashes to the host's content hash.
            Directory.Delete(Path.Combine(mods, "without"), recursive: true);
            var (loaded, problems) = ModCatalog.Load(mods);
            check(problems.Count == 0, "The mods load: " + string.Join(" | ", problems));
            var entry = ModSet.Of(ModCatalog.LoadOrder(loaded.Where(m => m.Id == "test.docs"))).Mods.Single();
            check(!entry.Files.Any(f => f.Path is "README.md" or "icon.png" or "LICENSE" || f.Path.EndsWith(".txt", StringComparison.Ordinal)) &&
                entry.Files.Any(f => f.Path == "data/0123456789abcdef.patch_0"), "The shared file list counts the wrong files.");
            var guestMods = Path.Combine(root, "mods-unloaded-guest");
            Directory.CreateDirectory(guestMods);
            var partial = SyncedStore.Partial(guestMods, entry.Id, entry.Hash);
            foreach (var file in entry.Files)
            {
                var target = Path.Combine(partial, file.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(Path.Combine(entry.Folder!, file.Path), target);
            }
            var installed = SyncedStore.Install(guestMods, entry, partial, (_, _) => true);
            check(SyncedStore.Find(guestMods, entry.Id, entry.Hash) == installed &&
                ModSets.ContentHash(ModCatalog.Parse(installed, EntityProfile.Current)) == entry.Hash,
                "A synced mod with a README and an icon does not rehash to the host's content hash.");
        });

        test("Co-op tiers: stats-only mods are per-player, and explicit coop tiers follow the manifest", () =>
        {
            var mods = Path.Combine(root, "mods-coop");
            const string stats = """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}}}""";
            void Write(string folder, string manifest, bool dll = false)
            {
                Directory.CreateDirectory(Path.Combine(mods, folder));
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), manifest);
                File.WriteAllText(Path.Combine(mods, folder, StatsData.FileName), stats);
                if (dll) File.WriteAllText(Path.Combine(mods, folder, "mod.dll"), "synthetic dll");
            }
            Write("stats", """{"format":1,"id":"test.stats-only","name":"Stats","version":"1","gameplay":true}""");
            var perPlayer = ModCatalog.Parse(Path.Combine(mods, "stats"), EntityProfile.Current);
            check(perPlayer.Coop == CoopTier.PerPlayer && !ModCatalog.Counted(perPlayer) && ModCatalog.Digest([perPlayer]) == "none" &&
                ModSet.Of([perPlayer]).Mods.Single().Tier == CoopTier.PerPlayer, "A stats-only mod is not per-player or is missing from the mod set.");
            Write("stats-code", """{"format":1,"id":"test.stats-code","name":"Stats code","version":"1","code":{"dll":"mod.dll","sdk":1}}""", dll: true);
            var withCode = ModCatalog.Parse(Path.Combine(mods, "stats-code"), EntityProfile.Current);
            check(withCode.Coop == CoopTier.Shared && ModCatalog.Counted(withCode) && ModCatalog.Digest([withCode]) != "none",
                "Adding a DLL did not make a stats mod shared.");
            Write("stats-shared", """{"format":1,"id":"test.stats-shared","name":"Stats shared","version":"1","coop":"shared"}""");
            check(ModCatalog.Parse(Path.Combine(mods, "stats-shared"), EntityProfile.Current).Coop == CoopTier.Shared,
                "\"coop\": \"shared\" did not put a stats mod back in the digest.");
            Write("stats-bad-coop", """{"format":1,"id":"test.stats-bad","name":"Bad","version":"1","coop":"sometimes"}""");
            reject(() => ModCatalog.Parse(Path.Combine(mods, "stats-bad-coop"), EntityProfile.Current));
            Write("stats-spec", """{"format":1,"id":"test.stats-spec","name":"Stats spec","version":"1","coop":"per-player"}""");
            File.WriteAllText(Path.Combine(mods, "stats-spec", DataSpecs.FileName),
                """{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{"type":"texture","include":["0x1"]}]}]}""");
            reject(() => ModCatalog.Parse(Path.Combine(mods, "stats-spec"), EntityProfile.Current));
            Write("stats-wire", """
                {"format":1,"id":"test.stats-wire","name":"Stats wire","version":"1","coop":"per-player","native":{"networkFieldTypes":[
                  {"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}]}}
                """);
            reject(() => ModCatalog.Parse(Path.Combine(mods, "stats-wire"), EntityProfile.Current));
        });

        test("Client-only data patches are per-player; a unit, another type or a truncated patch is shared", () =>
        {
            var mods = Path.Combine(root, "mods-patch-tiers");
            void WritePatch(string folder, string id, bool truncated, params ulong[] types)
            {
                var data = Path.Combine(mods, folder, "data");
                Directory.CreateDirectory(data);
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName),
                    $$"""{"format":1,"id":"{{id}}","name":"{{id}}","version":"1"}""");
                var resources = new Dictionary<ResourceKey, ResourceParts>();
                for (var i = 0; i < types.Length; i++) resources[new(types[i], (ulong)(0x100 + i))] = new([1, 2, 3, 4], [], []);
                var (archive, gpu, stream) = ResourceArchive.Write(resources);
                var patch = Path.Combine(data, "0123456789abcdef.patch_0");
                File.WriteAllBytes(patch, truncated ? archive[..40] : archive);
                File.WriteAllBytes(patch + ".gpu_resources", gpu);
                File.WriteAllBytes(patch + ".stream", stream);
            }
            ModManifest Parse(string folder) => ModCatalog.Parse(Path.Combine(mods, folder), EntityProfile.Current);

            WritePatch("texture", "test.texture-only", truncated: false, ResourceTypes.Texture);
            var texture = Parse("texture");
            check(texture.Coop == CoopTier.PerPlayer && ModCatalog.Digest([texture]) == "none" && ModSet.Of([texture]).Mods.Single().Tier == CoopTier.PerPlayer,
                "A texture-only patch is not per-player or is missing from the mod set.");
            check(Parse("texture").Coop == CoopTier.PerPlayer, "The cached patch answer changed.");
            WritePatch("client-only", "test.client-only", truncated: false, ResourceTypes.Texture, ResourceTypes.Material,
                ResourceTypes.Hash("wwise_bank"), ResourceTypes.Hash("wwise_stream"), ResourceTypes.Hash("wwise_dep"),
                ResourceTypes.Hash("wwise_metadata"), ResourceTypes.Hash("wwise_properties"), ResourceTypes.Hash("font"), ResourceTypes.Hash("strings"));
            check(Parse("client-only").Coop == CoopTier.PerPlayer, "A picture, sound, font or strings patch is not per-player.");
            WritePatch("unit", "test.unit-patch", truncated: false, ResourceTypes.Texture, ResourceTypes.Unit);
            var unit = Parse("unit");
            check(unit.Coop == CoopTier.Shared && ModCatalog.Counted(unit) && ModCatalog.Digest([unit]) != "none",
                "A unit in a texture patch left the mod per-player.");
            WritePatch("truncated", "test.truncated", truncated: true, ResourceTypes.Texture);
            check(Parse("truncated").Coop == CoopTier.Shared, "A patch that does not parse left the mod per-player.");
            WritePatch("lua", "test.lua-patch", truncated: false, ResourceTypes.Texture, ResourceTypes.Lua);
            check(Parse("lua").Coop == CoopTier.Shared, "A patch holding Lua left the mod per-player.");
            WritePatch("explicit-shared", "test.explicit-shared", truncated: false, ResourceTypes.Texture);
            File.WriteAllText(Path.Combine(mods, "explicit-shared", ModCatalog.ManifestName),
                """{"format":1,"id":"test.explicit-shared","name":"Explicit","version":"1","coop":"shared"}""");
            check(Parse("explicit-shared").Coop == CoopTier.Shared, "\"coop\": \"shared\" did not win over a client-only patch.");
        });

        test("Data mods are staged after existing patches and removed exactly", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "staging-install"));
            install.Create("12345");
            var data = Path.Combine(install.Game, "data");
            Directory.CreateDirectory(data);
            File.WriteAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_0"), "installed by hand");
            var mods = Path.Combine(install.Root, "mods");
            foreach (var (folder, id) in new[] { ("b", "test.b"), ("a", "test.a") })
            {
                Directory.CreateDirectory(Path.Combine(mods, folder, "data"));
                File.WriteAllText(Path.Combine(mods, folder, ModCatalog.ManifestName), $$"""{"format":1,"id":"{{id}}","name":"{{id}}","version":"1"}""");
                File.WriteAllText(Path.Combine(mods, folder, "data", "9ba626afa44a3aa3.patch_0"), id);
                File.WriteAllText(Path.Combine(mods, folder, "data", "9ba626afa44a3aa3.patch_0.gpu_resources"), id + " gpu");
            }
            var staged = ModStaging.Stage(install, ModCatalog.Load(mods).Mods);
            check(staged.SequenceEqual(["9ba626afa44a3aa3.patch_1", "9ba626afa44a3aa3.patch_1.gpu_resources", "9ba626afa44a3aa3.patch_2", "9ba626afa44a3aa3.patch_2.gpu_resources"]),
                "Staged names: " + string.Join(", ", staged));
            check(File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_1")) == "test.a" && File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_2")) == "test.b",
                "Mods are staged in id order.");
            ModStaging.Stage(install, ModCatalog.Load(mods).Mods);
            check(!File.Exists(Path.Combine(data, "9ba626afa44a3aa3.patch_3")), "Staging again duplicated files.");
            // A session after one with the same mods copies nothing again (a tester's packs: ~3 GB each session start).
            check(ModStaging.LastCounts == (0, 4), $"Unchanged mods were copied again: {ModStaging.LastCounts}.");
            // A changed patch is copied again, the others stay; one the player changed in game/data is put back.
            File.WriteAllText(Path.Combine(mods, "a", "data", "9ba626afa44a3aa3.patch_0"), "test.a, version 2");
            File.WriteAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_2.gpu_resources"), "tampered");
            staged = ModStaging.Stage(install, ModCatalog.Load(mods).Mods);
            check(ModStaging.LastCounts == (2, 2) && File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_1")) == "test.a, version 2" &&
                File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_2.gpu_resources")) == "test.b gpu", $"A change was not staged: {ModStaging.LastCounts}.");
            // A mod no longer loaded takes its staged files with it; the rest renumber as they must.
            staged = ModStaging.Stage(install, ModCatalog.Load(mods).Mods.Where(m => m.Id == "test.b").ToList());
            check(staged.SequenceEqual(["9ba626afa44a3aa3.patch_1", "9ba626afa44a3aa3.patch_1.gpu_resources"]) &&
                File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_1")) == "test.b" && !File.Exists(Path.Combine(data, "9ba626afa44a3aa3.patch_2")),
                "A removed mod's files stayed, or the others were not renumbered: " + string.Join(", ", staged));
            ModStaging.Stage(install, ModCatalog.Load(mods).Mods);
            ModStaging.Unstage(install);
            check(Directory.GetFiles(data).Select(Path.GetFileName).SequenceEqual(["9ba626afa44a3aa3.patch_0"]) &&
                File.ReadAllText(Path.Combine(data, "9ba626afa44a3aa3.patch_0")) == "installed by hand", "Unstaging touched other files.");
            File.WriteAllText(Path.Combine(mods, "a", "data", "notes.txt"), "not a patch");
            check(ModCatalog.Load(mods).Problems.Count == 1, "A stray data file was accepted.");
        });

    }

    static string WriteMod(string root, string name, string json)
    {
        var folder = Path.Combine(root, "mods-" + name, name);
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), json);
        return folder;
    }
}
