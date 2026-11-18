using System.IO.Compression;
using HellForge.Core;

// Mods inside what players download (ModSources): wrapper folders, zips inside zips, folders of mods in mods/, and the
// files a nested mod ships, which friends compare.
static class ModSourcesTests
{
    static string Manifest(string id, bool gameplay = false) =>
        $$"""{"format":1,"id":"{{id}}","name":"{{id}}","version":"1","gameplay":{{(gameplay ? "true" : "false")}}}""";

    const string Patch = "data/0123456789abcdef.patch_0";

    // A mod's manifest and data patch (a mod that changes nothing is refused), in the folder prefix names in a zip.
    static List<(string Entry, object Content)> Mod(string prefix, string id, string manifest = ModCatalog.ManifestName) =>
        [(prefix + manifest, Manifest(id)), (prefix + Patch, id)];

    // A gameplay mod's files, in the folder prefix names in a zip.
    static List<(string Entry, object Content)> Files(string prefix) =>
        [(prefix + ModCatalog.ManifestName, Manifest("test.shipped", gameplay: true)), (prefix + Patch, "patch bytes"), (prefix + "docs/notes.txt", "notes")];

    // A zip's bytes: each entry's text, or the bytes of a zip inside it.
    static byte[] Zip(params IEnumerable<(string Entry, object Content)> entries)
    {
        using var memory = new MemoryStream();
        using (var zip = new ZipArchive(memory, ZipArchiveMode.Create, leaveOpen: true))
            foreach (var (entry, content) in entries)
            {
                using var stream = zip.CreateEntry(entry).Open();
                stream.Write(content is byte[] bytes ? bytes : System.Text.Encoding.UTF8.GetBytes((string)content));
            }
        return memory.ToArray();
    }

    static void Write(string path, byte[] bytes)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, bytes);
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A zip's mod loads from its root, wrapper folders with readmes beside them, and zips inside it; two mods or none are refused", () =>
        {
            var mods = Directory.CreateDirectory(Path.Combine(root, "sources-zips")).FullName;
            Write(Path.Combine(mods, "root.zip"), Zip(Mod("", "test.root")));
            Write(Path.Combine(mods, "wrapper.zip"), Zip(Mod("Wrap v1/", "test.wrap")));
            Write(Path.Combine(mods, "ModName v1.2.zip"), Zip([("readme.txt", "read me"), ("ModName v1.2/README.md", "# read me"),
                ("ModName v1.2/preview.png", "png"), .. Mod("ModName v1.2/ModName/", "test.two-levels"), ("__MACOSX/ModName v1.2/._README.md", "mac")]));
            Write(Path.Combine(mods, "zip-in-zip.zip"), Zip(("readme.txt", "read me"), ("ModName.zip", Zip(Mod("", "test.zip-in-zip")))));
            Write(Path.Combine(mods, "zip-in-wrapper.zip"), Zip(("Download/README.md", "read me"),
                ("Download/ModName.zip", Zip([.. Mod("ModName/", "test.zip-in-wrapper"), ("ModName/LICENSE", "MIT")]))));
            Write(Path.Combine(mods, "two-inner.zip"), Zip(("a.zip", Zip(("b.zip", Zip(Mod("", "test.two-inner", ModCatalog.LegacyManifestName)))))));
            Write(Path.Combine(mods, "three-inner.zip"), Zip(("a.zip", Zip(("b.zip", Zip(("c.zip", Zip(Mod("", "test.three-inner")))))))));
            Write(Path.Combine(mods, "two-mods.zip"), Zip([.. Mod("Pack/ModA/", "test.a"), .. Mod("Pack/ModB/", "test.b")]));
            Write(Path.Combine(mods, "depth-4.zip"), Zip(Mod("a/b/c/d/", "test.depth-4")));
            Write(Path.Combine(mods, "depth-5.zip"), Zip(Mod("a/b/c/d/e/", "test.depth-5")));
            Write(Path.Combine(mods, "installer.zip"), Zip([("install.bat", "copy"), .. Mod("Mod/", "test.installer")]));
            Write(Path.Combine(mods, "inner-unsafe.zip"), Zip(("Mod.zip", Zip([("../escaped.txt", "no"), .. Mod("", "test.inner-unsafe")]))));

            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Select(m => m.Id).Order(StringComparer.Ordinal).SequenceEqual(["test.depth-4", "test.root", "test.two-inner", "test.two-levels",
                "test.wrap", "test.zip-in-wrapper", "test.zip-in-zip"]), "Loaded: " + string.Join(", ", loaded.Select(m => m.Id)) + " | " + string.Join(" | ", problems));
            check(loaded.All(m => m.Archive != null && m.Folder.Contains(ModArchives.UnpackedFolder)), "A zipped mod loads from outside the cache.");
            var nested = loaded.Single(m => m.Id == "test.zip-in-wrapper");
            check(Path.GetFileName(nested.Folder) == "ModName" && File.Exists(Path.Combine(nested.Folder, "LICENSE")) &&
                File.Exists(Path.Combine(Path.GetDirectoryName(nested.Folder)!, ModCatalog.ManifestName)) == false,
                "The inner zip's files are in the unpacked copy: " + nested.Folder);
            string Problem(string zip) => problems.SingleOrDefault(p => p.StartsWith(zip + ":", StringComparison.Ordinal)) ?? "";
            check(Problem("two-mods.zip").Contains("2 mods (Pack/ModA, Pack/ModB)"), "Two mods are named: " + Problem("two-mods.zip"));
            check(Problem("three-inner.zip").Contains("more than 2 deep"), "Three zips deep: " + Problem("three-inner.zip"));
            check(Problem("depth-5.zip").Contains("4 levels down") && Problem("installer.zip").Contains(ModCatalog.ManifestName),
                "Where HellForge looked: " + Problem("depth-5.zip") + " | " + Problem("installer.zip"));
            check(Problem("inner-unsafe.zip").Contains("unsafe path") && !File.Exists(Path.Combine(mods, ModArchives.UnpackedFolder, "escaped.txt")) &&
                !File.Exists(Path.Combine(mods, "escaped.txt")), "An unsafe inner zip: " + Problem("inner-unsafe.zip"));
            check(problems.Count == 5, "Refusals: " + string.Join(" | ", problems));
            check(!Directory.EnumerateDirectories(Path.Combine(mods, ModArchives.UnpackedFolder)).Any(d => d.Contains(".partial-") || d.Contains(".inner-")),
                "Staging folders are left in the cache.");

            // a copy an older HellForge unpacked, holding only the inner zip, is unpacked again
            var stale = Path.Combine(mods, ModArchives.UnpackedFolder, Path.GetFileName(loaded.Single(m => m.Id == "test.zip-in-zip").Folder));
            Directory.Delete(stale, recursive: true);
            Directory.CreateDirectory(stale);
            File.WriteAllBytes(Path.Combine(stale, "ModName.zip"), Zip(Mod("", "test.zip-in-zip")));
            check(ModCatalog.Load(mods).Mods.Any(m => m.Id == "test.zip-in-zip") && File.Exists(Path.Combine(stale, ModCatalog.ManifestName)), "The old copy was not opened.");
        });

        test("A nested or zip-in-zip mod ships the same files as the mod zipped flat, and has its modlist code", () =>
        {
            string Code(string name, byte[] zip)
            {
                var mods = Directory.CreateDirectory(Path.Combine(root, "sources-shipped-" + name)).FullName;
                Write(Path.Combine(mods, name + ".zip"), zip);
                var (loaded, problems) = ModCatalog.Load(mods);
                check(loaded.Count == 1 && problems.Count == 0, name + ": " + string.Join(" | ", problems));
                var files = ModArchives.Shipped(name + ".zip", loaded[0].Folder);
                check(files != null && files.Keys.Order(StringComparer.Ordinal).SequenceEqual([Patch, "docs/notes.txt", ModCatalog.ManifestName]),
                    name + " ships: " + string.Join(", ", files?.Keys ?? []));
                // what the mod writes into its unpacked folder as it runs is each player's own
                File.WriteAllText(Path.Combine(loaded[0].Folder, "settings.ini"), "mine");
                return ModCatalog.Digest(ModCatalog.Load(mods).Mods);
            }
            var flat = Code("flat", Zip(Files("")));
            check(flat != "none", "The gameplay mod counts.");
            check(Code("nested", Zip([("README.md", "read me"), .. Files("Mod v1/Mod/")])) == flat, "A mod in wrapper folders has another modlist code.");
            check(Code("inner", Zip(("readme.txt", "read me"), ("Download/Mod.zip", Zip(Files("Mod/"))))) == flat, "A mod in a zip inside a zip has another modlist code.");
        });

        test("Folders in mods/ that wrap a mod or hold several load them, and removing one keeps the others", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "sources-folders"));
            install.Create("25480438");
            void Put(string relative, string id)
            {
                var folder = Directory.CreateDirectory(Path.Combine(install.Mods, relative)).FullName;
                File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), Manifest(id));
                Write(Path.Combine(folder, Patch), System.Text.Encoding.UTF8.GetBytes(id));
            }
            Put("plain", "test.plain");
            Put(Path.Combine("Wrap v1", "Wrap"), "test.wrapped");
            File.WriteAllText(Path.Combine(install.Mods, "Wrap v1", "README.md"), "read me");
            Put(Path.Combine("My Mods", "ModA"), "test.mod-a");
            Put(Path.Combine("My Mods", "ModB v2", "ModB"), "test.mod-b");
            Put(Path.Combine("My Mods", ".stash", "ModC"), "test.hidden");
            Put(Path.Combine("a", "b", "c", "d", "e", "f"), "test.too-deep");
            // a mod that lost its manifest is not a pack of its data folder
            Directory.CreateDirectory(Path.Combine(install.Mods, "Broken", "data"));
            File.WriteAllText(Path.Combine(install.Mods, "Broken", "mod.dll"), "dll");
            File.WriteAllText(Path.Combine(install.Mods, "Broken", Patch), "patch");
            Write(Path.Combine(install.Mods, "Only Zip", "Thing.zip"), Zip((ModCatalog.ManifestName, Manifest("test.only-zip"))));

            var (loaded, problems) = ModCatalog.Load(install.Mods);
            check(loaded.Select(m => m.Id).Order(StringComparer.Ordinal).SequenceEqual(["test.mod-a", "test.mod-b", "test.plain", "test.wrapped"]),
                "Loaded: " + string.Join(", ", loaded.Select(m => m.Id)));
            check(problems.SequenceEqual(["Only Zip: it holds Thing.zip and no mod. A zip loads from the mods folder itself: move it there."]),
                "Problems: " + string.Join(" | ", problems));
            ModManifest Of(string id) => loaded.Single(m => m.Id == id);
            check(ModInstaller.InstalledPath(install, Of("test.plain")) == Path.Combine(install.Mods, "plain") &&
                ModInstaller.InstalledPath(install, Of("test.wrapped")) == Path.Combine(install.Mods, "Wrap v1") &&
                ModInstaller.InstalledPath(install, Of("test.mod-a")) == Path.Combine(install.Mods, "My Mods", "ModA") &&
                ModInstaller.InstalledPath(install, Of("test.mod-b")) == Path.Combine(install.Mods, "My Mods", "ModB v2"),
                "What removing deletes: " + string.Join(", ", loaded.Select(m => ModInstaller.InstalledPath(install, m))));
            ModInstaller.Remove(install, Of("test.wrapped"));
            ModInstaller.Remove(install, Of("test.mod-a"));
            check(!Directory.Exists(Path.Combine(install.Mods, "Wrap v1")) && !Directory.Exists(Path.Combine(install.Mods, "My Mods", "ModA")) &&
                Directory.Exists(Path.Combine(install.Mods, "My Mods", "ModB v2", "ModB")) && Directory.Exists(Path.Combine(install.Mods, "My Mods", ".stash")),
                "Removing took the wrapper whole and only the one mod of the folder of mods.");
            check(ModCatalog.Load(install.Mods).Mods.Select(m => m.Id).Order(StringComparer.Ordinal).SequenceEqual(["test.mod-b", "test.plain"]), "Left after removing.");
            reject(() => ModInstaller.Remove(install, Of("test.mod-a") with { Folder = Path.Combine(install.Mods, "My Mods", ".stash", "ModC") }));
            check(Directory.Exists(Path.Combine(install.Mods, "My Mods", ".stash", "ModC")), "A dot folder was deleted.");
        });

        test("A cosmetic pack in wrapper folders or in a zip inside a zip is a pack: dropped into mods/, it becomes its own folder", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "sources-packs"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            const string pack = """{"Version":1,"Guid":"22222222-3333-4444-5555-666666666666","Name":"Deep Pack","Options":[{"Name":"Red","Include":["Red"]}]}""";
            var wrapped = Path.Combine(install.Mods, "Deep Pack v3.zip");
            Write(wrapped, Zip(("readme.txt", "read me"), ("Deep Pack v3/screenshot.jpg", "jpg"), ("Deep Pack v3/Deep Pack/" + CosmeticPacks.ManifestName, pack),
                ("Deep Pack v3/Deep Pack/Red/9ba626afa44a3aa3.patch_0", "red")));
            var inner = Path.Combine(root, "sources-packs-inner.zip");
            Write(inner, Zip(("Bare.zip", Zip(("9ba626afa44a3aa3.patch_0", "bare")))));
            check(ModInstaller.ZipHoldsPack(wrapped) && ModInstaller.ZipHoldsPack(inner), "Not seen as packs.");
            var gameplay = Path.Combine(root, "sources-packs-gameplay.zip");
            Write(gameplay, Zip(("Mod.zip", Zip(("Mod/" + ModCatalog.ManifestName, Manifest("test.gameplay", gameplay: true)), ("Mod/" + Patch, "patch")))));
            check(!ModInstaller.ZipHoldsPack(gameplay), "A gameplay mod in a zip inside a zip is a pack.");
            var notes = ModInstaller.TakeDropped(install);
            var (loaded, problems) = ModCatalog.Load(install.Mods);
            check(notes.Count == 0 && problems.Count == 0 && !File.Exists(wrapped) && CosmeticPacks.IsPack(Path.Combine(install.Mods, "Deep Pack")) &&
                loaded.Single().Id == "pack.22222222-3333-4444-5555-666666666666", "Dropped: " + string.Join(" | ", notes.Concat(problems)));
            check(!Directory.EnumerateDirectories(install.Temp).Any(d => Path.GetFileName(d).StartsWith("mod-", StringComparison.Ordinal)), "The unpacked download is left in temp.");
        });
    }
}
