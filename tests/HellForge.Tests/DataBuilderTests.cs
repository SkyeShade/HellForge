using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core;
using HellForge.Core.GameData;

// The mod data builder against a synthetic game data folder (DSAR bundles and a DSAA index written here). The expected
// digests come from HD2Runtime's Python tools for the same inputs (artifacts/registry/build_talon_liberator.py's
// make_archive_with_streams, hd2_model.lut_mips and hd2_image.icon_family), so the port builds the same bytes.
static class DataBuilderTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The archive writer lays out resources as the game's archives (as the Python builders do)", () =>
        {
            var resources = new Dictionary<ResourceKey, ResourceParts>
            {
                [new(1, 5)] = new("abc"u8.ToArray(), Enumerable.Repeat((byte)1, 70).ToArray(), []),
                [new(1, 2)] = new(Enumerable.Repeat((byte)'x', 300).ToArray(), [], "st"u8.ToArray()),
                [new(ResourceTypes.Material, 9)] = new("m"u8.ToArray(), [], []),
            };
            var (archive, gpu, stream) = ResourceArchive.Write(resources);
            check(Sha(archive) == "F003B3E8E19937B34A9D055B7368D64C2E1649FA04F7909A98EF24858DBF4F59" && archive.Length == 833 &&
                Sha(gpu) == "0B208C7C876A86422D39C33964CECFEDF1E888A673BB00F7ECEFE243C47870A6" &&
                Sha(stream) == "19D50BCAB17C897DD880A362BA30C5531927120D2D71B97DCB535EE5EC3F91F1", "The archive differs from make_archive_with_streams.");
            var read = ResourceArchive.Read(archive, gpu, stream);
            check(read.Count == 3 && resources.All(r => read[r.Key].Main.SequenceEqual(r.Value.Main) && read[r.Key].Gpu.SequenceEqual(r.Value.Gpu) &&
                read[r.Key].Stream.SequenceEqual(r.Value.Stream)), "The archive does not read back.");
            check(ResourceTypes.Unit == 0xE0A48D0BE9A7453F && ResourceTypes.Material == 0xEAC0B497876ADEDF && ResourceTypes.Texture == 0xCD4238C6A0C69E32 &&
                ResourceTypes.Particles == 0xA8193123526FAD64, "Type names hash differently from the game's.");
        });

        test("Colour curves: warm keys turn blue (or towards a colour), everything else is kept", () =>
        {
            var floats = new List<float> { 3f, 10000f, 0.25f };
            floats.AddRange([0f, 0.5f, 0.9f, .. Enumerable.Repeat(10000f, 7)]);
            floats.AddRange([255f, 184f, 15f, 255f, 255f, 255f, 40f, 80f, 200f, .. Enumerable.Repeat(0f, 21)]);
            floats.AddRange([255f, 184f, 15f]);
            var effect = Floats(floats);
            var original = effect.ToArray();
            check(ResourceEdits.RecolourCurves(effect, ResourceEdits.WarmToBlue) == 1, "Only the curve's one warm key is recoloured.");
            var after = Enumerable.Range(0, floats.Count).Select(i => BinaryPrimitives.ReadSingleLittleEndian(effect.AsSpan(4 * i))).ToArray();
            check(after[13] == 15f && after[14] == (float)(184f * 0.6) && after[15] == 255f, "The warm key is not (b, g x 0.6, r).");
            check(effect.AsSpan(0, 52).SequenceEqual(original.AsSpan(0, 52)) && effect.AsSpan(64).SequenceEqual(original.AsSpan(64)), "Other floats changed.");
            var tinted = original.ToArray();
            ResourceEdits.RecolourCurves(tinted, ResourceEdits.TowardsColour([0, 1, 0.5]));
            check(BinaryPrimitives.ReadSingleLittleEndian(tinted.AsSpan(52)) == 0f && BinaryPrimitives.ReadSingleLittleEndian(tinted.AsSpan(56)) == 255f &&
                BinaryPrimitives.ReadSingleLittleEndian(tinted.AsSpan(60)) == 127.5f, "A colour target does not keep the key's brightness.");
            check(ResourceEdits.RecolourCurves(Floats([1f, 2f, 3f]), ResourceEdits.WarmToBlue) == 0, "Something that is no curve was recoloured.");
        });

        test("Material colours, references and colour lookup textures are edited only where expected", () =>
        {
            var material = Material(0x2222);
            ResourceEdits.SetMaterialColours(material, new Dictionary<uint, double[]> { [0x5086EEA5] = [0.1, 0.55, 1.0] });
            check(BinaryPrimitives.ReadSingleLittleEndian(material.AsSpan(0xBC + 16)) == 0.1f && BinaryPrimitives.ReadSingleLittleEndian(material.AsSpan(0xBC + 24)) == 1f,
                "The colour is not where the parameter's values are (right after the records).");
            reject(() => ResourceEdits.SetMaterialColours(Material(0x2222), new Dictionary<uint, double[]> { [0x12345678] = [1, 1, 1] }));
            check(ResourceEdits.Replace(material, 0x2222, 0xABCD) == 1 && BinaryPrimitives.ReadUInt64LittleEndian(material.AsSpan(0x8C)) == 0xABCD, "The reference was not replaced.");
            reject(() => ResourceEdits.Replace(material, 0x2222, 1));
            reject(() => ResourceEdits.Replace(Material(0x2222), 0x2222, 1, [0x90]));
            var lut = ResourceEdits.RecolourLut(IconImage.TextureMain(23, 8, 5, 10), Lut(), new(2,
                new Dictionary<int, double[]> { [0] = [0.83, 0.62, 0.18], [3] = [0.1, 0.2, 0.3] },
                new Dictionary<int, IReadOnlyDictionary<int, double[]>> { [5] = new Dictionary<int, double[]> { [1] = [0.5, 0.25, 0.125] } }));
            check(Sha(lut) == "580472BB9719E8FAD740A2E60DE80BF1BCF3BBBA13B56860949E008A50A8CBF7", "The LUT differs from hd2_model.lut_mips.");
            reject(() => ResourceEdits.RecolourLut(IconImage.TextureMain(23, 8, 5), Lut(), new(null, new Dictionary<int, double[]>(), new Dictionary<int, IReadOnlyDictionary<int, double[]>>())));
        });

        test("Stratagem icons build as hd2_image.py builds them: texture and GUI material", () =>
        {
            var png = Png(IconPixels());
            check(IconImage.ReadPng(png).Rgba.SequenceEqual(IconPixels()), "The PNG reader decoded other pixels.");
            const ulong name = 0x0123456789ABCDEF;
            var raw = IconImage.Family(png, name);
            var texture = raw[new(ResourceTypes.Texture, name)];
            check(Sha(texture.Main) == "4900AA6319D000D3F481B783B3C001164FACC32D494CE86CB2B470303B96BDF1" &&
                Sha(texture.Gpu) == "E39C5E78DC86FF2AF42EE0434FF80A6C5678AE5E56EA82C3FA9C9E4BECD89CA8", "The raw icon texture differs from icon_texture.");
            check(Sha(raw[new(ResourceTypes.Material, name)].Main) == "45202ECCE7AAF6F05F15806F796B822A101554FE8D2D2BA263ABFEC4E3831526", "The icon material differs from icon_material.");
            check(Sha(IconImage.Family(png, name, "auto")[new(ResourceTypes.Texture, name)].Gpu) == "23274C266E4B87E5006E1D88717706839AB4A26F495B95B0C35099DFAB1970C6",
                "The converted icon masks differ from icon_masks.");
            reject(() => IconImage.Family(Png(new byte[64 * 64 * 4], 64), name));
            // "image": BC3 (DXGI 77, as the UI atlas pages) with the full mip chain; transparency kept.
            var image = IconImage.Family(png, name, "image")[new(ResourceTypes.Texture, name)];
            var mipBytes = Enumerable.Range(0, 9).Sum(level => Math.Max(1, (256 >> level) / 4) * Math.Max(1, (256 >> level) / 4) * 16);
            check(BitConverter.ToInt32(image.Main, 0xC4 + 124) == 77 && image.Gpu.Length == mipBytes, "The image icon is not BC3 with its mips.");
            var clear = new byte[256 * 256 * 4];
            var clearBlock = IconImage.Family(Png(clear), name, "image")[new(ResourceTypes.Texture, name)].Gpu;
            check(clearBlock[0] == 0 && clearBlock[1] == 0, "A transparent picture did not stay transparent.");
            reject(() => IconImage.Family(png, name, "masks"));
            var corrupt = png.ToArray();
            corrupt[40] ^= 1;
            reject(() => IconImage.ReadPng(corrupt));
            // A zlib bomb: a 256 x 256 header over 64 MB of zeros (about 64 KB compressed) is refused once the header's
            // size is inflated, and a header claiming 8192 x 8192 at 16-bit RGBA is refused before anything inflates.
            var bomb = new MemoryStream();
            using (var zlib = new ZLibStream(bomb, CompressionLevel.Optimal, leaveOpen: true))
            {
                var zeros = new byte[1 << 20];
                for (var i = 0; i < 64; i++) zlib.Write(zeros);
            }
            var clock = System.Diagnostics.Stopwatch.StartNew();
            reject(() => IconImage.ReadPng(PngOf(256, 256, 8, 6, bomb.ToArray())));
            reject(() => IconImage.ReadPng(PngOf(8192, 8192, 16, 6, bomb.ToArray())));
            check(clock.Elapsed < TimeSpan.FromSeconds(2), $"The bombs took {clock.ElapsedMilliseconds} ms to refuse.");
            check(Lz4.DecompressBlock([0x41, (byte)'a', (byte)'b', (byte)'c', (byte)'d', 4, 0, 0x10, (byte)'x'], 10).SequenceEqual("abcdabcdax"u8.ToArray()), "LZ4 matches decode wrongly.");
        });

        test("Data specs are checked: fields, names, archives, colours and input files", () =>
        {
            var folder = Path.Combine(root, "data-spec-checks");
            Directory.CreateDirectory(folder);
            File.WriteAllBytes(Path.Combine(folder, "icon.png"), Png(IconPixels()));
            void Refused(string patch, string reason)
            {
                File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), $$"""{"format":1,"patches":[{{patch}}]}""");
                try { DataSpecs.Load(folder); }
                catch (InvalidDataException error) { check(error.Message.Contains(reason), $"Refused for another reason: {error.Message}"); return; }
                throw new Exception("Accepted: " + patch);
            }
            const string copy = """{"type":"unit","copy":"0x3333","name":"test/mod/unit"}""";
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"unit","copy":"0x3333","name":"test/mod/unit","colour":[1,1,1]}]}""", "unknown field colour");
            Refused("""{"archives":["NOTHEX"],"resources":[""" + copy + "]}", "16 lowercase hex");
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"unit","copy":"0x3333","name":"unit"}]}""", "the mod's own resource path");
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"unit","copy":"0x3333","include":["0x1"]}]}""", "exactly one of copy, include or icon");
            Refused("""{"archives":["1111111111111111"],"resources":[{"icon":"../icon.png","name":"test/mod/icon"}]}""", "a .png file in the mod folder");
            Refused("""{"archives":["1111111111111111"],"resources":[{"icon":"missing.png","name":"test/mod/icon"}]}""", "missing.png is missing");
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"particles","copy":"0x1","name":"test/mod/fx","curves":{"preset":"pink"}}]}""", "warm-to-blue");
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"material","copy":"0x1","name":"test/mod/m","colours":{"0x1":[1,2]}}]}""", "a colour is");
            Refused("""{"archives":["1111111111111111"],"resources":[{"type":"unit","copy":"0xZZ","name":"test/mod/unit"}]}""", "0x<up to 16 hex digits>");
            File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), """{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[""" + copy +
                "," + """{"icon":"icon.png","name":"test/mod/icon","prepare":"auto"}]}]}""");
            var spec = DataSpecs.Load(folder)!;
            check(spec.Inputs.SequenceEqual(["icon.png"]) && spec.Patches[0].Resources[0] is CopyResource { From: 0x3333, Type: var type } && type == ResourceTypes.Unit,
                "A valid spec did not parse.");
        });

        test("A mod's own mesh: OBJ faces are triangulated, turned Z-up with V flipped, and a unit copy names the file", () =>
        {
            var mesh = UnitMesh.ReadObj("""
                # a square and a triangle, Y-up
                v 0 0 0
                v 1 0 0
                v 1 1 0
                v 0 1 0
                v 0 0 -2
                vt 0 0
                vt 1 1
                vn 0 0 1
                f 1/1/1 2/1/1 3/2/1 4/2/1
                f -5 -4 -1
                """, 0.5, true);
            check(mesh.Triangles.Count == 9 && mesh.Positions.Count == 7, $"Quad as 2 triangles plus 1: {mesh.Triangles.Count / 3} triangles, {mesh.Positions.Count} vertices.");
            check(mesh.Positions[2] is [0.5, -0.0, 0.5] && mesh.Positions[6] is [0.0, 1.0, 0.0], "Scaled and turned Z-up: (x, -z, y).");
            check(mesh.Uvs[0] is [0.0, 1.0] && mesh.Uvs[2] is [1.0, 0.0], "V is flipped for the game.");
            // the triangle (0,0,0) (0.5,0,0) (0,1,0) after the turn lies flat: its face normal points up (+Z)
            check(mesh.Normals[0] is [0.0, -1.0, 0.0] && mesh.Normals[4] is { Length: 3 } faceNormal && faceNormal[0] == 0 && faceNormal[1] == 0 && faceNormal[2] > 0,
                "Given normals turn with the mesh; a corner without one takes its face's.");
            reject(() => UnitMesh.ReadObj("v 0 0 0\nf 1 2 3\n", 1, true));
            reject(() => UnitMesh.ReadObj("v 0 0 0\n", 1, true));

            var folder = Directory.CreateDirectory(Path.Combine(root, "mesh-spec")).FullName;
            Directory.CreateDirectory(Path.Combine(folder, "models"));
            File.WriteAllText(Path.Combine(folder, "models", "cube.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
            string Spec(string mesh, string type = "unit") =>
                $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{"type":"{{type}}","copy":"0x3333","name":"test/mod/unit","mesh":{{mesh}}}]}]}""";
            File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), Spec("""{"obj":"models/cube.obj","scale":2,"yUp":false}"""));
            var spec = DataSpecs.Load(folder)!;
            check(spec.Inputs.SequenceEqual(["models/cube.obj"]) && spec.Patches[0].Resources[0] is CopyResource { Mesh: { Obj: "models/cube.obj", Scale: 2, YUp: false } },
                "The mesh and its file are part of the spec (and of its cache key).");
            foreach (var (bad, why) in new[] { (Spec("""{"obj":"../cube.obj"}"""), "an .obj file in the mod folder"), (Spec("""{"obj":"models/none.obj"}"""), "is missing"),
                (Spec("""{"obj":"models/cube.obj"}""", "material"), "mesh is for a unit copy"), (Spec("""{"obj":"models/cube.obj","scale":0}"""), "scale is a positive number") })
            {
                File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), bad);
                try { DataSpecs.Load(folder); throw new Exception("Accepted: " + bad); }
                catch (InvalidDataException error) { check(error.Message.Contains(why), "Refused for another reason: " + error.Message); }
            }
        });

        test("A data spec builds from the game data, is cached per game data and spec, staged, and counted by its spec", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "data-install"));
            install.Create("12345");
            var gameData = Path.Combine(install.Game, "data");
            WriteGameData(gameData);
            var mods = Path.Combine(install.Root, "mods");
            var folder = Path.Combine(mods, "data-mod");
            Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), """{"format":1,"id":"test.data","name":"Data","version":"1"}""");
            File.WriteAllBytes(Path.Combine(folder, "icon.png"), Png(IconPixels()));
            void Spec(string colour, string extra = "") => File.WriteAllText(Path.Combine(folder, DataSpecs.FileName), """
                {"format":1,"patches":[
                  {"archives":["1111111111111111"],"resources":[
                    {"type":"unit","copy":"0x3333","name":"test/mod/unit","replace":[{"from":"0x1111","to":"test/mod/material","at":["0x40"]}]},
                    {"type":"material","copy":"0x1111","name":"test/mod/material","expect":{"0x18":"0xDED20067E81BFC4F"},
                     "replace":[{"from":"0x2222","to":"test/mod/lut"}],"colours":{"0xB4138578":COLOUR}},
                    {"type":"texture","copy":"0x2222","name":"test/mod/lut","lut":{"zonesFrom":2,"columns":{"0":[0.83,0.62,0.18]}}},
                    {"type":"particles","copy":"0x4444","name":"test/mod/bolt","curves":{"preset":"warm-to-blue","keys":1}},
                    {"type":"wwise_bank","include":["0x5555"]},
                    {"icon":"icon.png","name":"test/mod/icon"}EXTRA]},
                  {"archives":["1111111111111111","2222222222222222"],"resources":[{"type":"wwise_bank","include":["0x5555"]}]}]}
                """.Replace("COLOUR", colour).Replace("EXTRA", extra));
            Spec("[0,0.35,1]");
            var (loaded, problems) = ModCatalog.Load(mods);
            check(problems.Count == 0 && loaded.Single().HasDataSpec, "The data mod did not load: " + string.Join(" | ", problems));
            var digest = ModCatalog.Digest(loaded);
            reject(() => ModStaging.Stage(install, loaded));

            var (ready, failed) = ModDataCache.Prepare(install, loaded);
            check(failed.Count == 0 && ready.Single().GeneratedData is { } generated && Directory.Exists(generated), "The data was not built: " + string.Join(" | ", failed));
            var output = ready.Single().GeneratedData!;
            var built = ResourceArchive.Read(File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0")),
                File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0.gpu_resources")), File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0.stream")));
            ulong H(string name) => EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name));
            var unit = built[new(ResourceTypes.Unit, H("test/mod/unit"))];
            check(BinaryPrimitives.ReadUInt64LittleEndian(unit.Main.AsSpan(0x40)) == H("test/mod/material") && unit.Gpu.SequenceEqual(Enumerable.Repeat((byte)7, 100)),
                "The unit copy does not name the material copy or lost its GPU part.");
            var material = built[new(ResourceTypes.Material, H("test/mod/material"))].Main;
            check(BinaryPrimitives.ReadUInt64LittleEndian(material.AsSpan(0x8C)) == H("test/mod/lut") && BinaryPrimitives.ReadSingleLittleEndian(material.AsSpan(0xC0)) == 0.35f,
                "The material copy is not edited.");
            check(built[new(ResourceTypes.Particles, H("test/mod/bolt"))].Main.AsSpan(4 * 11, 4).SequenceEqual(BitConverter.GetBytes(15f)), "The bolt is not blue.");
            check(built[new(ResourceTypes.Hash("wwise_bank"), 0x5555)].Stream.SequenceEqual(Enumerable.Repeat((byte)9, 40)) &&
                built[new(ResourceTypes.Hash("wwise_bank"), 0x5555)].Main.SequenceEqual("bank-first"u8.ToArray()), "The included bank is not the first archive's.");
            check(built.ContainsKey(new(ResourceTypes.Texture, H("test/mod/icon"))) && built.ContainsKey(new(ResourceTypes.Material, H("test/mod/icon"))) && built.Count == 8,
                "The icon's texture or material is missing.");
            // the unit copy's physics, under its name and naming it inside (the game files physics by that), nothing else moved
            var physics = built[new(ResourceTypes.Physics, H("test/mod/unit"))].Main;
            check(new[] { 0x08, 0x38, 0x40 }.All(at => BinaryPrimitives.ReadUInt64LittleEndian(physics.AsSpan(at)) == H("test/mod/unit")) &&
                BinaryPrimitives.ReadUInt64LittleEndian(physics) == ResourceTypes.Physics && BinaryPrimitives.ReadUInt64LittleEndian(physics.AsSpan(0x50)) == 0x3333,
                "The physics copy does not name its unit.");
            check(File.Exists(Path.Combine(output, "1111111111111111.patch_1")) && File.Exists(Path.Combine(output, "2222222222222222.patch_0.stream")),
                "The second patch is not numbered per archive.");

            check(!ModDataCache.Ensure(install, loaded.Single(), () => throw new Exception("Rebuilt an up-to-date cache.")).Built, "The cache was not reused.");
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) == digest, "Generated data changed the modlist code.");
            // Staged after the patch already in the game's data folder.
            var staged = ModStaging.Stage(install, ready);
            check(staged.SequenceEqual(["1111111111111111.patch_1", "1111111111111111.patch_1.gpu_resources", "1111111111111111.patch_1.stream",
                "1111111111111111.patch_2", "1111111111111111.patch_2.gpu_resources", "1111111111111111.patch_2.stream",
                "2222222222222222.patch_0", "2222222222222222.patch_0.gpu_resources", "2222222222222222.patch_0.stream"]), "Staged: " + string.Join(", ", staged));
            check(File.ReadAllBytes(Path.Combine(gameData, "1111111111111111.patch_2")).SequenceEqual(File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_1"))),
                "The generated patches were staged out of order.");
            ModStaging.Unstage(install);

            Spec("[0,0.5,1]");
            check(ModCatalog.Digest(ModCatalog.Load(mods).Mods) != digest, "A changed spec kept the modlist code.");
            using (var game = new GameDataReader(gameData))
                check(ModDataCache.Ensure(install, ModCatalog.Load(mods).Mods.Single(), () => game).Built, "A changed spec reused the old data.");
            check(Directory.GetDirectories(Path.Combine(ModDataCache.Folder(install), "test.data")).Length == 1, "The stale cache was kept.");

            Spec("[0,0.5,1]", """,{"type":"texture","include":["0x9999"]}""");
            var (_, missing) = ModDataCache.Prepare(install, ModCatalog.Load(mods).Mods);
            check(missing.Single().Contains("not in the game data: texture 0x0000000000009999"), "A missing resource was not reported: " + string.Join(" | ", missing));
            Spec("[0,0.5,1]", """,{"type":"unit","copy":"0x3333","name":"test/mod/taken"}""");
            var (none, taken) = ModDataCache.Prepare(install, ModCatalog.Load(mods).Mods);
            check(none.Count == 0 && taken.Single().Contains("the game's own resources"), "A copy took a game resource's name: " + string.Join(" | ", taken));
        });

        test("A material of a newer build is rebuilt in its shader's layout as the game copy has it, values by name", () =>
        {
            // the game copy's shader: textures A, B; parameters P (scalar), Q (RGB), R (scalar)
            var shader = LaidOut(0, [(0xA, 0x100), (0xB, 0x200)], [(0, 0x50, 0), (2, 0x51, 4), (0, 0x52, 16)], [1, 2, 3, 4, 5]);
            // the newer build's child: a new texture slot first, a new parameter, Q after it, R missing
            var child = LaidOut(0x9999, [(0xE, 0x300), (0xA, 0x101), (0xB, 0x201)], [(0, 0x50, 0), (0, 0x58, 4), (2, 0x51, 8)], [10, 99, 20, 21, 22]);
            var ported = ResourceEdits.MaterialInLayout(child, shader);
            check(ported.Length == shader.Length && BinaryPrimitives.ReadUInt64LittleEndian(ported.AsSpan(0x18)) == 0x9999 &&
                BinaryPrimitives.ReadUInt32LittleEndian(ported.AsSpan(0x10)) == 0, "The port is not a child in the shader's layout.");
            check(BinaryPrimitives.ReadInt32LittleEndian(ported.AsSpan(0x40)) == 2 && BinaryPrimitives.ReadUInt64LittleEndian(ported.AsSpan(0x88 + 8)) == 0x101 &&
                BinaryPrimitives.ReadUInt64LittleEndian(ported.AsSpan(0x88 + 16)) == 0x201, "The textures are not the child's, by slot name.");
            check(Value(ported, 0x50) == 10 && Value(ported, 0x51) == 20 && Value(ported, 0x52) == 5, "Values are not the child's by name (the shader's where it has none).");
            check(BinaryPrimitives.ReadSingleLittleEndian(ported.AsSpan(0x88 + 24 + 60 + 12)) == 22, "An RGB value lost its channels.");
            var same = LaidOut(0x9999, [(0xA, 0x7), (0xB, 0x8)], [(0, 0x52, 0), (2, 0x51, 4), (0, 0x50, 16)], [1, 2, 3, 4, 5]);
            check(ReferenceEquals(ResourceEdits.MaterialInLayout(same, shader), same), "A material already in its shader's layout was rebuilt.");
            reject(() => ResourceEdits.MaterialInLayout(shader, shader));
            reject(() => ResourceEdits.MaterialInLayout(LaidOut(0x9999, [], [(0, 0x50, 64)], [1]), shader));
        });

        test("Resources come from Steam's install with \"source\": \"steam\", new ones only, materials in the copy's layout", () =>
        {
            var folder = Path.Combine(root, "steam-source");
            var game = Path.Combine(folder, "game");
            var steamData = Path.Combine(folder, "steam");
            var shader = LaidOut(0, [(0xA, 0x100)], [(0, 0x50, 0)], [1]);
            WriteBundles(game, new Dictionary<string, byte[]>
            {
                ["1111111111111111"] = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
                {
                    [new(ResourceTypes.Material, 0x9999)] = new(shader, [], []),
                    [new(ResourceTypes.Texture, 0x100)] = new([1], [], []),
                }).Archive
            });
            var steamArchive = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.Unit, 0x3333)] = new(new byte[0x40], [5, 5], []),
                [new(ResourceTypes.Physics, 0x3333)] = new(Physics(0x3333), [], []),
                [new(ResourceTypes.Material, 0x7777)] = new(LaidOut(0x9999, [(0xE, 0x300), (0xA, 0x101)], [(0, 0x58, 0), (0, 0x50, 4)], [99, 42]), [], []),
                [new(ResourceTypes.Texture, 0x100)] = new([2], [], []),
            });
            WriteBundles(steamData, new() { ["5555555555555555"] = steamArchive.Archive, ["5555555555555555.gpu_resources"] = steamArchive.Gpu });
            var mod = Directory.CreateDirectory(Path.Combine(folder, "mod")).FullName;
            DataSpec Spec(string resources)
            {
                File.WriteAllText(Path.Combine(mod, DataSpecs.FileName), $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{{resources}}]}]}""");
                return DataSpecs.Load(mod)!;
            }
            const string port = """{"type":"unit","copy":"0x3333","name":"test/mod/port","source":"steam"},{"type":"material","include":["0x7777"],"source":"steam"}""";
            check(Spec(port).UsesSteam && !Spec("""{"type":"texture","include":["0x100"],"source":"game"}""").UsesSteam, "source did not parse.");
            reject(() => Spec("""{"type":"texture","include":["0x100"],"source":"live"}"""));
            using var gameReader = new GameDataReader(game);
            using var steamReader = new GameDataReader(steamData);
            var output = Path.Combine(folder, "out");
            reject(() => ModDataBuilder.Build(Spec(port), mod, gameReader, output));
            ModDataBuilder.Build(Spec(port), mod, gameReader, output, steamReader);
            var built = ResourceArchive.Read(File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0")),
                File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0.gpu_resources")), []);
            var name = EngineHash.Murmur64A("test/mod/port"u8);
            check(built[new(ResourceTypes.Unit, name)].Gpu.SequenceEqual(new byte[] { 5, 5 }) && built.ContainsKey(new(ResourceTypes.Physics, name)),
                "The unit from Steam was not copied with its physics.");
            var material = built[new(ResourceTypes.Material, 0x7777)].Main;
            check(BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(0x40)) == 1 && Value(material, 0x50) == 42 &&
                BinaryPrimitives.ReadUInt64LittleEndian(material.AsSpan(0x88 + 4)) == 0x101, "The material from Steam is not in the game copy's shader layout.");
            try { ModDataBuilder.Build(Spec("""{"type":"texture","include":["0x100"],"source":"steam"}"""), mod, gameReader, output + "2", steamReader); throw new Exception("Shadowed."); }
            catch (InvalidDataException error) { check(error.Message.Contains("include them from the game"), "Refused for another reason: " + error.Message); }
            try { ModDataBuilder.Build(Spec("""{"type":"texture","include":["0x4242"],"source":"steam"}"""), mod, gameReader, output + "3", steamReader); throw new Exception("Missing."); }
            catch (InvalidDataException error) { check(error.Message.Contains("0x0000000000004242 (Steam)"), "A missing Steam resource is not named: " + error.Message); }
        });
    }

    // The synthetic game data: archive 1111111111111111 with a unit naming a material, the material naming a colour
    // lookup texture, an effect with one warm curve, a sound bank with a stream part, and a unit under the name
    // "test/mod/taken"; archive 2222222222222222 with another bank of the same name (the first archive's wins). Both
    // are in one bundle, split into 48-byte chunks (every other LZ4-coded), the archives in two index entries each.
    // A physics resource as the game lays them out: its type, then its unit's name at +0x08 (repeated at +0x38 and
    // +0x40), and a label and bodies after.
    static byte[] Physics(ulong unit)
    {
        var main = new byte[0x60];
        BinaryPrimitives.WriteUInt64LittleEndian(main, ResourceTypes.Physics);
        foreach (var at in new[] { 0x08, 0x38, 0x40 }) BinaryPrimitives.WriteUInt64LittleEndian(main.AsSpan(at), unit);
        BinaryPrimitives.WriteUInt64LittleEndian(main.AsSpan(0x50), unit); // a body's own data that happens to hold the same bits
        return main;
    }

    static void WriteGameData(string folder)
    {
        Directory.CreateDirectory(folder);
        var unit = new byte[0x100];
        BinaryPrimitives.WriteUInt64LittleEndian(unit.AsSpan(0x40), 0x1111);
        List<float> curve = [7f, 0f, .. Enumerable.Repeat(10000f, 9), 255f, 184f, 15f, .. Enumerable.Repeat(0f, 27)];
        var bank = new ResourceKey(ResourceTypes.Hash("wwise_bank"), 0x5555);
        var first = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
        {
            [new(ResourceTypes.Unit, 0x3333)] = new(unit, Enumerable.Repeat((byte)7, 100).ToArray(), []),
            [new(ResourceTypes.Physics, 0x3333)] = new(Physics(0x3333), [], []),
            [new(ResourceTypes.Material, 0x1111)] = new(Material(0x2222), [], []),
            [new(ResourceTypes.Texture, 0x2222)] = new(IconImage.TextureMain(23, 8, 5, 10), Lut(), []),
            [new(ResourceTypes.Particles, 0x4444)] = new(Floats(curve), [], []),
            [bank] = new("bank-first"u8.ToArray(), [], Enumerable.Repeat((byte)9, 40).ToArray()),
            [new(ResourceTypes.Unit, EngineHash.Murmur64A("test/mod/taken"u8))] = new([1], [], []),
        });
        var second = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [bank] = new("bank-second"u8.ToArray(), [], [1, 2, 3]) });
        WriteBundles(folder, new Dictionary<string, byte[]>
        {
            ["2222222222222222"] = second.Archive, ["2222222222222222.stream"] = second.Stream,
            ["1111111111111111"] = first.Archive, ["1111111111111111.gpu_resources"] = first.Gpu, ["1111111111111111.stream"] = first.Stream,
        });
        // A loose patch file, as a session stages them: never read as game data.
        File.WriteAllText(Path.Combine(folder, "1111111111111111.patch_0"), "staged by a session");
    }

    // Archives and their companions as the game ships them: one bundle of 48-byte chunks (every other LZ4-coded), each
    // item in two index entries.
    static void WriteBundles(string folder, Dictionary<string, byte[]> items)
    {
        Directory.CreateDirectory(folder);
        const int chunkSize = 48;
        var chunks = new List<(ulong Offset, byte[] Stored, uint Size, byte Compression)>();
        var entries = new Dictionary<string, List<(uint ArchiveOffset, uint BundleOffset)>>();
        ulong at = 0;
        foreach (var (name, data) in items)
        {
            entries[name] = [];
            for (var start = 0; start < data.Length; start += chunkSize)
            {
                var piece = data[start..Math.Min(data.Length, start + chunkSize)];
                if (start / chunkSize < 2) entries[name].Add(((uint)start, (uint)at));
                var lz4 = chunks.Count % 2 == 1;
                chunks.Add((at, lz4 ? Lz4Literals(piece) : piece, (uint)piece.Length, (byte)(lz4 ? 3 : 0)));
                at += (ulong)piece.Length;
            }
        }
        File.WriteAllBytes(Path.Combine(folder, "bundles.00.nxa"), Dsar(chunks));
        // The DSAA index: header, 24-byte items {size, name offset, entry count, entries offset}, bundle name offsets,
        // names, then 16-byte entries {archive offset, -, bundle offset, -, bundle index at +15}.
        var names = new MemoryStream();
        var nameAt = new Dictionary<string, int>();
        var headerSize = 24 + 24 * items.Count + 4;
        foreach (var name in items.Keys.Append("bundles.00.nxa"))
        {
            nameAt[name] = headerSize + (int)names.Length;
            names.Write(Encoding.ASCII.GetBytes(name + "\0"));
        }
        var entriesAt = headerSize + (int)names.Length;
        var index = new byte[entriesAt + 16 * entries.Values.Sum(e => e.Count)];
        "DSAA"u8.CopyTo(index);
        BinaryPrimitives.WriteInt32LittleEndian(index.AsSpan(12), 1);
        BinaryPrimitives.WriteInt32LittleEndian(index.AsSpan(16), items.Count);
        var i = 0;
        var e = entriesAt;
        foreach (var (name, data) in items)
        {
            var row = index.AsSpan(24 + 24 * i++, 24);
            BinaryPrimitives.WriteUInt64LittleEndian(row, (ulong)data.Length);
            BinaryPrimitives.WriteInt32LittleEndian(row[8..], nameAt[name]);
            BinaryPrimitives.WriteInt32LittleEndian(row[12..], entries[name].Count);
            BinaryPrimitives.WriteUInt64LittleEndian(row[16..], (ulong)e);
            foreach (var (archiveOffset, bundleOffset) in entries[name])
            {
                BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(e), archiveOffset);
                BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(e + 8), bundleOffset);
                e += 16;
            }
        }
        BinaryPrimitives.WriteInt32LittleEndian(index.AsSpan(24 + 24 * items.Count), nameAt["bundles.00.nxa"]);
        names.ToArray().CopyTo(index, headerSize);
        File.WriteAllBytes(Path.Combine(folder, GameDataReader.IndexFile), Dsar([(0, index, (uint)index.Length, 0)]));
    }

    static byte[] Dsar(List<(ulong Offset, byte[] Stored, uint Size, byte Compression)> chunks)
    {
        var header = 32 + 32 * chunks.Count;
        var file = new byte[header + chunks.Sum(c => c.Stored.Length)];
        "DSAR"u8.CopyTo(file);
        BinaryPrimitives.WriteInt32LittleEndian(file.AsSpan(8), chunks.Count);
        var at = header;
        for (var i = 0; i < chunks.Count; i++)
        {
            var row = file.AsSpan(32 + 32 * i, 32);
            BinaryPrimitives.WriteUInt64LittleEndian(row, chunks[i].Offset);
            BinaryPrimitives.WriteUInt64LittleEndian(row[8..], (ulong)at);
            BinaryPrimitives.WriteUInt32LittleEndian(row[16..], chunks[i].Size);
            BinaryPrimitives.WriteUInt32LittleEndian(row[20..], (uint)chunks[i].Stored.Length);
            row[24] = chunks[i].Compression;
            chunks[i].Stored.CopyTo(file, at);
            at += chunks[i].Stored.Length;
        }
        return file;
    }

    // An LZ4 block of literals only (one sequence without a match).
    static byte[] Lz4Literals(byte[] data)
    {
        var block = new List<byte> { (byte)(Math.Min(data.Length, 15) << 4) };
        if (data.Length >= 15)
        {
            var rest = data.Length - 15;
            for (; rest >= 255; rest -= 255) block.Add(255);
            block.Add((byte)rest);
        }
        block.AddRange(data);
        return [.. block];
    }

    // A material naming texture `lut`, with two RGB parameters (0xB4138578 at +0 of its values, 0x5086EEA5 at +16); the
    // values start right after the records, at 0xBC.
    static byte[] Material(ulong lut)
    {
        var data = new byte[0x100];
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(0x18), 0xDED20067E81BFC4F);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(0x40), 1);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(0x68), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x88), 0xAAAA);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(0x8C), lut);
        foreach (var (record, name, offset) in new[] { (0x94, 0xB4138578u, 0u), (0xA8, 0x5086EEA5u, 16u) })
        {
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(record), 2);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(record + 8), name);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(record + 12), offset);
        }
        return data;
    }

    // A material: parent at 0x18, texture slots {name, id}, parameters {type, name, offset} and their values right after
    // the records (size at 0x78), padded to 16 bytes.
    static byte[] LaidOut(ulong parent, (uint Name, ulong Id)[] textures, (uint Type, uint Name, int Offset)[] parameters, float[] values)
    {
        var at = 0x88 + 12 * textures.Length + 20 * parameters.Length;
        var data = new byte[(at + 4 * values.Length + 15) / 16 * 16];
        BinaryPrimitives.WriteUInt32LittleEndian(data, 0x120);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x10), parent == 0 ? 0x1234u : 0);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(0x18), parent);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(0x40), textures.Length);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(0x68), parameters.Length);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(0x78), 4 * values.Length);
        for (var i = 0; i < textures.Length; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x88 + 4 * i), textures[i].Name);
            BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(0x88 + 4 * textures.Length + 8 * i), textures[i].Id);
        }
        for (var i = 0; i < parameters.Length; i++)
        {
            var record = data.AsSpan(0x88 + 12 * textures.Length + 20 * i);
            BinaryPrimitives.WriteUInt32LittleEndian(record, parameters[i].Type);
            BinaryPrimitives.WriteUInt32LittleEndian(record[8..], parameters[i].Name);
            BinaryPrimitives.WriteInt32LittleEndian(record[12..], parameters[i].Offset);
        }
        for (var i = 0; i < values.Length; i++) BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(at + 4 * i), values[i]);
        return data;
    }

    static float Value(byte[] material, uint name)
    {
        int textures = BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(0x40)), parameters = BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(0x68));
        var records = 0x88 + 12 * textures;
        for (var i = 0; i < parameters; i++)
            if (BinaryPrimitives.ReadUInt32LittleEndian(material.AsSpan(records + 20 * i + 8)) == name)
                return BinaryPrimitives.ReadSingleLittleEndian(material.AsSpan(records + 20 * parameters + BinaryPrimitives.ReadInt32LittleEndian(material.AsSpan(records + 20 * i + 12))));
        throw new Exception($"no parameter {name:X8}");
    }

    // A 23 x 8 RGBA16F lookup texture with 5 mips; the top mip's values k/512 (exact halves), the rest zero.
    static byte[] Lut()
    {
        var gpu = new byte[1928];
        for (var y = 0; y < 8; y++)
            for (var x = 0; x < 23; x++)
                for (var c = 0; c < 4; c++)
                    BinaryPrimitives.WriteHalfLittleEndian(gpu.AsSpan(((y * 23 + x) * 4 + c) * 2), (Half)(((x * 8 + y) * 4 + c) / 512.0));
        return gpu;
    }

    static byte[] IconPixels()
    {
        var rgba = new byte[256 * 256 * 4];
        for (var y = 0; y < 256; y++)
            for (var x = 0; x < 256; x++)
            {
                var o = (y * 256 + x) * 4;
                rgba[o] = (byte)(x * 7 + y * 3);
                rgba[o + 1] = (byte)(x ^ y);
                rgba[o + 2] = (byte)(x * y >> 4);
                rgba[o + 3] = (x / 16 + y / 16) % 3 != 0 ? (byte)255 : (byte)(x + y);
            }
        return rgba;
    }

    // An RGBA PNG whose rows use every filter type in turn (the Python reference used filter 0 only: same pixels).
    static byte[] Png(byte[] rgba, int size = 256)
    {
        var stride = size * 4;
        var raw = new MemoryStream();
        var previous = new byte[stride];
        for (var y = 0; y < size; y++)
        {
            var line = rgba.AsSpan(y * stride, stride).ToArray();
            var filter = (byte)(y % 5);
            raw.WriteByte(filter);
            for (var i = 0; i < stride; i++)
            {
                int a = i >= 4 ? line[i - 4] : 0, b = previous[i], c = i >= 4 ? previous[i - 4] : 0, p = a + b - c;
                int pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
                raw.WriteByte((byte)(line[i] - filter switch { 0 => 0, 1 => a, 2 => b, 3 => (a + b) / 2, _ => pa <= pb && pa <= pc ? a : pb <= pc ? b : c }));
            }
            previous = line;
        }
        var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.Optimal, leaveOpen: true)) zlib.Write(raw.ToArray());
        var png = new MemoryStream();
        png.Write(IconImage.PngSignature);
        void Chunk(string kind, byte[] body)
        {
            var bytes = Encoding.ASCII.GetBytes(kind).Concat(body).ToArray();
            png.Write(BigEndian((uint)body.Length));
            png.Write(bytes);
            png.Write(BigEndian(IconImage.Crc32(bytes)));
        }
        Chunk("IHDR", [.. BigEndian((uint)size), .. BigEndian((uint)size), 8, 6, 0, 0, 0]);
        Chunk("IDAT", compressed.ToArray());
        Chunk("IEND", []);
        return png.ToArray();
    }

    // A PNG of any header and image data, as given.
    static byte[] PngOf(int width, int height, byte depth, byte colour, byte[] idat)
    {
        var png = new MemoryStream();
        png.Write(IconImage.PngSignature);
        void Chunk(string kind, byte[] body)
        {
            var bytes = Encoding.ASCII.GetBytes(kind).Concat(body).ToArray();
            png.Write(BigEndian((uint)body.Length));
            png.Write(bytes);
            png.Write(BigEndian(IconImage.Crc32(bytes)));
        }
        Chunk("IHDR", [.. BigEndian((uint)width), .. BigEndian((uint)height), depth, colour, 0, 0, 0]);
        Chunk("IDAT", idat);
        Chunk("IEND", []);
        return png.ToArray();
    }

    static byte[] BigEndian(uint value)
    {
        var bytes = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        return bytes;
    }

    static byte[] Floats(IEnumerable<float> values)
    {
        var list = values.ToList();
        var bytes = new byte[4 * list.Count];
        for (var i = 0; i < list.Count; i++) BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(4 * i), list[i]);
        return bytes;
    }

    static string Sha(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
}
