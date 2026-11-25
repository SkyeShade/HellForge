using System.Globalization;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// One argument of an RPC, as the game's network configuration declares it (mappings/rpcs.json). Kind: bool, int, float,
// vector3, vector2, quaternion, direction, string, u32, u64, id64 (a name hash), array. Bits/Min/Max: an int's or float's width and range;
// MaxCount and Element: an array's.
public sealed record SdkRpcArg(string Name, string Kind, int Bits, double Min, double Max, int MaxCount, SdkRpcArg? Element);
// One RPC: its name (rpc_...), its name's hash (the id mods use), its place in this build's vanilla configuration,
// whether a session id goes with it, and its arguments in order.
public sealed record SdkRpc(string Name, uint Hash, int Index, bool SessionBound, IReadOnlyList<SdkRpcArg> Args);

// The game's RPCs for mods (SDK 17, docs/modding/networking.md): mappings/rpcs.json to hellforge_rpcs.h (a typed struct,
// reader and sender per RPC), the loader's table (native/mod-loader/sdk_rpcs.h) and docs/rpcs.md.
public sealed class SdkRpcs
{
    public const string File = "rpcs.json";
    public static readonly string[] HeaderFiles = ["native/mod-sdk/hellforge_rpcs.h", "native/mod-loader/sdk_rpcs.h", "docs/rpcs.md"];
    // HfRpcArg kinds (the game's own numbering of the network types' kinds)
    static readonly Dictionary<string, int> Kinds = new()
    {
        ["bool"] = 0, ["int"] = 1, ["float"] = 2, ["vector3"] = 3, ["quaternion"] = 4, ["string"] = 5, ["u32"] = 6, ["id64"] = 7, ["u64"] = 9,
        ["array"] = 10, ["direction"] = 11, ["vector2"] = 12,
    };
    // the game's message buffer holds 0x4B0 bytes; an argument list longer than this is refused before it is built
    public const int MaxEntries = 256;

    public string Build { get; }
    public IReadOnlyList<SdkRpc> Rpcs { get; }
    SdkRpcs(string build, IReadOnlyList<SdkRpc> rpcs) { Build = build; Rpcs = rpcs; }

    public static SdkRpcs? Load(string folder)
    {
        var path = Path.Combine(folder, File);
        if (!System.IO.File.Exists(path)) return null;
        try { return Parse(System.IO.File.ReadAllText(path)); }
        catch (JsonException error) { throw new InvalidDataException($"mappings/{File}: {error.Message}"); }
    }

    public static SdkRpcs Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        if (root.GetProperty("format").GetInt32() != 1) throw new InvalidDataException("mappings/rpcs.json: format must be 1.");
        var build = root.GetProperty("build").GetString() ?? throw new InvalidDataException("mappings/rpcs.json: no build.");
        var list = new List<SdkRpc>();
        var names = new HashSet<string>(StringComparer.Ordinal);
        var hashes = new HashSet<uint>();
        foreach (var entry in root.GetProperty("rpcs").EnumerateObject())
        {
            var rpc = ParseRpc(entry);
            if (!names.Add(rpc.Name) || !hashes.Add(rpc.Hash)) throw new InvalidDataException($"mappings/rpcs.json: {rpc.Name} twice.");
            list.Add(rpc);
        }
        list.Sort((a, b) => a.Hash.CompareTo(b.Hash));
        return new SdkRpcs(build, list);
    }

    static SdkRpc ParseRpc(JsonProperty entry)
    {
        var name = entry.Name;
        if (!name.StartsWith("rpc_", StringComparison.Ordinal) || name.Length > 63 || !name.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_'))
            throw new InvalidDataException($"mappings/rpcs.json: {name} is not an rpc_ name of a-z, 0-9 and _.");
        var v = entry.Value;
        var hash = uint.Parse(v.GetProperty("hash").GetString()!.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        if (hash != EngineHash.Id32(name)) throw new InvalidDataException($"mappings/rpcs.json: {name}'s hash is not its name's.");
        var args = new List<SdkRpcArg>();
        var argNames = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var a in v.GetProperty("args").EnumerateArray())
        {
            var i = args.Count;
            var baseName = a.TryGetProperty("name", out var n) && Identifier(n.GetString()) is { } id ? id : $"arg{i}";
            argNames[baseName] = argNames.GetValueOrDefault(baseName) + 1;
            var unique = argNames[baseName] == 1 ? baseName : $"{baseName}_{argNames[baseName]}";
            args.Add(ParseArg(a, $"{name} argument {i}") with { Name = unique });
        }
        return new SdkRpc(name, hash, v.GetProperty("index").GetInt32(), v.GetProperty("session_bound").GetBoolean(), args);
    }

    static string? Identifier(string? s)
    {
        if (string.IsNullOrEmpty(s) || s.Length > 40 || !(char.IsAsciiLetterLower(s[0]) || s[0] == '_')) return null;
        if (!s.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_')) return null;
        // C and C++ keywords (bool, float, int, unsigned are network type names) get a suffix
        return SdkMappings.IsKeyword(s) ? s + "_value" : s;
    }

    static SdkRpcArg ParseArg(JsonElement a, string at)
    {
        var kind = a.GetProperty("kind").GetString() ?? "";
        if (!Kinds.ContainsKey(kind)) throw new InvalidDataException($"mappings/rpcs.json: {at}: unknown kind {kind}.");
        int Get(string p) => a.TryGetProperty(p, out var x) ? x.GetInt32() : 0;
        double GetD(string p) => a.TryGetProperty(p, out var x) ? x.GetDouble() : 0;
        if (kind != "array") return new SdkRpcArg("", kind, Get("bits"), GetD("min"), GetD("max"), 0, null);
        var max = Get("max_count");
        if (max is < 1 or > 128) throw new InvalidDataException($"mappings/rpcs.json: {at}: an array of 1 to 128, not {max}.");
        var element = ParseArg(a.GetProperty("element"), at + " element");
        if (element.Kind == "array") throw new InvalidDataException($"mappings/rpcs.json: {at}: an array of arrays.");
        return new SdkRpcArg("", "array", 0, 0, 0, max, element);
    }

    public IReadOnlyDictionary<string, string> Generate() => new Dictionary<string, string>
    {
        [HeaderFiles[0]] = ModHeader(),
        [HeaderFiles[1]] = LoaderTable(),
        [HeaderFiles[2]] = Reference(),
    };

    const string Generated = "Generated by `hellforge sdk-headers` from mappings/rpcs.json; do not edit.";

    static string Upper(SdkRpc r) => r.Name[4..].ToUpperInvariant();
    static string Lower(SdkRpc r) => r.Name[4..];
    static string Hex(uint v) => "0x" + v.ToString("X8", CultureInfo.InvariantCulture) + "u";

    // the C type of one value of a kind, and the array suffix
    static (string Type, string Dims) CValue(string kind) => kind switch
    {
        "bool" => ("uint8_t", ""), "int" => ("int32_t", ""), "float" => ("float", ""), "vector3" or "direction" => ("float", "[3]"),
        "vector2" => ("float", "[2]"), "quaternion" => ("float", "[4]"), "string" => ("const char*", ""), "u32" => ("uint32_t", ""),
        "u64" or "id64" => ("uint64_t", ""), _ => throw new InvalidDataException(kind)
    };

    static string Describe(SdkRpcArg a) => a.Kind switch
    {
        "int" => $"int, {a.Bits} bits, {a.Min.ToString(CultureInfo.InvariantCulture)}..{a.Max.ToString(CultureInfo.InvariantCulture)}",
        "float" or "vector3" or "vector2" => a.Bits == 32 ? $"{a.Kind}, raw" :
            $"{a.Kind}, {a.Bits} bits over {a.Min.ToString(CultureInfo.InvariantCulture)}..{a.Max.ToString(CultureInfo.InvariantCulture)}",
        "array" => $"up to {a.MaxCount} x {Describe(a.Element!)}",
        "string" => "string, at most 2047 bytes",
        "id64" => "id64, a name hash",
        _ => a.Kind,
    };

    static int MaxEntriesOf(SdkRpc r) => r.Args.Sum(a => a.Kind == "array" ? 1 + a.MaxCount : 1);

    string ModHeader()
    {
        var sb = new StringBuilder();
        sb.Append($"/* HellForge mod SDK: the game's RPCs (its network messages) by name, for game build {Build}.\n");
        sb.Append($" * {Generated}\n");
        sb.Append(" *\n");
        sb.Append(" * For each RPC (SDK 17; docs/modding/networking.md, docs/rpcs.md):\n");
        sb.Append(" *   HF_RPC_<NAME>                         its id, the hash of its name (rpc_listen, rpc_send, HfRpcEvent.rpc);\n");
        sb.Append(" *   HfRpc_<name>                          its arguments as a struct (an array as <name>_count and <name>[max]);\n");
        sb.Append(" *   hf_rpc_<name>_read(event, &out)       decodes an HfRpcEvent of it: 0 done, 1 not this RPC or not its arguments;\n");
        sb.Append(" *   hf_rpc_<name>_send(mod, to, &in)      sends it through rpc_send (HF_TO_ALL, HF_TO_HOST or a peer).\n");
        sb.Append(" * Arguments are named after their network type where that name is known, else arg<n>. A string read from an\n");
        sb.Append(" * event points into the game's message and is valid during the listener's call only. A mod includes it itself\n");
        sb.Append(" * (hellforge_mod.h does not: it holds every RPC of the game). */\n");
        sb.Append("#ifndef HELLFORGE_RPCS_H\n#define HELLFORGE_RPCS_H\n#include \"hellforge_mod.h\"\n\n");
        sb.Append("static inline int hf_has_rpcs_(HfMod* mod) { return HF_API(mod)->size >= offsetof(HfApi, rpc_send) + sizeof(void*); }\n");
        sb.Append("static inline int hf_rpc_listen_(HfMod* mod, uint32_t rpc, HfRpcListener listener, void* user, int32_t priority)\n");
        sb.Append("{\n    return hf_has_rpcs_(mod) ? HF_API(mod)->rpc_listen(mod, rpc, listener, user, priority) : 1;\n}\n");
        sb.Append("static inline int hf_rpc_send_(HfMod* mod, uint32_t rpc, uint64_t to, const HfRpcArg* args, uint32_t count)\n");
        sb.Append("{\n    return hf_has_rpcs_(mod) ? HF_API(mod)->rpc_send(mod, rpc, to, args, count) : 1;\n}\n");
        sb.Append("static inline uint32_t hf_rpc_strlen_(const char* s) { uint32_t n = 0; if (s != NULL) while (s[n] != 0) n++; return n; }\n");
        sb.Append("/* The next argument, when it is of kind: its value, or NULL. */\n");
        sb.Append("static inline const void* hf_rpc_take_(const HfRpcEvent* e, uint32_t* at, uint32_t kind)\n");
        sb.Append("{\n    if (*at >= e->count || e->args[*at].kind != kind) return NULL;\n    return e->args[(*at)++].value;\n}\n");
        sb.Append("/* An array's length (its entry holds the count where a value would be), at most max; 0xFFFFFFFF when it is not one. */\n");
        sb.Append("static inline uint32_t hf_rpc_take_count_(const HfRpcEvent* e, uint32_t* at, uint32_t max)\n");
        sb.Append("{\n    if (*at >= e->count || e->args[*at].kind != HF_RPC_KIND_ARRAY) return 0xFFFFFFFFu;\n");
        sb.Append("    uint32_t n = (uint32_t)(uintptr_t)e->args[(*at)++].value;\n    return n <= max ? n : 0xFFFFFFFFu;\n}\n\n");
        foreach (var r in Rpcs) ModRpc(sb, r);
        sb.Append("#endif\n");
        return sb.ToString();
    }

    static void ReadValue(StringBuilder sb, string indent, string kind, string target, int kindId)
    {
        var (type, dims) = CValue(kind);
        var size = dims switch { "[3]" => 3, "[2]" => 2, "[4]" => 4, _ => 1 };
        sb.Append($"{indent}{{\n{indent}    const void* v = hf_rpc_take_(e, &at, {kindId}u);\n{indent}    if (v == NULL) return 1;\n");
        if (kind == "bool") sb.Append($"{indent}    {target} = *(const int32_t*)v != 0;\n");
        else if (kind == "string") sb.Append($"{indent}    {target} = (const char*)v;\n");
        else if (size == 1) sb.Append($"{indent}    {target} = *(const {type}*)v;\n");
        else sb.Append($"{indent}    for (int k = 0; k < {size}; k++) {target}[k] = ((const float*)v)[k];\n");
        sb.Append($"{indent}}}\n");
    }

    static void SendValue(StringBuilder sb, string indent, string kind, string source, int kindId, string flag)
    {
        if (kind == "bool")
            sb.Append($"{indent}{flag}[nf] = {source} != 0;\n{indent}a[n].kind = {kindId}u; a[n].size = 4; a[n].value = &{flag}[nf++]; n++;\n");
        else if (kind == "string")
            sb.Append($"{indent}a[n].kind = {kindId}u; a[n].size = hf_rpc_strlen_({source}); a[n].value = {source}; n++;\n");
        else
        {
            var (_, dims) = CValue(kind);
            var amp = dims == "" ? "&" : "";
            sb.Append($"{indent}a[n].kind = {kindId}u; a[n].size = (uint32_t)sizeof {source}; a[n].value = {amp}{source}; n++;\n");
        }
    }

    void ModRpc(StringBuilder sb, SdkRpc r)
    {
        var args = r.Args;
        sb.Append($"/* {r.Name}{(r.SessionBound ? " (session-bound)" : "")}");
        sb.Append(args.Count == 0 ? ": no arguments. */\n" : ":\n");
        foreach (var a in args) sb.Append($" *   {a.Name,-24} {Describe(a)}\n");
        if (args.Count > 0) sb.Append(" */\n");
        sb.Append($"#define HF_RPC_{Upper(r)} {Hex(r.Hash)}\n");
        var low = Lower(r);
        if (args.Count == 0)
        {
            sb.Append($"static inline int hf_rpc_{low}_read(const HfRpcEvent* e) {{ return e->rpc == HF_RPC_{Upper(r)} && e->count == 0 ? 0 : 1; }}\n");
            sb.Append($"static inline int hf_rpc_{low}_send(HfMod* mod, uint64_t to) {{ return hf_rpc_send_(mod, HF_RPC_{Upper(r)}, to, NULL, 0); }}\n\n");
            return;
        }
        ModStruct(sb, r);
        ModReader(sb, r);
        ModSender(sb, r);
    }

    static void ModStruct(StringBuilder sb, SdkRpc r)
    {
        sb.Append($"typedef struct HfRpc_{Lower(r)}\n{{\n");
        foreach (var a in r.Args)
        {
            var (t, d) = CValue(a.Kind == "array" ? a.Element!.Kind : a.Kind);
            if (a.Kind == "array") sb.Append($"    uint32_t {a.Name}_count;\n    {t} {a.Name}[{a.MaxCount}]{d};\n");
            else sb.Append($"    {t} {a.Name}{d};\n");
        }
        sb.Append($"}} HfRpc_{Lower(r)};\n");
    }

    static void ModReader(StringBuilder sb, SdkRpc r)
    {
        sb.Append($"static inline int hf_rpc_{Lower(r)}_read(const HfRpcEvent* e, HfRpc_{Lower(r)}* out)\n{{\n");
        sb.Append($"    uint32_t at = 0;\n    if (e->rpc != HF_RPC_{Upper(r)}) return 1;\n");
        foreach (var a in r.Args)
        {
            if (a.Kind != "array") { ReadValue(sb, "    ", a.Kind, $"out->{a.Name}", Kinds[a.Kind]); continue; }
            sb.Append($"    out->{a.Name}_count = hf_rpc_take_count_(e, &at, {a.MaxCount}u);\n    if (out->{a.Name}_count == 0xFFFFFFFFu) return 1;\n");
            sb.Append($"    for (uint32_t i = 0; i < out->{a.Name}_count; i++)\n");
            ReadValue(sb, "    ", a.Element!.Kind, $"out->{a.Name}[i]", Kinds[a.Element.Kind]);
        }
        sb.Append("    return at == e->count ? 0 : 1;\n}\n");
    }

    static void ModSender(StringBuilder sb, SdkRpc r)
    {
        var bools = r.Args.Sum(a => a.Kind == "bool" ? 1 : a.Kind == "array" && a.Element!.Kind == "bool" ? a.MaxCount : 0);
        sb.Append($"static inline int hf_rpc_{Lower(r)}_send(HfMod* mod, uint64_t to, const HfRpc_{Lower(r)}* in)\n{{\n");
        sb.Append($"    HfRpcArg a[{MaxEntriesOf(r)}];\n    uint32_t n = 0;\n");
        if (bools > 0) sb.Append($"    int32_t f[{bools}];\n    uint32_t nf = 0;\n");
        foreach (var a in r.Args)
        {
            if (a.Kind != "array") { SendValue(sb, "    ", a.Kind, $"in->{a.Name}", Kinds[a.Kind], "f"); continue; }
            sb.Append($"    if (in->{a.Name}_count > {a.MaxCount}u) return 1;\n");
            sb.Append($"    a[n].kind = HF_RPC_KIND_ARRAY; a[n].size = 0; a[n].value = (const void*)(uintptr_t)in->{a.Name}_count; n++;\n");
            sb.Append($"    for (uint32_t i = 0; i < in->{a.Name}_count; i++)\n    {{\n");
            SendValue(sb, "        ", a.Element!.Kind, $"in->{a.Name}[i]", Kinds[a.Element.Kind], "f");
            sb.Append("    }\n");
        }
        sb.Append($"    return hf_rpc_send_(mod, HF_RPC_{Upper(r)}, to, a, n);\n}}\n\n");
    }

    string LoaderTable()
    {
        var sb = new StringBuilder();
        sb.Append($"/* The game's RPCs as the mod loader checks rpc_send against them (rpc.c), for game build {Build}.\n");
        sb.Append($" * {Generated}\n");
        sb.Append(" * Each argument: its kind (HF_RPC_KIND_...), an array's element kind and its most elements, and the widths (bits)\n");
        sb.Append(" * the game encodes them with (rpc.c bounds a message's size with them). Sorted by hash. */\n");
        sb.Append("typedef struct SdkRpcArg { uint8_t kind, element; uint16_t max; uint8_t bits, elementBits; } SdkRpcArg;\n");
        sb.Append("typedef struct SdkRpc { uint32_t hash; const char* name; uint8_t sessionBound, count; uint16_t first; } SdkRpc;\n");
        sb.Append($"#define SDK_RPCS_BUILD \"{Build}\"\n");
        sb.Append("static const SdkRpcArg kSdkRpcArgs[] = {\n");
        var first = new List<int>();
        var at = 0;
        foreach (var r in Rpcs)
        {
            first.Add(at);
            foreach (var a in r.Args)
            {
                var element = a.Kind == "array" ? Kinds[a.Element!.Kind] : 0;
                var elementBits = a.Kind == "array" ? a.Element!.Bits : 0;
                sb.Append($"    {{{Kinds[a.Kind]}, {element}, {a.MaxCount}, {a.Bits}, {elementBits}}},\n");
                at++;
            }
        }
        if (at == 0) sb.Append("    {0, 0, 0, 0, 0},\n");
        sb.Append("};\n");
        sb.Append("static const SdkRpc kSdkRpcs[] = {\n");
        for (var i = 0; i < Rpcs.Count; i++)
        {
            var r = Rpcs[i];
            sb.Append($"    {{{Hex(r.Hash)}, \"{r.Name}\", {(r.SessionBound ? 1 : 0)}, {r.Args.Count}, {first[i]}}},\n");
        }
        sb.Append("};\n");
        sb.Append("#define SDK_RPC_COUNT (sizeof kSdkRpcs / sizeof kSdkRpcs[0])\n");
        return sb.ToString();
    }

    string Reference()
    {
        var sb = new StringBuilder();
        sb.Append("# The game's RPCs\n\n");
        sb.Append("<!-- diataxis: reference -->\n");
        sb.Append($"<!-- {Generated} -->\n\n");
        sb.Append($"Every network message (RPC) of game build {Build}, as its network configuration declares it, for `rpc_listen` and\n");
        sb.Append("`rpc_send` ([networking](modding/networking.md)). Each one's `HF_RPC_<NAME>`, `HfRpc_<name>`, `hf_rpc_<name>_read` and\n");
        sb.Append("`hf_rpc_<name>_send` are in `native/mod-sdk/hellforge_rpcs.h`. Arguments are named after their network type where\n");
        sb.Append("that name is known, else `arg<n>`. Session-bound RPCs only travel inside a game session.\n\n");
        sb.Append("| RPC | Id | Arguments |\n| --- | --- | --- |\n");
        foreach (var r in Rpcs)
        {
            var args = r.Args.Count == 0 ? "none" : string.Join("; ", r.Args.Select(a => $"`{a.Name}` {Describe(a)}"));
            sb.Append($"| `{r.Name}`{(r.SessionBound ? "" : " (any time)")} | `0x{r.Hash:X8}` | {args} |\n");
        }
        return sb.ToString();
    }
}
