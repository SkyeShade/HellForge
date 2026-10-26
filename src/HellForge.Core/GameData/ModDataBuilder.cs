using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HellForge.Core.GameData;

// Builds a mod's data spec (DataSpecs) from the game's own data into patch files, in the game's mod format:
// <archive>.patch_<n> with its .gpu_resources and, when a resource has one, .stream. Each patch of the spec becomes one
// archive written for every bundle it names (n counts that bundle's earlier patches in the spec). Every resource the spec
// names is looked up first and all missing ones are reported together; nothing is written then.
public static class ModDataBuilder
{
    // Part of the cache key and the modlist digest: raise it whenever the same spec would build different bytes.
    // 2: a unit copy takes its base's physics under its own name; 3: and the physics names that unit inside it too;
    // 4: material colours land where the values are (no 16-byte rounding), and resources can come from Steam's install
    public const int Version = 4;

    // Where a physics resource names its unit (after the type hash at +0): the havok layer files physics by the name at
    // +0x08, not the archive's, and a unit finds its bodies by its own name there (exe 0x7EDE60, 0x788E90); +0x38 and
    // +0x40 repeat it. Every one of the game's 4821 physics resources is laid out so.
    static readonly int[] PhysicsNames = [0x08, 0x38, 0x40];

    public static List<string> Build(DataSpec spec, string modFolder, GameDataReader game, string output, GameDataReader? steam = null)
    {
        if (spec.UsesSteam && steam == null) throw new InvalidDataException("it takes resources from HELLDIVERS 2 as Steam has it installed, and Steam's copy was not found.");
        var (found, foundSteam) = FindSources(spec, game, steam);
        var read = new Dictionary<(ResourceKey, bool), ResourceParts>();
        var parents = new Dictionary<ulong, byte[]?>();
        ResourceParts Read(ResourceKey key, bool fromSteam)
        {
            if (read.TryGetValue((key, fromSteam), out var parts)) return parts;
            parts = fromSteam ? steam!.Read(foundSteam[key]) : game.Read(found[key]);
            if (fromSteam && key.Type == ResourceTypes.Material) parts = InGameLayout(key, parts);
            return read[(key, fromSteam)] = parts;
        }
        // A material from Steam's newer build, in the layout of its shader as the game copy has it (the shader's parameters
        // can change between builds); a material whose shader the game copy lacks stays as it is.
        ResourceParts InGameLayout(ResourceKey key, ResourceParts parts)
        {
            if (parts.Main.Length < 0x20 || BitConverter.ToUInt64(parts.Main, 0x18) is not (not 0 and var parent)) return parts;
            if (!parents.TryGetValue(parent, out var shader))
            {
                var at = game.Find([new(ResourceTypes.Material, parent)]);
                parents[parent] = shader = at.Count == 1 ? game.Read(at.Values.First()).Main : null;
            }
            if (shader == null) return parts;
            try { return parts with { Main = ResourceEdits.MaterialInLayout(parts.Main, shader) }; }
            catch (InvalidDataException error) { throw new InvalidDataException($"{key} (Steam): {error.Message}"); }
        }
        bool Has(ResourceKey key, bool fromSteam) => fromSteam ? foundSteam.ContainsKey(key) : found.ContainsKey(key);

        // A patch's resources: its includes, icons and copies, each key once.
        Dictionary<ResourceKey, ResourceParts> PatchResources(DataPatch patch, HashSet<ResourceKey> added)
        {
            var resources = new Dictionary<ResourceKey, ResourceParts>();
            foreach (var resource in patch.Resources)
                switch (resource)
                {
                    case IncludeResources include:
                        foreach (var id in include.Ids) AddResource(resources, include.Where, new(include.Type, id), Read(new(include.Type, id), include.Steam));
                        break;
                    case IconResource icon:
                        foreach (var (key, parts) in IconImage.Family(File.ReadAllBytes(Path.Combine(modFolder, icon.Png)), Hash(icon.Name), icon.Prepare))
                        {
                            AddResource(resources, icon.Where, key, parts);
                            added.Add(key);
                        }
                        break;
                    case CopyResource copy:
                        AddCopy(resources, copy, added);
                        break;
                }
            return resources;
        }
        // A copy under its own name, edited, and a unit copy's physics under that name too.
        void AddCopy(Dictionary<ResourceKey, ResourceParts> resources, CopyResource copy, HashSet<ResourceKey> added)
        {
            var copied = new ResourceKey(copy.Type, Hash(copy.Name));
            try { AddResource(resources, copy.Where, copied, Edit(copy, Read(new(copy.Type, copy.From), copy.Steam), modFolder)); }
            catch (InvalidDataException error) when (!error.Message.StartsWith(copy.Where, StringComparison.Ordinal))
            {
                throw new InvalidDataException($"{copy.Where} ({ResourceTypes.Describe(copy.Type)} 0x{copy.From:X16}): {error.Message}");
            }
            added.Add(copied);
            var physics = new ResourceKey(ResourceTypes.Physics, copy.From);
            if (copy.Type == ResourceTypes.Unit && Has(physics, copy.Steam))
            {
                var named = new ResourceKey(ResourceTypes.Physics, copied.Name);
                AddResource(resources, copy.Where, named, RenamePhysics(Read(physics, copy.Steam), copy.From, copied.Name));
                added.Add(named);
            }
        }

        var built = new List<(IReadOnlyList<string> Archives, byte[] Archive, byte[] Gpu, byte[] Stream)>();
        var added = new HashSet<ResourceKey>();
        foreach (var patch in spec.Patches)
        {
            var (archive, gpu, stream) = ResourceArchive.Write(PatchResources(patch, added));
            built.Add((patch.Archives, archive, gpu, stream));
        }
        // What a mod adds has a name of its own: a copy or icon may not stand in for one of the game's resources.
        var taken = game.Find(added).Keys.Select(k => k.ToString()).Order(StringComparer.Ordinal).ToList();
        if (taken.Count > 0) throw new InvalidDataException("these names are the game's own resources; give them names of the mod's: " + string.Join(", ", taken) + ".");
        return WriteFiles(output, built);
    }

    // Where each resource the spec reads is, in the game copy and in Steam's install. Every missing one is reported
    // together; nothing is read until all are found.
    static (Dictionary<ResourceKey, GameDataReader.Location> Game, Dictionary<ResourceKey, GameDataReader.Location> Steam) FindSources(DataSpec spec,
        GameDataReader game, GameDataReader? steam)
    {
        HashSet<ResourceKey> Sources(bool fromSteam) => spec.Patches.SelectMany(p => p.Resources).Where(r => r.Steam == fromSteam).SelectMany(r => r switch
        {
            // a unit copy's physics too, if it has one: the engine finds a unit's physics by the unit's name (its header names
            // its bones and state machine), so a copy without it gets a default body (a sentry: a huge box, and no aim)
            CopyResource copy => copy.Type == ResourceTypes.Unit ? [new ResourceKey(copy.Type, copy.From), new ResourceKey(ResourceTypes.Physics, copy.From)]
                : [new ResourceKey(copy.Type, copy.From)],
            IncludeResources include => include.Ids.Select(id => new ResourceKey(include.Type, id)),
            _ => []
        }).ToHashSet();
        var gameSources = Sources(false);
        var steamSources = Sources(true);
        var found = game.Find(gameSources);
        var foundSteam = steam?.Find(steamSources) ?? [];
        var missing = gameSources.Where(k => !found.ContainsKey(k) && k.Type != ResourceTypes.Physics).Select(k => k.ToString())
            .Concat(steamSources.Where(k => !foundSteam.ContainsKey(k) && k.Type != ResourceTypes.Physics).Select(k => $"{k} (Steam)"))
            .Order(StringComparer.Ordinal).ToList();
        if (missing.Count > 0) throw new InvalidDataException("not in the game data: " + string.Join(", ", missing) + ".");
        // An include from Steam stands under its own name, so it may not replace a resource the game copy has (that one's
        // bytes can differ between the builds): such a resource is included from the game.
        var steamIncludes = spec.Patches.SelectMany(p => p.Resources).OfType<IncludeResources>().Where(i => i.Steam)
            .SelectMany(i => i.Ids.Select(id => new ResourceKey(i.Type, id)));
        var shadowed = game.Find(steamIncludes).Keys.Select(k => k.ToString()).Order(StringComparer.Ordinal).ToList();
        if (shadowed.Count > 0) throw new InvalidDataException("the game copy has these already; include them from the game, not Steam: " + string.Join(", ", shadowed) + ".");
        var archives = spec.Patches.SelectMany(p => p.Archives).Distinct().Where(a => !game.HasArchive(a)).ToList();
        if (archives.Count > 0) throw new InvalidDataException("the game has no archive " + string.Join(", ", archives) + ".");
        return (found, foundSteam);
    }

    // A resource into a patch; a key it has already must come with the same bytes.
    static void AddResource(Dictionary<ResourceKey, ResourceParts> resources, string where, ResourceKey key, ResourceParts parts)
    {
        if (resources.TryGetValue(key, out var other) && !(other.Main.AsSpan().SequenceEqual(parts.Main)
            && other.Gpu.AsSpan().SequenceEqual(parts.Gpu) && other.Stream.AsSpan().SequenceEqual(parts.Stream)))
            throw new InvalidDataException($"{where}: the patch already has a different {key}.");
        resources[key] = parts;
    }

    // Each patch written for each archive it names, numbered per archive in the spec's order: the file names.
    static List<string> WriteFiles(string output, List<(IReadOnlyList<string> Archives, byte[] Archive, byte[] Gpu, byte[] Stream)> built)
    {
        Directory.CreateDirectory(output);
        var files = new List<string>();
        var next = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var (targets, archive, gpu, stream) in built)
            foreach (var target in targets)
            {
                var name = $"{target}.patch_{next.GetValueOrDefault(target)}";
                next[target] = next.GetValueOrDefault(target) + 1;
                File.WriteAllBytes(Path.Combine(output, name), archive);
                File.WriteAllBytes(Path.Combine(output, name + ".gpu_resources"), gpu);
                files.AddRange([name, name + ".gpu_resources"]);
                if (stream.Length == 0) continue;
                File.WriteAllBytes(Path.Combine(output, name + ".stream"), stream);
                files.Add(name + ".stream");
            }
        return files;
    }

    // A unit's physics for its copy: the copy's name where the base's stands (a sentry with the base's name there gets no
    // bodies at all: no aim, and a default field for collision).
    static ResourceParts RenamePhysics(ResourceParts source, ulong from, ulong to)
    {
        var main = source.Main.ToArray();
        if (main.Length < 0x48 || BitConverter.ToUInt64(main, 0) != ResourceTypes.Physics || BitConverter.ToUInt64(main, 0x08) != from)
            throw new InvalidDataException($"the physics of 0x{from:X16} is not laid out as reviewed (type, then its unit's name).");
        foreach (var at in PhysicsNames)
            if (BitConverter.ToUInt64(main, at) == from) BitConverter.TryWriteBytes(main.AsSpan(at), to);
        return new(main, source.Gpu, source.Stream);
    }

    static ResourceParts Edit(CopyResource copy, ResourceParts source, string modFolder)
    {
        var main = source.Main.ToArray();
        var gpu = source.Gpu;
        foreach (var (offset, value) in copy.Expect)
            if ((long)offset + 8 > main.Length || BitConverter.ToUInt64(main, offset) != value)
                throw new InvalidDataException($"0x{offset:X} does not hold 0x{value:X16}: not the resource the spec was written for.");
        foreach (var replace in copy.Replace) ResourceEdits.Replace(main, replace.From, replace.To, replace.At);
        if (copy.Curves is { } curves) EditCurves(main, curves);
        if (copy.Colours != null) ResourceEdits.SetMaterialColours(main, copy.Colours);
        if (copy.Lut != null) gpu = ResourceEdits.RecolourLut(main, gpu, copy.Lut);
        if (copy.Mesh is { } mesh)
        {
            if (source.Stream.Length != 0) throw new InvalidDataException("the model streams part of itself; HellForge cannot replace its mesh yet.");
            var (meshMain, meshGpu) = UnitMesh.Replace(main, gpu, UnitMesh.ReadObj(File.ReadAllText(Path.Combine(modFolder, mesh.Obj)), mesh.Scale, mesh.YUp));
            return new(meshMain, meshGpu, source.Stream);
        }
        return new(main, gpu, source.Stream);
    }

    // The warm colour curve keys recoloured, as many as the spec says when it says.
    static void EditCurves(byte[] main, CurveEdit curves)
    {
        var changed = ResourceEdits.RecolourCurves(main, curves.Preset == DataSpecs.WarmToBlue ? ResourceEdits.WarmToBlue : ResourceEdits.TowardsColour(curves.Colour!));
        if (changed == 0 || (curves.Keys is { } keys && changed != keys))
            throw new InvalidDataException($"{changed} warm colour curve keys found{(curves.Keys is { } k ? $", {k} expected" : "")}.");
    }

    static ulong Hash(string name) => EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name));
}

// Generated mod data for sessions: <installation>/temp/mod-data/<mod id>/<key>/, built from the installation's game copy
// when a session stages mods and reused while the key holds. The key covers the builder version, the supported game build,
// the game's bundles.nxa (which changes with any game data), the spec and its input files; a mod's older keys are removed
// when a new one is built. The cache is outside the mod folder, so it is never zipped or shared with the mod and never
// counted in the modlist code (the spec is, with the builder version; ModCatalog.Digest).
public static class ModDataCache
{
    const string Record = "hellforge-data-build.json";

    public static string Folder(InstallLayout install) => Path.Combine(install.Temp, "mod-data");

    // The mods with their generated data (GeneratedData), and why any could not be built; those are left out.
    public static (List<ModManifest> Mods, List<string> Problems) Prepare(InstallLayout install, IReadOnlyList<ModManifest> mods, Action<string>? log = null)
    {
        var ready = new List<ModManifest>();
        var problems = new List<string>();
        GameDataReader? game = null;
        try
        {
            foreach (var mod in mods)
            {
                if (!mod.HasDataSpec) { ready.Add(mod); continue; }
                try
                {
                    ready.Add(mod with { GeneratedData = Ensure(install, mod, () => game ??= new GameDataReader(Path.Combine(install.Game, "data")), log).Folder });
                }
                catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
                {
                    problems.Add($"{mod.Archive ?? Path.GetFileName(mod.Folder)}: its data could not be built: {error.Message}");
                }
            }
        }
        finally { game?.Dispose(); }
        return (ready, problems);
    }

    // The folder of a mod's generated patches, built first when there is none for the current key.
    public static (string Folder, IReadOnlyList<string> Files, bool Built) Ensure(InstallLayout install, ModManifest mod, Func<GameDataReader> game, Action<string>? log = null)
    {
        var spec = DataSpecs.Load(mod.Folder) ?? throw new InvalidDataException($"{DataSpecs.FileName} is missing.");
        var steamData = spec.UsesSteam ? SteamData() : null;
        var key = Key(spec, mod.Folder, Path.Combine(install.Game, "data"), steamData);
        var cache = Path.Combine(Folder(install), mod.Id);
        var target = Path.Combine(cache, key);
        if (File.Exists(Path.Combine(target, Record)))
            return (target, Directory.GetFiles(target).Select(Path.GetFileName).Where(f => ModCatalog.PatchFile().IsMatch(f!)).Order(StringComparer.Ordinal).ToList()!, false);
        log?.Invoke($"Building the data of {mod.Id} from the game data...");
        var staging = target + ".partial-" + Guid.NewGuid().ToString("N")[..8];
        try
        {
            using var steam = steamData == null ? null : new GameDataReader(steamData);
            var files = ModDataBuilder.Build(spec, mod.Folder, game(), staging, steam);
            File.WriteAllText(Path.Combine(staging, Record), JsonSerializer.Serialize(new { builder = ModDataBuilder.Version, key, mod = mod.Id, files },
                new JsonSerializerOptions { WriteIndented = true }) + "\n");
            try { Directory.Move(staging, target); }
            catch (IOException) when (Directory.Exists(target)) { Directory.Delete(staging, recursive: true); }
            foreach (var stale in Directory.GetDirectories(cache).Where(d => !string.Equals(d, target, StringComparison.OrdinalIgnoreCase)))
                try { Directory.Delete(stale, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            return (target, files, true);
        }
        catch
        {
            try { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); } catch (IOException) { }
            throw;
        }
    }

    // Steam's data folder, for a spec with resources from Steam; the build cannot be made without it.
    static string SteamData()
    {
        var steam = SteamLocator.FindGame() ?? throw new InvalidDataException("it takes resources from HELLDIVERS 2 in Steam, and Steam does not have the game installed.");
        return Path.Combine(steam.Report.Source, "data");
    }

    public static string Key(DataSpec spec, string modFolder, string gameData, string? steamData = null)
    {
        var text = new StringBuilder();
        text.Append("builder ").Append(ModDataBuilder.Version).Append('\n');
        text.Append("build ").Append(SupportedBuild.Current.BuildId).Append('\n');
        text.Append("index ").Append(FileHash(Path.Combine(gameData, GameDataReader.IndexFile))).Append('\n');
        if (steamData != null) text.Append("steam index ").Append(FileHash(Path.Combine(steamData, GameDataReader.IndexFile))).Append('\n');
        text.Append("spec ").Append(FileHash(Path.Combine(modFolder, DataSpecs.FileName))).Append('\n');
        foreach (var input in spec.Inputs) text.Append(input).Append(' ').Append(FileHash(Path.Combine(modFolder, input))).Append('\n');
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString())))[..16];
    }

    static string FileHash(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }
}
