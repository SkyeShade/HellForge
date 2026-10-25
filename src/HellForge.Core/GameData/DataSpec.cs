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
//   icon     a stratagem icon from a PNG in the mod folder: its texture and GUI material.
// A copy or include reads the player's game copy, or with "source": "steam" the game as Steam has it installed (a newer
// build's resources, read from the player's own files like the rest; an include from Steam must be new to the copy).
//   mesh     (on a unit copy) the mod's own geometry from an OBJ in the mod folder, in every render and shadow mesh.
// docs/modding/data.md "Data specs" describes the format.
public sealed record DataSpec(IReadOnlyList<DataPatch> Patches, IReadOnlyList<string> Inputs)
{
    public bool UsesSteam => Patches.Any(p => p.Resources.Any(r => r.Steam));
}
public sealed record DataPatch(IReadOnlyList<string> Archives, IReadOnlyList<DataResource> Resources);
public abstract record DataResource(string Where)
{
    public bool Steam { get; init; }
}
public sealed record CopyResource(string Where, ulong Type, ulong From, string Name, IReadOnlyDictionary<int, ulong> Expect,
    IReadOnlyList<Replacement> Replace, CurveEdit? Curves, IReadOnlyDictionary<uint, double[]>? Colours, ResourceEdits.LutRecolour? Lut, MeshFile? Mesh = null) : DataResource(Where);
// A unit copy's own geometry: an OBJ in the mod folder (UnitMesh), scaled, Y-up or the game's Z-up.
public sealed record MeshFile(string Obj, double Scale, bool YUp);
public sealed record IncludeResources(string Where, ulong Type, IReadOnlyList<ulong> Ids) : DataResource(Where);
public sealed record IconResource(string Where, string Name, string Png, string Prepare) : DataResource(Where);
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

    [GeneratedRegex("^#[0-9A-Fa-f]{6}$")]
    private static partial Regex HexColour();

    // The resource types a spec may copy and stay content for mod sync (ModSets.ClassOf), each with the one edit HellForge
    // reviewed for it: a unit's mesh (and its physics), a material's colours, a texture's colour lookup, an effect's
    // colour curves. An include also takes the game's sound banks: it adds the game's own bytes unchanged.
    static readonly HashSet<ulong> CopiedTypes = [ResourceTypes.Unit, ResourceTypes.Material, ResourceTypes.Texture, ResourceTypes.Particles];
    static readonly HashSet<ulong> IncludedTypes = [.. CopiedTypes, ResourceTypes.Hash("wwise_bank"), ResourceTypes.Hash("wwise_dep")];

    // Whether HellForge checks everything the spec builds, so mod sync may take it from any host: no "replace" (it writes
    // any 8 bytes over any value at any offset), only the types above (never lua), and each edit only on its own type.
    // Icons are always checked: a 256 x 256 PNG becomes a texture and its GUI material. Null: checked; otherwise why not.
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
                case CopyResource copy when copy.Curves != null && copy.Type != ResourceTypes.Particles || copy.Colours != null && copy.Type != ResourceTypes.Material ||
                    copy.Lut != null && copy.Type != ResourceTypes.Texture:
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
            Fields(patch, at, "archives", "resources");
            var archives = Items(patch, "archives", at).Select(a => a.ValueKind == JsonValueKind.String && ArchiveName().IsMatch(a.GetString()!)
                ? a.GetString()! : throw new InvalidDataException($"{at}: an archive is 16 lowercase hex digits, such as \"dbd9381f9ea9788f\".")).ToList();
            if (archives.Count == 0 || archives.Distinct().Count() != archives.Count) throw new InvalidDataException($"{at}: archives are missing or repeated.");
            var resources = new List<DataResource>();
            var r = 0;
            foreach (var resource in Items(patch, "resources", at))
                resources.Add(Resource(resource, $"{at}.resources[{r++}]", folder, inputs));
            if (resources.Count == 0) throw new InvalidDataException($"{at}: resources are missing.");
            patches.Add(new(archives, resources));
        }
        if (patches.Count == 0) throw new InvalidDataException("patches are missing.");
        return new(patches, [.. inputs]);
    }

    static DataResource Resource(JsonElement e, string at, string? folder, ISet<string> inputs)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at}: a resource is an object.");
        var kinds = new[] { "copy", "include", "icon" }.Where(k => e.TryGetProperty(k, out _)).ToList();
        if (kinds.Count != 1) throw new InvalidDataException($"{at}: a resource has exactly one of copy, include or icon.");
        switch (kinds[0])
        {
            case "include":
            {
                Fields(e, at, "type", "include", "source");
                var ids = Items(e, "include", at).Select(id => Id(id, $"{at}.include")).ToList();
                if (ids.Count == 0) throw new InvalidDataException($"{at}: include lists no resources.");
                return new IncludeResources(at, Type(e, at), ids) { Steam = FromSteam(e, at) };
            }
            case "icon":
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
        }
        Fields(e, at, "type", "copy", "name", "expect", "replace", "curves", "colours", "lut", "mesh", "source");
        var type = Type(e, at);
        var expect = new Dictionary<int, ulong>();
        if (e.TryGetProperty("expect", out var pins))
            foreach (var pin in Object(pins, $"{at}.expect"))
                expect[Offset(pin.Name, $"{at}.expect")] = Id(pin.Value, $"{at}.expect");
        var replace = new List<Replacement>();
        if (e.TryGetProperty("replace", out var replacements))
        {
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
        }
        CurveEdit? curves = null;
        if (e.TryGetProperty("curves", out var curveSpec))
        {
            Fields(curveSpec, $"{at}.curves", "preset", "colour", "keys");
            var preset = curveSpec.TryGetProperty("preset", out _) ? Text(curveSpec, "preset", $"{at}.curves") : null;
            var colour = curveSpec.TryGetProperty("colour", out var c) ? Colour(c, $"{at}.curves") : null;
            if ((preset == null) == (colour == null)) throw new InvalidDataException($"{at}.curves: give a preset or a colour.");
            if (preset != null && preset != WarmToBlue) throw new InvalidDataException($"{at}.curves: the preset is \"{WarmToBlue}\".");
            if (colour != null && colour.Max() <= 0) throw new InvalidDataException($"{at}.curves: the colour cannot be black.");
            int? keys = curveSpec.TryGetProperty("keys", out var k) ? (k.ValueKind == JsonValueKind.Number && k.TryGetInt32(out var n) && n > 0 ? n : throw new InvalidDataException($"{at}.curves: keys is a positive count.")) : null;
            curves = new(preset, colour, keys);
        }
        Dictionary<uint, double[]>? colours = null;
        if (e.TryGetProperty("colours", out var colourSpec))
        {
            colours = [];
            foreach (var item in Object(colourSpec, $"{at}.colours"))
                colours[Hash32().IsMatch(item.Name) ? uint.Parse(item.Name[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
                    : item.Name.Length > 0 ? EngineHash.Id32(item.Name) : throw new InvalidDataException($"{at}.colours: a parameter is 0x<8 hex digits> or a name.")]
                    = Colour(item.Value, $"{at}.colours");
            if (colours.Count == 0) throw new InvalidDataException($"{at}: colours is empty.");
        }
        ResourceEdits.LutRecolour? lut = null;
        if (e.TryGetProperty("lut", out var lutSpec))
        {
            Fields(lutSpec, $"{at}.lut", "zonesFrom", "columns", "rows");
            int? zonesFrom = lutSpec.TryGetProperty("zonesFrom", out var z) ? (z.ValueKind == JsonValueKind.Number && z.TryGetInt32(out var row) && row >= 0 ? row : throw new InvalidDataException($"{at}.lut: zonesFrom is a row number.")) : null;
            var columns = lutSpec.TryGetProperty("columns", out var cs) ? Cells(cs, $"{at}.lut.columns") : [];
            var rows = new Dictionary<int, IReadOnlyDictionary<int, double[]>>();
            if (lutSpec.TryGetProperty("rows", out var rs))
                foreach (var item in Object(rs, $"{at}.lut.rows")) rows[Index(item.Name, $"{at}.lut.rows")] = Cells(item.Value, $"{at}.lut.rows");
            lut = new(zonesFrom, columns, rows);
        }
        MeshFile? mesh = null;
        if (e.TryGetProperty("mesh", out var meshSpec))
        {
            if (type != ResourceTypes.Unit) throw new InvalidDataException($"{at}: mesh is for a unit copy.");
            Fields(meshSpec, $"{at}.mesh", "obj", "scale", "yUp");
            var obj = Text(meshSpec, "obj", $"{at}.mesh").Replace('\\', '/');
            if (Path.IsPathRooted(obj) || obj.Split('/').Any(part => part is "" or "." or "..") || !obj.EndsWith(".obj", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"{at}.mesh: obj is an .obj file in the mod folder, such as \"models/cube.obj\".");
            if (folder != null && !File.Exists(Path.Combine(folder, obj))) throw new InvalidDataException($"{at}.mesh: {obj} is missing.");
            var scale = meshSpec.TryGetProperty("scale", out var sc) ? (sc.ValueKind == JsonValueKind.Number && sc.GetDouble() is > 0 and < 1000 and var v ? v : throw new InvalidDataException($"{at}.mesh: scale is a positive number.")) : 1.0;
            var yUp = !meshSpec.TryGetProperty("yUp", out var up) || (up.ValueKind is JsonValueKind.True or JsonValueKind.False ? up.GetBoolean() : throw new InvalidDataException($"{at}.mesh: yUp is true or false."));
            inputs.Add(obj);
            mesh = new(obj, scale, yUp);
        }
        return new CopyResource(at, type, Id(Property(e, "copy", at), at), Name(e, at), expect, replace, curves, colours, lut, mesh) { Steam = FromSteam(e, at) };
    }

    static bool FromSteam(JsonElement e, string at) =>
        !e.TryGetProperty("source", out _) ? false : Text(e, "source", at) switch
        {
            "game" => false,
            "steam" => true,
            _ => throw new InvalidDataException($"{at}: source is \"game\" (the game copy, the default) or \"steam\" (the game as Steam has it installed).")
        };

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
