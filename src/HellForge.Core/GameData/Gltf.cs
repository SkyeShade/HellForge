using System.Buffers.Binary;
using System.Text.Json;

namespace HellForge.Core.GameData;

// A glTF 2.0 model (.glb, or .gltf with its buffers beside it or inline), as Blender and most tools export it: its node
// tree with each node's local transform, and the triangles of the nodes that have meshes. Only what a unit can hold is
// read; sparse accessors, morph targets and non-triangle primitives are refused.
public static class Gltf
{
    public sealed record Node(string Name, int Parent, double[] Translation, double[] Rotation, double[] Scale, Primitive[] Primitives, Skin? Skin);
    // A primitive's vertices (positions, normals, up to three UV sets; joints/weights when skinned), triangles and material.
    public sealed record Primitive(string Material, double[][] Positions, double[][] Normals, double[][][] Uvs, int[][]? Joints, double[][]? Weights, int[] Triangles);
    // A skin: its joints as node indices, and each joint's inverse bind matrix (16 values, column-major as glTF stores them).
    public sealed record Skin(int[] Joints, double[][] InverseBind);

    // The nodes of the default scene, parents before their children (Parent -1 for a scene root).
    public static List<Node> Read(byte[] file, Func<string, byte[]> external)
    {
        var (json, bin) = Split(file);
        using var doc = JsonDocument.Parse(json);
        var g = new Reader(doc.RootElement, bin, external);
        return g.Nodes();
    }

    // The files a .gltf's buffers name (not inline data), as written in it.
    public static List<string> ExternalFiles(byte[] file)
    {
        var (json, _) = Split(file);
        using var doc = JsonDocument.Parse(json);
        if (!doc.RootElement.TryGetProperty("buffers", out var buffers) || buffers.ValueKind != JsonValueKind.Array) return [];
        return buffers.EnumerateArray().Where(b => b.TryGetProperty("uri", out var u) && !u.GetString()!.StartsWith("data:", StringComparison.Ordinal))
            .Select(b => Uri.UnescapeDataString(b.GetProperty("uri").GetString()!)).ToList();
    }

    static (byte[] Json, byte[]? Bin) Split(byte[] file)
    {
        if (file.Length < 12 || BinaryPrimitives.ReadUInt32LittleEndian(file) != 0x46546C67) return (file, null);
        if (BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(4)) != 2) throw new InvalidDataException("the model is not glTF 2.");
        byte[]? json = null, bin = null;
        for (var at = 12; at + 8 <= file.Length;)
        {
            var length = (int)BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(at));
            var type = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(at + 4));
            if (length < 0 || at + 8 + length > file.Length) throw new InvalidDataException("the .glb file ends early.");
            if (type == 0x4E4F534A) json ??= file[(at + 8)..(at + 8 + length)];
            else if (type == 0x004E4942) bin ??= file[(at + 8)..(at + 8 + length)];
            at += 8 + length;
        }
        return (json ?? throw new InvalidDataException("the .glb file has no JSON chunk."), bin);
    }

    sealed class Reader(JsonElement root, byte[]? bin, Func<string, byte[]> external)
    {
        readonly Dictionary<int, byte[]> buffers = [];

        public List<Node> Nodes()
        {
            var nodes = Array(root, "nodes");
            var scene = root.TryGetProperty("scene", out var s) ? s.GetInt32() : 0;
            var scenes = Array(root, "scenes");
            var roots = scenes.Count > 0 ? Ints(scenes[Math.Min(scene, scenes.Count - 1)], "nodes") : Enumerable.Range(0, nodes.Count).Where(i => !nodes.Any(n => Ints(n, "children").Contains(i))).ToArray();
            var order = new List<(int Index, int Parent)>();
            void Visit(int i, int parent)
            {
                if (i < 0 || i >= nodes.Count || order.Any(o => o.Index == i)) throw new InvalidDataException("the model's node tree is not a tree.");
                order.Add((i, parent));
                var self = order.Count - 1;
                foreach (var c in Ints(nodes[i], "children")) Visit(c, self);
            }
            foreach (var r in roots) Visit(r, -1);
            var position = order.Select((o, k) => (o.Index, k)).ToDictionary(x => x.Index, x => x.k);
            return order.Select(o => ReadNode(nodes[o.Index], o.Index, o.Parent, position)).ToList();
        }

        Node ReadNode(JsonElement n, int index, int parent, Dictionary<int, int> position)
        {
            var name = n.TryGetProperty("name", out var nm) && nm.GetString() is { Length: > 0 } text ? text : $"node{index}";
            var (t, r, s) = Transform(n, name);
            var primitives = n.TryGetProperty("mesh", out var m) ? Mesh(Array(root, "meshes")[m.GetInt32()], name) : [];
            Skin? skin = n.TryGetProperty("skin", out var sk) ? ReadSkin(Array(root, "skins")[sk.GetInt32()], position) : null;
            return new(name, parent, t, r, s, primitives, skin);
        }

        static (double[] T, double[] R, double[] S) Transform(JsonElement n, string name)
        {
            if (n.TryGetProperty("matrix", out var matrix)) return Decompose(matrix.EnumerateArray().Select(x => x.GetDouble()).ToArray(), name);
            return (Numbers(n, "translation", [0, 0, 0]), Numbers(n, "rotation", [0, 0, 0, 1]), Numbers(n, "scale", [1, 1, 1]));
        }

        // A node matrix (column-major) as translation, rotation and scale; one with shear is refused.
        static (double[] T, double[] R, double[] S) Decompose(double[] m, string name)
        {
            if (m.Length != 16) throw new InvalidDataException($"node {name}: a matrix has 16 numbers.");
            double[] Col(int c) => [m[4 * c], m[4 * c + 1], m[4 * c + 2]];
            var cols = new[] { Col(0), Col(1), Col(2) };
            var s = cols.Select(c => Math.Sqrt(c.Sum(v => v * v))).ToArray();
            if (s.Any(v => v < 1e-12)) throw new InvalidDataException($"node {name}: a matrix with a scale of 0.");
            var r = cols.Select((c, k) => c.Select(v => v / s[k]).ToArray()).ToArray();
            for (var a = 0; a < 3; a++) for (var b = a + 1; b < 3; b++)
                if (Math.Abs(r[a].Zip(r[b], (x, y) => x * y).Sum()) > 1e-4) throw new InvalidDataException($"node {name}: its matrix shears; use translation, rotation and scale.");
            return ([m[12], m[13], m[14]], Quaternion(r), s);
        }

        // A rotation's columns as a quaternion (x, y, z, w).
        static double[] Quaternion(double[][] c)
        {
            double M(int row, int col) => c[col][row];
            double tr = M(0, 0) + M(1, 1) + M(2, 2);
            if (tr > 0) { var s = Math.Sqrt(tr + 1) * 2; return [(M(2, 1) - M(1, 2)) / s, (M(0, 2) - M(2, 0)) / s, (M(1, 0) - M(0, 1)) / s, s / 4]; }
            if (M(0, 0) > M(1, 1) && M(0, 0) > M(2, 2)) { var s = Math.Sqrt(1 + M(0, 0) - M(1, 1) - M(2, 2)) * 2; return [s / 4, (M(0, 1) + M(1, 0)) / s, (M(0, 2) + M(2, 0)) / s, (M(2, 1) - M(1, 2)) / s]; }
            if (M(1, 1) > M(2, 2)) { var s = Math.Sqrt(1 + M(1, 1) - M(0, 0) - M(2, 2)) * 2; return [(M(0, 1) + M(1, 0)) / s, s / 4, (M(1, 2) + M(2, 1)) / s, (M(0, 2) - M(2, 0)) / s]; }
            var q = Math.Sqrt(1 + M(2, 2) - M(0, 0) - M(1, 1)) * 2;
            return [(M(0, 2) + M(2, 0)) / q, (M(1, 2) + M(2, 1)) / q, q / 4, (M(1, 0) - M(0, 1)) / q];
        }

        Primitive[] Mesh(JsonElement mesh, string node) =>
            Array(mesh, "primitives").Select(p => ReadPrimitive(p, node)).ToArray();

        Primitive ReadPrimitive(JsonElement p, string node)
        {
            if (p.TryGetProperty("mode", out var mode) && mode.GetInt32() != 4) throw new InvalidDataException($"node {node}: only triangles are read (primitive mode {mode.GetInt32()}).");
            if (p.TryGetProperty("targets", out _)) throw new InvalidDataException($"node {node}: morph targets are not read.");
            var a = p.GetProperty("attributes");
            var positions = Accessor(Required(a, "POSITION", node), 3);
            var normals = a.TryGetProperty("NORMAL", out var n) ? Accessor(n.GetInt32(), 3) : throw new InvalidDataException($"node {node}: its mesh has no normals; export them.");
            var uvs = Enumerable.Range(0, 3).TakeWhile(k => a.TryGetProperty($"TEXCOORD_{k}", out _)).Select(k => Accessor(a.GetProperty($"TEXCOORD_{k}").GetInt32(), 2)).ToArray();
            if (uvs.Length == 0) throw new InvalidDataException($"node {node}: its mesh has no UVs (TEXCOORD_0).");
            var joints = a.TryGetProperty("JOINTS_0", out var j) ? Accessor(j.GetInt32(), 4).Select(v => v.Select(x => (int)x).ToArray()).ToArray() : null;
            var weights = a.TryGetProperty("WEIGHTS_0", out var w) ? Accessor(w.GetInt32(), 4) : null;
            var triangles = p.TryGetProperty("indices", out var ix) ? Accessor(ix.GetInt32(), 1).Select(v => (int)v[0]).ToArray() : Enumerable.Range(0, positions.Length).ToArray();
            if (triangles.Length % 3 != 0 || triangles.Any(t => t < 0 || t >= positions.Length)) throw new InvalidDataException($"node {node}: its triangles do not index its vertices.");
            var material = p.TryGetProperty("material", out var mt) ? MaterialName(mt.GetInt32()) : throw new InvalidDataException($"node {node}: a primitive has no material.");
            return new(material, positions, normals, uvs, joints, weights, triangles);
        }

        string MaterialName(int i)
        {
            var m = Array(root, "materials")[i];
            return m.TryGetProperty("name", out var n) && n.GetString() is { Length: > 0 } name ? name : $"material{i}";
        }

        Skin ReadSkin(JsonElement s, Dictionary<int, int> position)
        {
            var joints = Ints(s, "joints").Select(j => position.TryGetValue(j, out var k) ? k : throw new InvalidDataException("a skin joint is not in the scene.")).ToArray();
            var inverse = s.TryGetProperty("inverseBindMatrices", out var ib) ? Accessor(ib.GetInt32(), 16)
                : joints.Select(_ => new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }).ToArray();
            return new(joints, inverse);
        }

        static int Required(JsonElement a, string name, string node) =>
            a.TryGetProperty(name, out var v) ? v.GetInt32() : throw new InvalidDataException($"node {node}: its mesh has no {name}.");

        // An accessor's elements as doubles (normalized integers scaled to 0..1).
        double[][] Accessor(int index, int size)
        {
            var a = Array(root, "accessors")[index];
            if (a.TryGetProperty("sparse", out _)) throw new InvalidDataException("sparse accessors are not read.");
            var count = a.GetProperty("count").GetInt32();
            var type = a.GetProperty("componentType").GetInt32();
            var width = ComponentSize(type);
            var normalized = a.TryGetProperty("normalized", out var nz) && nz.GetBoolean();
            var view = Array(root, "bufferViews")[a.GetProperty("bufferView").GetInt32()];
            var data = Buffer(view.GetProperty("buffer").GetInt32());
            var start = Int(view, "byteOffset") + Int(a, "byteOffset");
            var stride = view.TryGetProperty("byteStride", out var st) ? st.GetInt32() : width * size;
            if (count < 0 || start < 0 || (count > 0 && start + stride * (count - 1) + width * size > data.Length)) throw new InvalidDataException("an accessor reads past its buffer.");
            return Enumerable.Range(0, count).Select(e => Enumerable.Range(0, size).Select(c => Component(data, start + stride * e + width * c, type, normalized)).ToArray()).ToArray();
        }

        static int ComponentSize(int type) => type switch
        {
            5120 or 5121 => 1, 5122 or 5123 => 2, 5125 or 5126 => 4, _ => throw new InvalidDataException($"accessor component type {type} is not glTF's."),
        };

        static double Component(byte[] d, int at, int type, bool normalized) => type switch
        {
            5126 => BinaryPrimitives.ReadSingleLittleEndian(d.AsSpan(at)),
            5125 => BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at)),
            5123 => normalized ? BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at)) / 65535.0 : BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at)),
            5121 => normalized ? d[at] / 255.0 : d[at],
            5122 => normalized ? Math.Max(BinaryPrimitives.ReadInt16LittleEndian(d.AsSpan(at)) / 32767.0, -1) : BinaryPrimitives.ReadInt16LittleEndian(d.AsSpan(at)),
            _ => normalized ? Math.Max((sbyte)d[at] / 127.0, -1) : (sbyte)d[at],
        };

        byte[] Buffer(int index)
        {
            if (buffers.TryGetValue(index, out var cached)) return cached;
            var b = Array(root, "buffers")[index];
            byte[] data;
            if (!b.TryGetProperty("uri", out var uri)) data = bin ?? throw new InvalidDataException("the model's buffer is missing (a .gltf without its .bin?).");
            else if (uri.GetString()! is var u && u.StartsWith("data:", StringComparison.Ordinal)) data = Convert.FromBase64String(u[(u.IndexOf(',') + 1)..]);
            else data = external(Uri.UnescapeDataString(u));
            return buffers[index] = data;
        }

        static List<JsonElement> Array(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray().ToList() : [];

        static int[] Ints(JsonElement e, string name) => Array(e, name).Select(x => x.GetInt32()).ToArray();

        static int Int(JsonElement e, string name) => e.TryGetProperty(name, out var v) ? v.GetInt32() : 0;

        static double[] Numbers(JsonElement e, string name, double[] fallback) =>
            e.TryGetProperty(name, out var v) ? v.EnumerateArray().Select(x => x.GetDouble()).ToArray() : fallback;
    }
}
