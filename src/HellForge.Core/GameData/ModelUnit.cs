using System.Text.RegularExpressions;

namespace HellForge.Core.GameData;

// A glTF model as a unit (a data spec's "model"; the conventions are in docs/modding/models.md). Every node becomes a joint
// under the root joint StingrayEntityRoot, turned from glTF's Y-up to the game's Z-up: (x, y, z) -> (x, -z, y), so +Y
// forward in Blender stays +Y. A mesh's vertices are rigid on its own node, or on the joint a skin weights most. Node
// names give the LOD (suffix _lod1 to _lod7; none is LOD 0) and shadow casters (suffix _shadow); a LOD without a shadow
// caster casts its own geometry, except the last of several.
public static partial class ModelUnit
{
    public const string Root = "StingrayEntityRoot";

    public static UnitWriter.Model Build(IReadOnlyList<Gltf.Node> nodes, IReadOnlyDictionary<string, ulong> materials, IReadOnlyList<double>? lodSizes, ulong bones = 0, ulong stateMachine = 0)
    {
        if (nodes.Count == 0) throw new InvalidDataException("the model has no nodes.");
        var joints = new List<UnitWriter.Joint> { new(Root, 0, [0, 0, 0], [0, 0, 0, 1], [1, 1, 1]) };
        joints.AddRange(nodes.Select(n => new UnitWriter.Joint(n.Name, n.Parent + 1, Turn(n.Translation), [n.Rotation[0], -n.Rotation[2], n.Rotation[1], n.Rotation[3]], [n.Scale[0], n.Scale[2], n.Scale[1]])));
        var world = GltfWorld(nodes);
        var parts = new Dictionary<(int Lod, bool Shadow), Dictionary<string, PartBuilder>>();
        for (var i = 0; i < nodes.Count; i++)
        {
            if (nodes[i].Primitives.Length == 0) continue;
            var (lod, shadow) = Kind(nodes[i].Name);
            if (!parts.TryGetValue((lod, shadow), out var byMaterial)) parts[(lod, shadow)] = byMaterial = [];
            foreach (var p in nodes[i].Primitives) Add(byMaterial, p, nodes[i], i, world);
        }
        if (parts.Count == 0) throw new InvalidDataException("the model has no meshes.");
        var meshes = Meshes(parts);
        var used = meshes.SelectMany(m => m.Parts.Select(p => p.Material)).Distinct().ToList();
        var missing = used.Where(m => !materials.ContainsKey(m)).ToList();
        if (missing.Count > 0) throw new InvalidDataException($"materials names no resource for the model's {string.Join(", ", missing)}.");
        var lods = meshes.Max(m => m.Lod) + 1;
        var sizes = lodSizes ?? (lods <= UnitWriter.WeaponLodSizes.Length ? UnitWriter.WeaponLodSizes[..lods] : throw new InvalidDataException($"the model has {lods} LODs; give their sizes."));
        return new(joints, meshes, used.ToDictionary(m => m, m => materials[m]), sizes, bones, stateMachine);
    }

    static double[] Turn(double[] v) => [v[0], -v[2], v[1]];

    // A game unit's animations fit the model when every joint its bones resource names (after the two root entries) is a
    // joint of the model with the same rest transform: its clips hold whole local transforms, not offsets from rest.
    public static void CheckAnimations(UnitWriter.Model model, byte[] baseUnit, byte[] bones)
    {
        var moving = BoneHashes(bones).Skip(2).ToList();
        var own = model.Joints.ToDictionary(j => EngineHash.Id32(j.Name));
        var rest = BaseJoints(baseUnit);
        foreach (var hash in moving)
        {
            if (!rest.TryGetValue(hash, out var r)) continue;
            if (!own.TryGetValue(hash, out var j)) throw new InvalidDataException($"the model has no joint 0x{hash:X8}, which the animations move; add it with the base unit's name and rest pose (HellForge.Cli unit-joints).");
            var mine = Matrix.Compose(j.Translation, j.Rotation, j.Scale);
            if (mine.Zip(r).Max(p => Math.Abs(p.First - p.Second)) > 1e-3)
                throw new InvalidDataException($"joint {j.Name}'s rest pose is not the animated unit's; its clips would move it to theirs (HellForge.Cli unit-joints lists it).");
        }
    }

    // A bones resource's thin name hashes: u32 count, u32 LOD count, the LOD thresholds, then the hashes.
    static IEnumerable<uint> BoneHashes(byte[] b)
    {
        var n = (int)U32(b, 0);
        var lods = (int)U32(b, 4);
        return Enumerable.Range(0, n).Select(i => U32(b, 8 + 4 * lods + 4 * i)).ToList();
    }

    // A unit's joints by thin name hash, each its local transform as a matrix (header +0x34; rotations column-major).
    static Dictionary<uint, double[]> BaseJoints(byte[] u)
    {
        var at = (int)U32(u, 0x34);
        var n = at == 0 ? 0 : (int)U32(u, at);
        var map = new Dictionary<uint, double[]>();
        for (var i = 0; i < n; i++)
        {
            var o = at + 16 + 64 * i;
            double F(int k) => BitConverter.ToSingle(u, o + 4 * k);
            map[U32(u, at + 16 + 132 * n + 4 * i)] = [F(0) * F(12), F(3) * F(13), F(6) * F(14), F(9), F(1) * F(12), F(4) * F(13), F(7) * F(14), F(10), F(2) * F(12), F(5) * F(13), F(8) * F(14), F(11), 0, 0, 0, 1];
        }
        return map;
    }

    static uint U32(byte[] b, int at) =>
        at >= 0 && at + 4 <= b.Length ? BitConverter.ToUInt32(b, at) : throw new InvalidDataException("the animated unit's data ends early.");

    [GeneratedRegex(@"_lod([0-7])$", RegexOptions.IgnoreCase)]
    private static partial Regex LodSuffix();

    static (int Lod, bool Shadow) Kind(string name)
    {
        var shadow = name.EndsWith("_shadow", StringComparison.OrdinalIgnoreCase);
        if (shadow) name = name[..^"_shadow".Length];
        var m = LodSuffix().Match(name);
        return (m.Success ? m.Groups[1].Value[0] - '0' : 0, shadow);
    }

    // Each node's world matrix in glTF's own space (row-major, column vectors).
    static double[][] GltfWorld(IReadOnlyList<Gltf.Node> nodes)
    {
        var world = new double[nodes.Count][];
        for (var i = 0; i < nodes.Count; i++)
        {
            var local = Matrix.Compose(nodes[i].Translation, nodes[i].Rotation, nodes[i].Scale);
            world[i] = nodes[i].Parent < 0 ? local : Matrix.Multiply(world[nodes[i].Parent], local);
        }
        return world;
    }

    sealed class PartBuilder
    {
        public readonly List<UnitWriter.Vertex> Vertices = [];
        public readonly List<int> Triangles = [];
    }

    // A primitive's vertices in the unit's space (turned to Z-up), each on its node's joint (index + 1, after the root)
    // or, skinned, on the joint its largest weight names.
    static void Add(Dictionary<string, PartBuilder> byMaterial, Gltf.Primitive p, Gltf.Node node, int index, double[][] world)
    {
        if (!byMaterial.TryGetValue(p.Material, out var part)) byMaterial[p.Material] = part = new();
        var first = part.Vertices.Count;
        for (var v = 0; v < p.Positions.Length; v++)
        {
            var (place, joint) = Placement(p, node, index, world, v);
            var normal = Normalize(Matrix.ApplyDirection(place, p.Normals[v]));
            var uvs = p.Uvs.Select(set => new[] { set[v][0], set[v][1] }).ToList();
            part.Vertices.Add(new(Turn(Matrix.Apply(place, p.Positions[v])), Turn(normal), uvs, joint));
        }
        part.Triangles.AddRange(p.Triangles.Select(t => first + t));
    }

    static (double[] Place, int Joint) Placement(Gltf.Primitive p, Gltf.Node node, int index, double[][] world, int v)
    {
        if (node.Skin is not { } skin || p.Joints == null || p.Weights == null) return (world[index], index + 1);
        var w = p.Weights[v];
        var k = Enumerable.Range(0, 4).MaxBy(c => w[c]);
        var slot = p.Joints[v][k];
        if (slot < 0 || slot >= skin.Joints.Length) throw new InvalidDataException($"node {node.Name}: a vertex names skin joint {slot}, which the skin lacks.");
        var j = skin.Joints[slot];
        return (Matrix.Multiply(world[j], ColumnMajor(skin.InverseBind[slot])), j + 1);
    }

    static double[] ColumnMajor(double[] m) => [m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]];

    static double[] Normalize(double[] v)
    {
        var l = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return l < 1e-12 ? [0, 0, 1] : [v[0] / l, v[1] / l, v[2] / l];
    }

    // One mesh per LOD and kind, its parts by material; a LOD's missing shadow caster is its render mesh (not the last's).
    static List<UnitWriter.Mesh> Meshes(Dictionary<(int Lod, bool Shadow), Dictionary<string, PartBuilder>> parts)
    {
        var lods = parts.Keys.Max(k => k.Lod) + 1;
        var meshes = new List<UnitWriter.Mesh>();
        for (var l = 0; l < lods; l++)
        {
            if (!parts.TryGetValue((l, false), out var render)) throw new InvalidDataException($"the model has no meshes for LOD {l} (names ending _lod{l}).");
            meshes.Add(Mesh($"lod{l}", l, false, render));
            if (parts.TryGetValue((l, true), out var shadow)) meshes.Add(Mesh($"lod{l}_shadow", l, true, shadow));
            else if (l < lods - 1 || lods == 1) meshes.Add(Mesh($"lod{l}_shadow", l, true, render));
        }
        return meshes;
    }

    static UnitWriter.Mesh Mesh(string name, int lod, bool shadow, Dictionary<string, PartBuilder> parts) =>
        new(name, lod, shadow, parts.Select(p => new UnitWriter.Part(p.Key, p.Value.Vertices, p.Value.Triangles)).ToList());
}
