using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core.GameData;

// A mod's data spec, hellforge-data.json in its folder: the game data patches HellForge generates for the mod from the
// player's own game files (ModDataBuilder), so a mod never ships game bytes. Each patch lists the archives it patches
// (the bundles that load with what it adds) and its resources:
//   copy     a game resource under the mod's own name, optionally edited: references replaced, colour curves,
//            material colours, a colour lookup texture recoloured;
//   include  game resources unchanged under their own names (what a copy needs loaded with it);
//   icon, texture, sounds  a stratagem icon, a texture (TextureImage) or a sound bank (SoundBank) from the mod's files.
// A copy or include reads the player's game copy, or with "source": "steam" the newer build of the player's own Steam install,
// or with "source": {"build": ...} a past build's (cut content), fetched from Steam once (BuildResources). An include from Steam or a past build must be new to the copy.
//   mesh     (on a unit copy) the mod's own geometry from an OBJ in the mod folder, in every render and shadow mesh.
// docs/modding/data.md "Data specs" describes the format.
public sealed record DataSpec(IReadOnlyList<DataPatch> Patches, IReadOnlyList<string> Inputs)
{
    public bool UsesSteam => Patches.Any(p => p.Resources.Any(r => r.Steam));
    // The past builds its resources come from ("<depot>:<manifest>", AssetMigration.GameBuilds), in order.
    public IReadOnlyList<string> Builds => [.. Patches.SelectMany(p => p.Resources).Select(r => r.Build).OfType<string>().Distinct().Order(StringComparer.Ordinal)];
}
// A patch for game archives, or (Package) a package of the mod's own: a whole archive of its own id, loaded at session
// start or only when asked for (load_package, or a record that names it).
public sealed record DataPatch(IReadOnlyList<string> Archives, IReadOnlyList<DataResource> Resources, ModDataPackage? Package = null);
public sealed record ModDataPackage(string Name, ulong Id, bool Session);
public abstract record DataResource(string Where)
{
    public bool Steam { get; init; }
    // A past build it is read from (BuildResources), or null.
    public string? Build { get; init; }
}
public sealed record CopyResource(string Where, ulong Type, ulong From, string Name, IReadOnlyDictionary<int, ulong> Expect,
    IReadOnlyList<Replacement> Replace, CurveEdit? Curves, IReadOnlyDictionary<uint, double[]>? Colours, ResourceEdits.LutRecolour? Lut, MeshFile? Mesh = null,
    IReadOnlyDictionary<uint, ulong>? Textures = null, IReadOnlyList<ShaderFile>? Shaders = null) : DataResource(Where);
// A unit copy's own geometry: an OBJ in the mod folder (UnitMesh), scaled, Y-up or the game's Z-up; with UnitSpace its
// coordinates are the unit's own (the frame its joints and attach points are in), otherwise its render mesh's.
public sealed record MeshFile(string Obj, double Scale, bool YUp, bool UnitSpace = false);
// A material copy's own code for a shader stage: the stage's offset in the GPU part, and a compiled shader in the mod folder.
public sealed record ShaderFile(int Offset, string Dxbc);
public sealed record IncludeResources(string Where, ulong Type, IReadOnlyList<ulong> Ids) : DataResource(Where);
public sealed record IconResource(string Where, string Name, string Png, string Prepare) : DataResource(Where);
// A texture of the mod's own: one PNG per layer (more than one is an array texture), its format and category.
public sealed record TextureResource(string Where, string Name, IReadOnlyList<string> Pngs, string Format, uint Category) : DataResource(Where);
// A unit's own animations from a JSON file in the mod folder (AnimationSet): its bones, clips and state machine, built for
// the unit copy named Unit in the same patch, which then uses them.
public sealed record AnimationsResource(string Where, string File, string Unit) : DataResource(Where);
// A unit of the mod's own from a glTF model (ModelUnit): materials by the model's material names, LOD sizes (null: the
// weapons' own), and optionally a game unit whose physics it takes under its name and one whose animations it plays.
public sealed record ModelResource(string Where, string Name, string File, IReadOnlyDictionary<string, ulong> Materials, IReadOnlyList<double>? LodSizes,
    ulong? Physics, ulong? AnimationsFrom) : DataResource(Where);
// A sound bank of the mod's own: its events from WAVs in the mod folder, under a copy of a game bank's root mixer.
public sealed record SoundsResource(string Where, string Name, IReadOnlyList<SoundFile> Sounds, ulong MixerBank, uint Mixer) : DataResource(Where);
public sealed record SoundFile(string Event, string Wav, double Gain);
public sealed record Replacement(ulong From, ulong To, IReadOnlyList<int>? At);
// Warm colour curve keys recoloured by a preset or towards a colour; Keys, if given, is how many keys must change.
public sealed record CurveEdit(string? Preset, double[]? Colour, int? Keys);

public static partial class DataSpecs
{
    public const string FileName = "hellforge-data.json";
    public const string WarmToBlue = "warm-to-blue";

    [GeneratedRegex("^[0-9a-f]{16}$")]
    private static partial Regex ArchiveName();

    [GeneratedRegex("^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)+$")]
    private static partial Regex ResourceName();

    [GeneratedRegex("^0x[0-9A-Fa-f]{1,16}$")]
    private static partial Regex Hash64();

    [GeneratedRegex("^0x[0-9A-Fa-f]{1,8}$")]
    private static partial Regex Hash32();

    [GeneratedRegex("^[A-Za-z0-9_]{1,100}$")]
    private static partial Regex EventName();

    [GeneratedRegex("^#[0-9A-Fa-f]{6}$")]
    private static partial Regex HexColour();

    // The resource types a spec may copy and stay content for mod sync (ModSets.ClassOf), each with the one edit HellForge
    // reviewed for it: a unit's mesh (and its physics), a material's colours and textures, a texture's colour lookup, an effect's
    // colour curves. An include also takes the game's sound banks: it adds the game's own bytes unchanged.
    static readonly HashSet<ulong> CopiedTypes = [ResourceTypes.Unit, ResourceTypes.Material, ResourceTypes.Texture, ResourceTypes.Particles];
    static readonly HashSet<ulong> IncludedTypes = [.. CopiedTypes, ResourceTypes.WwiseBank, ResourceTypes.WwiseDependency];

    // Whether HellForge checks everything the spec builds, so mod sync may take it from any host: no "replace" (it writes
    // any 8 bytes over any value at any offset), only the types above (never lua), and each edit only on its own type.
    // Icons and textures are always checked: HellForge encodes their PNGs itself. Animations too: HellForge encodes
    // every value of them itself, in range. Null: checked; otherwise why not.
    public static string? Unchecked(DataSpec spec)
    {
        foreach (var resource in spec.Patches.SelectMany(p => p.Resources))
            switch (resource)
            {
                case IncludeResources include when !IncludedTypes.Contains(include.Type):
                    return $"{include.Where} includes {ResourceTypes.Describe(include.Type)} resources";
                case CopyResource copy when !CopiedTypes.Contains(copy.Type):
                    return $"{copy.Where} copies a {ResourceTypes.Describe(copy.Type)} resource";
                case CopyResource copy when copy.Replace.Count > 0:
                    return $"{copy.Where} replaces values (\"replace\")";
                case CopyResource { Shaders.Count: > 0 } copy:
                    return $"{copy.Where} brings its own shader code";
                case CopyResource copy when copy.Curves != null && copy.Type != ResourceTypes.Particles || copy.Colours != null && copy.Type != ResourceTypes.Material ||
                    copy.Lut != null && copy.Type != ResourceTypes.Texture || copy.Textures != null && copy.Type != ResourceTypes.Material:
                    return $"{copy.Where} edits a {ResourceTypes.Describe(copy.Type)} resource as another type";
            }
        return null;
    }

    // The spec of a mod folder, or null when it has none. Its structure and input files are checked here; whether the
    // game has what it names is checked when it is built.
    public static DataSpec? Load(string folder)
    {
        var path = Path.Combine(folder, FileName);
        if (!File.Exists(path)) return null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            return Parse(document.RootElement, folder);
        }
        catch (JsonException error) { throw new InvalidDataException($"{FileName}: {error.Message}"); }
        catch (InvalidDataException error) { throw new InvalidDataException($"{FileName}: {error.Message}"); }
    }

    // A spec from its text alone, its input files not looked for (folder null): what mod sync reads of a download before
    // its other files (ModSets.Declared).
    public static DataSpec FromJson(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            return Parse(document.RootElement, null);
        }
        catch (JsonException error) { throw new InvalidDataException($"{FileName}: {error.Message}"); }
    }

    static DataSpec Parse(JsonElement root, string? folder)
    {
        Fields(root, "", "format", "patches");
        if (!root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number || !format.TryGetInt32(out var version) || version != 1)
            throw new InvalidDataException("format must be 1.");
        var patches = new List<DataPatch>();
        var inputs = new SortedSet<string>(StringComparer.Ordinal);
        var p = 0;
        foreach (var patch in Items(root, "patches", ""))
        {
            var at = $"patches[{p++}]";
            Fields(patch, at, "archives", "package", "load", "resources");
            var package = ReadPackage(patch, at);
            var archives = package != null ? [] : ReadArchives(patch, at);
            var resources = new List<DataResource>();
            var r = 0;
            foreach (var resource in Items(patch, "resources", at))
                resources.Add(Resource(resource, $"{at}.resources[{r++}]", folder, inputs));
            if (resources.Count == 0) throw new InvalidDataException($"{at}: resources are missing.");
            patches.Add(new(archives, resources, package));
        }
        if (patches.Count == 0) throw new InvalidDataException("patches are missing.");
        if (patches.Where(x => x.Package != null).GroupBy(x => x.Package!.Id).FirstOrDefault(g => g.Count() > 1) is { } twice)
            throw new InvalidDataException($"package {twice.First().Package!.Name} is named by two patches.");
        return new(patches, [.. inputs]);
    }

    static List<string> ReadArchives(JsonElement patch, string at)
    {
        var archives = Items(patch, "archives", at).Select(a => a.ValueKind == JsonValueKind.String && ArchiveName().IsMatch(a.GetString()!)
            ? a.GetString()! : throw new InvalidDataException($"{at}: an archive is 16 lowercase hex digits, such as \"dbd9381f9ea9788f\".")).ToList();
        return archives.Count > 0 && archives.Distinct().Count() == archives.Count ? archives : throw new InvalidDataException($"{at}: archives are missing or repeated.");
    }

    // "package": the mod's own package by name (exactly one of it and "archives"), "load": "session" (the default) or
    // "request". Its id must not be a 16-hex-digit name of the game's that a hash could meet, so it is the name's hash.
    static ModDataPackage? ReadPackage(JsonElement patch, string at)
    {
        var has = patch.TryGetProperty("package", out _);
        if (has == patch.TryGetProperty("archives", out _)) throw new InvalidDataException($"{at}: a patch has exactly one of archives or package.");
        if (!has)
        {
            if (patch.TryGetProperty("load", out _)) throw new InvalidDataException($"{at}: load is for a package.");
            return null;
        }
        var name = Text(patch, "package", at);
        if (name.Length > 200 || !ResourceName().IsMatch(name)) throw new InvalidDataException($"{at}: package is the mod's own package path, such as \"you/your_mod/rifle\".");
        var load = patch.TryGetProperty("load", out _) ? Text(patch, "load", at) : "session";
        if (load is not ("session" or "request")) throw new InvalidDataException($"{at}: load is session or request.");
        return new(name, EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name)), load == "session");
    }

    static DataResource Resource(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at}: a resource is an object.");
        var kinds = new[] { "copy", "include", "icon", "texture", "animations", "model", "sounds" }.Where(k => e.TryGetProperty(k, out _)).ToList();
        if (kinds.Count != 1) throw new InvalidDataException($"{at}: a resource has exactly one of copy, include, icon, texture, animations, model or sounds.");
        switch (kinds[0])
        {
            case "include": return ReadInclude(e, at);
            case "icon": return ReadIcon(e, at, folder, inputs);
            case "texture": return ReadTexture(e, at, folder, inputs);
            case "animations": return ReadAnimations(e, at, folder, inputs);
            case "model": return ReadModel(e, at, folder, inputs);
            case "sounds": return ReadSounds(e, at, folder, inputs);
        }
        return ReadCopy(e, at, folder, inputs);
    }

    static IncludeResources ReadInclude(JsonElement e, string at)
    {
        Fields(e, at, "type", "include", "source");
        var ids = Items(e, "include", at).Select(id => Id(id, $"{at}.include")).ToList();
        if (ids.Count == 0) throw new InvalidDataException($"{at}: include lists no resources.");
        var (steam, build) = Source(e, at);
        return new IncludeResources(at, Type(e, at), ids) { Steam = steam, Build = build };
    }

    static AnimationsResource ReadAnimations(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "animations", "unit");
        var file = Text(e, "animations", at).Replace('\\', '/');
        if (Path.IsPathRooted(file) || file.Split('/').Any(part => part is "" or "." or "..") || !file.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{at}: animations is a .json file in the mod folder, such as \"animations/my_gun.json\".");
        if (folder != null && !File.Exists(Path.Combine(folder, file))) throw new InvalidDataException($"{at}: {file} is missing.");
        var unit = Text(e, "unit", at);
        if (unit.Length == 0) throw new InvalidDataException($"{at}: unit names the unit copy the animations are for.");
        inputs.Add(file);
        return new AnimationsResource(at, file, unit);
    }

    static IconResource ReadIcon(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "icon", "name", "prepare");
        var png = Text(e, "icon", at).Replace('\\', '/');
        if (Path.IsPathRooted(png) || png.Split('/').Any(part => part is "" or "." or "..") || !png.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{at}: icon is a .png file in the mod folder, such as \"icons/my_stratagem.png\".");
        if (folder != null && !File.Exists(Path.Combine(folder, png))) throw new InvalidDataException($"{at}: {png} is missing.");
        var prepare = e.TryGetProperty("prepare", out _) ? Text(e, "prepare", at) : "raw";
        if (prepare is not ("raw" or "auto" or "image")) throw new InvalidDataException($"{at}: prepare is raw, auto or image.");
        inputs.Add(png);
        return new IconResource(at, Name(e, at), png, prepare);
    }

    static TextureResource ReadTexture(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "texture", "name", "format", "category");
        var texture = Property(e, "texture", at);
        var pngs = (texture.ValueKind == JsonValueKind.Array ? texture.EnumerateArray().ToList() : [texture])
            .Select(p => p.ValueKind == JsonValueKind.String ? p.GetString()!.Replace('\\', '/') : "").ToList();
        if (pngs.Count == 0 || pngs.Any(p => !InModFolder(p) || !p.EndsWith(".png", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException($"{at}: texture is a .png file in the mod folder, or a list of them for an array texture.");
        foreach (var png in pngs)
        {
            if (folder != null && !File.Exists(Path.Combine(folder, png))) throw new InvalidDataException($"{at}: {png} is missing.");
            inputs.Add(png);
        }
        var format = Text(e, "format", at);
        if (!TextureImage.Formats.ContainsKey(format)) throw new InvalidDataException($"{at}: format is {string.Join(", ", TextureImage.Formats.Keys)}.");
        var category = 0u;
        if (e.TryGetProperty("category", out _))
        {
            var name = Text(e, "category", at);
            category = Hash32().IsMatch(name) ? uint.Parse(name[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
                : name.Length > 0 ? TextureImage.Category(name) : throw new InvalidDataException($"{at}: category is a name, such as normal, or 0x<8 hex digits>.");
        }
        return new TextureResource(at, Name(e, at), pngs, format, category);
    }

    static SoundsResource ReadSounds(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "sounds", "name", "mixer_bank", "mixer");
        var sounds = Items(e, "sounds", at).Select((s, i) => ReadSound(s, $"{at}.sounds[{i}]", folder, inputs)).ToList();
        if (sounds.Count == 0) throw new InvalidDataException($"{at}: sounds lists no sounds.");
        var mixer = Text(e, "mixer", at);
        if (!Hash32().IsMatch(mixer)) throw new InvalidDataException($"{at}: mixer is a root actor-mixer of mixer_bank, 0x<8 hex digits>.");
        return new SoundsResource(at, Name(e, at), sounds, Id(Property(e, "mixer_bank", at), $"{at}.mixer_bank"),
            uint.Parse(mixer[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture));
    }

    static SoundFile ReadSound(JsonElement s, string at, string? folder, ISet<string> inputs)
    {
        Fields(s, at, "event", "wav", "gain");
        var name = Text(s, "event", at);
        if (!EventName().IsMatch(name)) throw new InvalidDataException($"{at}: event is a name of letters, digits and _ (up to 100), prefixed with yours.");
        var wav = Text(s, "wav", at).Replace('\\', '/');
        if (!InModFolder(wav) || !wav.EndsWith(".wav", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"{at}: wav is a .wav file in the mod folder.");
        if (folder != null && !File.Exists(Path.Combine(folder, wav))) throw new InvalidDataException($"{at}: {wav} is missing.");
        inputs.Add(wav);
        var gain = s.TryGetProperty("gain", out var g) && g.ValueKind == JsonValueKind.Number ? g.GetDouble() : s.TryGetProperty("gain", out _) ? double.NaN : 0;
        if (!double.IsFinite(gain) || gain is < -96 or > 24) throw new InvalidDataException($"{at}: gain is a number of dB, -96 to 24.");
        return new SoundFile(name, wav, gain);
    }

    static ModelResource ReadModel(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "model", "name", "materials", "lods", "physics", "animationsFrom");
        var file = Text(e, "model", at).Replace('\\', '/');
        if (!InModFolder(file) || !(file.EndsWith(".glb", StringComparison.OrdinalIgnoreCase) || file.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException($"{at}: model is a .glb or .gltf file in the mod folder, such as \"models/rifle.glb\".");
        if (folder != null && !File.Exists(Path.Combine(folder, file))) throw new InvalidDataException($"{at}: {file} is missing.");
        inputs.Add(file);
        if (folder != null) foreach (var buffer in ModelBuffers(folder, file, at)) inputs.Add(buffer);
        var materials = Object(Property(e, "materials", at), $"{at}.materials").ToDictionary(m => m.Name, m => Id(m.Value, $"{at}.materials.{m.Name}"));
        if (materials.Count == 0) throw new InvalidDataException($"{at}: materials names a material resource for each of the model's materials.");
        var lods = e.TryGetProperty("lods", out _) ? Items(e, "lods", at).Select(v => v.ValueKind == JsonValueKind.Number && v.GetDouble() is > 0 and <= 100 ? v.GetDouble()
            : throw new InvalidDataException($"{at}: lods are the share of the screen at which each LOD starts, such as [1, 0.5, 0.25].")).ToList() : null;
        ulong? physics = e.TryGetProperty("physics", out var p) ? Id(p, $"{at}.physics") : null;
        ulong? animations = e.TryGetProperty("animationsFrom", out var a) ? Id(a, $"{at}.animationsFrom") : null;
        return new ModelResource(at, Name(e, at), file, materials, lods, physics, animations);
    }

    // A path in the mod folder: relative, without . or .. parts.
    internal static bool InModFolder(string path) => !Path.IsPathRooted(path) && !path.Split('/').Any(part => part is "" or "." or "..");

    // A .gltf file's buffer files, as mod-folder paths (they count toward the spec like the model).
    internal static List<string> ModelBuffers(string folder, string file, string at)
    {
        if (!file.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase)) return [];
        var dir = Path.GetDirectoryName(file)?.Replace('\\', '/') ?? "";
        var list = new List<string>();
        foreach (var uri in Gltf.ExternalFiles(File.ReadAllBytes(Path.Combine(folder, file))))
        {
            var path = (dir.Length > 0 ? dir + "/" : "") + uri.Replace('\\', '/');
            if (!InModFolder(path) || !File.Exists(Path.Combine(folder, path))) throw new InvalidDataException($"{at}: the model's buffer {uri} is not a file beside it in the mod folder.");
            list.Add(path);
        }
        return list;
    }

    // A copy, its edits read in the spec's order.
    static CopyResource ReadCopy(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        Fields(e, at, "type", "copy", "name", "expect", "replace", "curves", "colours", "textures", "lut", "mesh", "shader", "source");
        var type = Type(e, at);
        var expect = ReadExpect(e, at);
        var replace = ReadReplace(e, at);
        var curves = ReadCurves(e, at);
        var colours = ReadColours(e, at);
        var textures = ReadTextures(e, at);
        var lut = ReadLut(e, at);
        var mesh = ReadMesh(e, at, type, folder, inputs);
        var (steam, build) = Source(e, at);
        var shaders = ReadShaders(e, at, type, folder, inputs);
        return new CopyResource(at, type, Id(Property(e, "copy", at), at), Name(e, at), expect, replace, curves, colours, lut, mesh, textures, shaders) { Steam = steam, Build = build };
    }

    static List<ShaderFile>? ReadShaders(JsonElement e, string at, ulong type, string? folder, ISet<string> inputs)
    {
        if (!e.TryGetProperty("shader", out var list)) return null;
        if (type != ResourceTypes.Material) throw new InvalidDataException($"{at}.shader: only a material has shaders.");
        if (list.ValueKind != JsonValueKind.Array) throw new InvalidDataException($"{at}.shader: a list of stages.");
        var shaders = new List<ShaderFile>();
        foreach (var stage in list.EnumerateArray())
        {
            Fields(stage, $"{at}.shader", "offset", "dxbc");
            var dxbc = Text(stage, "dxbc", $"{at}.shader").Replace('\\', '/');
            if (folder != null && !File.Exists(Path.Combine(folder, dxbc))) throw new InvalidDataException($"{at}.shader: {dxbc} is not in the mod's folder.");
            inputs.Add(dxbc);
            shaders.Add(new(Offset(Text(stage, "offset", $"{at}.shader"), $"{at}.shader"), dxbc));
        }
        return shaders;
    }

    static Dictionary<int, ulong> ReadExpect(JsonElement e, string at)
    {
        var expect = new Dictionary<int, ulong>();
        if (e.TryGetProperty("expect", out var pins))
            foreach (var pin in Object(pins, $"{at}.expect"))
                expect[Offset(pin.Name, $"{at}.expect")] = Id(pin.Value, $"{at}.expect");
        return expect;
    }

    static List<Replacement> ReadReplace(JsonElement e, string at)
    {
        var replace = new List<Replacement>();
        if (!e.TryGetProperty("replace", out var replacements)) return replace;
        if (replacements.ValueKind != JsonValueKind.Array) throw new InvalidDataException($"{at}: replace is a list.");
        foreach (var item in replacements.EnumerateArray())
        {
            Fields(item, $"{at}.replace", "from", "to", "at");
            List<int>? offsets = null;
            if (item.TryGetProperty("at", out var list))
                offsets = Items(item, "at", $"{at}.replace").Select(o => o.ValueKind == JsonValueKind.String ? Offset(o.GetString()!, $"{at}.replace")
                    : throw new InvalidDataException($"{at}.replace: at lists offsets such as \"0x2E44\".")).ToList();
            replace.Add(new(Id(Property(item, "from", $"{at}.replace"), $"{at}.replace"), Id(Property(item, "to", $"{at}.replace"), $"{at}.replace"), offsets));
        }
        return replace;
    }

    static CurveEdit? ReadCurves(JsonElement e, string at)
    {
        if (!e.TryGetProperty("curves", out var curveSpec)) return null;
        Fields(curveSpec, $"{at}.curves", "preset", "colour", "keys");
        var preset = curveSpec.TryGetProperty("preset", out _) ? Text(curveSpec, "preset", $"{at}.curves") : null;
        var colour = curveSpec.TryGetProperty("colour", out var c) ? Colour(c, $"{at}.curves") : null;
        if ((preset == null) == (colour == null)) throw new InvalidDataException($"{at}.curves: give a preset or a colour.");
        if (preset != null && preset != WarmToBlue) throw new InvalidDataException($"{at}.curves: the preset is \"{WarmToBlue}\".");
        if (colour != null && colour.Max() <= 0) throw new InvalidDataException($"{at}.curves: the colour cannot be black.");
        int? keys = curveSpec.TryGetProperty("keys", out var k) ? (k.ValueKind == JsonValueKind.Number && k.TryGetInt32(out var n) && n > 0 ? n : throw new InvalidDataException($"{at}.curves: keys is a positive count.")) : null;
        return new(preset, colour, keys);
    }

    // {slot: texture}: a slot by name (base_data) or 0x<8 hex digits>, a texture by resource name or 0x id.
    static Dictionary<uint, ulong>? ReadTextures(JsonElement e, string at)
    {
        if (!e.TryGetProperty("textures", out var spec)) return null;
        var textures = new Dictionary<uint, ulong>();
        foreach (var item in Object(spec, $"{at}.textures"))
            textures[Hash32().IsMatch(item.Name) ? uint.Parse(item.Name[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
                : item.Name.Length > 0 ? EngineHash.Id32(item.Name) : throw new InvalidDataException($"{at}.textures: a slot is 0x<8 hex digits> or a name.")]
                = Id(item.Value, $"{at}.textures.{item.Name}");
        return textures.Count > 0 ? textures : throw new InvalidDataException($"{at}: textures is empty.");
    }

    static Dictionary<uint, double[]>? ReadColours(JsonElement e, string at)
    {
        if (!e.TryGetProperty("colours", out var colourSpec)) return null;
        Dictionary<uint, double[]> colours = [];
        foreach (var item in Object(colourSpec, $"{at}.colours"))
            colours[Hash32().IsMatch(item.Name) ? uint.Parse(item.Name[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
                : item.Name.Length > 0 ? EngineHash.Id32(item.Name) : throw new InvalidDataException($"{at}.colours: a parameter is 0x<8 hex digits> or a name.")]
                = Colour(item.Value, $"{at}.colours");
        if (colours.Count == 0) throw new InvalidDataException($"{at}: colours is empty.");
        return colours;
    }

    static ResourceEdits.LutRecolour? ReadLut(JsonElement e, string at)
    {
        if (!e.TryGetProperty("lut", out var lutSpec)) return null;
        Fields(lutSpec, $"{at}.lut", "zonesFrom", "columns", "rows");
        int? zonesFrom = lutSpec.TryGetProperty("zonesFrom", out var z) ? (z.ValueKind == JsonValueKind.Number && z.TryGetInt32(out var row) && row >= 0 ? row : throw new InvalidDataException($"{at}.lut: zonesFrom is a row number.")) : null;
        var columns = lutSpec.TryGetProperty("columns", out var cs) ? Cells(cs, $"{at}.lut.columns") : [];
        var rows = new Dictionary<int, IReadOnlyDictionary<int, double[]>>();
        if (lutSpec.TryGetProperty("rows", out var rs))
            foreach (var item in Object(rs, $"{at}.lut.rows")) rows[Index(item.Name, $"{at}.lut.rows")] = Cells(item.Value, $"{at}.lut.rows");
        return new(zonesFrom, columns, rows);
    }

    static MeshFile? ReadMesh(JsonElement e, string at, ulong type, string? folder, ISet<string> inputs)
    {
        if (!e.TryGetProperty("mesh", out var meshSpec)) return null;
        if (type != ResourceTypes.Unit) throw new InvalidDataException($"{at}: mesh is for a unit copy.");
        Fields(meshSpec, $"{at}.mesh", "obj", "scale", "yUp", "unitSpace");
        var obj = Text(meshSpec, "obj", $"{at}.mesh").Replace('\\', '/');
        if (Path.IsPathRooted(obj) || obj.Split('/').Any(part => part is "" or "." or "..") || !obj.EndsWith(".obj", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{at}.mesh: obj is an .obj file in the mod folder, such as \"models/cube.obj\".");
        if (folder != null && !File.Exists(Path.Combine(folder, obj))) throw new InvalidDataException($"{at}.mesh: {obj} is missing.");
        var scale = meshSpec.TryGetProperty("scale", out var sc) ? (sc.ValueKind == JsonValueKind.Number && sc.GetDouble() is > 0 and < 1000 and var v ? v : throw new InvalidDataException($"{at}.mesh: scale is a positive number.")) : 1.0;
        var yUp = !meshSpec.TryGetProperty("yUp", out var up) || (up.ValueKind is JsonValueKind.True or JsonValueKind.False ? up.GetBoolean() : throw new InvalidDataException($"{at}.mesh: yUp is true or false."));
        var unitSpace = meshSpec.TryGetProperty("unitSpace", out var us) && (us.ValueKind is JsonValueKind.True or JsonValueKind.False ? us.GetBoolean() : throw new InvalidDataException($"{at}.mesh: unitSpace is true or false."));
        inputs.Add(obj);
        return new(obj, scale, yUp, unitSpace);
    }

    // "source": "game" (the default), "steam", or {"build": "<depot>:<manifest>"} / {"build": "yyyy-MM-dd"} (the build of
    // that day, from the depot history HellForge ships): from Steam's install, or the past build it names.
    static (bool Steam, string? Build) Source(JsonElement e, string at)
    {
        if (!e.TryGetProperty("source", out var source)) return (false, null);
        if (source.ValueKind == JsonValueKind.Object)
        {
            Fields(source, $"{at}.source", "build");
            return (false, PastBuild(Text(source, "build", $"{at}.source"), $"{at}.source"));
        }
        return Text(e, "source", at) switch
        {
            "game" => (false, null),
            "steam" => (true, null),
            _ => throw new InvalidDataException($"{at}: source is \"game\" (the game copy, the default), \"steam\" (the game as Steam has it installed) or {{\"build\": ...}} (a past build)."),
        };
    }

    // A past build by its data manifest ("553853:<manifest>") or by day ("2024-03-01": the manifest Steam published last on
    // or before that day, UTC), as its label.
    static string PastBuild(string text, string at)
    {
        var history = AssetMigration.DepotHistory.Embedded(AssetMigration.GameBuilds.DataDepot);
        if (DateTime.TryParseExact(text, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var day))
            return history.Where(h => h.Time < day.AddDays(1) && h.Branch == null).MaxBy(h => h.Time) is { } h
                ? AssetMigration.GameBuilds.Label(AssetMigration.GameBuilds.DataDepot, h.Manifest.ToString())
                : throw new InvalidDataException($"{at}: no build of the game is older than {text}.");
        var parts = text.Split(':');
        if (parts.Length == 2 && parts[0] == AssetMigration.GameBuilds.DataDepot.ToString() && ulong.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out var manifest) &&
            history.Any(h => h.Manifest == manifest))
            return text;
        throw new InvalidDataException($"{at}: build is a day (\"2024-03-01\") or a data manifest of the game (\"553853:<manifest>\", as an asset report names it).");
    }

    static Dictionary<int, double[]> Cells(JsonElement e, string at) =>
        Object(e, at).ToDictionary(item => Index(item.Name, at), item => Colour(item.Value, at));

    static int Index(string text, string at) =>
        int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var index) && index < 256 ? index
            : throw new InvalidDataException($"{at}: {text} is not a row or column number.");

    // A resource or reference: 0x<hash> or its name.
    static ulong Id(JsonElement e, string at)
    {
        var text = e.ValueKind == JsonValueKind.String ? e.GetString()! : "";
        if (Hash64().IsMatch(text)) return ulong.Parse(text[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture);
        if (text.Length == 0 || text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{at}: a resource is 0x<up to 16 hex digits> or its name.");
        return EngineHash.Murmur64A(Encoding.UTF8.GetBytes(text));
    }

    static ulong Type(JsonElement e, string at)
    {
        var text = Text(e, "type", at);
        return Hash64().IsMatch(text) ? ulong.Parse(text[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
            : text.Length is > 0 and <= 64 ? ResourceTypes.Hash(text) : throw new InvalidDataException($"{at}: type is a name such as \"material\" or 0x<hash>.");
    }

    // A mod's own resource name: a path, prefixed with the author and mod (you/your_mod/...).
    static string Name(JsonElement e, string at)
    {
        var name = Text(e, "name", at);
        return name.Length <= 200 && ResourceName().IsMatch(name) ? name
            : throw new InvalidDataException($"{at}: name is the mod's own resource path, such as \"you/your_mod/rifle\".");
    }

    // An RGB colour: [r, g, b] (as the game stores it) or "#RRGGBB" (0..1).
    static double[] Colour(JsonElement e, string at)
    {
        if (e.ValueKind == JsonValueKind.String && HexColour().IsMatch(e.GetString()!))
            return [.. Enumerable.Range(0, 3).Select(c => int.Parse(e.GetString()!.AsSpan(1 + 2 * c, 2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture) / 255.0)];
        if (e.ValueKind == JsonValueKind.Array && e.GetArrayLength() == 3 && e.EnumerateArray().All(v => v.ValueKind == JsonValueKind.Number))
        {
            var rgb = e.EnumerateArray().Select(v => v.GetDouble()).ToArray();
            if (rgb.All(v => double.IsFinite(v) && v is >= 0 and <= 1024)) return rgb;
        }
        throw new InvalidDataException($"{at}: a colour is [r, g, b] or \"#RRGGBB\".");
    }

    static int Offset(string text, string at) =>
        text.StartsWith("0x", StringComparison.Ordinal) && int.TryParse(text.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var offset) && offset >= 0
            ? offset : throw new InvalidDataException($"{at}: {text} is not an offset such as \"0x2E44\".");

    static void Fields(JsonElement e, string at, params string[] allowed)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at}: expected an object.");
        foreach (var property in e.EnumerateObject())
            if (!allowed.Contains(property.Name)) throw new InvalidDataException($"{(at.Length > 0 ? at + ": " : "")}unknown field {property.Name}.");
    }

    static JsonElement Property(JsonElement e, string name, string at) =>
        e.TryGetProperty(name, out var value) ? value : throw new InvalidDataException($"{at}: {name} is required.");

    static string Text(JsonElement e, string name, string at) =>
        Property(e, name, at) is { ValueKind: JsonValueKind.String } value ? value.GetString()! : throw new InvalidDataException($"{at}: {name} must be text.");

    static IEnumerable<JsonElement> Items(JsonElement e, string name, string at) =>
        Property(e, name, at) is { ValueKind: JsonValueKind.Array } value ? value.EnumerateArray() : throw new InvalidDataException($"{(at.Length > 0 ? at + ": " : "")}{name} must be a list.");

    static IEnumerable<JsonProperty> Object(JsonElement e, string at) =>
        e.ValueKind == JsonValueKind.Object ? e.EnumerateObject() : throw new InvalidDataException($"{at}: expected an object.");
}
