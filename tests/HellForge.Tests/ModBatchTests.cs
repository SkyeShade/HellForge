using HellForge.Core;
using HellForge.Core.GameData;

// Several mods in one call (HellForge.Cli's mod-remove and mod-switch --ids, mod-add with several --file): every id is
// checked first, one refused id changes nothing, and one write of hellforge-mods.json switches them all.
static class ModBatchTests
{
    static string Manifest(string id) => $$"""{"format":1,"id":"{{id}}","name":"{{id}} mod","version":"1.0.0","gameplay":false}""";

    // A cosmetic mod: its manifest and one synthetic texture patch (a mod that changes nothing isn't loaded).
    static void WriteMod(string folder, string id)
    {
        Directory.CreateDirectory(Path.Combine(folder, "data"));
        File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), Manifest(id));
        var (archive, _, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
        {
            [new(ResourceTypes.Texture, 0x1234)] = new([1, 2, 3, 4], [], []),
            [new(ResourceTypes.Unit, 0x5678)] = new([5, 6, 7, 8], [], [])
        });
        File.WriteAllBytes(Path.Combine(folder, "data", "9ba626afa44a3aa3.patch_0"), archive);
    }

    static InstallLayout Install(string root, string name, params string[] ids)
    {
        var install = new InstallLayout(Path.Combine(root, name));
        install.Create("12345");
        foreach (var id in ids) WriteMod(Path.Combine(install.Mods, id.Replace('.', '-')), id);
        WriteMod(Path.Combine(install.Mods, "local-stats"), ModCatalog.LocalStatsId);
        return install;
    }

    static List<string> Ids(InstallLayout install)
    {
        var (mods, problems) = ModCatalog.Load(install.Mods);
        if (problems.Count > 0) throw new Exception("The test mods did not load: " + string.Join("; ", problems));
        return mods.Select(m => m.Id).ToList();
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A list of ids is read in order, and an empty list or an id named twice is refused", () =>
        {
            check(ModBatch.Ids(" a.one, b.two ,,c.three").SequenceEqual(["a.one", "b.two", "c.three"]), "The ids were not read in order.");
            foreach (var bad in new[] { "", " , ", "a.one,a.one" })
            {
                var refused = false;
                try { ModBatch.Ids(bad); }
                catch (ArgumentException) { refused = true; }
                check(refused, "Not refused: '" + bad + "'");
            }
        });

        test("Removing several mods deletes each one, and a missing or shipped one refuses the list with nothing deleted", () =>
        {
            var install = Install(root, "batch-remove", "test.one", "test.two", "test.three");
            var missing = false;
            try { ModBatch.Remove(install, ["test.one", "test.nope", "test.gone"]); }
            catch (ArgumentException error) { missing = error.Message.Contains("test.nope, test.gone"); }
            check(missing, "Missing ids were not all named in one refusal.");
            var shipped = false;
            try { ModBatch.Remove(install, ["test.one", ModCatalog.LocalStatsId]); }
            catch (InvalidDataException error) { shipped = error.Message.Contains("Nothing was removed"); }
            check(shipped, "Local Stats was not refused.");
            check(Ids(install).Count == 4, "A refused list removed mods: " + string.Join(", ", Ids(install)));

            var removed = ModBatch.Remove(install, ["test.three", "test.one"]);
            check(removed.Select(r => r.Id).SequenceEqual(["test.three", "test.one"]) && removed.All(r => r.Error == null && r.Path != null),
                "The outcomes are not one per mod, in order.");
            check(Ids(install).Order(StringComparer.Ordinal).SequenceEqual(["test.two", ModCatalog.LocalStatsId]), "Left: " + string.Join(", ", Ids(install)));
        });

        test("Switching several mods changes all of them with one write, and Local Stats or a missing id changes none", () =>
        {
            var install = Install(root, "batch-switch", "test.one", "test.two", "test.three");
            var off = ModBatch.Switch(install, ["test.one", "test.three"], false);
            check(off.SequenceEqual(["test.one", "test.three"]), "The switched ids are not the ones given.");
            var mods = ModCatalog.Load(install.Mods).Mods;
            check(!mods.Single(m => m.Id == "test.one").Enabled && !mods.Single(m => m.Id == "test.three").Enabled && mods.Single(m => m.Id == "test.two").Enabled,
                "The mods were not switched off.");

            var before = File.ReadAllText(Path.Combine(install.Mods, ModSettings.FileName));
            foreach (var refused in new[] { new[] { "test.one", ModCatalog.LocalStatsId }, ["test.one", "test.nope"] })
            {
                var stopped = false;
                try { ModBatch.Switch(install, refused, refused.Contains(ModCatalog.LocalStatsId) ? false : true); }
                catch (Exception error) when (error is ArgumentException or InvalidDataException) { stopped = true; }
                check(stopped, "Not refused: " + string.Join(", ", refused));
            }
            check(File.ReadAllText(Path.Combine(install.Mods, ModSettings.FileName)) == before, "A refused list changed hellforge-mods.json.");

            ModBatch.Switch(install, ["test.one", "test.three"], true);
            check(ModCatalog.Load(install.Mods).Mods.All(m => m.Enabled), "The mods were not switched back on.");
        });

        test("Adding several files puts every mod in, and a file that isn't a mod adds none", () =>
        {
            var install = Install(root, "batch-add");
            var source = Path.Combine(root, "batch-add-source");
            WriteMod(Path.Combine(source, "alpha"), "test.alpha");
            WriteMod(Path.Combine(source, "beta"), "test.beta");
            var notMod = Path.Combine(source, "readme.txt");
            File.WriteAllText(notMod, "not a mod");
            var alpha = Path.Combine(source, "alpha", ModCatalog.ManifestName);
            var beta = Path.Combine(source, "beta", ModCatalog.ManifestName);

            var refused = false;
            try { ModBatch.Add(install, [alpha, notMod, beta]); }
            catch (InvalidDataException error) { refused = error.Message.Contains("readme.txt") && error.Message.Contains("Nothing was added"); }
            check(refused, "A file that isn't a mod did not refuse the list.");
            check(Ids(install).SequenceEqual([ModCatalog.LocalStatsId]), "A refused list added mods: " + string.Join(", ", Ids(install)));

            var added = ModBatch.Add(install, [alpha, beta]);
            check(added.Count == 2 && added.All(a => a.Error == null && a.Installed != null), "Not every mod went in.");
            check(Ids(install).Order(StringComparer.Ordinal).SequenceEqual(["test.alpha", "test.beta", ModCatalog.LocalStatsId]),
                "Installed: " + string.Join(", ", Ids(install)));
        });
    }
}
