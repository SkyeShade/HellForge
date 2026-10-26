using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.SdkShimGen;

// The Steam bridge's generated half (docs/steam-bridge.md, phase 3), from the pinned SDK's steam_api.json, the pinned
// steam_api64.dll (each method's vtable slot, read from its flat export's thunk) and the reviewed allow-list
// (native/steam-cloud/bridge-methods.json):
//   bridge_gen.h           the call-mode methods' numbers, and the prototypes of the hand-written methods (bridge_local.c)
//   bridge_proxies_gen.c   the game's side: a vtable per bridged interface; call-mode slots marshal to the broker,
//                          every other method the SDK declares answers as refused (logged once)
//   broker_dispatch_gen.c  the broker's side: each call-mode method's arguments read with bounds, checked as the table
//                          says, and passed to Valve's flat export
//   bridge_exports_gen.c, steam_api64_bridge.def   the bridge build of the stand-in: every export it does not write
//                          itself is a stub that refuses, so no export forwards to Valve's DLL in the game
// Generation fails on anything it cannot marshal exactly, so a table entry never guesses.
public static partial class SteamBridge
{
    const int MaxMessage = 8192 - 16;  // BRIDGE_MSG_BUF less the call's and the reply's headers
    const int DefaultString = 1024;
    const int ReturnedString = 1024;

    // InBuffer: bytes the game hands in (inBuffers: its size parameter and the most the broker accepts); OutBytes: a struct
    // the method writes through a pointer (outBytes: its size), carried as bytes.
    enum Kind { Value, SteamId, InString, Buffer, Out, OutSteamId, InBuffer, OutBytes }

    sealed record Param(string Name, string Type, Kind Kind, string CType, int Size, int Max, string? SizeParam);

    sealed record Method(string Interface, string Version, string Class, string Name, string Flat, int Slot, string ReturnType,
        IReadOnlyList<Param> Params, JsonElement? Rule)
    {
        public string Mode => Rule is { } r && r.TryGetProperty("mode", out var m) ? m.GetString()! : "refuse";
        public string Id => $"{Version}_{Name}";
    }

    sealed record Return(string CType, string Kind, int Size); // Kind: void, value, steamid, string, pointer

    public static int Write(string sdkJson, string dll, string tablePath, string handwritten, string outDir, Func<string, HashSet<string>> ownFunctions)
    {
        using var sdk = JsonDocument.Parse(File.ReadAllText(sdkJson));
        using var tableDoc = JsonDocument.Parse(File.ReadAllText(tablePath));
        var table = tableDoc.RootElement;
        if (table.GetProperty("format").GetInt32() != 1) throw new InvalidDataException("bridge-methods.json: format must be 1.");
        var types = new TypeResolver(sdk.RootElement);
        var slots = ThunkSlots(dll);
        var exports = PeTables.Exports(dll);
        var exportNames = exports.Select(e => e.Name).ToHashSet(StringComparer.Ordinal);
        var enumRefusals = table.TryGetProperty("enumRefusals", out var er)
            ? er.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.GetInt64(), StringComparer.Ordinal) : [];
        foreach (var (name, value) in enumRefusals)
            if (!types.EnumHas(name, value)) throw new InvalidDataException($"enumRefusals: {name} has no value {value}.");

        var interfaces = new List<(string Version, string Class, string Accessor, List<Method> Methods, int SlotCount)>();
        foreach (var entry in table.GetProperty("interfaces").EnumerateObject())
            interfaces.Add(BuildInterface(entry, sdk.RootElement, types, slots, exportNames, enumRefusals));

        Directory.CreateDirectory(outDir);
        var calls = interfaces.SelectMany(i => i.Methods).Where(m => m.Mode == "call").ToList();
        var callbacks = table.GetProperty("callbacks").EnumerateArray().Select(e => e.GetInt32()).ToList();
        File.WriteAllText(Path.Combine(outDir, "bridge_gen.h"), Header(interfaces.SelectMany(i => i.Methods).ToList(), calls, callbacks, types));
        File.WriteAllText(Path.Combine(outDir, "bridge_proxies_gen.c"), Proxies(interfaces, types, enumRefusals));
        File.WriteAllText(Path.Combine(outDir, "broker_dispatch_gen.c"), Dispatch(interfaces, calls, types));
        WriteExportStubs(dll, handwritten, outDir, ownFunctions);
        Console.WriteLine($"Steam bridge: {interfaces.Count} interfaces, {interfaces.Sum(i => i.Methods.Count)} methods " +
            $"({calls.Count} call, {interfaces.SelectMany(i => i.Methods).Count(m => m.Mode is "hand" or "interface")} hand, " +
            $"{interfaces.SelectMany(i => i.Methods).Count(m => m.Mode == "refuse")} refused), {callbacks.Count} callbacks.");
        return 0;
    }

    // One interface version from the table: the SDK's matching class, its accessor, its methods and its slot count,
    // every mismatch or unmarshallable method throwing.
    static (string Version, string Class, string Accessor, List<Method> Methods, int SlotCount) BuildInterface(
        JsonProperty entry, JsonElement sdkRoot, TypeResolver types, Dictionary<string, int> slots, HashSet<string> exportNames,
        Dictionary<string, long> enumRefusals)
    {
        var version = entry.Name;
        var className = entry.Value.GetProperty("class").GetString()!;
        var declared = sdkRoot.GetProperty("interfaces").EnumerateArray().FirstOrDefault(i => i.GetProperty("classname").GetString() == className);
        if (declared.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{version}: the SDK has no {className}.");
        var sdkVersion = declared.TryGetProperty("version_string", out var vs) ? vs.GetString() : null;
        if (sdkVersion != null && sdkVersion != version) throw new InvalidDataException($"{version}: the SDK's {className} is {sdkVersion}.");
        var accessor = declared.TryGetProperty("accessors", out var acc)
            ? acc.EnumerateArray().Where(a => a.GetProperty("kind").GetString() == "user").Select(a => a.GetProperty("name_flat").GetString()!).FirstOrDefault() ?? "" : "";
        var rules = entry.Value.GetProperty("methods");
        var known = new HashSet<string>(StringComparer.Ordinal);
        var methods = BuildMethods(declared, rules, slots, types, version, className, known);
        foreach (var listed in rules.EnumerateObject().Where(p => !known.Contains(p.Name)))
            throw new InvalidDataException($"{version}: the table lists {listed.Name}, which the SDK's {className} does not declare.");
        var dup = methods.GroupBy(m => m.Slot).FirstOrDefault(g => g.Count() > 1);
        if (dup != null) throw new InvalidDataException($"{version}: slot {dup.Key} holds {string.Join(", ", dup.Select(m => m.Name))}.");
        if (methods.Any(m => m.Mode == "call") && (accessor.Length == 0 || !exportNames.Contains(accessor)))
            throw new InvalidDataException($"{version}: call-mode methods need the {className} accessor export.");
        foreach (var m in methods) Validate(m, types, enumRefusals);
        return (version, className, accessor, methods, methods.Max(m => m.Slot) + 1);
    }

    // Every method the SDK's interface declares, as a Method (its slot from the DLL's thunks, its rule from the table);
    // known gathers the table's method names that matched.
    static List<Method> BuildMethods(JsonElement declared, JsonElement rules, Dictionary<string, int> slots, TypeResolver types,
        string version, string className, HashSet<string> known)
    {
        var methods = new List<Method>();
        var overloads = declared.GetProperty("methods").EnumerateArray().GroupBy(m => m.GetProperty("methodname").GetString()!).Where(g => g.Count() > 1).Select(g => g.Key).ToHashSet();
        foreach (var m in declared.GetProperty("methods").EnumerateArray())
        {
            var name = m.GetProperty("methodname").GetString()!;
            var flat = m.GetProperty("methodname_flat").GetString()!;
            if (!slots.TryGetValue(flat, out var slot)) throw new InvalidDataException($"{flat}: its thunk does not decode to a vtable slot.");
            JsonElement? rule = rules.TryGetProperty(name, out var r) ? r : null;
            if (rule != null && overloads.Contains(name)) throw new InvalidDataException($"{version}::{name} is overloaded; the table cannot name one overload.");
            if (rule != null) known.Add(name);
            var ps = m.TryGetProperty("params", out var pa) ? pa.EnumerateArray().ToList() : [];
            methods.Add(new(className, version, className, overloads.Contains(name) ? $"{name}_{slot}" : name, flat, slot,
                m.GetProperty("returntype").GetString()!, ps.Select(p => ClassifyParam(types, p, rule, $"{version}::{name}")).ToList(), rule));
        }
        return methods;
    }

    // ---- the DLL's slots ----

    // Each SteamAPI_ISteam* export is a thunk through the interface's vtable: [REX] FF /2|/4 through [reg+d], or
    // mov r10|r11,[reg+d] then jmp, with mov r10,[rcx] (the vtable itself) skipped; stops at the int3 padding.
    public static Dictionary<string, int> ThunkSlots(string dll)
    {
        using var pe = new PEReader(File.OpenRead(dll));
        var directory = pe.PEHeaders.PEHeader!.ExportTableDirectory;
        var at = directory.RelativeVirtualAddress;
        uint U32(int rva) => BitConverter.ToUInt32(pe.GetSectionData(rva).GetContent(0, 4).AsSpan());
        ushort U16(int rva) => BitConverter.ToUInt16(pe.GetSectionData(rva).GetContent(0, 2).AsSpan());
        string Name(int rva) { var b = pe.GetSectionData(rva).GetContent(); return Encoding.ASCII.GetString(b.AsSpan(0, b.IndexOf((byte)0))); }
        var slots = new Dictionary<string, int>(StringComparer.Ordinal);
        for (var i = 0; i < U32(at + 24); i++)
        {
            var name = Name((int)U32((int)U32(at + 32) + i * 4));
            if (!name.StartsWith("SteamAPI_ISteam", StringComparison.Ordinal)) continue;
            var rva = (int)U32((int)U32(at + 28) + U16((int)U32(at + 36) + i * 2) * 4);
            if (Displacement(pe, rva, 0) is { } d && d >= 0 && d % 8 == 0) slots[name] = d / 8;
        }
        return slots;
    }

    static int? Displacement(PEReader pe, int rva, int depth)
    {
        var code = pe.GetSectionData(rva).GetContent(0, 96);
        for (var i = 0; i < code.Length - 7 && code[i] != 0xCC;)
        {
            if (code[i] == 0x4C && code[i + 1] == 0x8B && code[i + 2] == 0x11) { i += 3; continue; } // mov r10,[rcx]: the vtable itself
            if (SlotDisplacement(code, i) is { } d) return d;
            if (code[i] == 0xE9 && depth == 0) return Displacement(pe, rva + i + 5 + BitConverter.ToInt32(code.AsSpan()[(i + 1)..]), 1);
            i++;
        }
        return null;
    }

    // The vtable displacement of an FF /2|/4 or mov r10|r11,[reg+d] at code[i], or null when it is neither.
    static int? SlotDisplacement(System.Collections.Immutable.ImmutableArray<byte> code, int i)
    {
        static int Disp(System.Collections.Immutable.ImmutableArray<byte> c, int k, byte modrm) =>
            (modrm & 0xC0) switch { 0 => 0, 0x40 => (sbyte)c[k], _ => BitConverter.ToInt32(c.AsSpan()[k..]) };
        var j = (code[i] is 0x48 or 0x49) && code[i + 1] == 0xFF ? i + 1 : i;
        byte op = code[j], modrm = code[j + 1];
        if (op == 0xFF && ((modrm >> 3) & 7) is 2 or 4 && (modrm & 0xC0) != 0xC0 && (modrm & 7) is not (4 or 5)) return Disp(code, j + 2, modrm);
        if (code[i] == 0x4C && code[i + 1] == 0x8B && (code[i + 2] & 0xC0) != 0xC0 && (code[i + 2] & 7) is not (4 or 5)) return Disp(code, i + 3, code[i + 2]);
        return null;
    }

    // ---- types ----

    sealed class TypeResolver
    {
        readonly Dictionary<string, string> typedefs = new(StringComparer.Ordinal);
        readonly Dictionary<string, HashSet<long>> enums = new(StringComparer.Ordinal);

        public TypeResolver(JsonElement sdk)
        {
            foreach (var t in sdk.GetProperty("typedefs").EnumerateArray()) typedefs[t.GetProperty("typedef").GetString()!] = t.GetProperty("type").GetString()!;
            void Enums(JsonElement list)
            {
                foreach (var e in list.EnumerateArray())
                    enums[e.GetProperty("enumname").GetString()!] = e.GetProperty("values").EnumerateArray().Select(v => long.Parse(v.GetProperty("value").GetString()!)).ToHashSet();
            }
            Enums(sdk.GetProperty("enums"));
            foreach (var i in sdk.GetProperty("interfaces").EnumerateArray()) if (i.TryGetProperty("enums", out var e)) Enums(e);
        }

        public bool IsEnum(string type) => enums.ContainsKey(type);
        public bool EnumHas(string type, long value) => enums.TryGetValue(type, out var v) && v.Contains(value);

        // A scalar's C type and size, or null.
        public (string CType, int Size)? Scalar(string type)
        {
            for (var depth = 0; depth < 8; depth++)
            {
                if (enums.ContainsKey(type)) return ("int32_t", 4);
                switch (type)
                {
                    case "bool": return ("bool", 1);
                    case "char": case "signed char": case "int8": return ("int8_t", 1);
                    case "unsigned char": case "uint8": return ("uint8_t", 1);
                    case "short": case "int16": return ("int16_t", 2);
                    case "unsigned short": case "uint16": return ("uint16_t", 2);
                    case "int": case "int32": return ("int32_t", 4);
                    case "unsigned int": case "uint32": return ("uint32_t", 4);
                    case "long long": case "int64": case "lint64": return ("int64_t", 8);
                    case "unsigned long long": case "uint64": case "ulint64": return ("uint64_t", 8);
                    case "float": return ("float", 4);
                    case "double": return ("double", 8);
                }
                if (!typedefs.TryGetValue(type, out var next)) return null;
                type = next;
            }
            return null;
        }
    }

    static Param ClassifyParam(TypeResolver types, JsonElement p, JsonElement? rule, string where)
    {
        var name = p.GetProperty("paramname").GetString()!;
        var type = p.GetProperty("paramtype").GetString()!.Trim();
        int Get(string section, int fallback) => rule is { } r && r.TryGetProperty(section, out var s) && s.TryGetProperty(name, out var v) ? v.GetInt32() : fallback;
        bool Listed(string section) => rule is { } r && r.TryGetProperty(section, out var s) && s.EnumerateArray().Any(x => x.GetString() == name);
        if (type == "CSteamID") return new(name, type, Kind.SteamId, "uint64_t", 8, 0, null);
        if (types.Scalar(type) is { } scalar) return new(name, type, Kind.Value, scalar.CType, scalar.Size, 0, null);
        if (type == "const char *") return new(name, type, Kind.InString, "const char *", 0, Get("strings", DefaultString), null);
        if (RuleParam(name, type, rule) is { } ruleParam) return ruleParam;
        if (type.EndsWith('*') && Listed("outs")) return OutParam(name, type, types, where);
        // anything else: only a refused or hand-written method may have it (Validate)
        return new(name, type, (Kind)(-1), type.EndsWith('*') ? "void *" : "uint64_t", 0, 0, null);
    }

    // A parameter the table describes as an in-buffer, an out-bytes struct or a sized buffer, or null for none of those.
    static Param? RuleParam(string name, string type, JsonElement? rule)
    {
        if (rule is not { } r) return null;
        if (r.TryGetProperty("inBuffers", out var inBuffers) && inBuffers.TryGetProperty(name, out var ib))
            return new(name, type, Kind.InBuffer, "const void *", 0, ib.GetProperty("max").GetInt32(), ib.GetProperty("size").GetString());
        if (type.EndsWith('*') && r.TryGetProperty("outBytes", out var outBytes) && outBytes.TryGetProperty(name, out var ob))
            return new(name, type, Kind.OutBytes, "void *", ob.GetInt32(), 0, null);
        if (r.TryGetProperty("buffers", out var buffers) && buffers.TryGetProperty(name, out var b))
            return new(name, type, Kind.Buffer, type.Replace("const ", "").Trim() is "char *" ? "char *" : "void *", 0, b.GetProperty("max").GetInt32(), b.GetProperty("size").GetString());
        return null;
    }

    // A pointer parameter the table lists as an out: a CSteamID or scalar it writes through (anything else throws).
    static Param OutParam(string name, string type, TypeResolver types, string where)
    {
        var pointee = type[..^1].Trim();
        if (pointee == "CSteamID") return new(name, type, Kind.OutSteamId, "uint64_t *", 8, 0, null);
        if (types.Scalar(pointee) is { } s) return new(name, type, Kind.Out, s.CType + " *", s.Size, 0, null);
        throw new InvalidDataException($"{where}: out parameter {name} ({type}) is not a scalar.");
    }

    static Return ReturnOf(TypeResolver types, string type)
    {
        type = type.Trim();
        if (type == "void") return new("void", "void", 0);
        if (type == "CSteamID") return new("uint64_t", "steamid", 8);
        if (type == "const char *") return new("const char *", "string", 0);
        if (types.Scalar(type) is { } s) return new(s.CType, "value", s.Size);
        if (type.EndsWith('*')) return new("void *", "pointer", 8);
        // another class or struct: MSVC returns it through a hidden pointer, as CSteamID; only refused, unwritten
        return new("void", "struct", 0);
    }

    static void Validate(Method m, TypeResolver types, Dictionary<string, long> enumRefusals)
    {
        var where = $"{m.Version}::{m.Name}";
        var ret = ReturnOf(types, m.ReturnType);
        if (ret.Kind == "struct" && m.Mode != "refuse") throw new InvalidDataException($"{where}: it returns {m.ReturnType}, which only a refused method may.");
        if (m.Mode is not ("call" or "hand" or "interface" or "refuse")) throw new InvalidDataException($"{where}: mode {m.Mode}.");
        // a refused enum whose 0 is a success needs an explicit refusal value
        if (types.IsEnum(m.ReturnType) && !enumRefusals.ContainsKey(m.ReturnType) && SuccessAtZero().IsMatch(m.ReturnType))
            throw new InvalidDataException($"{where}: {m.ReturnType}'s 0 may mean success; give its refusal in enumRefusals.");
        if (m.Mode == "interface" && (m.Params.Count == 0 || m.Params[^1].Kind != Kind.InString || ret.Kind != "pointer"))
            throw new InvalidDataException($"{where}: interface mode needs a pointer result and a version string last.");
        if (m.Mode != "call") return;
        if (ret.Kind == "pointer") throw new InvalidDataException($"{where}: a pointer result cannot cross the pipe.");
        ValidateCallParams(m, where);
        ValidateMessageSize(m, ret, where);
        ValidateRuleNames(m, where);
    }

    // Every call-mode parameter marshals, and a buffer's size names an integer parameter.
    static void ValidateCallParams(Method m, string where)
    {
        foreach (var p in m.Params)
        {
            if ((int)p.Kind < 0) throw new InvalidDataException($"{where}: parameter {p.Name} ({p.Type}) needs a buffers or outs entry, or the method stays refused.");
            if (p.Kind is Kind.Buffer or Kind.InBuffer && m.Params.FirstOrDefault(q => q.Name == p.SizeParam) is not { Kind: Kind.Value } size)
                throw new InvalidDataException($"{where}: buffer {p.Name}'s size {p.SizeParam} is not an integer parameter.");
        }
    }

    // Neither the call's arguments nor its reply exceed one bridge message.
    static void ValidateMessageSize(Method m, Return ret, string where)
    {
        var args = m.Params.Sum(p => p.Kind switch { Kind.InString => 2 + p.Max, Kind.InBuffer => 4 + p.Max, Kind.Buffer or Kind.Out or Kind.OutSteamId or Kind.OutBytes => 1, _ => p.Size });
        var result = (ret.Kind == "string" ? 2 + ReturnedString : ret.Size) + m.Params.Sum(p => p.Kind switch { Kind.Buffer => p.Max, Kind.Out or Kind.OutSteamId or Kind.OutBytes => p.Size, _ => 0 });
        if (args > MaxMessage || result > MaxMessage) throw new InvalidDataException($"{where}: {Math.Max(args, result)} bytes do not fit one message.");
    }

    // Every parameter the table's checks name is a parameter, and an adjusted one is an integer.
    static void ValidateRuleNames(Method m, string where)
    {
        foreach (var rangeName in Names(m.Rule, "range").Concat(Names(m.Rule, "ownLobby")).Concat(Names(m.Rule, "printable")).Concat(Names(m.Rule, "logged"))
            .Concat(Names(m.Rule, "adjust")).Concat(Names(m.Rule, "p2pPeer")).Concat(Names(m.Rule, "refusedStrings")))
            if (m.Params.All(p => p.Name != rangeName)) throw new InvalidDataException($"{where}: the table names {rangeName}, which is not a parameter.");
        foreach (var refused in Names(m.Rule, "refusedStrings"))
        {
            if (m.Params.First(p => p.Name == refused).Kind != Kind.InString) throw new InvalidDataException($"{where}: only a string can have refused texts ({refused}).");
            if (RefusedTexts(m, refused).Any(t => t.Length == 0 || !t.All(c => c is >= ' ' and <= '~' and not '"' and not '\\')))
                throw new InvalidDataException($"{where}: {refused}'s refused texts must be printable, without quotes or backslashes.");
        }
        foreach (var adjusted in Names(m.Rule, "adjust"))
            if (m.Params.First(p => p.Name == adjusted).Kind != Kind.Value) throw new InvalidDataException($"{where}: only an integer parameter can be adjusted ({adjusted}).");
    }

    [GeneratedRegex("^E(BeginAuthSessionResult|UserHasLicenseForAppResult|VoiceResult|Result)$")]
    private static partial Regex SuccessAtZero();

    static IEnumerable<string> Names(JsonElement? rule, string section) =>
        rule is { } r && r.TryGetProperty(section, out var s)
            ? s.ValueKind == JsonValueKind.Array ? s.EnumerateArray().Select(x => x.GetString()!) : s.EnumerateObject().Select(x => x.Name)
            : [];

    // "refusedStrings": {"<parameter>": ["<text>", ...]}, texts the parameter may not be (ASCII case ignored)
    static IEnumerable<string> RefusedTexts(Method m, string parameter) =>
        m.Rule is { } r && r.TryGetProperty("refusedStrings", out var s) && s.TryGetProperty(parameter, out var texts)
            ? texts.EnumerateArray().Select(x => x.GetString() ?? "")
            : [];

    // ---- C ----

    static string Signature(Method m, TypeResolver types, string name, bool withSelf = true)
    {
        var ret = ReturnOf(types, m.ReturnType);
        var ps = new List<string>();
        if (withSelf) ps.Add("void *self");
        if (ret.Kind == "steamid") ps.Add("uint64_t *ret_");
        if (ret.Kind == "struct") ps.Add("void *ret_");
        foreach (var p in m.Params) ps.Add(p.Kind switch
        {
            Kind.InString => $"const char *{p.Name}",
            Kind.InBuffer => $"const void *{p.Name}",
            Kind.OutBytes => $"void *{p.Name}",
            Kind.Buffer => $"{p.CType}{p.Name}",
            Kind.Out or Kind.OutSteamId => $"{p.CType}{p.Name}",
            Kind.SteamId => $"uint64_t {p.Name}",
            Kind.Value => $"{p.CType} {p.Name}",
            _ => $"{p.CType} {p.Name}",
        });
        var retType = ret.Kind switch { "steamid" => "uint64_t *", "struct" => "void *", _ => ret.CType + (ret.CType.EndsWith('*') ? "" : " ") };
        return $"{retType}{name}({string.Join(", ", ps)})";
    }

    static string Refusal(Method m, TypeResolver types, Dictionary<string, long> enumRefusals)
    {
        var ret = ReturnOf(types, m.ReturnType);
        return ret.Kind switch
        {
            "void" => "return;",
            "steamid" => "*ret_ = 0; return ret_;",
            "struct" => "return ret_;",
            "string" => "return \"\";",
            "pointer" => "return NULL;",
            _ => enumRefusals.TryGetValue(m.ReturnType, out var v) ? $"return ({ret.CType}){v};" : $"return ({ret.CType})0;",
        };
    }

    static string Header(List<Method> all, List<Method> calls, List<int> callbacks, TypeResolver types)
    {
        var s = new StringBuilder();
        s.AppendLine("// Generated by HellForge.SdkShimGen steam-bridge (SteamBridge.cs): do not edit.");
        s.AppendLine("#pragma once");
        s.AppendLine("#include <stdbool.h>\n#include <stdint.h>\n#include \"bridge_protocol.h\"\n");
        s.AppendLine("enum {");
        for (var i = 0; i < calls.Count; i++) s.AppendLine($"    BRIDGE_M_{calls[i].Id} = BRIDGE_FN_METHODS + {i},");
        s.AppendLine($"    BRIDGE_M_COUNT = {calls.Count}");
        s.AppendLine("};\n");
        s.AppendLine($"static const int g_bridgeCallbacks[{callbacks.Count}] = {{ {string.Join(", ", callbacks)} }};");
        s.AppendLine($"#define BRIDGE_CALLBACK_COUNT {callbacks.Count}\n");
        s.AppendLine("// Each method's vtable slot, as the pinned steam_api64.dll's thunks give it.");
        foreach (var m in all) s.AppendLine($"#define BRIDGE_SLOT_{m.Id} {m.Slot}");
        s.AppendLine();
        s.AppendLine("// The game's side (bridge_proxies_gen.c): the proxy object of an interface version, or NULL.");
        s.AppendLine("void *BridgeProxy(const char *version);\n");
        s.AppendLine("// Written by hand in bridge_local.c, with these signatures (the methods' vtable form: self first, a CSteamID");
        s.AppendLine("// result through ret_):");
        foreach (var m in all.Where(m => m.Mode == "hand")) s.AppendLine(Signature(m, types, $"Hand_{m.Id}") + ";");
        return s.ToString();
    }

    static string Proxies(List<(string Version, string Class, string Accessor, List<Method> Methods, int SlotCount)> interfaces, TypeResolver types,
        Dictionary<string, long> enumRefusals)
    {
        var s = new StringBuilder();
        s.AppendLine("// Generated by HellForge.SdkShimGen steam-bridge (SteamBridge.cs): do not edit.");
        s.AppendLine("#include <string.h>\n#include \"bridge_gen.h\"\n#include \"bridge_local.h\"\n");
        s.AppendLine("static uint64_t BridgeSlotPrivate(void) { BridgeRefused(\"a private method\"); return 0; }\n");
        foreach (var (version, cls, _, methods, slotCount) in interfaces)
        {
            foreach (var m in methods)
            {
                var name = $"P_{m.Id}";
                switch (m.Mode)
                {
                    case "hand":
                        continue;
                    case "interface":
                        s.AppendLine($"static {Signature(m, types, name)} {{ (void)self; return BridgeInterface({m.Params[^1].Name}); }}");
                        break;
                    case "refuse":
                        s.AppendLine($"static {Signature(m, types, name)}");
                        s.AppendLine($"{{ (void)self; BridgeRefused(\"{cls}::{m.Name}\"); {Unused(m)}{Refusal(m, types, enumRefusals)} }}");
                        break;
                    case "call":
                        s.Append(ProxyCall(m, types, enumRefusals));
                        break;
                }
            }
            s.AppendLine($"static void *const VT_{version}[{slotCount}] = {{");
            for (var slot = 0; slot < slotCount; slot++)
            {
                var m = methods.FirstOrDefault(x => x.Slot == slot);
                s.AppendLine(m == null ? "    (void *)BridgeSlotPrivate," : $"    (void *){(m.Mode == "hand" ? "Hand_" : "P_")}{m.Id}, /* {slot} {m.Name} */");
            }
            s.AppendLine("};");
            s.AppendLine($"static struct {{ void *const *vtable; }} PX_{version} = {{ VT_{version} }};\n");
        }
        s.AppendLine("void *BridgeProxy(const char *version)\n{\n    if (!version) return NULL;");
        foreach (var i in interfaces) s.AppendLine($"    if (strcmp(version, \"{i.Version}\") == 0) return &PX_{i.Version};");
        s.AppendLine("    return NULL;\n}");
        return s.ToString();
    }

    static string Unused(Method m) => string.Concat(m.Params.Select(p => $"(void){p.Name}; "));

    static string ProxyCall(Method m, TypeResolver types, Dictionary<string, long> enumRefusals)
    {
        var ret = ReturnOf(types, m.ReturnType);
        var s = new StringBuilder();
        s.AppendLine($"static {Signature(m, types, $"P_{m.Id}")}");
        s.AppendLine("{");
        s.AppendLine("    (void)self;");
        s.AppendLine("    BridgeWriter w = BridgeWriterStart();");
        foreach (var p in m.Params)
            switch (p.Kind)
            {
                case Kind.Value: s.AppendLine($"    BridgePut(&w, &{p.Name}, {p.Size});"); break;
                case Kind.SteamId: s.AppendLine($"    BridgePut(&w, &{p.Name}, 8);"); break;
                case Kind.InString: s.AppendLine($"    if (!BridgePutString(&w, {p.Name}, {p.Max})) {{ BridgeRefused(\"{m.Class}::{m.Name}: {p.Name} too long\"); {Refusal(m, types, enumRefusals)} }}"); break;
                case Kind.Buffer:
                    s.AppendLine($"    int64_t cap_{p.Name} = {p.Name} && {p.SizeParam} > 0 ? (int64_t){p.SizeParam} : 0;");
                    s.AppendLine($"    if (cap_{p.Name} > {p.Max}) {{ BridgeRefused(\"{m.Class}::{m.Name}: {p.Name} larger than {p.Max} bytes\"); {Refusal(m, types, enumRefusals)} }}");
                    s.AppendLine($"    BridgePutFlag(&w, {p.Name} != NULL);");
                    break;
                case Kind.Out: case Kind.OutSteamId: case Kind.OutBytes: s.AppendLine($"    BridgePutFlag(&w, {p.Name} != NULL);"); break;
                case Kind.InBuffer:
                    s.AppendLine($"    int64_t len_{p.Name} = {p.Name} && {p.SizeParam} > 0 ? (int64_t){p.SizeParam} : 0;");
                    s.AppendLine($"    if (len_{p.Name} > {p.Max}) {{ BridgeRefused(\"{m.Class}::{m.Name}: {p.Name} larger than {p.Max} bytes\"); {Refusal(m, types, enumRefusals)} }}");
                    s.AppendLine($"    uint32_t n_{p.Name} = (uint32_t)len_{p.Name};\n    BridgePut(&w, &n_{p.Name}, 4);\n    if (n_{p.Name}) BridgePut(&w, {p.Name}, n_{p.Name});");
                    break;
            }
        s.AppendLine("    BridgeReader r;");
        s.AppendLine($"    if (!BridgeInvoke(BRIDGE_M_{m.Id}, &w, &r)) {{ {Refusal(m, types, enumRefusals)} }}");
        switch (ret.Kind)
        {
            case "value": s.AppendLine($"    {ret.CType} result_ = 0;\n    if (!BridgeGet(&r, &result_, {ret.Size})) {{ {Refusal(m, types, enumRefusals)} }}"); break;
            case "steamid": s.AppendLine($"    uint64_t result_ = 0;\n    if (!BridgeGet(&r, &result_, 8)) {{ {Refusal(m, types, enumRefusals)} }}"); break;
            case "string": s.AppendLine($"    static char result_[{ReturnedString + 1}];\n    if (!BridgeGetString(&r, result_, sizeof result_)) return \"\";"); break;
        }
        foreach (var p in m.Params)
            switch (p.Kind)
            {
                case Kind.Out: case Kind.OutSteamId: case Kind.OutBytes: s.AppendLine($"    if ({p.Name}) BridgeGet(&r, {p.Name}, {p.Size});"); break;
                case Kind.Buffer: s.AppendLine($"    if ({p.Name}) BridgeGet(&r, {p.Name}, (uint32_t)cap_{p.Name});"); break;
            }
        s.AppendLine(ret.Kind switch { "void" => "    return;", "steamid" => "    *ret_ = result_;\n    return ret_;", _ => "    return result_;" });
        s.AppendLine("}");
        return s.ToString();
    }

    static string Dispatch(List<(string Version, string Class, string Accessor, List<Method> Methods, int SlotCount)> interfaces, List<Method> calls,
        TypeResolver types)
    {
        var s = new StringBuilder();
        s.AppendLine("// Generated by HellForge.SdkShimGen steam-bridge (SteamBridge.cs): do not edit.");
        s.AppendLine("#include <string.h>\n#include \"bridge_gen.h\"\n#include \"broker_dispatch.h\"\n");
        var accessorOf = interfaces.ToDictionary(i => i.Version, i => i.Accessor);
        s.AppendLine("uint32_t BrokerDispatchMethod(const BrokerApi *api, uint32_t method, BridgeReader *r, BridgeWriter *w)\n{");
        s.AppendLine("    switch (method)\n    {");
        foreach (var m in calls) DispatchCase(s, m, types, accessorOf);
        s.AppendLine("    default:\n        return BRIDGE_STATUS_UNKNOWN;\n    }\n}");
        return s.ToString();
    }

    // One call-mode method's case: read its arguments, apply the table's checks, call Valve's flat export, write the reply.
    static void DispatchCase(StringBuilder s, Method m, TypeResolver types, Dictionary<string, string> accessorOf)
    {
        var ret = ReturnOf(types, m.ReturnType);
        s.AppendLine($"    case BRIDGE_M_{m.Id}:\n    {{");
        DispatchReadParams(s, m);
        s.AppendLine("        if (r->at != r->size) return BRIDGE_STATUS_MALFORMED;");
        foreach (var p in m.Params) DispatchParamChecks(s, m, p);
        // the broker's own rule for a value (BrokerApi.adjust), once every check of the table passed: a refused call
        // never reaches it
        foreach (var p in m.Params.Where(p => Names(m.Rule, "adjust").Contains(p.Name)))
            s.AppendLine($"        {p.Name} = ({p.CType})api->adjust(api, BRIDGE_M_{m.Id}, \"{p.Name}\", (int64_t){p.Name});");
        DispatchCall(s, m, ret, accessorOf);
        DispatchWriteOuts(s, m);
        s.AppendLine("        return w->failed ? BRIDGE_STATUS_MALFORMED : BRIDGE_STATUS_OK;\n    }");
    }

    // Reads each argument the game sent, with bounds; a malformed or over-long one ends the call.
    static void DispatchReadParams(StringBuilder s, Method m)
    {
        foreach (var p in m.Params)
            switch (p.Kind)
            {
                case Kind.Value: s.AppendLine($"        {p.CType} {p.Name} = 0;\n        if (!BridgeGet(r, &{p.Name}, {p.Size})) return BRIDGE_STATUS_MALFORMED;"); break;
                case Kind.SteamId: s.AppendLine($"        uint64_t {p.Name} = 0;\n        if (!BridgeGet(r, &{p.Name}, 8)) return BRIDGE_STATUS_MALFORMED;"); break;
                case Kind.InString:
                    s.AppendLine($"        char {p.Name}_[{p.Max + 1}];\n        int {p.Name}_null = 0;");
                    s.AppendLine($"        if (!BridgeGetBoundedString(r, {p.Name}_, {p.Max}, &{p.Name}_null)) return BRIDGE_STATUS_REFUSED;");
                    s.AppendLine($"        const char *{p.Name} = {p.Name}_null ? NULL : {p.Name}_;");
                    break;
                case Kind.Buffer: case Kind.Out: case Kind.OutSteamId: case Kind.OutBytes:
                    s.AppendLine($"        uint8_t has_{p.Name} = 0;\n        if (!BridgeGet(r, &has_{p.Name}, 1)) return BRIDGE_STATUS_MALFORMED;"); break;
                case Kind.InBuffer:
                    s.AppendLine($"        uint32_t n_{p.Name} = 0;\n        if (!BridgeGet(r, &n_{p.Name}, 4) || n_{p.Name} > {p.Max}) return BRIDGE_STATUS_MALFORMED;");
                    s.AppendLine($"        static uint8_t in_{p.Name}[{p.Max}];\n        if (n_{p.Name} && !BridgeGet(r, in_{p.Name}, n_{p.Name})) return BRIDGE_STATUS_MALFORMED;");
                    break;
            }
    }

    // The table's checks and local buffers for one parameter (range, ownLobby, printable, refusedStrings, logged, p2pPeer,
    // and the broker-side buffers the call writes through).
    static void DispatchParamChecks(StringBuilder s, Method m, Param p)
    {
        if (m.Rule is { } rule && rule.TryGetProperty("range", out var range) && range.TryGetProperty(p.Name, out var bounds))
            s.AppendLine($"        if ((int64_t){p.Name} < {bounds[0].GetInt64()} || (int64_t){p.Name} > {bounds[1].GetInt64()}) return api->refuse(api, \"{m.Class}::{m.Name}: {p.Name} out of range\");");
        if (Names(m.Rule, "ownLobby").Contains(p.Name))
            s.AppendLine($"        if (!api->ownLobby(api, {p.Name})) return api->refuse(api, \"{m.Class}::{m.Name}: not a lobby the game made\");");
        if (Names(m.Rule, "printable").Contains(p.Name))
            s.AppendLine($"        if ({p.Name} && !BridgePrintable({p.Name})) return api->refuse(api, \"{m.Class}::{m.Name}: {p.Name} is not printable\");");
        foreach (var text in RefusedTexts(m, p.Name))
            s.AppendLine($"        if ({p.Name} && BridgeSameText({p.Name}, \"{text}\")) return api->refuse(api, \"{m.Class}::{m.Name}: {p.Name} {text} is refused\");");
        if (Names(m.Rule, "logged").Contains(p.Name))
            s.AppendLine($"        api->log(api, \"{m.Class}::{m.Name} {p.Name}\", {p.Name});");
        if (Names(m.Rule, "p2pPeer").Contains(p.Name))
            s.AppendLine($"        if (!api->p2pPeer(api, {p.Name})) return api->refuse(api, \"{m.Class}::{m.Name}: not a co-op peer\");");
        // the size the game gave is the size of the bytes it sent
        if (p.Kind == Kind.InBuffer)
            s.AppendLine($"        if ((int64_t){p.SizeParam} != (int64_t)n_{p.Name}) return BRIDGE_STATUS_MALFORMED;");
        if (p.Kind == Kind.OutBytes)
            s.AppendLine($"        uint8_t out_{p.Name}[{p.Size}];\n        memset(out_{p.Name}, 0, sizeof out_{p.Name});");
        if (p.Kind == Kind.Buffer)
        {
            s.AppendLine($"        if (has_{p.Name} && ((int64_t){p.SizeParam} < 0 || (int64_t){p.SizeParam} > {p.Max})) return api->refuse(api, \"{m.Class}::{m.Name}: {p.Name} too large\");");
            s.AppendLine($"        static uint8_t buf_{p.Name}[{p.Max}];\n        uint32_t cap_{p.Name} = has_{p.Name} ? (uint32_t){p.SizeParam} : 0;\n        memset(buf_{p.Name}, 0, cap_{p.Name});");
        }
        if (p.Kind is Kind.Out or Kind.OutSteamId)
            s.AppendLine($"        {(p.Kind == Kind.OutSteamId ? "uint64_t" : p.CType[..^2])} out_{p.Name} = 0;");
    }

    // The flat export's typedef, the resolve/self lookups and the call itself, writing a value or string result.
    static void DispatchCall(StringBuilder s, Method m, Return ret, Dictionary<string, string> accessorOf)
    {
        var flatParams = new List<string> { "void *" };
        var flatArgs = new List<string> { "self_" };
        foreach (var p in m.Params)
        {
            flatParams.Add(p.Kind switch
            {
                Kind.InString => "const char *", Kind.Buffer => p.CType, Kind.SteamId => "uint64_t", Kind.OutSteamId => "uint64_t *",
                Kind.InBuffer => "const void *", Kind.OutBytes => "void *", _ => p.CType
            });
            flatArgs.Add(p.Kind switch
            {
                Kind.Buffer => $"has_{p.Name} ? ({p.CType})buf_{p.Name} : NULL",
                Kind.Out or Kind.OutSteamId => $"has_{p.Name} ? &out_{p.Name} : NULL",
                Kind.InBuffer => $"n_{p.Name} ? (const void *)in_{p.Name} : NULL",
                Kind.OutBytes => $"has_{p.Name} ? (void *)out_{p.Name} : NULL",
                _ => p.Name,
            });
        }
        var flatRet = ret.Kind switch { "void" => "void", "steamid" => "uint64_t", _ => ret.CType };
        s.AppendLine($"        typedef {flatRet} (*fn_t)({string.Join(", ", flatParams)});");
        s.AppendLine($"        static fn_t fn;\n        if (!fn) fn = (fn_t)api->resolve(api, \"{m.Flat}\");");
        s.AppendLine($"        void *self_ = api->self(api, \"{accessorOf[m.Version]}\");");
        s.AppendLine("        if (!fn || !self_) return BRIDGE_STATUS_UNKNOWN;");
        var call = $"fn({string.Join(", ", flatArgs)})";
        switch (ret.Kind)
        {
            case "void": s.AppendLine($"        {call};"); break;
            case "string": s.AppendLine($"        BridgePutStringTruncated(w, {call}, {ReturnedString});"); break;
            default: s.AppendLine($"        {flatRet} result_ = {call};\n        BridgePut(w, &result_, {(ret.Kind == "steamid" ? 8 : ret.Size)});"); break;
        }
    }

    // Writes back each out parameter and buffer the call filled.
    static void DispatchWriteOuts(StringBuilder s, Method m)
    {
        foreach (var p in m.Params)
            switch (p.Kind)
            {
                case Kind.Out: case Kind.OutSteamId: s.AppendLine($"        if (has_{p.Name}) BridgePut(w, &out_{p.Name}, {p.Size});"); break;
                case Kind.OutBytes: s.AppendLine($"        if (has_{p.Name}) BridgePut(w, out_{p.Name}, {p.Size});"); break;
                case Kind.Buffer: s.AppendLine($"        if (has_{p.Name}) BridgePut(w, buf_{p.Name}, cap_{p.Name});"); break;
            }
    }

    // The bridge build's exports: the hand-written ones as code, every other function a stub that refuses (logged once,
    // 0 in RAX), and the data exports as zeroed data. Nothing forwards to Valve's DLL.
    static void WriteExportStubs(string dll, string handwritten, string outDir, Func<string, HashSet<string>> ownFunctions)
    {
        var exports = PeTables.Exports(dll);
        var own = ownFunctions(handwritten);
        var unknown = own.Where(name => exports.All(e => e.Name != name)).ToList();
        if (unknown.Count > 0) throw new InvalidDataException("Hand-written functions the original does not export: " + string.Join(", ", unknown));
        var data = PeTables.DataExports(dll);
        var def = new StringBuilder().AppendLine($"LIBRARY {Path.GetFileName(dll)}").AppendLine("EXPORTS");
        var code = new StringBuilder();
        code.AppendLine("// Generated by HellForge.SdkShimGen steam-bridge (SteamBridge.cs): do not edit.");
        code.AppendLine("#include <stdint.h>\n#include \"bridge_local.h\"\n");
        foreach (var e in exports.OrderBy(e => e.Ordinal))
        {
            if (!Regex.IsMatch(e.Name, @"^[A-Za-z_][A-Za-z0-9_]*$")) throw new InvalidDataException($"Export {e.Name} is not a C identifier.");
            if (own.Contains(e.Name)) def.AppendLine($"    {e.Name} @{e.Ordinal}");
            else if (data.Contains(e.Name))
            {
                def.AppendLine($"    {e.Name} = hf_bridge_data_{e.Ordinal} @{e.Ordinal} DATA");
                code.AppendLine($"uint64_t hf_bridge_data_{e.Ordinal}[8];");
            }
            else
            {
                def.AppendLine($"    {e.Name} = hf_bridge_stub_{e.Ordinal} @{e.Ordinal}");
                code.AppendLine($"uint64_t hf_bridge_stub_{e.Ordinal}(void) {{ BridgeRefused(\"{e.Name}\"); return 0; }}");
            }
        }
        File.WriteAllText(Path.Combine(outDir, "steam_api64_bridge.def"), def.ToString());
        File.WriteAllText(Path.Combine(outDir, "bridge_exports_gen.c"), code.ToString());
    }
}
