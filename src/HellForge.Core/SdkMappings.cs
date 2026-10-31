using System.Globalization;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// The SDK's mappings (docs/sdk-roadmap.md "Mappings"): mappings/components.json names the fields of component records,
// mappings/catalog.json names vanilla content, mappings/functions.json names game functions. They generate the typed C
// headers mods include and the loader's tables of reviewed fields and functions (hellforge sdk-headers), so a mod's
// field or function and the loader's check come from one entry.
public sealed record SdkField(string Name, int Offset, string Type, int Count, string Meaning, bool Verified, string Source)
{
    public int Size => Count * SdkMappings.TypeSize(Type);
    // When a change to it takes effect in a running game (live editing): now, respawn or mission.
    public string Applies { get; init; } = "respawn";
    public bool AppliesVerified { get; init; }
    // A reference to another table's row (projectile, explosion, status, damage): the loader takes only a row of that
    // table as its value (or 0 where the field allows none), and loads the package the row needs first.
    public string? Ref { get; init; }
}
// A named set of fields copied or set together (HF_COPY_GROUP, HF_SET_GROUP): "the firing sounds".
public sealed record SdkGroup(string Name, string Meaning, IReadOnlyList<string> Fields);
// An entity component record (Index: its component number), or a settings table row (Table; Index -1), found by its id
// (a projectile type, a damage id...). Row names the HfApi function that returns one, when there is one (difficulty_row).
public sealed record SdkComponent(string Name, string Component, int Index, int Stride, uint Layout, string Meaning,
    IReadOnlyList<SdkField> Fields, IReadOnlyList<SdkGroup> Groups)
{
    public string? Row { get; init; }
    public bool Table { get; init; }
}
public sealed record SdkContent(string Kind, string Name, string Constant, ulong Id, string Note);
// A game function's argument: its C type (SdkMappings.ParamTypes) and what it is.
public sealed record SdkParam(string Name, string Type, string Meaning);
// A game function (SDK 16): where it is in its module, its first bytes as reviewed (an IDA-style pattern), its signature.
// Hook: the signature is whole (every argument, unnamed ones as pass-through slots, and the return value), so a detour
// can pass everything on; without it the function is for calling only.
// Aliases: its former names, which the loader still resolves (mods built with them keep working).
public sealed record SdkFunction(string Name, string Module, uint Rva, string Code, string Returns, IReadOnlyList<SdkParam> Params,
    bool Hook, string Meaning, string Source)
{
    public IReadOnlyList<string> Aliases { get; init; } = [];
}
// A named field of a global's object: its offset from the object's start, a field type (u8 ... u64, f32, ptr).
public sealed record SdkGlobalField(string Name, uint Offset, string Type, string Meaning);
// An instruction that loads a global's cell: its rva, and its bytes with the ones after it (an IDA-style pattern).
public sealed record SdkWitness(uint Rva, string Code);
// A game global (SDK 16): a cell in a module's data holding a pointer to one of the game's objects, the instructions that
// witness it (1 to 3; the loader needs any one of them as reviewed), the object's named fields, by offset, and its former
// names (Aliases), which the loader still resolves.
public sealed record SdkGlobal(string Name, string Module, uint Rva, IReadOnlyList<SdkWitness> Witnesses, string Meaning,
    IReadOnlyList<SdkGlobalField> Fields, string Source)
{
    public IReadOnlyList<string> Aliases { get; init; } = [];
}

public sealed class SdkMappings
{
    public const string ComponentsFile = "components.json";
    public const string CatalogFile = "catalog.json";
    public const string FunctionsFile = "functions.json";
    public const string GlobalsFile = "globals.json";
    public static readonly string[] HeaderFiles =
        ["native/mod-sdk/hellforge_components.h", "native/mod-sdk/hellforge_content.h", "native/mod-loader/sdk_fields.h",
         "native/mod-loader/sdk_content.h", "docs/sdk-reference.md", "native/mod-sdk/hellforge_functions.h",
         "native/mod-loader/sdk_functions.h", "native/mod-sdk/hellforge_globals.h", "native/mod-loader/sdk_globals.h"];

    // hellforge_mod.h's other Hf types, which a global's struct (Hf<Name>) can't take either.
    static readonly HashSet<string> SdkTypes = ["Ability", "BugholeEvent", "Content", "DamageEvent", "DamagePreEvent", "EditDef",
        "EditorEntity", "EventInfo", "ExplosionEvent", "HealEvent", "Homing", "KillEvent", "LimbState", "LiveValue", "ModInfo",
        "Notice", "Passives", "PelicanCall", "PlayerEvent", "Session", "Shot", "ShotRule", "SpawnEvent", "Stat", "StatEvent",
        "StatSource", "StateEvent", "StratagemCategory", "StratagemEvent", "SyncMod", "SyncState", "WeaponEvent", "Listener"];
    // A global's field types: the record field types and ptr, a pointer.
    static readonly string[] GlobalFieldTypes = ["u8", "u16", "u32", "i32", "f32", "u64", "ptr"];

    // The C types a function's arguments and return value may have (and void, for a return value).
    public static readonly string[] ParamTypes = ["uint8_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "float", "void*", "const void*",
        "uint8_t*", "const uint8_t*", "uint32_t*", "const uint32_t*", "float*", "const float*"];
    // Names an argument can't take: C's keywords, and mod (the hf_ wrappers' own argument).
    static readonly HashSet<string> CKeywords = ["auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
        "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "bool", "mod"];

    // Content kinds, in header order: (JSON key, constant prefix, C type of the ids, what the ids are for).
    static readonly (string Key, string Prefix, bool Wide, string Doc)[] Kinds =
    [
        ("items", "HF_ITEM_", false, "Loadout ids of vanilla items, for HfWeapon.base, sound_from and HfStratagem.loads_item. Every session with a\n * weapon added lists the whole arsenal (loadout id, armory type, entity, designation, name) in its log folder as arsenal.tsv."),
        ("projectiles", "HF_PROJECTILE_", false, "Vanilla projectile types, for HfProjectile.base and visual_from (HD2Runtime research projectile-identities)."),
        ("stratagems", "HF_STRATAGEM_", false, "Vanilla stratagem types, for HfStratagem.base (the game's StratagemType enum)."),
        ("behaviours", "HF_BEHAVIOUR_", false, "Vanilla behaviours, for HfBehaviour.base, HfEntity.behaviour and HfEntityDef.behaviour_id."),
        ("entities", "HF_ENTITY_", true, "Vanilla entities, for HfEntity.base, HfEntityDef.base, model_from and HfCopy.from (entity resource ids)."),
        ("damage", "HF_DAMAGE_", false, "Vanilla damage ids, for HfDamage.base and HfProjectile.damage (the projectile row's +0x3C)."),
        ("packages", "HF_PACKAGE_", true, "Vanilla packages, for HfStratagem.package (the resource ids LoadoutPackage records name)."),
        ("tags", "HF_TAG_", true, "Entity tags, for HfEntityDef.add_tags and remove_tags (TagComponentData)."),
        ("traits", "HF_TRAIT_", false, "Stratagem traits, for HfStratagem.flags (row +0x104, the game's StratagemInfoFlag bits; OR them together).\n * Bits 13 and 15 are unnamed so far; HF_TRAIT_VEHICLE allows one per loadout."),
        ("categories", "HF_CATEGORY_", false, "Loadout sections and ping colours, for HfStratagem.category (row +0xB8 + 1)."),
        ("beams", "HF_BEAM_", false, "Beacon beam colours, for HfStratagem.beam (row +0xD4)."),
        ("boosters", "HF_BOOSTER_", false, "Boosters, for the BoosterSettings table (set_live, get_live): a row is the game's Booster value."),
    ];

    // hellforge_mod.h's own Hf names: a component record struct can't take one.
    static readonly HashSet<string> Reserved = ["Mod", "Event", "EventCallback", "Weapon", "Entity", "Behaviour", "Damage",
        "Stratagem", "Projectile", "FieldType", "Difficulty", "Api", "Ref", "Field", "Value", "Copy", "Set", "MountDef", "EntityDef"];

    public required string Build { get; init; }
    public required IReadOnlyList<SdkComponent> Components { get; init; }
    public required IReadOnlyList<SdkContent> Content { get; init; }
    public IReadOnlyList<SdkFunction> Functions { get; init; } = [];
    public IReadOnlyList<SdkGlobal> Globals { get; init; } = [];

    public static readonly string[] AppliesValues = ["now", "respawn", "mission"];
    public static readonly string[] RefValues = ["projectile", "explosion", "status", "damage", "mount", "pickup"];

    // The field a stats-schema field is (by component or table kind, offset and storage): itself, or one element of an
    // array field of that type. Null if the mappings do not have it.
    public SdkField? FieldAt(string component, int offset, string storage)
    {
        var c = Components.FirstOrDefault(c => c.Component == component);
        return c?.Fields.FirstOrDefault(f => f.Type == storage && offset >= f.Offset && offset < f.Offset + f.Size &&
            (offset - f.Offset) % TypeSize(f.Type) == 0);
    }

    static readonly Lazy<SdkMappings> CurrentMappings = new(() =>
    {
        string Resource(string name)
        {
            using var stream = typeof(SdkMappings).Assembly.GetManifestResourceStream("HellForge.Core.mappings." + name)!;
            return new StreamReader(stream).ReadToEnd();
        }
        return Parse(Resource(ComponentsFile), Resource(CatalogFile), Resource(FunctionsFile), Resource(GlobalsFile));
    });
    // The mappings this build of HellForge was made with (embedded from mappings/).
    public static SdkMappings Current => CurrentMappings.Value;

    public static int TypeSize(string type) => type switch
    {
        "u8" or "bytes" => 1, "u16" => 2, "u32" or "i32" or "f32" => 4, "u64" => 8,
        _ => throw new InvalidDataException($"Unknown field type {type}.")
    };

    public static SdkMappings Load(string folder)
    {
        try
        {
            return Parse(File.ReadAllText(Path.Combine(folder, ComponentsFile)), File.ReadAllText(Path.Combine(folder, CatalogFile)),
                File.ReadAllText(Path.Combine(folder, FunctionsFile)), File.ReadAllText(Path.Combine(folder, GlobalsFile)));
        }
        catch (JsonException error) { throw new InvalidDataException($"mappings: {error.Message}"); }
    }

    // functionsJson, globalsJson: mappings/functions.json and globals.json, or null for none.
    public static SdkMappings Parse(string componentsJson, string catalogJson, string? functionsJson = null, string? globalsJson = null)
    {
        using var components = JsonDocument.Parse(componentsJson);
        using var catalog = JsonDocument.Parse(catalogJson);
        var root = components.RootElement;
        Allowed(root, "mappings", "format", "build", "components");
        if (Int(root, "format") != 1) throw new InvalidDataException("mappings/components.json: format must be 1.");
        var build = Text(root, "build");
        var list = new List<SdkComponent>();
        foreach (var entry in Property(root, "components").EnumerateObject()) list.Add(ParseComponent(entry));

        var croot = catalog.RootElement;
        Allowed(croot, "catalog", ["format", "build", .. Kinds.Select(k => k.Key)]);
        if (Int(croot, "format") != 1 || Text(croot, "build") != build) throw new InvalidDataException("mappings/catalog.json: format 1, the components' build.");
        var content = new List<SdkContent>();
        var constants = new HashSet<string>(StringComparer.Ordinal);
        foreach (var kind in Kinds)
        {
            if (!croot.TryGetProperty(kind.Key, out var section)) continue;
            foreach (var item in section.EnumerateObject()) content.Add(ParseContent(kind, item, constants));
        }
        var functions = functionsJson != null ? ParseFunctions(functionsJson, build) : [];
        var globals = globalsJson != null ? ParseGlobals(globalsJson, build, list) : [];
        return new SdkMappings { Build = build, Components = list, Content = content, Functions = functions, Globals = globals };
    }

    static List<SdkGlobal> ParseGlobals(string json, string build, List<SdkComponent> components)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        Allowed(root, "globals", "format", "build", "note", "globals");
        if (Int(root, "format") != 1 || Text(root, "build") != build) throw new InvalidDataException("mappings/globals.json: format 1, the components' build.");
        var globals = new List<SdkGlobal>();
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in Property(root, "globals").EnumerateObject())
        {
            var at = $"global {entry.Name}";
            Identifier(entry.Name, at);
            var global = ParseGlobal(entry, at);
            foreach (var name in global.Aliases.Prepend(global.Name))
                if (name.Length > 63 || !names.Add(name) || Reserved.Contains(name) || SdkTypes.Contains(name) || components.Any(c => c.Name == name))
                    throw new InvalidDataException($"{at}: {name} must be a unique name (or alias) of 63 characters at most, and no other SDK type's (Hf{name}).");
            globals.Add(global);
        }
        return globals;
    }

    static SdkGlobal ParseGlobal(JsonProperty entry, string at)
    {
        var v = entry.Value;
        Allowed(v, at, "module", "rva", "witnesses", "aliases", "meaning", "fields", "source");
        var module = v.TryGetProperty("module", out _) ? Text(v, "module") : "game.dll";
        if (!System.Text.RegularExpressions.Regex.IsMatch(module, @"^[A-Za-z0-9_.-]{1,60}\.(dll|exe)$")) throw new InvalidDataException($"{at}: module must be a dll or exe name.");
        var rva = Hex32(Text(v, "rva"), $"{at} rva");
        var witnesses = Property(v, "witnesses").EnumerateArray().Select(w => ParseWitness(w, rva, at)).ToList();
        if (witnesses.Count is < 1 or > 3 || witnesses.DistinctBy(w => w.Rva).Count() != witnesses.Count)
            throw new InvalidDataException($"{at}: 1 to 3 witnesses, at different places.");
        var fields = v.TryGetProperty("fields", out var fieldsElement) ? fieldsElement.EnumerateObject().Select(f => ParseGlobalField(f, at)).ToList() : [];
        fields.Sort((a, b) => a.Offset.CompareTo(b.Offset));
        for (var i = 1; i < fields.Count; i++)
            if (fields[i].Offset < fields[i - 1].Offset + GlobalFieldSize(fields[i - 1].Type))
                throw new InvalidDataException($"{at}: {fields[i - 1].Name} and {fields[i].Name} overlap.");
        return new SdkGlobal(entry.Name, module, rva, witnesses, Text(v, "meaning"), fields, Text(v, "source")) { Aliases = AliasesOf(v, at) };
    }

    static SdkWitness ParseWitness(JsonElement w, uint rva, string at)
    {
        Allowed(w, at + " witness", "rva", "code");
        var witness = new SdkWitness(Hex32(Text(w, "rva"), $"{at} witness rva"), Pattern(Text(w, "code"), $"{at} witness"));
        Witnesses(witness.Code, witness.Rva, rva, at);
        return witness;
    }

    // A function's or global's former names ("aliases", optional): C identifiers.
    static List<string> AliasesOf(JsonElement v, string at)
    {
        if (!v.TryGetProperty("aliases", out var aliases)) return [];
        var list = aliases.EnumerateArray().Select(a => a.GetString() ?? "").ToList();
        foreach (var alias in list) Identifier(alias, $"{at} alias {alias}");
        return list;
    }

    static SdkGlobalField ParseGlobalField(JsonProperty f, string global)
    {
        var at = $"{global}.{f.Name}";
        Identifier(f.Name, at);
        if (f.Name.StartsWith("_unknown", StringComparison.Ordinal)) throw new InvalidDataException($"{at}: _unknown names are generated.");
        Allowed(f.Value, at, "offset", "type", "meaning");
        var type = Text(f.Value, "type");
        if (!GlobalFieldTypes.Contains(type)) throw new InvalidDataException($"{at}: type must be one of {string.Join(", ", GlobalFieldTypes)}.");
        var offset = Hex32(Text(f.Value, "offset"), at + " offset", allowZero: true);
        if (offset % (uint)Math.Min(GlobalFieldSize(type), 4) != 0) throw new InvalidDataException($"{at}: misaligned.");
        return new SdkGlobalField(f.Name, offset, type, Text(f.Value, "meaning"));
    }

    // A witness is a RIP-relative load, store or lea of the cell: a REX prefix, 8B, 89 or 8D, a ModRM of [rip+disp32], the
    // displacement fixed and pointing at the cell (the instruction ends after it).
    static void Witnesses(string code, uint witnessRva, uint rva, string where)
    {
        var bytes = code.Split(' ');
        if (bytes.Length < 7 || bytes.Take(7).Any(b => b == "??")) throw new InvalidDataException($"{where}: the witness's first 7 bytes must be fixed.");
        var b = bytes.Take(7).Select(x => Convert.ToByte(x, 16)).ToArray();
        if (b[0] is < 0x48 or > 0x4F || b[1] is not (0x8B or 0x89 or 0x8D) || (b[2] & 0xC7) != 0x05)
            throw new InvalidDataException($"{where}: the witness must be a mov or lea with [rip+disp32] (48-4F, 8B/89/8D, ModRM 05-3D).");
        var target = (long)witnessRva + 7 + BitConverter.ToInt32(b, 3);
        if (target != rva) throw new InvalidDataException($"{where}: the witness points at 0x{target:X}, not the global's 0x{rva:X}.");
    }

    static uint Hex32(string text, string where, bool allowZero = false)
    {
        if (!text.StartsWith("0x", StringComparison.Ordinal) || !uint.TryParse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value) || (value == 0 && !allowZero))
            throw new InvalidDataException($"{where}: must be a hex number (0x...).");
        return value;
    }

    static int GlobalFieldSize(string type) => type == "ptr" ? 8 : TypeSize(type);

    static List<SdkFunction> ParseFunctions(string json, string build)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        Allowed(root, "functions", "format", "build", "note", "functions");
        if (Int(root, "format") != 1 || Text(root, "build") != build) throw new InvalidDataException("mappings/functions.json: format 1, the components' build.");
        var functions = new List<SdkFunction>();
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in Property(root, "functions").EnumerateObject())
        {
            var function = ParseFunction(entry);
            foreach (var name in function.Aliases.Prepend(function.Name))
                if (name.Length > 63 || !names.Add(name))
                    throw new InvalidDataException($"function {entry.Name}: {name} must be a unique name (or alias) of 63 characters at most.");
            functions.Add(function);
        }
        return functions;
    }

    static SdkFunction ParseFunction(JsonProperty entry)
    {
        var at = $"function {entry.Name}";
        Identifier(entry.Name, at);
        var v = entry.Value;
        Allowed(v, at, "module", "rva", "code", "returns", "params", "hook", "aliases", "meaning", "source");
        var module = v.TryGetProperty("module", out _) ? Text(v, "module") : "game.dll";
        if (!System.Text.RegularExpressions.Regex.IsMatch(module, @"^[A-Za-z0-9_.-]{1,60}\.(dll|exe)$")) throw new InvalidDataException($"{at}: module must be a dll or exe name.");
        var rva = Hex32(Text(v, "rva"), $"{at} rva");
        var returns = Text(v, "returns");
        if (returns != "void" && !ParamTypes.Contains(returns)) throw new InvalidDataException($"{at}: returns {returns}, not void or one of {string.Join(", ", ParamTypes)}.");
        var parameters = new List<SdkParam>();
        foreach (var p in Property(v, "params").EnumerateArray())
        {
            Allowed(p, at + " param", "name", "type", "meaning");
            var param = new SdkParam(Text(p, "name"), Text(p, "type"), Text(p, "meaning"));
            Identifier(param.Name, $"{at} param {param.Name}");
            if (CKeywords.Contains(param.Name) || parameters.Any(q => q.Name == param.Name)) throw new InvalidDataException($"{at}: param {param.Name} is a C keyword, mod, or named twice.");
            if (!ParamTypes.Contains(param.Type)) throw new InvalidDataException($"{at}: param {param.Name}'s type {param.Type} is not one of {string.Join(", ", ParamTypes)}.");
            parameters.Add(param);
        }
        return new SdkFunction(entry.Name, module, rva, Pattern(Text(v, "code"), at), returns, parameters, Bool(v, "hook"), Text(v, "meaning"), Text(v, "source"))
        {
            Aliases = AliasesOf(v, at)
        };
    }

    // A function's reviewed first bytes as the loader's code check takes them: two hex digits or ?? a byte, upper case,
    // single spaces; 8 to 256 bytes, the first one fixed.
    static string Pattern(string code, string where)
    {
        var bytes = code.Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(b => b.ToUpperInvariant()).ToList();
        if (bytes.Count < 8 || bytes.Count > 256 || bytes[0] == "??" || bytes.Any(b => b != "??" && !(b.Length == 2 && b.All(char.IsAsciiHexDigit))))
            throw new InvalidDataException($"{where}: code must be 8 to 256 bytes (two hex digits or ?? each, the first fixed).");
        return string.Join(' ', bytes);
    }

    static SdkComponent ParseComponent(JsonProperty entry)
    {
        var where = $"component {entry.Name}";
        Identifier(entry.Name, where);
        if (Reserved.Contains(entry.Name)) throw new InvalidDataException($"{where}: Hf{entry.Name} is an SDK type already.");
        var c = entry.Value;
        Allowed(c, where, "component", "index", "row", "table", "stride", "layout", "meaning", "applies", "appliesVerified", "appliesSource",
            "fields", "groups");
        var row = c.TryGetProperty("row", out _) ? Text(c, "row") : null;
        if (row != null) Identifier(row, where);
        var table = row != null || (c.TryGetProperty("table", out _) && Bool(c, "table"));
        var applies = Applies(c, where, null);
        var appliesVerified = c.TryGetProperty("appliesVerified", out _) && Bool(c, "appliesVerified");
        var stride = Int(c, "stride");
        var fields = new List<SdkField>();
        foreach (var f in Property(c, "fields").EnumerateObject()) fields.Add(ParseField(entry.Name, f, applies, appliesVerified, stride));
        fields.Sort((a, b) => a.Offset.CompareTo(b.Offset));
        for (var i = 1; i < fields.Count; i++)
            if (fields[i].Offset < fields[i - 1].Offset + fields[i - 1].Size)
                throw new InvalidDataException($"{entry.Name}: {fields[i - 1].Name} and {fields[i].Name} overlap.");
        var groups = ParseGroups(entry.Name, c, fields);
        return new SdkComponent(entry.Name, Text(c, "component"), table ? -1 : Int(c, "index"), stride,
            uint.Parse(Text(c, "layout"), CultureInfo.InvariantCulture), Text(c, "meaning"), fields, groups) { Row = row, Table = table };
    }

    // A component's field; its applies and appliesVerified default to the component's.
    static SdkField ParseField(string component, JsonProperty f, string applies, bool appliesVerified, int stride)
    {
        var at = $"{component}.{f.Name}";
        Identifier(f.Name, at);
        if (f.Name.StartsWith("_unknown", StringComparison.Ordinal)) throw new InvalidDataException($"{at}: _unknown names are generated.");
        var v = f.Value;
        Allowed(v, at, "offset", "type", "count", "meaning", "verified", "source", "applies", "appliesVerified", "appliesSource", "ref");
        var type = Text(v, "type");
        TypeSize(type);
        var count = v.TryGetProperty("count", out _) ? Int(v, "count") : 1;
        var field = new SdkField(f.Name, Int(v, "offset"), type, count, Text(v, "meaning"), Bool(v, "verified"), Text(v, "source"))
        {
            Applies = Applies(v, at, applies),
            AppliesVerified = v.TryGetProperty("applies", out _) ? v.TryGetProperty("appliesVerified", out _) && Bool(v, "appliesVerified") : appliesVerified,
            Ref = v.TryGetProperty("ref", out _) ? Text(v, "ref") : null
        };
        // an array of u32 references is checked element by element (a magazine's projectile pattern)
        if (field.Ref != null && (!RefValues.Contains(field.Ref) || type != (field.Ref is "mount" or "pickup" ? "u64" : "u32") ||
                                  (count != 1 && field.Ref is "mount" or "pickup")))
            throw new InvalidDataException($"{at}: ref must be projectile, explosion, status or damage on u32s, or mount on a single u64.");
        if (count < 1 || field.Offset < 0 || field.Offset + field.Size > stride || field.Offset % Math.Min(TypeSize(type), 4) != 0)
            throw new InvalidDataException($"{at}: outside the {stride}-byte record or misaligned.");
        return field;
    }

    static List<SdkGroup> ParseGroups(string component, JsonElement c, List<SdkField> fields)
    {
        var groups = new List<SdkGroup>();
        if (!c.TryGetProperty("groups", out var groupsElement)) return groups;
        foreach (var g in groupsElement.EnumerateObject())
        {
            var at = $"{component} group {g.Name}";
            Identifier(g.Name, at);
            Allowed(g.Value, at, "meaning", "fields");
            var names = Property(g.Value, "fields").EnumerateArray().Select(n => n.GetString() ?? "").ToList();
            if (names.Count == 0 || names.Distinct().Count() != names.Count || names.Any(n => fields.All(f => f.Name != n)))
                throw new InvalidDataException($"{at}: its fields must be distinct fields of {component}.");
            groups.Add(new SdkGroup(g.Name, Text(g.Value, "meaning"), names));
        }
        return groups;
    }

    // A catalog entry: its constant (named, or made from its name) unique among its kind's, and its id.
    static SdkContent ParseContent((string Key, string Prefix, bool Wide, string Doc) kind, JsonProperty item, HashSet<string> constants)
    {
        var at = $"{kind.Key} \"{item.Name}\"";
        Allowed(item.Value, at, "constant", "id", "note");
        var constant = item.Value.TryGetProperty("constant", out _) ? Text(item.Value, "constant") : Constant(item.Name);
        if (constant.Length == 0 || !constant.All(ch => ch is >= 'A' and <= 'Z' or >= '0' and <= '9' or '_') || !constants.Add(kind.Prefix + constant))
            throw new InvalidDataException($"{at}: constant {constant} is not a unique C name.");
        var idText = Text(item.Value, "id");
        var id = idText.StartsWith("0x", StringComparison.Ordinal)
            ? ulong.Parse(idText[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : ulong.Parse(idText, CultureInfo.InvariantCulture);
        if (!kind.Wide && id > uint.MaxValue) throw new InvalidDataException($"{at}: id is wider than 32 bits.");
        return new SdkContent(kind.Key, item.Name, constant, id, item.Value.TryGetProperty("note", out _) ? Text(item.Value, "note") : "");
    }

    // "MG-206 Heavy Machine Gun" -> MG206_HEAVY_MACHINE_GUN, as the SDK's constants have always been named.
    public static string Constant(string name)
    {
        var text = new StringBuilder();
        foreach (var ch in name.Replace("-", "").Replace("/", "").ToUpperInvariant())
            if (ch is >= 'A' and <= 'Z' or >= '0' and <= '9') text.Append(ch);
            else if (text.Length > 0 && text[^1] != '_') text.Append('_');
        return text.ToString().Trim('_');
    }

    // The generated files, by repository-relative path.
    // The generated files, by repository-relative path. The reference documents the API from hellforge_mod.h (apiHeader:
    // its text; without it, the reference has the mappings only).
    public IReadOnlyDictionary<string, string> Generate(string? apiHeader = null) => new Dictionary<string, string>
    {
        [HeaderFiles[0]] = ComponentsHeader(),
        [HeaderFiles[1]] = ContentHeader(),
        [HeaderFiles[2]] = LoaderTable(),
        [HeaderFiles[3]] = LoaderContent(),
        [HeaderFiles[4]] = Reference(apiHeader),
        [HeaderFiles[5]] = FunctionsHeader(),
        [HeaderFiles[6]] = LoaderFunctions(),
        [HeaderFiles[7]] = GlobalsHeader(),
        [HeaderFiles[8]] = LoaderGlobals(),
    };

    // Writes the generated files under the repository root (the SDK's headers and reference, and the mod loader's
    // tables in native/mod-loader); with check, only lists the ones that differ.
    public static IReadOnlyList<string> Sync(string repository, bool check)
    {
        var stale = new List<string>();
        // Line endings as git checks them out (CRLF on Windows with core.autocrlf): compared and parsed as LF.
        var api = File.ReadAllText(Path.Combine(repository, "native", "mod-sdk", "hellforge_mod.h")).ReplaceLineEndings("\n");
        foreach (var (path, text) in Load(Path.Combine(repository, "mappings")).Generate(api))
        {
            var file = Path.Combine(repository, path);
            if (File.Exists(file) && File.ReadAllText(file).ReplaceLineEndings("\n") == text.ReplaceLineEndings("\n")) continue;
            stale.Add(path);
            if (!check) File.WriteAllText(file, text);
        }
        return stale;
    }

    const string Generated = "Generated by `hellforge sdk-headers` from mappings/; do not edit (docs/sdk-roadmap.md \"Mappings\").";

    static string CType(string type) => type switch
    {
        "u8" or "bytes" => "uint8_t", "u16" => "uint16_t", "u32" => "uint32_t", "i32" => "int32_t", "f32" => "float", "u64" => "uint64_t",
        _ => throw new InvalidDataException(type)
    };
    static string TypeTag(string type) => "HF_TYPE_" + type.ToUpperInvariant();

    string ComponentsHeader()
    {
        var s = new StringBuilder();
        s.Append($$"""
            /* HellForge mod SDK: component records with named fields, for game build {{Build}}.
             * {{Generated}}
             *
             * One packed struct per component record: named fields at their offsets, unresearched bytes as _unknown_XX
             * padding. Use HF_FIELD(Component, field) in definition blocks and hf_record(mod, ref, HfComponent) for a typed
             * pointer to an entity's own record (hellforge_mod.h). Fields marked unverified are named from research but not
             * yet confirmed in the game. Each component's layout is the game build its offsets were reviewed on: the
             * loader refuses fields of a layout it does not have, so a mod built against another layout is reported, not
             * run. Each field's [tag] says when a change applies in a running game (set_live): [now] the next time the game
             * reads it, [respawn] when the entity is next made (spawn, re-equip, call-in), [mission] when a mission loads;
             * a "?" means it is inferred, not yet confirmed. Included by hellforge_mod.h. */
            #ifndef HELLFORGE_COMPONENTS_H
            #define HELLFORGE_COMPONENTS_H

            #define HF_MAPPINGS_BUILD "{{Build}}"

            #pragma pack(push, 1)

            """);
        foreach (var c in Components)
        {
            if (c.Table)
                s.Append($"\n/* {c.Component} ({c.Stride}-byte table rows, found by id{(c.Row != null ? $"; api->{c.Row} returns one: (Hf{c.Name}*)api->{c.Row}(...)" : "")}): {c.Meaning} */\n");
            else s.Append($"\n/* {c.Component} (component {c.Index}, {c.Stride}-byte records): {c.Meaning} */\n");
            s.Append($"#define Hf{c.Name}_INFO \"{c.Component}\", {c.Layout}u\n");
            s.Append($"typedef struct Hf{c.Name}\n{{\n");
            var at = 0;
            foreach (var f in c.Fields)
            {
                if (f.Offset > at) s.Append($"    uint8_t _unknown_{at:X2}[{f.Offset - at}];\n");
                var declaration = $"    {CType(f.Type)} {f.Name}{(f.Count > 1 || f.Type == "bytes" ? $"[{f.Count}]" : "")};";
                s.Append($"{declaration.PadRight(48)} /* +0x{f.Offset:X2} {f.Meaning}{(f.Verified ? "" : " (unverified)")} [{f.Applies}{(f.AppliesVerified ? "" : "?")}] */\n");
                at = f.Offset + f.Size;
            }
            if (c.Stride > at) s.Append($"    uint8_t _unknown_{at:X2}[{c.Stride - at}];\n");
            s.Append($"}} Hf{c.Name};\n");
            s.Append($"HF_ASSERT_SIZE(Hf{c.Name}, {c.Stride});\n");
            foreach (var f in c.Fields)
            {
                s.Append($"HF_ASSERT_OFFSET(Hf{c.Name}, {f.Name}, {f.Offset});\n");
                s.Append($"#define HF_TYPE_{c.Name}_{f.Name} {TypeTag(f.Type)}\n");
            }
            foreach (var g in c.Groups)
                s.Append($"/* Group {g.Name}: {g.Meaning} */\n#define HF_GROUP_{c.Name}_{g.Name}(X, ...) " +
                    string.Join(", ", g.Fields.Select(f => $"X({c.Name}, {f}, __VA_ARGS__)")) + "\n");
        }
        s.Append("\n#pragma pack(pop)\n\n#endif\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    string ContentHeader()
    {
        var s = new StringBuilder();
        s.Append($$"""
            /* HellForge mod SDK: vanilla content by name, for game build {{Build}}.
             * {{Generated}}
             * Each constant's comment starts with the content's catalog name (mappings/catalog.json). Included by
             * hellforge_mod.h. */
            #ifndef HELLFORGE_CONTENT_H
            #define HELLFORGE_CONTENT_H

            /* Content kinds, for find (HfApi): find(HF_KIND_ENTITY, "Pelican Transport"). */

            """);
        for (var i = 0; i < Kinds.Length; i++) s.Append($"#define {KindConstant(Kinds[i].Prefix)} {i + 1}u\n");
        foreach (var kind in Kinds)
        {
            var items = Content.Where(c => c.Kind == kind.Key).ToList();
            if (items.Count == 0) continue;
            s.Append($"\n/* {kind.Doc} */\n");
            foreach (var item in items)
            {
                var value = kind.Wide ? $"0x{item.Id:X16}ull" : item.Id > 0xFFFF || kind.Key == "traits" ? $"0x{item.Id:X8}u" : $"{item.Id}u";
                var line = $"#define {kind.Prefix}{item.Constant} {value}";
                s.Append($"{line.PadRight(56)} /* {item.Name}{(item.Note.Length > 0 ? " - " + item.Note : "")} */\n");
            }
        }
        s.Append("\n#endif\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    // The SDK reference (docs/sdk-reference.md): every API call with its comment, every component and table with its
    // fields, every vanilla content constant and every mapped game function, after an opening and a contents list that
    // links every section.
    string Reference(string? apiHeader)
    {
        var s = new StringBuilder();
        // Contents: each top-level section with the level-3 headings under it (title as linked, heading text).
        var contents = new List<(string Section, List<(string Title, string Heading)> Entries)>();
        void Section(string title, string intro)
        {
            contents.Add((title, []));
            s.Append($"\n## {title}\n\n{intro}");
        }
        void Entry(string title, string heading)
        {
            contents[^1].Entries.Add((title, heading));
            s.Append($"\n### {heading}\n");
        }
        void Api(string header)
        {
            Section("API", """
                The `HfApi` table a mod's `hellforge_mod_init(const HfApi* hf, HfMod* mod)` receives. Functions are only ever added at
                the end, and a mod checks `hf->version` to know which ones the loader has. Each section lists what one SDK version
                added, in table order; a version that added nothing to the table has no section. Each entry gives the member's
                name, its type, and its comment from `hellforge_mod.h`. Where one comment covers several members, it is on the
                first of them.

                See also: [Logic at run time](modding/runtime.md) for events, actions and live edits; [Content](modding/content.md)
                for changing and adding content; [Hooks and game memory](modding/hooks.md) for `hook`, `find_pattern`, `read` and
                `write`; [Debugging](modding/debugging.md) for the mod's log and refusals.

                """);
            foreach (var line in ApiReference(header))
            {
                if (line.StartsWith("\n### ", StringComparison.Ordinal))
                {
                    var heading = line[5..^1];
                    var colon = heading.IndexOf(':');
                    Entry(colon < 0 ? heading : heading[..colon], heading);
                    s.Append('\n');
                }
                else s.Append(line).Append('\n');
            }
        }
        void FunctionsSection()
        {
            Section("Functions", """
                Game functions by name (`hellforge_functions.h`, SDK 16), from `mappings/functions.json`. `hf_fn_<Name>(mod)` gives
                the function to call; `HF_HOOK(mod, <Name>, detour, &original)` and `hf_hook_<Name>` hook it, for the functions
                marked "hook or call" (their whole signature is known). The compiler checks the detour's type against the
                signature. Each entry gives where the function is in this build, its signature and its arguments.

                See also: [Hooks and game memory](modding/hooks.md#hooking-by-name).

                """);
            foreach (var f in Functions)
            {
                Entry(f.Name, f.Name);
                s.Append($"\n`{f.Module}+0x{f.Rva:X}`, {(f.Hook ? "hook or call" : "call only")}. {f.Meaning}. From {f.Source}." +
                    (f.Aliases.Count > 0 ? $" Former names: {string.Join(", ", f.Aliases.Select(a => $"`{a}`"))}." : "") + "\n\n");
                s.Append($"```c\ntypedef {Signature(f, $"*HfFn_{f.Name}")};\n```\n");
                if (f.Params.Count > 0)
                {
                    s.Append("\n| Argument | Type | Meaning |\n|---|---|---|\n");
                    foreach (var p in f.Params) s.Append($"| `{p.Name}` | `{p.Type}` | {p.Meaning.Replace("|", "\\|")} |\n");
                }
            }
        }
        void GlobalsSection()
        {
            Section("Globals", """
                The game's objects by name (`hellforge_globals.h`, SDK 16), from `mappings/globals.json`: its managers and queues,
                what the functions above take. `hf_global_<Name>(mod)` gives the object as an `Hf<Name>*`, or NULL. Each entry gives
                where its cell is in this build, what is known of the object, and its named fields.

                See also: [Hooks and game memory](modding/hooks.md#game-globals).

                """);
            foreach (var g in Globals)
            {
                Entry(g.Name, g.Name);
                s.Append($"\n`{g.Module}+0x{g.Rva:X}` (a pointer to it), `Hf{g.Name}`. {g.Meaning}. From {g.Source}." +
                    (g.Aliases.Count > 0 ? $" Former names: {string.Join(", ", g.Aliases.Select(a => $"`{a}`"))}." : "") + "\n");
                if (g.Fields.Count == 0) continue;
                s.Append("\n| Field | Offset | Type | Meaning |\n|---|---|---|---|\n");
                foreach (var f in g.Fields) s.Append($"| `{f.Name}` | +0x{f.Offset:X} | {f.Type} | {f.Meaning.Replace("|", "\\|")} |\n");
            }
        }
        if (apiHeader != null) Api(apiHeader);
        if (Functions.Count > 0) FunctionsSection();
        if (Globals.Count > 0) GlobalsSection();
        string Applies(SdkField f) => f.Applies + (f.AppliesVerified ? "" : "?");
        void Component(SdkComponent c)
        {
            Entry($"Hf{c.Name}", $"Hf{c.Name}");
            s.Append($"\n`{c.Component}`: {(c.Table ? $"{c.Stride}-byte rows found by id" + (c.Row != null ? $", `api->{c.Row}`" : "") : $"component {c.Index}, {c.Stride}-byte records")}. {c.Meaning}.\n\n");
            if (c.Fields.Count == 0) { s.Append("No fields mapped yet.\n"); return; }
            s.Append("| Field | Offset | Type | Applies | Meaning |\n|---|---|---|---|---|\n");
            foreach (var f in c.Fields)
                s.Append($"| `{f.Name}` | +0x{f.Offset:X2} | {f.Type}{(f.Count > 1 || f.Type == "bytes" ? $"[{f.Count}]" : "")} | {Applies(f)} | {f.Meaning.Replace("|", "\\|")}{(f.Verified ? "" : " (unverified)")}{(f.Ref != null ? $" ({(f.Ref == "explosion" ? "an" : "a")} {f.Ref} reference)" : "")} |\n");
            foreach (var g in c.Groups) s.Append($"\nGroup `{g.Name}` ({g.Meaning}): {string.Join(", ", g.Fields.Select(f => $"`{f}`"))}.\n");
        }
        Section("Components", """
            Entity component records. In a definition block, `HF_FIELD(Component, field)` names a field; for an entity's own
            record, `hf_record(mod, entity, HfComponent)` gives a typed pointer (both in `hellforge_mod.h`). Each entry starts
            with the game's name for the component, its component number and its record size, then lists its fields:

            - Offset: where the field starts in the record, in bytes.
            - Type: `u8`, `u16`, `u32`, `i32`, `f32`, `u64` or `bytes`, with a count in brackets for an array.
            - Applies: when a live change takes effect in a running game. `now` is the next time the game reads it, `respawn`
              when the entity is next made (a spawn, a re-equip, the next call-in), `mission` when a mission loads. A `?` means
              inferred, not yet confirmed.
            - Meaning: what the field does. "(unverified)" marks a field named from research but not yet confirmed in the game;
              "(a projectile reference)" and the like mark a field whose value is a row of that table.

            Each component's layout is the game build its offsets were reviewed on. The loader refuses fields of a layout it
            does not have, so a mod built against another layout is reported instead of run.

            See also: [Content](modding/content.md) for changing existing content by field name, [Logic at run
            time](modding/runtime.md) for live edits, [Stats mods](modding/stats-mods.md) for number changes without code, and
            [Debugging](modding/debugging.md) for why a field was refused.

            """);
        foreach (var c in Components.Where(c => !c.Table)) Component(c);
        Section("Settings tables", """
            Rows of the game's settings tables, found by their id (`set_live`, `set_field`), or through a row function where the
            entry names one (`api->...`). The columns are the same as for components; each entry gives the row size instead of
            a component number.

            See also: [Content](modding/content.md) for settings rows and their row functions, [Logic at run
            time](modding/runtime.md) for live edits, and [Stats mods](modding/stats-mods.md) for projectiles, damage, explosions
            and statuses without code.

            """);
        foreach (var c in Components.Where(c => c.Table)) Component(c);
        Section("Content", """
            Vanilla content constants (`hellforge_content.h`), one list per kind. `hf->find(HF_KIND_..., "name")` resolves the
            same names at run time. Each list is folded: open it to see every constant with its value and the game's name for
            it.

            See also: [Content](modding/content.md) for finding content, including content other mods added.

            """);
        foreach (var kind in Kinds)
        {
            var items = Content.Where(c => c.Kind == kind.Key).ToList();
            if (items.Count == 0) continue;
            Entry($"{kind.Prefix}...", $"{kind.Prefix}... ({items.Count}, `{KindConstant(kind.Prefix)}`)");
            s.Append($"\n{kind.Doc.Replace("\n * ", " ")}\n\n<details><summary>All {items.Count}</summary>\n\n| Constant | Value | Name |\n|---|---|---|\n");
            foreach (var item in items)
                s.Append($"| `{kind.Prefix}{item.Constant}` | {(kind.Wide ? $"0x{item.Id:X16}" : item.Id > 0xFFFF || kind.Key == "traits" ? $"0x{item.Id:X}" : item.Id.ToString(CultureInfo.InvariantCulture))} | {item.Name.Replace("|", "\\|")}{(item.Note.Length > 0 ? " - " + item.Note.Replace("|", "\\|") : "")} |\n");
            s.Append("\n</details>\n");
        }

        var page = new StringBuilder();
        page.Append($"""
            # HellForge mod SDK reference (game build {Build})

            For code mod authors who need to look something up: every call in the `HfApi` table, every game function and global mapped
            by name, every component and settings table field, and every vanilla content constant, for game build {Build}. To
            learn how to make a mod, follow the
            guide in [modding/README.md](modding/README.md), starting with [your first mod](modding/first-mod.md), and keep this
            page open beside it. Models, textures, effects, sounds and icons are covered in [Data mods](modding/data.md).

            Search the page for the name you need (a call such as `set_live`, a struct such as `HfWeaponMagazine`, a constant
            such as `HF_ITEM_...`), or use the contents below.

            This page is generated by `HellForge.Cli sdk-headers` from `native/mod-sdk/hellforge_mod.h` (the API, as documented
            there) and `mappings/` (functions, globals, fields and content); do not edit it by hand. To change it, edit those sources and regenerate
            (docs/sdk-roadmap.md, "Mappings"; [Tools and sharing](modding/tools.md) has the command).

            ## Contents


            """);
        foreach (var (section, entries) in contents)
            page.Append($"- [{section}](#{Anchor(section)})" +
                (entries.Count > 0 ? ": " + string.Join(", ", entries.Select(e => $"[{e.Title}](#{Anchor(e.Heading)})")) : "") + "\n");
        page.Append(s);
        return page.ToString().ReplaceLineEndings("\n");
    }

    // The anchor GitHub gives a heading: its text without markup, lower case, punctuation other than - and _ dropped,
    // spaces as hyphens.
    static string Anchor(string heading)
    {
        var text = new StringBuilder();
        foreach (var ch in heading.Replace("`", "").ToLowerInvariant())
            if (char.IsLetterOrDigit(ch) || ch is '-' or '_') text.Append(ch);
            else if (ch == ' ') text.Append('-');
        return text.ToString();
    }

    // HfApi's members, each with the comment above it, grouped by the SDK version that added them.
    static IEnumerable<string> ApiReference(string header)
    {
        var start = header.IndexOf("typedef struct HfApi\n{", StringComparison.Ordinal);
        var end = header.IndexOf("} HfApi;", Math.Max(start, 0), StringComparison.Ordinal);
        if (start < 0 || end < 0) throw new InvalidDataException("hellforge_mod.h has no HfApi table.");
        var comment = new List<string>();
        var inComment = false;
        yield return "\n### SDK 1\n";
        foreach (var raw in header[start..end].Split('\n').Skip(2))
        {
            var line = raw.Trim();
            if (line.StartsWith("/* ----", StringComparison.Ordinal))
            {
                comment.Clear();
                var version = line.Trim('/', '*', ' ', '-');
                yield return $"\n### {version}\n";
                continue;
            }
            if (inComment || line.StartsWith("/*", StringComparison.Ordinal))
            {
                inComment = AddComment(line, comment);
                continue;
            }
            if (MemberEntry(line, comment) is { } entry)
            {
                yield return entry;
                comment.Clear();
            }
            else if (line.Length == 0) comment.Clear();
        }
    }

    // A comment line's text, added to comment: true while the comment goes on past this line.
    static bool AddComment(string line, List<string> comment)
    {
        var text = line.TrimStart('/').TrimStart('*').Trim();
        if (text.EndsWith("*/", StringComparison.Ordinal)) text = text[..^2].Trim();
        if (text.Length > 0) comment.Add(text);
        return !line.EndsWith("*/", StringComparison.Ordinal);
    }

    // A member's entry: a function with the comment above it, or a data member with its own; null for any other line.
    static string? MemberEntry(string line, List<string> comment)
    {
        var member = System.Text.RegularExpressions.Regex.Match(line, @"^(.+?)\s*\(\*(\w+)\)\((.*)\);");
        var data = System.Text.RegularExpressions.Regex.Match(line, @"^(\w+)\s+(\w+);\s*(?:/\*\s*(.*?)\s*\*/)?$");
        if (member.Success)
            return $"- `{member.Groups[2].Value}`: `{member.Groups[1].Value} ({member.Groups[3].Value})`" +
                (comment.Count > 0 ? ". " + string.Join(" ", comment) : "");
        if (data.Success)
            return $"- `{data.Groups[2].Value}`: `{data.Groups[1].Value}`" + (data.Groups[3].Value.Length > 0 ? ". " + data.Groups[3].Value : "");
        return null;
    }

    // HF_ITEM_ -> HF_KIND_ITEM
    static string KindConstant(string prefix) => "HF_KIND_" + prefix[3..^1];

    // Vanilla content by catalog name, for find.
    string LoaderContent()
    {
        var s = new StringBuilder();
        s.Append($"/* Vanilla content by catalog name, for game build {Build}: what find resolves.\n * {Generated} Included by fields.c. */\n");
        s.Append("static const SdkContent kSdkContent[] = {\n");
        foreach (var item in Content)
        {
            var kind = Kinds.First(k => k.Key == item.Kind);
            s.Append($"    {{{KindConstant(kind.Prefix)}, \"{CString(item.Name)}\", 0x{item.Id:X}ull}},\n");
        }
        s.Append("};\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    // Text as comment lines of 120 columns at most: the first line after `first`, the others after `rest`.
    static string Wrap(string text, string first, string rest)
    {
        var s = new StringBuilder();
        var line = new StringBuilder(first);
        foreach (var word in text.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var prefix = s.Length == 0 ? first : rest;
            if (line.Length > prefix.Length && line.Length + 1 + word.Length > 120) { s.Append(line).Append('\n'); line.Clear().Append(rest); }
            if (line.Length > (s.Length == 0 ? first : rest).Length) line.Append(' ');
            line.Append(word);
        }
        return s.Append(line).Append('\n').ToString();
    }

    // "uint64_t (*HfFn_SpawnEntity)(void* manager, uint32_t entity)": a function's typedef without the typedef.
    static string Signature(SdkFunction f, string declarator) =>
        $"{f.Returns} ({declarator})({(f.Params.Count == 0 ? "void" : string.Join(", ", f.Params.Select(p => $"{p.Type} {p.Name}")))})";

    string FunctionsHeader()
    {
        var s = new StringBuilder();
        s.Append($$"""
            /* HellForge mod SDK: game functions by name, for game build {{Build}}.
             * {{Generated}}
             *
             * Hook or call a game function by its name, the compiler checking the detour's and the original's types (SDK 16;
             * docs/modding/hooks.md, "Hooking by name"). For each function:
             *   HfFn_<Name>                      its C signature;
             *   hf_fn_<Name>(mod)                the function to call, or NULL: before HF_EVENT_GAME_LOADED, on another game
             *                                    build, or when its code is not the reviewed one;
             *   hf_hook_<Name>(mod, detour, &original, priority), HF_HOOK(mod, <Name>, detour, &original)
             *                                    hooks it as hook_ex does (HF_HOOK with priority 0), only where its whole
             *                                    signature is known. Ask from hellforge_mod_init on: HellForge puts the hook
             *                                    in once the game's code is decoded and reads as reviewed; *original stays
             *                                    NULL until then. 0 when the hook is in or will be; the mod's log says when
             *                                    it went in, or why not.
             * Names stay the same across game builds; HellForge updates where they are. A renamed function keeps its old name
             * as an alias, which the loader still resolves. Calling a function through hf_fn_ runs its whole hook chain (every
             * mod's detour, then HellForge's events, then the game's code), as the game's own calls do: do not call the
             * function you are hooking from its own detour (call the original instead). On a HellForge older than SDK 16,
             * hf_fn_ returns NULL and hf_hook_ 1. hook, hook_ex and find_pattern still work as before. Included by
             * hellforge_mod.h. */
            #ifndef HELLFORGE_FUNCTIONS_H
            #define HELLFORGE_FUNCTIONS_H

            /* Whether the loader has function and hook_function (SDK 16). */
            static inline int hf_has_functions_(HfMod* mod) { return HF_API(mod)->size >= offsetof(HfApi, hook_function) + sizeof(void*); }
            static inline void* hf_function_(HfMod* mod, const char* name) { return hf_has_functions_(mod) ? (void*)HF_API(mod)->function(mod, name) : NULL; }
            static inline int hf_hook_function_(HfMod* mod, const char* name, void* detour, void** original, int32_t priority)
            {
                return hf_has_functions_(mod) ? HF_API(mod)->hook_function(mod, name, detour, original, priority) : 1;
            }

            /* Hooks a mapped function with priority 0: HF_HOOK(mod, SpawnEntity, MySpawn, &g_spawn). */
            #define HF_HOOK(mod, Name, detour, original) hf_hook_##Name((mod), (detour), (original), 0)

            """);
        foreach (var f in Functions)
        {
            s.Append('\n').Append(Wrap($"{f.Name} ({f.Module}+0x{f.Rva:X}; {(f.Hook ? "hook or call" : "call only")}). {f.Meaning}.", "/* ", " * "));
            var width = f.Params.Count == 0 ? 0 : f.Params.Max(p => p.Name.Length);
            foreach (var p in f.Params) s.Append(Wrap(p.Meaning, $" *   {p.Name.PadRight(width)}  ", " *   " + new string(' ', width + 2)));
            s.Append(Wrap($"From {f.Source}. */", " * ", " * "));
            s.Append($"typedef {Signature(f, $"*HfFn_{f.Name}")};\n");
            s.Append($"static inline HfFn_{f.Name} hf_fn_{f.Name}(HfMod* mod) {{ return (HfFn_{f.Name})hf_function_(mod, \"{f.Name}\"); }}\n");
            if (f.Hook)
                s.Append($"static inline int hf_hook_{f.Name}(HfMod* mod, HfFn_{f.Name} detour, HfFn_{f.Name}* original, int32_t priority)\n{{\n" +
                    $"    return hf_hook_function_(mod, \"{f.Name}\", (void*)detour, (void**)original, priority);\n}}\n");
            foreach (var alias in f.Aliases)
            {
                s.Append($"/* {alias}: {f.Name}'s former name, kept for mods written with it; use {f.Name}. */\n");
                s.Append($"typedef HfFn_{f.Name} HfFn_{alias};\n");
                s.Append($"static inline HfFn_{f.Name} hf_fn_{alias}(HfMod* mod) {{ return hf_fn_{f.Name}(mod); }}\n");
                if (f.Hook)
                    s.Append($"static inline int hf_hook_{alias}(HfMod* mod, HfFn_{f.Name} detour, HfFn_{f.Name}* original, int32_t priority)\n{{\n" +
                        $"    return hf_hook_{f.Name}(mod, detour, original, priority);\n}}\n");
            }
        }
        s.Append("\n#endif\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    static string GlobalCType(string type) => type == "ptr" ? "void*" : CType(type);

    string GlobalsHeader()
    {
        var s = new StringBuilder();
        s.Append($$"""
            /* HellForge mod SDK: game globals by name, for game build {{Build}}.
             * {{Generated}}
             *
             * The game's objects (its managers and queues: what the functions of hellforge_functions.h take), by name (SDK 16;
             * docs/modding/hooks.md, "Game globals"). For each global:
             *   Hf<Name>                 the object, its named fields at their offsets and the bytes between as _unknown_XX
             *                            (an incomplete type where none is named yet: its comment gives what is known);
             *   hf_global_<Name>(mod)    the object, or NULL: before HF_EVENT_GAME_LOADED, on another game build, when the code
             *                            that witnesses it is not the reviewed one, or while the game has not made it (out of
             *                            a mission, some are not).
             * The object is the game's own, alive: read it, and write it only where its field says so, on the game thread (an
             * event listener's or on_game_thread's call). Ask for it each time you use it rather than keeping it: the game
             * makes some again (each mission). Use the pointer, never a copy: a struct reaches as far as its last named field
             * (HfGame is 0xAC220 bytes), so `HfGame g = *game` copies most of a megabyte onto the stack. A renamed global keeps
             * its old name as an alias. On a HellForge older than SDK 16, hf_global_ returns NULL. Included by
             * hellforge_mod.h. */
            #ifndef HELLFORGE_GLOBALS_H
            #define HELLFORGE_GLOBALS_H

            /* The object a global's cell points at, or NULL. */
            static inline void* hf_global_object_(HfMod* mod, const char* name)
            {
                const HfApi* hf = HF_API(mod);
                void* object = NULL;
                uintptr_t cell = hf->size >= offsetof(HfApi, global) + sizeof(void*) ? hf->global(mod, name) : 0;
                return cell != 0 && hf->read(cell, &object, sizeof object) == 0 ? object : NULL;
            }

            #pragma pack(push, 1)

            """);
        foreach (var g in Globals)
        {
            s.Append('\n').Append(Wrap($"{g.Name} ({g.Module}+0x{g.Rva:X}, a pointer to it). {g.Meaning}.", "/* ", " * "));
            s.Append(Wrap($"From {g.Source}. */", " * ", " * "));
            GlobalStruct(s, g);
            s.Append($"static inline Hf{g.Name}* hf_global_{g.Name}(HfMod* mod) {{ return (Hf{g.Name}*)hf_global_object_(mod, \"{g.Name}\"); }}\n");
            foreach (var alias in g.Aliases)
            {
                s.Append($"/* {alias}: {g.Name}'s former name, kept for mods written with it; use {g.Name}. */\n");
                s.Append($"typedef Hf{g.Name} Hf{alias};\n");
                s.Append($"static inline Hf{g.Name}* hf_global_{alias}(HfMod* mod) {{ return hf_global_{g.Name}(mod); }}\n");
            }
        }
        s.Append("\n#pragma pack(pop)\n\n#endif\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    // A global's object as a struct: its named fields at their offsets, the bytes between as _unknown_XX (an incomplete
    // type when none is named).
    static void GlobalStruct(StringBuilder s, SdkGlobal g)
    {
        if (g.Fields.Count == 0) { s.Append($"typedef struct Hf{g.Name} Hf{g.Name};\n"); return; }
        s.Append($"typedef struct Hf{g.Name}\n{{\n");
        uint at = 0;
        foreach (var f in g.Fields)
        {
            if (f.Offset > at) s.Append($"    uint8_t _unknown_{at:X2}[0x{f.Offset - at:X}];\n");
            s.Append($"{$"    {GlobalCType(f.Type)} {f.Name};".PadRight(40)} /* +0x{f.Offset:X} {f.Meaning} */\n");
            at = f.Offset + (uint)GlobalFieldSize(f.Type);
        }
        s.Append($"}} Hf{g.Name};\n");
        foreach (var f in g.Fields) s.Append($"HF_ASSERT_OFFSET(Hf{g.Name}, {f.Name}, 0x{f.Offset:X});\n");
    }

    // The globals by name, for global: a table ending in {0}.
    string LoaderGlobals()
    {
        var s = new StringBuilder();
        s.Append($"/* Game globals by name, for game build {Build}: what global resolves (a former name as a row of its own).\n * Witnesses: up to 3, any one of which must read as reviewed.\n * {Generated} Included by globals.c. */\n");
        s.Append($"#define SDK_GLOBALS_BUILD \"{Build}\"\n");
        s.Append("static const SdkGlobal kSdkGlobals[] = {\n");
        foreach (var g in Globals)
        {
            var rvas = string.Join(", ", Enumerable.Range(0, 3).Select(i => i < g.Witnesses.Count ? $"0x{g.Witnesses[i].Rva:X}u" : "0"));
            var codes = string.Join(", ", Enumerable.Range(0, 3).Select(i => i < g.Witnesses.Count ? $"\"{g.Witnesses[i].Code}\"" : "NULL"));
            foreach (var name in g.Aliases.Prepend(g.Name)) s.Append($"    {{\"{name}\", \"{g.Module}\", 0x{g.Rva:X}u, {{{rvas}}}, {{{codes}}}}},\n");
        }
        s.Append("    {0},\n};\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    // The functions by name, for function and hook_function: a table ending in {0}.
    string LoaderFunctions()
    {
        var s = new StringBuilder();
        s.Append($"/* Game functions by name, for game build {Build}: what function and hook_function resolve (a former name as a row\n * of its own). {Generated} Included by functions.c. */\n");
        s.Append($"#define SDK_FUNCTIONS_BUILD \"{Build}\"\n");
        s.Append("static const SdkFunction kSdkFunctions[] = {\n");
        foreach (var f in Functions)
            foreach (var name in f.Aliases.Prepend(f.Name)) s.Append($"    {{\"{name}\", \"{f.Module}\", 0x{f.Rva:X}u, \"{f.Code}\", {(f.Hook ? 1 : 0)}}},\n");
        s.Append("    {0},\n};\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    static string CString(string text)
    {
        var s = new StringBuilder();
        foreach (var b in Encoding.UTF8.GetBytes(text))
            s.Append(b is (byte)'"' or (byte)'\\' ? $"\\{(char)b}" : b is >= 0x20 and < 0x7F ? ((char)b).ToString() : $"\\{Convert.ToString(b, 8).PadLeft(3, '0')}");
        return s.ToString();
    }

    string LoaderTable()
    {
        var s = new StringBuilder();
        s.Append($"/* The loader's table of reviewed component fields, for game build {Build}: what a mod's HF_FIELD must match.\n * {Generated} Included by fields.c. */\n");
        foreach (var c in Components)
        {
            s.Append($"static const SdkField kSdkFields{c.Name}[] = {{\n");
            foreach (var f in c.Fields)
                s.Append($"    {{\"{f.Name}\", {f.Offset}, {f.Size}, {TypeTag(f.Type)}, HF_APPLIES_{f.Applies.ToUpperInvariant()}, REF_{(f.Ref ?? "none").ToUpperInvariant()}}},\n");
            s.Append("};\n");
        }
        s.Append("static const SdkComponent kSdkComponents[] = {\n");
        foreach (var c in Components)
            s.Append($"    {{\"{c.Name}\", \"{c.Component}\", {c.Layout}u, {c.Stride}u, kSdkFields{c.Name}, {c.Fields.Count}, {(c.Table ? 1 : 0)}, {c.Index}}},\n");
        s.Append("};\n");
        return s.ToString().ReplaceLineEndings("\n");
    }

    // A component's "applies" (required), or a field's (defaulting to its component's).
    static string Applies(JsonElement element, string where, string? fallback)
    {
        if (!element.TryGetProperty("applies", out _)) return fallback ?? throw new InvalidDataException($"{where}: missing applies (now, respawn or mission).");
        var value = Text(element, "applies");
        return AppliesValues.Contains(value) ? value : throw new InvalidDataException($"{where}: applies must be now, respawn or mission.");
    }

    static void Identifier(string name, string where)
    {
        if (name.Length == 0 || !(char.IsAsciiLetter(name[0]) || name[0] == '_') || !name.All(ch => char.IsAsciiLetterOrDigit(ch) || ch == '_'))
            throw new InvalidDataException($"{where}: not a C identifier.");
    }
    static void Allowed(JsonElement element, string where, params string[] names)
    {
        if (element.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{where}: must be an object.");
        foreach (var property in element.EnumerateObject())
            if (!names.Contains(property.Name)) throw new InvalidDataException($"{where}: unknown key {property.Name}.");
    }
    static JsonElement Property(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) ? value : throw new InvalidDataException($"Missing {name}.");
    static string Text(JsonElement element, string name) =>
        Property(element, name) is { ValueKind: JsonValueKind.String } value ? value.GetString()! : throw new InvalidDataException($"{name} must be text.");
    static int Int(JsonElement element, string name) =>
        Property(element, name) is { ValueKind: JsonValueKind.Number } value && value.TryGetInt32(out var number) ? number : throw new InvalidDataException($"{name} must be an integer.");
    static bool Bool(JsonElement element, string name) =>
        Property(element, name) is { ValueKind: JsonValueKind.True or JsonValueKind.False } value ? value.GetBoolean() : throw new InvalidDataException($"{name} must be true or false.");
}
