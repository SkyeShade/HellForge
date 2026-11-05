namespace HellForge.Core.GameData;

// A live change's game data (docs/modding/live.md; native/mod-loader/live_data.c): what the mods it adds or replaces put in
// the game's data, as one hot package the running game loads by its id (a loose archive data/<16 hex>, as the game loads
// its own loose archives), and the resource overrides that make the game's lookups find it. A resource the game has not
// loaded keeps its name; one it has (a vanilla resource, or one a mod of the session staged) gets a name of its own in the
// package, and its name is overridden to it. A removed mod's resources the game had get their bytes back the same way:
// the bytes of the mods that stay (the last in load order), else the game's own. The game never unloads a package an
// object may hold, so each change writes a new one; the session's next staging removes them (ModStaging).
public static class LiveData
{
    // The package's id (null when the change needs none), its loader lines (moddata, modres; the session owns them: "-"),
    // and the files written into the game's data folder.
    public sealed record Result(string? PackageId, IReadOnlyList<string[]> Lines, IReadOnlyList<string> Files);

    // A mod's patches in load order: each a patch's main file (its .gpu_resources and .stream beside it).
    public sealed record ModData(string Id, IReadOnlyList<string> Patches);

    // keeps: the session's data mods that stay; adds: the change's (added and new versions); removes: the mods it takes out
    // (and replaced mods' old versions), each in load order. seed: unique to the change (its package's id comes from it).
    public static Result Build(string gameData, IReadOnlyList<ModData> keeps, IReadOnlyList<ModData> adds, IReadOnlyList<ModData> removes, string seed)
    {
        var added = Resources(adds);
        var kept = Resources(keeps);
        var leaving = Resources(removes);
        var gone = leaving.Keys.Where(k => !added.ContainsKey(k)).ToHashSet();
        if (added.Count == 0 && gone.Count == 0) return new(null, [], []);
        var package = EngineHash.Murmur64A(System.Text.Encoding.UTF8.GetBytes("hellforge.live:" + seed)) | 1;
        var id = package.ToString("x16", System.Globalization.CultureInfo.InvariantCulture);
        var loaded = Loaded(gameData, added.Keys.Concat(gone), kept, leaving);
        var resources = new Dictionary<ResourceKey, ResourceParts>();
        var lines = new List<string[]>();
        void Override(ResourceKey key, ResourceParts parts)
        {
            var own = new ResourceKey(key.Type, EngineHash.Murmur64A(System.Text.Encoding.UTF8.GetBytes($"{key.Name:x16}@{id}")));
            resources[own] = parts;
            lines.Add(["modres", "-", Hex(key.Type), Hex(key.Name), Hex(own.Name)]);
        }
        foreach (var (key, parts) in added)
        {
            if (loaded.Contains(key)) Override(key, parts);
            else resources[key] = parts; /* new to the game: found by its own name once the package is in */
        }
        using (var game = gone.Count > 0 ? new GameDataReader(gameData) : null)
        {
            var vanilla = game?.Find(gone) ?? [];
            foreach (var key in gone.OrderBy(k => k.Type).ThenBy(k => k.Name))
                if (kept.TryGetValue(key, out var parts)) Override(key, parts);
                else if (vanilla.TryGetValue(key, out var at)) Override(key, game!.Read(at));
                /* else the removed mod added it: nothing names it once its mod is gone, and it stays loaded */
        }
        if (resources.Count == 0) return new(null, lines, []);
        lines.Insert(0, ["moddata", "-", id]);
        return new(id, lines, Write(gameData, id, resources));
    }

    static string Hex(ulong value) => value.ToString("x16", System.Globalization.CultureInfo.InvariantCulture);

    // Every resource of the mods' patches, the last mod's where two have one.
    static Dictionary<ResourceKey, ResourceParts> Resources(IEnumerable<ModData> mods)
    {
        var found = new Dictionary<ResourceKey, ResourceParts>();
        foreach (var mod in mods)
            foreach (var patch in mod.Patches)
                foreach (var (key, parts) in ResourceArchive.Read(File.ReadAllBytes(patch), Sidecar(patch, ".gpu_resources"), Sidecar(patch, ".stream")))
                    found[key] = parts;
        return found;
    }

    static byte[] Sidecar(string patch, string extension) => File.Exists(patch + extension) ? File.ReadAllBytes(patch + extension) : [];

    // Which of the keys the running game has loaded by name: the game's own, and those of the session's data mods.
    static HashSet<ResourceKey> Loaded(string gameData, IEnumerable<ResourceKey> keys, IReadOnlyDictionary<ResourceKey, ResourceParts> kept,
        IReadOnlyDictionary<ResourceKey, ResourceParts> leaving)
    {
        var wanted = keys.ToHashSet();
        var loaded = wanted.Where(k => kept.ContainsKey(k) || leaving.ContainsKey(k)).ToHashSet();
        using var game = new GameDataReader(gameData);
        loaded.UnionWith(game.Find(wanted.Where(k => !loaded.Contains(k))).Keys);
        return loaded;
    }

    // The package in the game's data folder: the sidecars first (the engine opens a package's .stream as it loads it, and a
    // missing one crashes it), the main file last.
    static List<string> Write(string gameData, string id, IReadOnlyDictionary<ResourceKey, ResourceParts> resources)
    {
        var (archive, gpu, stream) = ResourceArchive.Write(resources);
        var main = Path.Combine(gameData, id);
        var files = new List<string> { main + ".gpu_resources", main + ".stream", main };
        File.WriteAllBytes(files[0], gpu);
        File.WriteAllBytes(files[1], stream);
        File.WriteAllBytes(main + ".partial", archive);
        File.Move(main + ".partial", main);
        return files;
    }
}
