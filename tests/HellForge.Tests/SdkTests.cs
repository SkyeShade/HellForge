using HellForge.Core;

static class SdkTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        const string catalog = """{"format":1,"build":"1","entities":{"A/G-16 Gatling Sentry":{"id":"0xEF85D6CF58E31D70"}}}""";
        static string Components(string fields) =>
            """{"format":1,"build":"1","components":{"Fixture":{"component":"FixtureComponentData","index":9,"stride":16,"layout":"1","meaning":"m","applies":"now","fields":{""" +
            fields + "}}}}";
        static string Field(string name, int offset, string type, string more = "") =>
            $"\"{name}\":{{\"offset\":{offset},\"type\":\"{type}\",{more}\"meaning\":\"m\",\"verified\":true,\"source\":\"s\"}}";
        var round = Field("round", 4, "u32");
        static string Grouped(string fields, string group) =>
            Components(fields + "},\"groups\":{\"g\":{\"meaning\":\"m\",\"fields\":[" + group + "]}");

        test("The SDK headers and the loader's field table are generated from the committed mappings", () =>
        {
            var stale = SdkMappings.Sync(Directory.GetCurrentDirectory(), check: true);
            check(stale.Count == 0, "Run `hellforge sdk-headers --repo .`; stale: " + string.Join(", ", stale));
        });

        test("Generated records pad unresearched bytes and catalog names become the SDK's constant names", () =>
        {
            var mappings = SdkMappings.Parse(Components(round + "," + Field("rate", 8, "f32", "\"count\":2,").Replace("true", "false")), catalog);
            var files = mappings.Generate();
            var header = files["native/mod-sdk/hellforge_components.h"];
            check(header.Contains("uint8_t _unknown_00[4];") && header.Contains("uint32_t round;") && header.Contains("float rate[2];") &&
                header.Contains("(unverified)") && header.Contains("HF_ASSERT_SIZE(HfFixture, 16);") &&
                header.Contains("#define HfFixture_INFO \"FixtureComponentData\", 1u") && header.Contains("#define HF_TYPE_Fixture_rate HF_TYPE_F32"),
                "Record layout: " + header);
            check(files["native/mod-sdk/hellforge_content.h"].Contains("#define HF_ENTITY_AG16_GATLING_SENTRY 0xEF85D6CF58E31D70ull"), "Content constant.");
            check(files["native/mod-loader/sdk_fields.h"].Contains("{\"rate\", 8, 8, HF_TYPE_F32, HF_APPLIES_NOW, REF_NONE}") &&
                files["native/mod-sdk/hellforge_components.h"].Contains("[now?]"), "Loader table and timing tags.");
            var referenced = SdkMappings.Parse(Components(Field("blast", 4, "u32", "\"ref\":\"explosion\",")), catalog).Generate();
            check(referenced["native/mod-loader/sdk_fields.h"].Contains("{\"blast\", 4, 4, HF_TYPE_U32, HF_APPLIES_NOW, REF_EXPLOSION}") &&
                referenced["docs/sdk-reference.md"].Contains("(an explosion reference)"),
                "A reference field: its kind in the loader's table.");
            check(SdkMappings.Parse(Grouped(round, "\"round\""), catalog).Generate()["native/mod-sdk/hellforge_components.h"]
                .Contains("#define HF_GROUP_Fixture_g(X, ...) X(Fixture, round, __VA_ARGS__)"), "Group macro.");
            var table = SdkMappings.Parse(Components(round).Replace("\"index\":9", "\"row\":\"fixture_row\""), catalog).Generate();
            check(table["native/mod-sdk/hellforge_components.h"].Contains("(HfFixture*)api->fixture_row(...)") &&
                table["native/mod-sdk/hellforge_components.h"].Contains("HfFixture_INFO") && table["native/mod-loader/sdk_fields.h"].Contains("kSdkFieldsFixture, 1, 1, -1}"),
                "A table row is found by id: HF_FIELD reaches it and the loader's table marks it.");
            var kinds = SdkMappings.Parse(Components(round), """{"format":1,"build":"1","traits":{"Anti-Tank":{"id":"0x1000"}}}""").Generate();
            check(kinds["native/mod-sdk/hellforge_content.h"].Contains("#define HF_TRAIT_ANTITANK 0x00001000u") &&
                kinds["native/mod-sdk/hellforge_content.h"].Contains("#define HF_KIND_TRAIT 9u") &&
                kinds["native/mod-loader/sdk_content.h"].Contains("{HF_KIND_TRAIT, \"Anti-Tank\", 0x1000ull}"), "Traits are hex flags that find resolves.");
            check(SdkMappings.Constant("MG-206 Heavy Machine Gun") == "MG206_HEAVY_MACHINE_GUN" &&
                SdkMappings.Constant("Orbital 120mm HE Barrage") == "ORBITAL_120MM_HE_BARRAGE", "Constant names.");
        });

        static string Functions(string entries) => """{"format":1,"build":"1","functions":{""" + entries + "}}";
        static string Function(string name, bool hook, string code = "48 89 5C 24 08 57 48 83 EC 20", string param = """{"name":"entity","type":"uint32_t","meaning":"the entity"}""",
            string returns = "uint64_t", string rva = "0x123450", string extra = "") =>
            $$"""
            "{{name}}":{"rva":"{{rva}}","code":"{{code}}","returns":"{{returns}}","params":[{"name":"manager","type":"void*","meaning":"the manager"},{{param}}],
            "hook":{{(hook ? "true" : "false")}},{{extra}}"meaning":"Does it","source":"s"}
            """;

        test("Game functions become typed hf_fn_ calls, hf_hook_ only where the whole signature is known, and the loader's table", () =>
        {
            var files = SdkMappings.Parse(Components(round), catalog, Functions(Function("SpawnThing", true, "48 89 5c 24 08 ?? 48 83 EC 20") + "," + Function("CallThing", false))).Generate();
            var header = files["native/mod-sdk/hellforge_functions.h"];
            check(header.Contains("typedef uint64_t (*HfFn_SpawnThing)(void* manager, uint32_t entity);") &&
                header.Contains("static inline HfFn_SpawnThing hf_fn_SpawnThing(HfMod* mod) { return (HfFn_SpawnThing)hf_function_(mod, \"SpawnThing\"); }") &&
                header.Contains("static inline int hf_hook_SpawnThing(HfMod* mod, HfFn_SpawnThing detour, HfFn_SpawnThing* original, int32_t priority)") &&
                header.Contains("/* SpawnThing (game.dll+0x123450; hook or call). Does it.") && header.Contains(" *   manager  the manager\n"),
                "A function to hook: " + header);
            check(header.Contains("hf_fn_CallThing") && !header.Contains("hf_hook_CallThing") && header.Contains("CallThing (game.dll+0x123450; call only)"),
                "A function only to call has no hf_hook_.");
            check(files["native/mod-loader/sdk_functions.h"].Contains("#define SDK_FUNCTIONS_BUILD \"1\"") &&
                files["native/mod-loader/sdk_functions.h"].Contains("{\"SpawnThing\", \"game.dll\", 0x123450u, \"48 89 5C 24 08 ?? 48 83 EC 20\", 1},") &&
                files["native/mod-loader/sdk_functions.h"].Contains("{\"CallThing\", \"game.dll\", 0x123450u, \"48 89 5C 24 08 57 48 83 EC 20\", 0},\n    {0},"),
                "The loader's table, its code as upper-case pattern, ending in {0}: " + files["native/mod-loader/sdk_functions.h"]);
            check(files["docs/sdk-reference.md"].Contains("### SpawnThing") && files["docs/sdk-reference.md"].Contains("| `entity` | `uint32_t` | the entity |"),
                "The reference lists each function.");
            check(SdkMappings.Parse(Components(round), catalog).Generate()["native/mod-loader/sdk_functions.h"].Contains("kSdkFunctions[] = {\n    {0},"),
                "No functions: an empty table.");
        });

        test("Function mappings refuse bad types, names, code, offsets, other builds and duplicate names", () =>
        {
            foreach (var entries in new[]
            {
                Function("Bad", true, param: """{"name":"x","type":"double","meaning":"m"}"""),
                Function("Bad", true, param: """{"name":"int","type":"uint32_t","meaning":"m"}"""),
                Function("Bad", true, param: """{"name":"mod","type":"uint32_t","meaning":"m"}"""),
                Function("Bad", true, param: """{"name":"manager","type":"uint32_t","meaning":"m"}"""),
                Function("Bad", true, returns: "uint16_t"),
                Function("Bad", true, code: "48 89 5C"),
                Function("Bad", true, code: "?? 89 5C 24 08 57 48 83"),
                Function("Bad", true, code: "48 89 5C 24 08 57 48 8G"),
                Function("Bad", true, rva: "123450"),
                Function("Bad", true, rva: "0x0"),
                Function("bad name", true),
                Function("A", true) + "," + Function("A", false),
            }) reject(() => SdkMappings.Parse(Components(round), catalog, Functions(entries)));
            reject(() => SdkMappings.Parse(Components(round), catalog, Functions(Function("A", true)).Replace("\"build\":\"1\"", "\"build\":\"2\"")));
            reject(() => SdkMappings.Parse(Components(round), catalog, Functions(Function("A", true)).Replace("\"hook\"", "\"module\":\"game\",\"hook\"")));
            foreach (var entries in new[]
            {
                Function("A", true, extra: "\"aliases\":[\"B\"],") + "," + Function("B", true),   // an alias is another's name
                Function("A", true, extra: "\"aliases\":[\"A\"],"),                               // its own name
                Function("A", true, extra: "\"aliases\":[\"not ok\"],"),
            }) reject(() => SdkMappings.Parse(Components(round), catalog, Functions(entries)));
        });

        test("A renamed function keeps its former name: wrappers for mods written with it, and a loader row for mods built with it", () =>
        {
            var files = SdkMappings.Parse(Components(round), catalog, Functions(Function("SpawnThing", true, extra: "\"aliases\":[\"MakeThing\"],") + "," +
                Function("CallThing", false, extra: "\"aliases\":[\"UseThing\"],"))).Generate();
            var header = files["native/mod-sdk/hellforge_functions.h"];
            check(header.Contains("typedef HfFn_SpawnThing HfFn_MakeThing;") &&
                header.Contains("static inline HfFn_SpawnThing hf_fn_MakeThing(HfMod* mod) { return hf_fn_SpawnThing(mod); }") &&
                header.Contains("    return hf_hook_SpawnThing(mod, detour, original, priority);") &&
                header.Contains("hf_fn_UseThing") && !header.Contains("hf_hook_UseThing"), "Alias wrappers: " + header);
            check(files["native/mod-loader/sdk_functions.h"].Contains("{\"MakeThing\", \"game.dll\", 0x123450u, \"48 89 5C 24 08 57 48 83 EC 20\", 1},") &&
                files["docs/sdk-reference.md"].Contains("Former names: `MakeThing`."), "The alias's loader row and reference.");
        });

        // A global at 0x3000000 whose witness at 0x1000 loads it: 48 8B 05, disp 0x3000000 - 0x1007 = 0x02FFEFF9.
        static string Globals(string entries) => """{"format":1,"build":"1","globals":{""" + entries + "}}";
        // A second one at 0x2000: disp 0x3000000 - 0x2007 = 0x02FFDFF9.
        const string secondWitness = ""","rva":"0x2000","code":"48 8B 0D F9 DF FF 02 90"}""";
        static string Global(string name, string witness = "48 8B 05 F9 EF FF 02 48 85 C0", string fields = ""","fields":{"count":{"offset":"0x20","type":"u32","meaning":"queued"},"next":{"offset":"0x28","type":"ptr","meaning":"the next"}}""",
            string more = "", string extra = "") =>
            $$"""
            "{{name}}":{"rva":"0x3000000","witnesses":[{"rva":"0x1000","code":"{{witness}}"}{{more}}],{{extra}}"meaning":"The queue","source":"s"{{fields}}}
            """;

        test("Game globals become typed objects with their named fields, hf_global_ calls and the loader's table", () =>
        {
            var files = SdkMappings.Parse(Components(round), catalog, null, Globals(Global("Queue") + "," + Global("Opaque", fields: ""))).Generate();
            var header = files["native/mod-sdk/hellforge_globals.h"];
            check(header.Contains("typedef struct HfQueue\n{\n    uint8_t _unknown_00[0x20];\n    uint32_t count;") &&
                header.Contains("    uint8_t _unknown_24[0x4];\n    void* next;") && header.Contains("HF_ASSERT_OFFSET(HfQueue, next, 0x28);") &&
                header.Contains("static inline HfQueue* hf_global_Queue(HfMod* mod) { return (HfQueue*)hf_global_object_(mod, \"Queue\"); }") &&
                header.Contains("/* Queue (game.dll+0x3000000, a pointer to it). The queue."), "A global with fields: " + header);
            check(header.Contains("typedef struct HfOpaque HfOpaque;") && header.Contains("hf_global_Opaque"), "A global with no fields is an incomplete type.");
            check(files["native/mod-loader/sdk_globals.h"].Contains("{\"Queue\", \"game.dll\", 0x3000000u, {0x1000u, 0, 0}, {\"48 8B 05 F9 EF FF 02 48 85 C0\", NULL, NULL}},") &&
                files["native/mod-loader/sdk_globals.h"].Contains("#define SDK_GLOBALS_BUILD \"1\""), "The loader's table: " + files["native/mod-loader/sdk_globals.h"]);
            check(files["docs/sdk-reference.md"].Contains("### Queue") && files["docs/sdk-reference.md"].Contains("| `count` | +0x20 | u32 | queued |"),
                "The reference lists each global.");
        });

        test("Global mappings refuse witnesses that do not load the cell, bad fields, SDK type names and other builds", () =>
        {
            foreach (var entries in new[]
            {
                Global("Q", witness: "48 8B 05 F9 EF FF 03 48 85 C0"),   // points elsewhere
                Global("Q", witness: "48 8B 45 F9 EF FF 02 48 85 C0"),   // not [rip+disp32]
                Global("Q", witness: "48 0F 05 F9 EF FF 02 48 85 C0"),   // not a mov or lea
                Global("Q", witness: "48 8B 05 ?? EF FF 02 48 85 C0"),   // displacement not fixed
                Global("Q", fields: ""","fields":{"a":{"offset":"0x22","type":"u32","meaning":"m"}}"""),
                Global("Q", fields: ""","fields":{"a":{"offset":"0x20","type":"u64","meaning":"m"},"b":{"offset":"0x24","type":"u32","meaning":"m"}}"""),
                Global("Q", fields: ""","fields":{"a":{"offset":"32","type":"u32","meaning":"m"}}"""),
                Global("Q", fields: ""","fields":{"a":{"offset":"0x20","type":"f64","meaning":"m"}}"""),
                Global("Session"),
                Global("Fixture"),
                Global("Q") + "," + Global("Q"),
            }) reject(() => SdkMappings.Parse(Components(round), catalog, null, Globals(entries)));
            reject(() => SdkMappings.Parse(Components(round), catalog, null, Globals(Global("Q")).Replace("\"build\":\"1\"", "\"build\":\"2\"")));
            foreach (var entries in new[]
            {
                Global("Q", more: "," + "{" + secondWitness[1..].Replace("0x2000", "0x1000")),                  // the same place twice
                Global("Q", more: ",{" + secondWitness[1..] + ",{" + secondWitness[1..] + ",{" + secondWitness[1..]),  // 4
                Global("Q", more: ",{" + secondWitness[1..].Replace("F9 DF", "F9 DE")),                       // the second points elsewhere
                Global("Q").Replace("[{\"rva\":\"0x1000\",\"code\":\"48 8B 05 F9 EF FF 02 48 85 C0\"}]", "[]"),       // none
                Global("Q", extra: "\"aliases\":[\"Session\"],"),
                Global("Q", extra: "\"aliases\":[\"R\"],") + "," + Global("R"),
            }) reject(() => SdkMappings.Parse(Components(round), catalog, null, Globals(entries)));
        });

        test("A global's witnesses: any of up to three; a renamed global keeps its former name", () =>
        {
            var files = SdkMappings.Parse(Components(round), catalog, null, Globals(Global("Queue", more: ",{" + secondWitness[1..], extra: "\"aliases\":[\"Line\"],"))).Generate();
            var table = files["native/mod-loader/sdk_globals.h"];
            check(table.Contains("{\"Queue\", \"game.dll\", 0x3000000u, {0x1000u, 0x2000u, 0}, {\"48 8B 05 F9 EF FF 02 48 85 C0\", \"48 8B 0D F9 DF FF 02 90\", NULL}},") &&
                table.Contains("{\"Line\", \"game.dll\", 0x3000000u, {0x1000u, 0x2000u, 0},"), "Both witnesses, and the alias's row: " + table);
            var header = files["native/mod-sdk/hellforge_globals.h"];
            check(header.Contains("typedef HfQueue HfLine;") && header.Contains("static inline HfQueue* hf_global_Line(HfMod* mod) { return hf_global_Queue(mod); }"),
                "The alias's wrappers: " + header);
        });

        test("Mappings refuse overlapping, out-of-record, misnamed and unknown fields, bad groups, SDK type names and duplicate constants", () =>
        {
            foreach (var fields in new[]
            {
                round + "," + Field("over", 6, "u16"),
                Field("tail", 12, "u64"),
                Field("odd", 2, "u32"),
                Field("_unknown_04", 4, "u32"),
                Field("bad name", 4, "u32"),
                Field("kind", 4, "u24"),
                Field("extra", 4, "u32", "\"extra\":1,"),
                Field("rate", 4, "f32", "\"ref\":\"explosion\","),
                Field("other", 4, "u32", "\"ref\":\"sound\","),
            }) reject(() => SdkMappings.Parse(Components(fields), catalog));
            foreach (var group in new[] { "\"nope\"", "\"round\",\"round\"", "" }) reject(() => SdkMappings.Parse(Grouped(round, group), catalog));
            reject(() => SdkMappings.Parse(Components(round).Replace("\"Fixture\":", "\"Weapon\":"), catalog));
            reject(() => SdkMappings.Parse(Components(round).Replace("\"applies\":\"now\"", "\"applies\":\"later\""), catalog));
            reject(() => SdkMappings.Parse(Components(round), """{"format":1,"build":"1","tags":{"A":{"id":"1"},"a":{"id":"2"}}}"""));
            reject(() => SdkMappings.Parse(Components(round), """{"format":1,"build":"2"}"""));
            reject(() => SdkMappings.Parse(Components(round), """{"format":1,"build":"1","items":{"Wide":{"id":"0x100000000"}}}"""));
        });
    }
}
