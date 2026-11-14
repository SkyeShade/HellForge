using System.Text;
using System.Text.RegularExpressions;
using HellForge.Core;
using HellForge.Core.GameData;

// A mod's own shaders: the digest, the splice into a stage, the ids a copy changes, the binding check and the spec's "shader"
// resource. Containers and GPU parts are synthetic, laid out as the game's are; no game data.
static class ShaderTests
{
    // float4 main(float4 p : SV_POSITION) : SV_TARGET0 { return p; }, compiled here with d3dcompiler_47 (ps_5_0, optimisation 3).
    // It carries the compiler's own digest, so it is a known vector.
    const string Compiled = "RFhCQ6aaYQ21fDv69q8CRmqrq0sBAAAA7AEAAAUAAAA0AAAAoAAAANQAAAAIAQAAUAEAAFJERUZkAAAAAAAAAAAAAAAAAAAAPAAAAAAF//8AgQAAPAAAAFJEMTE8AAAAGAAAACAAAAAoAAAAJAAAAAwAAAAAAAAATWljcm9zb2Z0IChSKSBITFNMIFNoYWRlciBDb21waWxlciAxMC4xAElTR04sAAAAAQAAAAgAAAAgAAAAAAAAAAEAAAADAAAAAAAAAA8PAABTVl9QT1NJVElPTgBPU0dOLAAAAAEAAAAIAAAAIAAAAAAAAAAAAAAAAwAAAAAAAAAPAAAAU1ZfVEFSR0VUAKurU0hFWEAAAABQAAAAEAAAAGoIAAFkIAAE8hAQAAAAAAABAAAAZQAAA/IgEAAAAAAANgAABfIgEAAAAAAARh4QAAAAAAA+AAABU1RBVJQAAAACAAAAAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    const int Texture = 2, Sampler = 3, Cbuffer = 0, Float = 5, Texture2D = 4, TextureCube = 9;
    const uint Name32 = 0xAABB0011, Template0 = 0xAABB0022, Template1 = 0xAABB0033, StageId = 0xAABB0044;

    sealed record Shape((string Name, int Index, int Type, int Register, int Mask)[] Inputs, (string Name, int Index, int Type, int Register, int Mask)[] Outputs,
        (string Name, int Kind, int Return, int Dimension, int Point, int Count, int Flags)[] Bindings, (string Name, int Size)[] Cbuffers, uint Version = 0x50);

    static readonly Shape Stage = new(
        [("SV_POSITION", 0, 3, 0, 15), ("TEXCOORD", 0, 3, 1, 7), ("TEXCOORD", 1, 3, 2, 3)],
        [("SV_TARGET", 0, 3, 0, 15), ("SV_TARGET", 1, 3, 1, 1)],
        [("__samp_clamp_point", Sampler, 0, 0, 3, 1, 0), ("__tex_linear_depth", Texture, Float, Texture2D, 3, 1, 12), ("__tex_gbuffer", Texture, Float, Texture2D, 4, 1, 12),
            ("global_viewport", Cbuffer, 0, 0, 0, 1, 0)],
        [("global_viewport", 816)]);

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        void Refused(Action action, string why)
        {
            try { action(); throw new Exception("Accepted, and should fail with: " + why); }
            catch (InvalidDataException error) { check(error.Message.Contains(why), $"Refused for another reason, not \"{why}\": {error.Message}"); }
        }

        test("The DXBC digest matches the compiler's on a real shader, and covers its bytes", () =>
        {
            var compiled = Convert.FromBase64String(Compiled);
            check(ShaderSplice.Digest(compiled).AsSpan().SequenceEqual(compiled.AsSpan(4, 16)), "The digest differs from the compiler's.");
            compiled[^1] ^= 1;
            check(!ShaderSplice.Digest(compiled).AsSpan().SequenceEqual(compiled.AsSpan(4, 16)), "The digest did not change with the bytes.");
            var fixture = Container(Stage, 64);
            check(ShaderSplice.Digest(fixture).AsSpan().SequenceEqual(fixture.AsSpan(4, 16)), "The fixtures are not signed.");
        });

        test("A shader splices into a stage: same size, its code, the game's reflection, signed again, every id changed", () =>
        {
            var (gpu, stages) = Part(2);
            var ours = Container(Stage, 16);
            var output = ShaderSplice.ApplyAll(gpu, stages.Select(s => (s, ours, "x.dxbc")).ToList());
            check(output.Length == gpu.Length, "The GPU part changed size.");
            var size = BitConverter.ToInt32(gpu, stages[0] + 24);
            foreach (var offset in stages)
            {
                var stage = output.AsSpan(offset, size).ToArray();
                check(BitConverter.ToInt32(stage, 24) == size && ShaderSplice.Digest(stage).AsSpan().SequenceEqual(stage.AsSpan(4, 16)), "The stage is not a signed container of the same size.");
                var chunks = ShaderSplice.Read(stage, 0, "the stage").Chunks;
                check(chunks.Single(c => c.Name == "SHEX").Data.SequenceEqual(ShaderSplice.Read(ours, 0, "ours").Chunks.Single(c => c.Name == "SHEX").Data), "The code is not the mod's.");
                check(chunks.Single(c => c.Name == "RDEF").Data.SequenceEqual(ShaderSplice.Read(gpu, offset, "game").Chunks.Single(c => c.Name == "RDEF").Data), "The reflection is not the game's.");
                check(chunks[^1].Name == "PRIV", "No padding chunk.");
                check(BitConverter.ToUInt32(output, offset + size) == (uint)(EngineHash.Murmur64A(stage) >> 32), "The id after the stage is not the top of its hash.");
            }
            check(Count(output, StageId) == 0 && Count(output, BitConverter.ToUInt32(output, stages[0] + size)) == 4, "The id must change in every stage and record.");

            var big = Container(Stage, BitConverter.ToInt32(gpu, stages[0] + 24));
            Refused(() => ShaderSplice.ApplyAll(gpu, [(stages[0], big, "big.dxbc")]), "big.dxbc: the compiled shader needs");
            var broken = gpu.ToArray();
            broken[stages[0] + 100] ^= 1;
            Refused(() => ShaderSplice.ApplyAll(broken, [(stages[0], ours, "x.dxbc"), (stages[1], ours, "x.dxbc")]), "not a signed shader stage");
            Refused(() => ShaderSplice.ApplyAll(gpu, [(0x10, ours, "x.dxbc")]), "not a DXBC container");
            Refused(() => ShaderSplice.ApplyAll(gpu, [(stages[0], ours[..40], "x.dxbc")]), "the compiled shader has a broken header");
        });

        test("Ids that do not stand as often as expected refuse the edit", () =>
        {
            var (gpu, stages) = Part(1);
            var ours = Container(Stage, 16);
            var (extra, _) = Part(1);
            BitConverter.TryWriteBytes(extra.AsSpan(0x1F0), StageId);
            Refused(() => ShaderSplice.ApplyAll(extra, [(stages[0], ours, "x.dxbc")]), "the stage id");
            // two stages of one id, of which only one is listed: the other would stay the game's
            var (two, both) = Part(2);
            Refused(() => ShaderSplice.ApplyAll(two, [(both[0], ours, "x.dxbc")]), "the stage id");
            var name = gpu.ToArray();
            BitConverter.TryWriteBytes(name.AsSpan(0x1F0), Name32);
            Refused(() => ShaderSplice.RenameMaterial(name, 0x1234), "the material name");
            var template = gpu.ToArray();
            BitConverter.TryWriteBytes(template.AsSpan(0x1F0), Template0);
            Refused(() => ShaderSplice.RenameTemplates(template, 0x1234), "the template id");
            var table = gpu.ToArray();
            BitConverter.TryWriteBytes(table.AsSpan(0x2C), 1000);
            Refused(() => ShaderSplice.RenameTemplates(table, 0x1234), "template table is broken");
            Refused(() => ShaderSplice.RenameMaterial(new byte[4], 0x1234), "no GPU part");
        });

        test("A copy's template ids are its own: equal ones stay equal, and two names differ", () =>
        {
            uint At(byte[] gpu, int offset) => BitConverter.ToUInt32(gpu, offset);
            var (first, _) = Part(1);
            var (second, _) = Part(1);
            ShaderSplice.RenameTemplates(first, 0x1111111111111111);
            ShaderSplice.RenameTemplates(second, 0x2222222222222222);
            check(At(first, 0x30) == At(first, 0xE0) && At(first, 0x30) == At(first, 0x1A0) && At(first, 0x30) == At(first, 0x1A8), "Equal template ids did not stay equal.");
            check(At(first, 0x30) != At(first, 0x88) && At(first, 0x88) == At(first, 0x1A4), "Different ids must stay different, in both tables.");
            check(At(first, 0x30) != Template0 && At(first, 0x88) != Template1, "The ids are the game's.");
            check(At(first, 0x30) != At(second, 0x30) && At(first, 0x88) != At(second, 0x88), "Two names took the same ids: the copies would replace each other.");
            var (again, _) = Part(1);
            ShaderSplice.RenameTemplates(again, 0x1111111111111111);
            check(At(again, 0x30) == At(first, 0x30), "One name must build the same ids on every machine.");
            ShaderSplice.RenameMaterial(first, 0xCAFE000011112222);
            check(At(first, 4) == 0xCAFE0000 && At(first, 0x1B0) == 0xCAFE0000, "name32 is the top of the copy's name, in both places.");
        });

        test("A shader that does not fit its stage is refused, and says why", () =>
        {
            var game = Container(Stage, 64);
            void Bad(Shape shape, string why) => Refused(() => Check(game, Container(shape, 16)), why);
            Check(game, Container(Stage, 16));

            // Fewer bindings fit, and so do fewer trailing inputs and a smaller cbuffer: the code reads the kept declarations.
            Check(game, Container(Stage with { Bindings = [Stage.Bindings[1]] }, 16));
            Check(game, Container(Stage with { Inputs = [.. Stage.Inputs.Take(2)] }, 16));
            Check(game, Container(Stage with { Cbuffers = [("global_viewport", 64)] }, 16));
            Check(game, Container(Stage with { Inputs = [("sv_position", 0, 3, 0, 15), .. Stage.Inputs.Skip(1)] }, 16));

            // Inputs
            Bad(Stage with { Inputs = [("COLOR", 0, 3, 1, 15)] }, "its input COLOR0 is not one of the stage's");
            Bad(Stage with { Inputs = [("SV_POSITION", 0, 3, 0, 15), ("TEXCOORD", 1, 3, 1, 3)] }, "its input TEXCOORD1 is in v1, and the stage has it in v2");
            Bad(Stage with { Inputs = [("SV_POSITION", 0, 3, 0, 15), ("TEXCOORD", 0, 3, 1, 15)] }, "the stage has xyz, it asks for xyzw");
            Bad(Stage with { Inputs = [("SV_POSITION", 0, 3, 0, 15), ("TEXCOORD", 0, 1, 1, 7)] }, "its input TEXCOORD0 is not the type or size");

            // Outputs
            Bad(Stage with { Outputs = [Stage.Outputs[0]] }, "it does not write SV_TARGET1");
            Bad(Stage with { Outputs = [.. Stage.Outputs, ("SV_TARGET", 2, 3, 2, 15)] }, "it writes SV_TARGET2");
            Bad(Stage with { Outputs = [Stage.Outputs[0], ("SV_TARGET", 1, 3, 1, 15)] }, "its output SV_TARGET1 is xyzw");

            // Bindings
            Bad(Stage with { Bindings = [("my_noise", Texture, Float, Texture2D, 9, 1, 12)] }, "it binds my_noise at t9, which the stage does not have");
            Bad(Stage with { Bindings = [("my_cbuffer", Cbuffer, 0, 0, 2, 1, 0)], Cbuffers = [("my_cbuffer", 16)] }, "it binds my_cbuffer at b2");
            Bad(Stage with { Bindings = [("my_sampler", Sampler, 0, 0, 7, 1, 0)] }, "it binds my_sampler at s7");
            Bad(Stage with { Bindings = [("__tex_linear_depth", Texture, Float, Texture2D, 4, 1, 12)] }, "it binds __tex_linear_depth at t4, and the stage has it at t3");
            Bad(Stage with { Bindings = [("__samp_clamp_point", Sampler, 0, 0, 2, 1, 0)] }, "the stage has it at s3");
            Bad(Stage with { Bindings = [("__tex_linear_depth", Texture, Float, TextureCube, 3, 1, 12)] }, "as another kind of resource");
            Bad(Stage with { Bindings = [("__tex_linear_depth", Texture, Float, Texture2D, 3, 2, 12)] }, "as another kind of resource");
            Bad(Stage with { Bindings = [("__samp_clamp_point", Sampler, 0, 0, 3, 1, 2)] }, "as another kind of resource");
            Bad(Stage with { Cbuffers = [("global_viewport", 832)] }, "its cbuffer global_viewport is 832 bytes, and the stage's is 816");
            Bad(Stage with { Bindings = [("$Globals", Cbuffer, 0, 0, 0, 1, 0)], Cbuffers = [("$Globals", 16)] }, "global variables");

            // The container
            Bad(Stage with { Version = 0x10050 }, "not a pixel shader");
            Bad(Stage with { Version = 0x51 }, "compiled for ps_5_1, and the stage is ps_5_0");
            Refused(() => Check(game, Remove(Container(Stage, 16), "RDEF")), "it has no reflection");
            Refused(() => Check(game, Remove(Container(Stage, 16), "ISGN")), "the compiled shader has no ISGN chunk");
            Refused(() => Check(game, Container(Stage, 16, [("SFI0", [1, 0, 0, 0, 0, 0, 0, 0])])), "optional GPU features");
            Refused(() => Check(game, Container(Stage, 16, [], rdef: new byte[8])), "a chunk of the shader is broken");
        });

        test("The shader hosts are for the supported build, and their headers declare what the stages have", () =>
        {
            var hosts = ShaderHosts.Current.Hosts;
            check(hosts.Keys.Order().SequenceEqual(["decal", "refract", "transparent"]), "The hosts changed.");
            check(hosts.Values.All(h => h.Stages.Count > 0 && h.Stages.All(s => s.Digest.Length == 16 && s.Size > 0)), "A stage has no pin.");
            check(hosts["transparent"].Stages.Count == 4 && hosts["transparent"].Child != null && hosts["decal"].Child == null && hosts["refract"].Child == null, "The hosts' shape changed.");
            check(hosts["decal"].Stages[0].Offset == 0x11C38 && hosts["decal"].UnitMaterialAt == 0x69C && !hosts["decal"].Mesh && hosts["refract"].Mesh, "The decal host is not the reviewed one.");
            const string json = """{"format":1,"build":"1","hosts":{}}""";
            check(ShaderHosts.Parse(json, "1").Hosts.Count == 0, "A catalogue for this build must load.");
            Refused(() => ShaderHosts.Parse(json, "2"), "for game build 1, and this is build 2");

            var sdk = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var mapping = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(sdk.Folder, "..", "..", "mappings", "shader-hosts.json"))).RootElement.GetProperty("hosts");
            var common = File.ReadAllText(Path.Combine(sdk.Folder, "shaders", "hellforge_shader.hlsli"));
            foreach (var (name, host) in hosts)
            {
                var header = File.ReadAllText(Path.Combine(sdk.Folder, "shaders", $"hellforge_shader_{name}.hlsli"));
                foreach (Match slot in Regex.Matches(header + common, @"(\w+)\s*:\s*register\((\w+)\)"))
                    check(host.Bindings.Any(b => b.StartsWith($"{slot.Groups[2].Value} {slot.Groups[1].Value}", StringComparison.Ordinal)),
                        $"{name}'s header binds {slot.Groups[1].Value} at {slot.Groups[2].Value}, which the host does not list.");
                string[] Semantics(string block) => [.. Regex.Matches(block, @":\s*((?:SV_|TEXCOORD)\w*)").Select(m => m.Groups[1].Value.ToUpperInvariant())];
                var structs = Regex.Matches(header, @"struct (In|Out)\s*\{(.*?)\};", RegexOptions.Singleline).ToDictionary(m => m.Groups[1].Value, m => m.Groups[2].Value);
                string[] Listed(string list) => [.. mapping.GetProperty(name).GetProperty(list).EnumerateArray().Select(e => e.GetString()!.Split(' ')[1])];
                check(Semantics(structs["In"]).Select(s => s == "SV_POSITION" ? "SV_POSITION0" : s == "SV_ISFRONTFACE" ? "SV_ISFRONTFACE0" : s)
                    .SequenceEqual(Listed("inputs").Select(s => s.ToUpperInvariant())), $"{name}'s In is not the stage's inputs, in order.");
                check(Semantics(structs["Out"]).SequenceEqual(Listed("outputs")), $"{name}'s Out is not the stage's outputs.");
            }
        });

        test("A spec's shader resource: a host, a compiled file and a mesh when the host draws one, checked, tracked and unchecked for mod sync", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "shader-spec")).FullName;
            Directory.CreateDirectory(Path.Combine(folder, "shaders"));
            Directory.CreateDirectory(Path.Combine(folder, "models"));
            File.WriteAllBytes(Path.Combine(folder, "shaders", "rings.dxbc"), [1]);
            File.WriteAllText(Path.Combine(folder, "models", "wall.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
            static string Spec(string resource) => $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{{resource}}]}]}""";
            DataSpec Load(string resource)
            {
                File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), Spec(resource));
                return DataSpecs.Load(folder)!;
            }
            const string decal = """{"shader":"decal","name":"test/mod/rings","dxbc":"shaders/rings.dxbc"}""";
            const string wall = """{"shader":"refract","name":"test/mod/wall","dxbc":"shaders/rings.dxbc","mesh":{"obj":"models/wall.obj","scale":2}}""";

            var spec = Load(decal);
            check(spec.Inputs.SequenceEqual(["shaders/rings.dxbc"]) && spec.Patches[0].Resources[0] is ShaderResource { Host: "decal", Name: "test/mod/rings", Dxbc: "shaders/rings.dxbc", Mesh: null },
                "The decal form did not parse, or its file is not an input of the build.");
            check(DataSpecs.Unchecked(spec) == "patches[0].resources[0] brings its own shader code", "A shader must count as unchecked: " + DataSpecs.Unchecked(spec));
            check(ModSets.Declared(["hellforge-data.json"], """{"id":"a"}""", null, Spec(decal)) == ModClass.Raw, "A spec with a shader must read as raw data from its text alone.");
            spec = Load(wall);
            check(spec.Inputs.SequenceEqual(["models/wall.obj", "shaders/rings.dxbc"]) && spec.Patches[0].Resources[0] is ShaderResource { Mesh: { Obj: "models/wall.obj", Scale: 2 } }, "The mesh is part of the resource and its cache key.");
            var key = ModDataCache.Key(spec, folder, WriteIndex(root));
            File.WriteAllBytes(Path.Combine(folder, "shaders", "rings.dxbc"), [2]);
            check(ModDataCache.Key(spec, folder, WriteIndex(root)) != key, "A new compiled shader must rebuild the data.");
            check(DataSpecs.FromJson(Spec(wall)).Patches[0].Resources[0] is ShaderResource, "Mod sync reads the spec without its files.");

            void Bad(string resource, string why) => Refused(() => Load(resource), why);
            Bad("""{"shader":"glass","name":"test/mod/x","dxbc":"shaders/rings.dxbc"}""", "shader is a host: decal, transparent, refract");
            Bad("""{"shader":"decal","dxbc":"shaders/rings.dxbc"}""", "name is required");
            Bad("""{"shader":"decal","name":"rings","dxbc":"shaders/rings.dxbc"}""", "the mod's own resource path");
            Bad("""{"shader":"decal","name":"test/mod/x"}""", "dxbc is required");
            Bad("""{"shader":"decal","name":"test/mod/x","dxbc":"shaders/none.dxbc"}""", "shaders/none.dxbc is missing");
            Bad("""{"shader":"decal","name":"test/mod/x","dxbc":"../rings.dxbc"}""", "a .dxbc file in the mod folder");
            Bad("""{"shader":"decal","name":"test/mod/x","dxbc":"shaders/rings.hlsl"}""", "a .dxbc file in the mod folder");
            Bad("""{"shader":"decal","name":"test/mod/x","dxbc":"shaders/rings.dxbc","mesh":{"obj":"models/wall.obj"}}""", "takes no mesh");
            Bad("""{"shader":"transparent","name":"test/mod/x","dxbc":"shaders/rings.dxbc"}""", "the transparent host draws a mesh");
            Bad("""{"shader":"refract","name":"test/mod/x","dxbc":"shaders/rings.dxbc"}""", "the refract host draws a mesh");
            Bad("""{"shader":"decal","name":"test/mod/x","dxbc":"shaders/rings.dxbc","offset":"0x10"}""", "unknown field offset");
            Bad("""{"shader":[],"name":"test/mod/x","dxbc":"shaders/rings.dxbc"}""", "shader must be text");
            Bad("""{"type":"material","copy":"0x1","name":"test/mod/x","shader":[{"offset":"0x10","dxbc":"../x.dxbc"}]}""", "a .dxbc file in the mod folder");
            Bad("""{"type":"unit","copy":"0x1","name":"test/mod/x","shader":[{"offset":"0x10","dxbc":"shaders/rings.dxbc"}]}""", "only a material has shaders");

            var copy = Load("""{"type":"material","copy":"0x1","name":"test/mod/x","shader":[{"offset":"0x10","dxbc":"shaders/rings.dxbc"}]}""");
            check(copy.Patches[0].Resources[0] is CopyResource { Shaders: [{ Offset: 0x10, Host: null }] } && DataSpecs.Unchecked(copy) != null, "The low-level form stays, and stays unchecked.");
        });

        test("A shader resource builds its materials and unit from the game data, pinned to the host's stage", () =>
        {
            const ulong Root = 0x7001, Child = 0x7002, Unit = 0x7003, BoxMaterial = 0x7004;
            var data = Directory.CreateDirectory(Path.Combine(root, "shader-game")).FullName;
            var (gpu, stages) = Part(1);
            var unit = new byte[0x800];
            BitConverter.TryWriteBytes(unit.AsSpan(0x69C), BoxMaterial);
            var child = new byte[0x40];
            BitConverter.TryWriteBytes(child.AsSpan(0x18), Root);
            var game = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.Material, Root)] = new([1, 2, 3], gpu, []),
                [new(ResourceTypes.Material, Child)] = new(child, [], []),
                [new(ResourceTypes.Unit, Unit)] = new(unit, [9], []),
            });
            DataBuilderTests.WriteBundles(data, new Dictionary<string, byte[]> { ["1111111111111111"] = game.Archive, ["1111111111111111.gpu_resources"] = game.Gpu });
            var stage = new HostStage(stages[0], BitConverter.ToInt32(gpu, stages[0] + 24), gpu.AsSpan(stages[0] + 4, 16).ToArray());
            var plain = new ShaderHost("plain", Root, [stage], null, Unit, BoxMaterial, 0x69C, false, []);
            var hosts = new ShaderHosts(new Dictionary<string, ShaderHost> { ["plain"] = plain, ["child"] = plain with { Name = "child", Child = (Child, 0x18) } });
            var mod = Directory.CreateDirectory(Path.Combine(root, "shader-mod")).FullName;
            File.WriteAllBytes(Path.Combine(mod, "x.dxbc"), Container(Stage, 16));
            static ulong Hash(string name) => EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name));
            static DataSpec Spec(params DataResource[] resources) => new([new(["1111111111111111"], resources)], ["x.dxbc"]);

            using var reader = new GameDataReader(data);
            var spec = hosts.Expand(Spec(new ShaderResource("r0", "plain", "test/mod/a", "x.dxbc", null), new ShaderResource("r1", "child", "test/mod/b", "x.dxbc", null)));
            var output = Path.Combine(root, "shader-out");
            var files = ModDataBuilder.Build(spec, mod, reader, output);
            var main = Path.Combine(output, files[0]);
            var built = ResourceArchive.Read(File.ReadAllBytes(main), File.ReadAllBytes(main + ".gpu_resources"), []);
            check(built.Count == 5, $"Two units and three materials, not {built.Count}.");
            ResourceParts Named(ulong type, string name) => built[new(type, Hash(name))];
            var coded = Named(ResourceTypes.Material, "test/mod/a/material");
            check(coded.Gpu.Length == gpu.Length && Count(coded.Gpu, StageId) == 0 && Count(coded.Gpu, Name32) == 0 && Count(coded.Gpu, Template0) == 0 && BitConverter.ToUInt32(coded.Gpu, 4) == (uint)(Hash("test/mod/a/material") >> 32),
                "The material is not a copy with ids of its own.");
            check(BitConverter.ToUInt64(Named(ResourceTypes.Unit, "test/mod/a").Main, 0x69C) == Hash("test/mod/a/material"), "The unit does not use its material.");
            var rootCopy = Named(ResourceTypes.Material, "test/mod/b/root");
            check(BitConverter.ToUInt32(rootCopy.Gpu, 4) == (uint)(Hash("test/mod/b/root") >> 32) && BitConverter.ToUInt32(rootCopy.Gpu, 0x30) != BitConverter.ToUInt32(coded.Gpu, 0x30), "Copies of one root must not share ids.");
            check(BitConverter.ToUInt64(Named(ResourceTypes.Material, "test/mod/b/material").Main, 0x18) == Hash("test/mod/b/root"), "The child does not name the root copy.");
            check(BitConverter.ToUInt64(Named(ResourceTypes.Unit, "test/mod/b").Main, 0x69C) == Hash("test/mod/b/material"), "The unit does not use the child.");

            var other = hosts.Expand(Spec(new ShaderResource("r0", "plain", "test/mod/a", "x.dxbc", null)));
            var moved = new ShaderHosts(new Dictionary<string, ShaderHost> { ["plain"] = plain with { Stages = [stage with { Digest = new byte[16] }] } });
            Refused(() => ModDataBuilder.Build(moved.Expand(Spec(new ShaderResource("r0", "plain", "test/mod/a", "x.dxbc", null))), mod, reader, Path.Combine(root, "shader-moved")),
                "is not the one HellForge's shader hosts list");
            File.WriteAllBytes(Path.Combine(mod, "x.dxbc"), Container(Stage with { Bindings = [("my_noise", Texture, Float, Texture2D, 9, 1, 12)] }, 16));
            Refused(() => ModDataBuilder.Build(other, mod, reader, Path.Combine(root, "shader-bad")), "x.dxbc: it binds my_noise at t9");
            check(!Directory.Exists(Path.Combine(root, "shader-bad")), "A shader that is refused must write nothing.");
        });

        test("An edit sets bytes in a game resource's stage under its own key, signed again with its stage id kept, and refuses another stage", () =>
        {
            const ulong Library = 0x7101;
            var type = ResourceTypes.Hash("shader_library");
            var data = Directory.CreateDirectory(Path.Combine(root, "edit-game")).FullName;
            var (gpu, stages) = Part(1);
            var game = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(type, Library)] = new([1, 2, 3, 4], gpu, []) });
            DataBuilderTests.WriteBundles(data, new Dictionary<string, byte[]> { ["1111111111111111"] = game.Archive, ["1111111111111111.gpu_resources"] = game.Gpu });
            var size = BitConverter.ToInt32(gpu, stages[0] + 24);
            var digest = Convert.ToHexString(gpu.AsSpan(stages[0] + 4, 16));
            var folder = Directory.CreateDirectory(Path.Combine(root, "edit-mod")).FullName;
            DataSpec Load(string stage)
            {
                File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{"type":"shader_library","edit":"0x7101","stages":[{{stage}}]}]}]}""");
                return DataSpecs.Load(folder)!;
            }
            string Stage(string set, string? pin = null) => $"{{\"at\":\"0x{stages[0]:X}\",\"digest\":\"{pin ?? digest}\",\"set\":{{{set}}}}}";
            var at = size - 8;
            var spec = Load(Stage($"\"0x{at:X}\":\"0100003A0100003A\""));
            check(DataSpecs.Unchecked(spec)?.Contains("in place") == true, "An edit in place must be unchecked for mod sync.");

            using var reader = new GameDataReader(data);
            var output = Path.Combine(root, "edit-out");
            var files = ModDataBuilder.Build(spec, folder, reader, output);
            var main = Path.Combine(output, files[0]);
            var built = ResourceArchive.Read(File.ReadAllBytes(main), File.ReadAllBytes(main + ".gpu_resources"), []);
            check(built.Count == 1 && built.ContainsKey(new(type, Library)), "The edit is not the game's resource under its own key.");
            var edited = built[new(type, Library)];
            var stage = edited.Gpu.AsSpan(stages[0], size);
            check(BitConverter.ToUInt32(edited.Gpu, stages[0] + at) == 0x3A000001 && BitConverter.ToUInt32(edited.Gpu, stages[0] + at + 4) == 0x3A000001, "The bytes were not set.");
            check(ShaderSplice.Digest(stage).AsSpan().SequenceEqual(stage.Slice(4, 16)) && !stage.Slice(4, 16).SequenceEqual(gpu.AsSpan(stages[0] + 4, 16)), "The stage is not signed again.");
            check(Count(edited.Gpu, StageId) == Count(gpu, StageId) && edited.Main.SequenceEqual(new byte[] { 1, 2, 3, 4 }), "The stage id or the main part changed.");
            var changed = Enumerable.Range(0, gpu.Length).Where(i => gpu[i] != edited.Gpu[i]).ToList();
            check(changed.All(i => i - stages[0] is >= 4 and < 20 || i - stages[0] >= at && i - stages[0] < at + 8), "Bytes outside the edit and the digest changed.");

            Refused(() => ModDataBuilder.Build(Load(Stage($"\"0x{at:X}\":\"00\"", new string('0', 32))), folder, reader, Path.Combine(root, "edit-pin")), "not the signed shader the spec was written for");
            Refused(() => ModDataBuilder.Build(Load(Stage($"\"0x{size - 2:X}\":\"00000000\"")), folder, reader, Path.Combine(root, "edit-far")), "is outside them");
            Refused(() => ModDataBuilder.Build(Load(Stage("\"0x20\":\"00\"")), folder, reader, Path.Combine(root, "edit-table")), "is outside them");
            Refused(() => Load(Stage($"\"0x{at:X}\":\"0000\",\"0x{at + 1:X}\":\"00\"")), "twice");
            Refused(() => Load(Stage($"\"0x{at:X}\":\"0g\"")), "is not bytes as hex digits");
            check(!Directory.Exists(Path.Combine(root, "edit-pin")), "A refused edit must write nothing.");
        });

        test("An edit sets bytes outside the stages where the game's own bytes stand, and refuses a stage's or another build's", () =>
        {
            const ulong Library = 0x7102;
            var type = ResourceTypes.Hash("shader_library");
            var data = Directory.CreateDirectory(Path.Combine(root, "bytes-game")).FullName;
            var (gpu, stages) = Part(1);
            BitConverter.TryWriteBytes(gpu.AsSpan(0x280), 0xEFBEADDEu);
            var game = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(type, Library)] = new([1, 2, 3, 4], gpu, []) });
            DataBuilderTests.WriteBundles(data, new Dictionary<string, byte[]> { ["1111111111111111"] = game.Archive, ["1111111111111111.gpu_resources"] = game.Gpu });
            var size = BitConverter.ToInt32(gpu, stages[0] + 24);
            var digest = Convert.ToHexString(gpu.AsSpan(stages[0] + 4, 16));
            var folder = Directory.CreateDirectory(Path.Combine(root, "bytes-mod")).FullName;
            DataSpec Load(string fields)
            {
                File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{"type":"shader_library","edit":"0x7102"{{fields}}}]}]}""");
                return DataSpecs.Load(folder)!;
            }
            static string Bytes(params string[] items) => ",\"bytes\":[" + string.Join(",", items) + "]";
            static string Item(int at, string expect, string set) => $"{{\"at\":\"0x{at:X}\",\"expect\":\"{expect}\",\"set\":\"{set}\"}}";
            using var reader = new GameDataReader(data);
            ResourceParts Built(DataSpec spec, string name)
            {
                var output = Path.Combine(root, name);
                var main = Path.Combine(output, ModDataBuilder.Build(spec, folder, reader, output)[0]);
                return ResourceArchive.Read(File.ReadAllBytes(main), File.ReadAllBytes(main + ".gpu_resources"), [])[new(type, Library)];
            }

            var spec = Load(Bytes(Item(0x280, "DEADBEEF", "01020304")));
            check(DataSpecs.Unchecked(spec)?.Contains("in place") == true, "An edit of bytes must be unchecked for mod sync.");
            var edited = Built(spec, "bytes-out");
            var changed = Enumerable.Range(0, gpu.Length).Where(i => gpu[i] != edited.Gpu[i]).ToList();
            check(changed.SequenceEqual([0x280, 0x281, 0x282, 0x283]) && BitConverter.ToUInt32(edited.Gpu, 0x280) == 0x04030201u, "Only the four bytes may change.");
            check(edited.Main.SequenceEqual(new byte[] { 1, 2, 3, 4 }), "The main part changed.");

            var at = size - 8;
            var both = Built(Load($",\"stages\":[{{\"at\":\"0x{stages[0]:X}\",\"digest\":\"{digest}\",\"set\":{{\"0x{at:X}\":\"0100003A0100003A\"}}}}]" + Bytes(Item(0x280, "DEADBEEF", "01020304"))), "bytes-both");
            var stage = both.Gpu.AsSpan(stages[0], size);
            check(BitConverter.ToUInt32(both.Gpu, 0x280) == 0x04030201u && BitConverter.ToUInt32(both.Gpu, stages[0] + at) == 0x3A000001 &&
                ShaderSplice.Digest(stage).AsSpan().SequenceEqual(stage.Slice(4, 16)), "Stages and bytes in one edit must both apply, the stage signed again.");

            Refused(() => Built(Load(Bytes(Item(0x280, "DEADBEEE", "01020304"))), "bytes-pin"), "not the expected DEADBEEE");
            Refused(() => Built(Load(Bytes(Item(stages[0] + 0x40, Convert.ToHexString(gpu, stages[0] + 0x40, 2), "0000"))), "bytes-stage"), $"inside the shader stage at 0x{stages[0]:X}");
            Refused(() => Built(Load(Bytes(Item(stages[0] - 2, Convert.ToHexString(gpu, stages[0] - 2, 4), "00000000"))), "bytes-edge"), "inside the shader stage");
            Refused(() => Built(Load(Bytes(Item(gpu.Length - 2, "0000", "00000000"))), "bytes-short"), "must be as long");
            Refused(() => Built(Load(Bytes(Item(gpu.Length - 2, "00000000", "00000000"))), "bytes-far"), "outside the GPU part");
            Refused(() => Load(Bytes(Item(0x280, "DEAD", "0102"), Item(0x281, "AD", "02"))), "bytes[0] and bytes[1] both write 0x281");
            Refused(() => Load(Bytes(Item(0x281, "AD", "02"), Item(0x280, "DEAD", "0102"))), "bytes[1] and bytes[0] both write 0x281");
            Refused(() => Load(Bytes()), "bytes lists nothing");
            Refused(() => Load(""), "needs stages or bytes");
            Refused(() => Load(",\"stages\":[]" + Bytes(Item(0x280, "DEADBEEF", "01020304"))), "stages lists nothing");
            Refused(() => Load(Bytes("{\"at\":\"0x280\",\"expect\":\"DE\",\"set\":\"01\",\"why\":\"x\"}")), "unknown field why");
            foreach (var name in new[] { "bytes-pin", "bytes-stage", "bytes-edge", "bytes-far" })
                check(!Directory.Exists(Path.Combine(root, name)), $"A refused edit must write nothing ({name}).");
            var stageEnd = stages[0] + size;
            Refused(() => Built(Load(Bytes(Item(stageEnd, Convert.ToHexString(gpu, stageEnd, 4), "00000000"))), "bytes-id"), $"touch the stage id at 0x{stageEnd:X}");
            Refused(() => Built(Load(Bytes(Item(0x1C2, Convert.ToHexString(gpu, 0x1C2, 2), "0000"))), "bytes-record"), "touch the stage id at 0x1C0");
            check(Built(Load(Bytes(Item(stageEnd + 4, Convert.ToHexString(gpu, stageEnd + 4, 4), "01000000"))), "bytes-after").Gpu.AsSpan(stageEnd + 4, 4).SequenceEqual(new byte[] { 1, 0, 0, 0 }),
                "The bytes after a stage's id are outside it.");

            var sha = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(gpu));
            string Pin(string pin) => $",\"gpu\":{pin}" + Bytes(Item(0x280, "DEADBEEF", "01020304"));
            check(BitConverter.ToUInt32(Built(Load(Pin($"{{\"size\":{gpu.Length},\"sha256\":\"{sha}\"}}")), "pin-both").Gpu, 0x280) == 0x04030201u, "A matching pin must let the edit through.");
            Refused(() => Built(Load(Pin($"{{\"size\":{gpu.Length + 4}}}")), "pin-size"), $"has {gpu.Length} bytes, not the {gpu.Length + 4} the spec pins");
            Refused(() => Built(Load(Pin($"{{\"sha256\":\"{new string('0', 64)}\"}}")), "pin-sha"), $"SHA-256 is {sha}, not the");
            Refused(() => Load(Pin("{}")), "pin size, sha256 or both");
            Refused(() => Load(Pin("{\"sha256\":\"00\"}")), "64 hex digits");
            Refused(() => Load(Pin("{\"size\":0}")), "size is the GPU part's length");
            Refused(() => Load(Pin("{\"crc\":1}")), "unknown field crc");
            foreach (var name in new[] { "bytes-id", "bytes-record", "pin-size", "pin-sha" })
                check(!Directory.Exists(Path.Combine(root, name)), $"A refused edit must write nothing ({name}).");
        });
    }

    static void Check(byte[] game, byte[] ours) => ShaderCheck.Check(ShaderSplice.Read(game, 0, "game").Chunks, ShaderSplice.Read(ours, 0, "ours").Chunks);

    static int Count(byte[] gpu, uint value)
    {
        var count = 0;
        for (var i = 0; i + 4 <= gpu.Length; i++) if (BitConverter.ToUInt32(gpu, i) == value) count++;
        return count;
    }

    static string WriteIndex(string root)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "shader-index")).FullName;
        File.WriteAllBytes(Path.Combine(folder, GameDataReader.IndexFile), [1]);
        return folder;
    }

    // A GPU part laid out as a root's: three template entries (the first and third one id), each again in the context table, the
    // name again in the shader record, the stage id once per stage in a record, and each stage followed by its id.
    static (byte[] Gpu, int[] Stages) Part(int stages)
    {
        var stage = Container(Stage, 64);
        var at = Enumerable.Range(0, stages).Select(i => 0x300 + i * (stage.Length + 16)).ToArray();
        var gpu = new byte[0x300 + stages * (stage.Length + 16)];
        void Put(int offset, uint value) => BitConverter.TryWriteBytes(gpu.AsSpan(offset), value);
        Put(4, Name32);
        Put(0x2C, 3);
        Put(0x30, Template0);
        Put(0x88, Template1);
        Put(0xE0, Template0);
        Put(0x1A0, Template0);
        Put(0x1A4, Template1);
        Put(0x1A8, Template0);
        Put(0x1B0, Name32);
        for (var i = 0; i < stages; i++)
        {
            Put(0x1C0 + 8 * i, StageId);
            stage.CopyTo(gpu, at[i]);
            Put(at[i] + stage.Length, StageId);
        }
        return (gpu, at);
    }

    // A signed container of the shape, with `code` bytes of SHEX.
    static byte[] Container(Shape shape, int code, (string Name, byte[] Data)[]? extra = null, byte[]? rdef = null)
    {
        var shex = new byte[code];
        BitConverter.TryWriteBytes(shex, shape.Version);
        return Sign([("RDEF", rdef ?? Reflection(shape)), ("ISGN", Signature(shape.Inputs)), ("OSGN", Signature(shape.Outputs)), ("SHEX", shex), ("STAT", new byte[16]), .. extra ?? []]);
    }

    static byte[] Remove(byte[] container, string chunk) => Sign(ShaderSplice.Read(container, 0, "fixture").Chunks.Where(c => c.Name != chunk).ToArray());

    static byte[] Sign((string Name, byte[] Data)[] chunks)
    {
        var head = 32 + 4 * chunks.Length;
        var output = new byte[head + chunks.Sum(c => 8 + c.Data.Length)];
        Encoding.ASCII.GetBytes("DXBC").CopyTo(output, 0);
        BitConverter.TryWriteBytes(output.AsSpan(20), 1);
        BitConverter.TryWriteBytes(output.AsSpan(24), output.Length);
        BitConverter.TryWriteBytes(output.AsSpan(28), chunks.Length);
        var at = head;
        for (var i = 0; i < chunks.Length; i++)
        {
            BitConverter.TryWriteBytes(output.AsSpan(32 + 4 * i), at);
            Encoding.ASCII.GetBytes(chunks[i].Name).CopyTo(output, at);
            BitConverter.TryWriteBytes(output.AsSpan(at + 4), chunks[i].Data.Length);
            chunks[i].Data.CopyTo(output, at + 8);
            at += 8 + chunks[i].Data.Length;
        }
        ShaderSplice.Digest(output).CopyTo(output, 4);
        return output;
    }

    static byte[] Signature((string Name, int Index, int Type, int Register, int Mask)[] elements)
    {
        var names = new List<byte>();
        var table = 8 + 24 * elements.Length;
        var output = new byte[table];
        BitConverter.TryWriteBytes(output, elements.Length);
        BitConverter.TryWriteBytes(output.AsSpan(4), 8);
        for (var i = 0; i < elements.Length; i++)
        {
            var e = elements[i];
            foreach (var (field, value) in new[] { (0, table + names.Count), (4, e.Index), (12, e.Type), (16, e.Register), (20, e.Mask) })
                BitConverter.TryWriteBytes(output.AsSpan(8 + 24 * i + field), value);
            names.AddRange(Encoding.ASCII.GetBytes(e.Name + "\0"));
        }
        return Padded([.. output, .. names]);
    }

    static byte[] Reflection(Shape shape)
    {
        var names = new List<byte>();
        var buffersAt = 28;
        var bindingsAt = buffersAt + 24 * shape.Cbuffers.Length;
        var table = bindingsAt + 32 * shape.Bindings.Length;
        var output = new byte[table];
        void Put(int at, int value) => BitConverter.TryWriteBytes(output.AsSpan(at), value);
        int Name(string name)
        {
            var at = table + names.Count;
            names.AddRange(Encoding.ASCII.GetBytes(name + "\0"));
            return at;
        }
        Put(0, shape.Cbuffers.Length);
        Put(4, buffersAt);
        Put(8, shape.Bindings.Length);
        Put(12, bindingsAt);
        for (var i = 0; i < shape.Cbuffers.Length; i++)
        {
            Put(buffersAt + 24 * i, Name(shape.Cbuffers[i].Name));
            Put(buffersAt + 24 * i + 12, shape.Cbuffers[i].Size);
        }
        for (var i = 0; i < shape.Bindings.Length; i++)
        {
            var b = shape.Bindings[i];
            foreach (var (field, value) in new[] { (0, Name(b.Name)), (4, b.Kind), (8, b.Return), (12, b.Dimension), (20, b.Point), (24, b.Count), (28, b.Flags) })
                Put(bindingsAt + 32 * i + field, value);
        }
        return Padded([.. output, .. names]);
    }

    static byte[] Padded(byte[] data) => [.. data, .. new byte[(4 - data.Length % 4) % 4]];
}
