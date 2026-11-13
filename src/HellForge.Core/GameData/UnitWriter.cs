using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// A unit written whole from a model of its own: its joints, its meshes per LOD and the materials they use. Each vertex is
// rigid on one joint, as on the game's weapons, so a clip that moves a joint moves the geometry on it. The layout is the
// one every weapon unit of the supported build has (header, LOD groups, joints, skeleton maps, vertex layouts, meshes,
// materials, mesh count, then vertex and index buffers), with the fields that are constant in all of them as constants.
public static class UnitWriter
{
    // A joint: its parent (an earlier joint; the root is joint 0 and its own parent) and local transform.
    public sealed record Joint(string Name, int Parent, double[] Translation, double[] Rotation, double[] Scale);
    // A vertex in its mesh joint's space, with up to three UV sets, rigid on a joint.
    public sealed record Vertex(double[] Position, double[] Normal, IReadOnlyList<double[]> Uvs, int Joint);
    // One material's triangles (indices into Vertices).
    public sealed record Part(string Material, IReadOnlyList<Vertex> Vertices, IReadOnlyList<int> Triangles);
    // A mesh drawn at one LOD (Shadow: the shadow caster), placed by its joint.
    public sealed record Mesh(string Name, int Lod, bool Shadow, IReadOnlyList<Part> Parts, int Joint = 0);
    // LodSizes: where each LOD gives way to the next, as the share of the screen the unit covers (1 > a > b > ...).
    public sealed record Model(IReadOnlyList<Joint> Joints, IReadOnlyList<Mesh> Meshes, IReadOnlyDictionary<string, ulong> Materials,
        IReadOnlyList<double> LodSizes, ulong Bones = 0, ulong StateMachine = 0);

    // The LOD sizes most of the game's weapons use.
    public static readonly double[] WeaponLodSizes = [1.0, 0.486, 0.243, 0.097, 0.049];

    const uint RenderMesh = 9, ShadowMesh = 10;
    const int LayoutSize = 0x1C0;
    // Render meshes: position, normal and tangent, three UV sets, weight, joint index; shadow meshes one UV set.
    static readonly (uint Type, uint Format, uint Layer)[] RenderItems = [(0, 2, 0), (1, 30, 0), (4, 33, 0), (4, 33, 1), (4, 33, 2), (7, 0, 0), (6, 28, 0)];
    static readonly (uint Type, uint Format, uint Layer)[] ShadowItems = [(0, 2, 0), (1, 30, 0), (4, 33, 0), (7, 0, 0), (6, 28, 0)];

    public static (byte[] Main, byte[] Gpu) Write(Model model)
    {
        Check(model);
        var world = World(model.Joints);
        var meshes = model.Meshes.OrderBy(m => m.Lod).ThenBy(m => m.Shadow).ToList();
        var slots = model.Materials.Keys.OrderBy(EngineHash.Id32).ToList();
        var maps = meshes.Select(m => SkeletonMap(m, world)).ToList();
        var gpu = new Writer();
        var layouts = new[] { false, true }.Where(shadow => meshes.Any(m => m.Shadow == shadow))
            .Select(shadow => VertexLayout(meshes, maps, shadow, gpu)).ToList();

        var main = new Writer();
        main.Bytes(new byte[0x80]);
        main.Section(0x3C, w => w.Bytes(new byte[16]));
        main.Section(0x38, w => w.Bytes(new byte[16]));
        main.Section(0x30, w => WriteLods(w, model, meshes));
        main.Section(0x34, w => WriteJoints(w, model.Joints, world));
        main.Section(0x58, w => WriteSkeletonMaps(w, maps));
        main.Section(0x5C, w => WriteLayouts(w, layouts, meshes));
        main.Section(0x64, w => WriteMeshes(w, meshes, layouts));
        main.Section(0x70, w => WriteMaterials(w, slots, model.Materials));
        main.Section(0x60, w => w.U32((uint)meshes.Count));
        main.Patch64(0x08, model.Bones);
        main.Patch64(0x20, model.StateMachine);
        main.Patch(0x28, 2);
        main.Patch(0x2C, 0xA4CD36);
        return (main.Done(), gpu.Bytes());
    }

    static void Check(Model model)
    {
        var joints = model.Joints;
        if (joints.Count is 0 or > 4096 || joints[0].Parent != 0) throw new InvalidDataException("a unit has 1 to 4096 joints, the first its root.");
        for (var i = 1; i < joints.Count; i++)
            if (joints[i].Parent < 0 || joints[i].Parent >= i) throw new InvalidDataException($"joint {joints[i].Name}'s parent comes after it.");
        if (joints.Select(j => EngineHash.Id32(j.Name)).Distinct().Count() != joints.Count) throw new InvalidDataException("two joints have the same name.");
        if (model.Meshes.Count == 0) throw new InvalidDataException("a unit has at least one mesh.");
        var lods = model.Meshes.Max(m => m.Lod) + 1;
        if (model.Meshes.Any(m => m.Lod < 0) || Enumerable.Range(0, lods).Any(l => !model.Meshes.Any(m => m.Lod == l && !m.Shadow)))
            throw new InvalidDataException("every LOD from 0 up has a mesh.");
        if (model.LodSizes.Count < lods || model.LodSizes.Zip(model.LodSizes.Skip(1)).Any(p => p.Second >= p.First) || model.LodSizes.Any(s => s <= 0))
            throw new InvalidDataException($"the LOD sizes are {lods} positive numbers, each smaller than the one before.");
        foreach (var mesh in model.Meshes) CheckMesh(mesh, model);
    }

    static void CheckMesh(Mesh mesh, Model model)
    {
        if (mesh.Joint < 0 || mesh.Joint >= model.Joints.Count) throw new InvalidDataException($"mesh {mesh.Name}'s joint is not one of the unit's.");
        if (mesh.Parts.Count == 0) throw new InvalidDataException($"mesh {mesh.Name} has no triangles.");
        foreach (var part in mesh.Parts)
        {
            if (!model.Materials.ContainsKey(part.Material)) throw new InvalidDataException($"mesh {mesh.Name} uses material {part.Material}, which has no resource.");
            if (part.Triangles.Count == 0 || part.Triangles.Count % 3 != 0 || part.Triangles.Any(t => t < 0 || t >= part.Vertices.Count))
                throw new InvalidDataException($"mesh {mesh.Name}'s {part.Material} triangles do not index its vertices.");
            if (part.Vertices.Any(v => v.Joint < 0 || v.Joint >= model.Joints.Count || v.Uvs.Count is 0 or > 3))
                throw new InvalidDataException($"mesh {mesh.Name} has a vertex on a joint the unit lacks, or without 1 to 3 UV sets.");
        }
    }

    // Each joint's world matrix (row-major 4x4, column vectors): its parent's times its own.
    static double[][] World(IReadOnlyList<Joint> joints)
    {
        var world = new double[joints.Count][];
        for (var i = 0; i < joints.Count; i++)
        {
            var local = Matrix.Compose(joints[i].Translation, joints[i].Rotation, joints[i].Scale);
            world[i] = i == 0 ? local : Matrix.Multiply(world[joints[i].Parent], local);
        }
        return world;
    }

    // A mesh's skeleton map: the joints its vertices are on, each with its inverse bind (the joint's world inverted, times
    // the mesh joint's world), and per part the list its vertices index.
    sealed record Map(List<int> Joints, List<double[]> Inverse, List<List<int>> Remaps);

    static Map SkeletonMap(Mesh mesh, double[][] world)
    {
        var joints = mesh.Parts.SelectMany(p => p.Vertices.Select(v => v.Joint)).Distinct().ToList();
        var remaps = mesh.Parts.Select(p => p.Vertices.Select(v => joints.IndexOf(v.Joint)).Distinct().Order().ToList()).ToList();
        if (remaps.Any(r => r.Count > 256)) throw new InvalidDataException($"mesh {mesh.Name} has a material on more than 256 joints.");
        return new(joints, joints.Select(j => Matrix.Multiply(Matrix.Invert(world[j]), world[mesh.Joint])).ToList(), remaps);
    }

    sealed record Layout(bool Shadow, (uint Type, uint Format, uint Layer)[] Items, uint Stride, uint Vertices, uint Indices, bool Wide,
        uint VbOffset, uint VbSize, uint IbOffset, uint IbSize, Dictionary<int, (uint First, uint Count, List<(uint At, uint Count)> Parts)> Ranges);

    // One vertex layout for all render meshes (or all shadow meshes), their vertices and indices into the GPU part.
    static Layout VertexLayout(List<Mesh> meshes, List<Map> maps, bool shadow, Writer gpu)
    {
        var items = shadow ? ShadowItems : RenderItems;
        var stride = (uint)(shadow ? 28 : 36);
        var own = Enumerable.Range(0, meshes.Count).Where(i => meshes[i].Shadow == shadow).ToList();
        var wide = own.Any(i => meshes[i].Parts.Sum(p => p.Vertices.Count) > 0xFFFF);
        gpu.Align(16);
        var vb = (uint)gpu.Pos;
        var ranges = new Dictionary<int, (uint, uint, List<(uint, uint)>)>();
        uint vertices = 0;
        foreach (var i in own)
        {
            var count = (uint)meshes[i].Parts.Sum(p => p.Vertices.Count);
            foreach (var (part, remap) in meshes[i].Parts.Zip(maps[i].Remaps))
                WriteVertices(gpu, part, remap, maps[i].Joints, shadow);
            ranges[i] = (vertices, count, []);
            vertices += count;
        }
        var vbSize = (uint)gpu.Pos - vb;
        gpu.Align(16);
        var ib = (uint)gpu.Pos;
        uint indices = 0;
        foreach (var i in own) indices = WriteIndices(gpu, meshes[i], wide, indices, ranges[i].Item3);
        return new(shadow, items, stride, vertices, indices, wide, vb, vbSize, ib, (uint)gpu.Pos - ib, ranges);
    }

    // A mesh's indices, relative to its first vertex, each part's range noted; returns the layout's index count after it.
    static uint WriteIndices(Writer gpu, Mesh mesh, bool wide, uint indices, List<(uint At, uint Count)> ranges)
    {
        uint first = 0;
        foreach (var part in mesh.Parts)
        {
            foreach (var t in part.Triangles)
            {
                if (wide) gpu.U32((uint)(first + t));
                else gpu.U16((ushort)(first + t));
            }
            ranges.Add((indices, (uint)part.Triangles.Count));
            indices += (uint)part.Triangles.Count;
            first += (uint)part.Vertices.Count;
        }
        return indices;
    }

    static void WriteVertices(Writer gpu, Part part, List<int> remap, List<int> joints, bool shadow)
    {
        var tangents = UnitMesh.Tangents(new(part.Vertices.Select(v => v.Position).ToList(), part.Vertices.Select(v => v.Normal).ToList(),
            part.Vertices.Select(v => v.Uvs[0]).ToList(), part.Triangles));
        for (var v = 0; v < part.Vertices.Count; v++)
        {
            var x = part.Vertices[v];
            foreach (var c in x.Position) gpu.F32(c);
            gpu.U32(UnitMesh.PackNormal(x.Normal, tangents[v].T, tangents[v].Sign));
            for (var k = 0; k < (shadow ? 1 : 3); k++)
            {
                var uv = x.Uvs[Math.Min(k, x.Uvs.Count - 1)];
                gpu.Half(uv[0]); gpu.Half(uv[1]);
            }
            gpu.F32(1);
            gpu.U32((uint)remap.IndexOf(joints.IndexOf(x.Joint)));
        }
    }

    // LOD group "lod" (flags 3): per LOD its size range and meshes; one footer with the render meshes' box in unit space.
    static void WriteLods(Writer w, Model model, List<Mesh> meshes)
    {
        var lods = meshes.Max(m => m.Lod) + 1;
        w.U32(1); w.U32(8);
        var at = w.Pos;
        w.U32(3); w.U32(EngineHash.Id32("lod")); w.U32(0); w.U32(1); w.U32((uint)lods);
        var table = w.Pos;
        for (var k = 0; k <= lods; k++) w.U32(0);
        for (var l = 0; l < lods; l++)
        {
            w.Patch(table + 4 * l, (uint)(w.Pos - at));
            var own = Enumerable.Range(0, meshes.Count).Where(i => meshes[i].Lod == l).ToList();
            w.F32(model.LodSizes[l]); w.F32(l + 1 < lods ? model.LodSizes[l + 1] : 0);
            w.U32((uint)own.Count);
            foreach (var i in own) w.U32((uint)i);
        }
        w.Patch(table + 4 * lods, (uint)(w.Pos - at));
        var (lo, hi) = Box(meshes.Where(m => !m.Shadow).SelectMany(m => UnitSpace(m, model)));
        foreach (var c in lo.Concat(hi)) w.F32(c);
        w.F32(Distance(lo, hi) / 2); w.U32(0); w.F32(1); w.U32(0); w.U32(uint.MaxValue);
    }

    static IEnumerable<double[]> UnitSpace(Mesh mesh, Model model)
    {
        var place = World(model.Joints)[mesh.Joint];
        return mesh.Parts.SelectMany(p => p.Vertices).Select(v => Matrix.Apply(place, v.Position));
    }

    static void WriteJoints(Writer w, IReadOnlyList<Joint> joints, double[][] world)
    {
        w.U32((uint)joints.Count); w.Bytes(new byte[12]);
        foreach (var j in joints)
        {
            w.ColumnMajor(Matrix.Rotation(j.Rotation), 3);
            foreach (var v in j.Translation.Concat(j.Scale)) w.F32(v);
            w.F32(0);
        }
        foreach (var m in world) w.ColumnMajor(m, 4);
        for (var i = 0; i < joints.Count; i++) { w.U16((ushort)(i == 0 ? 0 : 1)); w.U16((ushort)joints[i].Parent); }
        foreach (var j in joints) w.U32(EngineHash.Id32(j.Name));
    }

    static void WriteSkeletonMaps(Writer w, List<Map> maps)
    {
        var section = w.Pos;
        w.U32((uint)maps.Count);
        var table = w.Pos;
        foreach (var _ in maps) w.U32(0);
        for (var i = 0; i < maps.Count; i++)
        {
            w.Patch(table + 4 * i, (uint)(w.Pos - section));
            WriteMap(w, maps[i]);
        }
    }

    // {count, matrices at 16, joints after, remaps after}, then the remap lists' offsets and counts and their entries.
    static void WriteMap(Writer w, Map map)
    {
        var n = map.Joints.Count;
        w.U32((uint)n); w.U32(16); w.U32((uint)(16 + 64 * n)); w.U32((uint)(16 + 68 * n));
        foreach (var m in map.Inverse) w.ColumnMajor(m, 4);
        foreach (var j in map.Joints) w.U32((uint)j);
        w.U32((uint)map.Remaps.Count);
        var data = 4 + 8 * map.Remaps.Count;
        foreach (var r in map.Remaps) { w.U32((uint)data); w.U32((uint)r.Count); data += 4 * r.Count; }
        foreach (var e in map.Remaps.SelectMany(r => r)) w.U32((uint)e);
    }

    static void WriteLayouts(Writer w, List<Layout> layouts, List<Mesh> meshes)
    {
        var section = w.Pos;
        w.U32((uint)layouts.Count);
        var table = w.Pos;
        foreach (var _ in layouts) w.U32(0);
        foreach (var l in layouts) w.U32(EngineHash.Id32(meshes[l.Ranges.Keys.Min()].Name));
        for (var i = 0; i < layouts.Count; i++)
        {
            w.Align(i == 0 ? 8 : 16);
            w.Patch(table + 4 * i, (uint)(w.Pos - section));
            WriteLayout(w, layouts[i]);
        }
    }

    static void WriteLayout(Writer w, Layout l)
    {
        var at = w.Pos;
        w.Bytes(new byte[LayoutSize]);
        w.Patch(at, 0x0AFFFFFF);
        for (var k = 0; k < l.Items.Length; k++) { w.Patch(at + 8 + 20 * k, l.Items[k].Type); w.Patch(at + 12 + 20 * k, l.Items[k].Format); w.Patch(at + 16 + 20 * k, l.Items[k].Layer); }
        w.Patch(at + 328, (uint)l.Items.Length);
        w.Patch(at + 336, 0x06FFFFFF); w.Patch(at + 344, 1);
        w.Patch(at + 352, l.Vertices); w.Patch(at + 356, l.Stride); w.Patch(at + 360, 1);
        w.Patch(at + 376, 0x07FFFFFF); w.Patch(at + 384, 1);
        w.Patch(at + 392, l.Indices); w.Patch(at + 396, l.Wide ? 1u : 0u); w.Patch(at + 400, 1);
        w.Patch(at + 416, l.VbOffset); w.Patch(at + 420, l.VbSize); w.Patch(at + 424, l.IbOffset); w.Patch(at + 428, l.IbSize);
    }

    static void WriteMeshes(Writer w, List<Mesh> meshes, List<Layout> layouts)
    {
        var section = w.Pos;
        w.U32((uint)meshes.Count);
        var table = w.Pos;
        foreach (var _ in meshes) w.U32(0);
        foreach (var m in meshes) w.U32(EngineHash.Id32(m.Name));
        for (var i = 0; i < meshes.Count; i++)
        {
            w.Patch(table + 4 * i, (uint)(w.Pos - section));
            var layout = layouts.FindIndex(l => l.Ranges.ContainsKey(i));
            WriteMesh(w, meshes[i], i, layouts[layout], layout);
        }
    }

    static void WriteMesh(Writer w, Mesh mesh, int index, Layout layout, int layoutIndex)
    {
        var at = w.Pos;
        var materials = mesh.Parts.Select(p => p.Material).Distinct().ToList();
        var (lo, hi) = Box(mesh.Parts.SelectMany(p => p.Vertices).Select(v => v.Position));
        w.Bytes(new byte[0x80]);
        for (var k = 0; k < 3; k++) { w.PatchF(at + 8 + 4 * k, lo[k]); w.PatchF(at + 20 + 4 * k, hi[k]); }
        w.PatchF(at + 0x20, mesh.Parts.SelectMany(p => p.Vertices).Max(v => Distance([0, 0, 0], v.Position)));
        w.Patch(at + 0x24, mesh.Shadow ? ShadowMesh : RenderMesh);
        w.Patch(at + 0x28, EngineHash.Id32(mesh.Name));
        w.Patch(at + 0x2C, (uint)mesh.Joint); w.Patch(at + 0x30, (uint)mesh.Joint);
        w.Patch(at + 0x38, (uint)index); w.Patch(at + 0x3C, (uint)layoutIndex);
        w.Patch(at + 0x68, (uint)materials.Count); w.Patch(at + 0x6C, 0x80);
        w.Patch(at + 0x78, (uint)mesh.Parts.Count); w.Patch(at + 0x7C, (uint)(0x80 + 4 * materials.Count));
        foreach (var m in materials) w.U32(EngineHash.Id32(m));
        var (first, count, parts) = layout.Ranges[index];
        for (var g = 0; g < mesh.Parts.Count; g++)
        {
            w.U32((uint)materials.IndexOf(mesh.Parts[g].Material)); w.U32(first); w.U32(count);
            w.U32(parts[g].At); w.U32(parts[g].Count); w.U32((uint)g);
        }
    }

    static void WriteMaterials(Writer w, List<string> slots, IReadOnlyDictionary<string, ulong> resources)
    {
        w.U32((uint)slots.Count);
        foreach (var s in slots) w.U32(EngineHash.Id32(s));
        foreach (var s in slots) w.U64(resources[s]);
    }

    static (double[] Lo, double[] Hi) Box(IEnumerable<double[]> points)
    {
        double[] lo = [double.MaxValue, double.MaxValue, double.MaxValue], hi = [double.MinValue, double.MinValue, double.MinValue];
        foreach (var p in points) for (var k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); }
        return (lo, hi);
    }

    static double Distance(double[] a, double[] b) => Math.Sqrt(Enumerable.Range(0, 3).Sum(k => (a[k] - b[k]) * (a[k] - b[k])));

    // Little-endian output with 32-bit patches; a section is written 16-aligned and its offset set in the header.
    sealed class Writer
    {
        readonly MemoryStream s = new();
        public int Pos => (int)s.Length;
        public void Bytes(ReadOnlySpan<byte> b) => s.Write(b);
        public void U16(ushort v) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteUInt16LittleEndian(b, v); s.Write(b); }
        public void U32(uint v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, v); s.Write(b); }
        public void U64(ulong v) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(b, v); s.Write(b); }
        public void F32(double v) => U32(BitConverter.SingleToUInt32Bits((float)v));
        public void Half(double v) => U16(BitConverter.HalfToUInt16Bits((Half)v));
        public void Align(int a) { while (s.Length % a != 0) s.WriteByte(0); }
        // An n x n row-major matrix column by column (the unit stores rotations and matrices so).
        public void ColumnMajor(double[] m, int n)
        {
            for (var c = 0; c < n; c++)
                for (var row = 0; row < n; row++) F32(m[n * row + c]);
        }
        public void Patch(int at, uint v) { var p = s.Position; s.Position = at; U32(v); s.Position = p; }
        public void PatchF(int at, double v) => Patch(at, BitConverter.SingleToUInt32Bits((float)v));
        public void Patch64(int at, ulong v) { Patch(at, (uint)v); Patch(at + 4, (uint)(v >> 32)); }
        public void Section(int header, Action<Writer> write)
        {
            var at = Pos;
            write(this);
            Patch(header, (uint)at);
            Align(16);
        }
        public byte[] Bytes() => s.ToArray();
        public byte[] Done() { Align(16); return s.ToArray(); }
    }
}

// 4x4 matrices as 16 doubles, row-major, acting on column vectors (translation in the last column).
static class Matrix
{
    public static double[] Rotation(double[] q)
    {
        var l = Math.Sqrt(q.Sum(v => v * v));
        if (l < 1e-12) throw new InvalidDataException("a rotation is not a quaternion.");
        double x = q[0] / l, y = q[1] / l, z = q[2] / l, w = q[3] / l;
        return [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)];
    }

    public static double[] Compose(double[] t, double[] q, double[] s)
    {
        var r = Rotation(q);
        return [r[0] * s[0], r[1] * s[1], r[2] * s[2], t[0], r[3] * s[0], r[4] * s[1], r[5] * s[2], t[1], r[6] * s[0], r[7] * s[1], r[8] * s[2], t[2], 0, 0, 0, 1];
    }

    public static double[] Multiply(double[] a, double[] b)
    {
        var m = new double[16];
        for (var r = 0; r < 4; r++) for (var c = 0; c < 4; c++) for (var k = 0; k < 4; k++) m[4 * r + c] += a[4 * r + k] * b[4 * k + c];
        return m;
    }

    public static double[] Apply(double[] m, double[] p) =>
        [m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7], m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]];

    // A normal through m: by the inverse transpose of its 3x3, so it stays at right angles under a non-uniform scale.
    public static double[] ApplyDirection(double[] m, double[] n)
    {
        var i = Invert(m);
        return [i[0] * n[0] + i[4] * n[1] + i[8] * n[2], i[1] * n[0] + i[5] * n[1] + i[9] * n[2], i[2] * n[0] + i[6] * n[1] + i[10] * n[2]];
    }

    // Gauss-Jordan with partial pivoting.
    public static double[] Invert(double[] m)
    {
        var a = new double[4, 8];
        for (var r = 0; r < 4; r++) { for (var c = 0; c < 4; c++) a[r, c] = m[4 * r + c]; a[r, 4 + r] = 1; }
        for (var c = 0; c < 4; c++) Eliminate(a, c);
        var inv = new double[16];
        for (var r = 0; r < 4; r++) for (var c = 0; c < 4; c++) inv[4 * r + c] = a[r, 4 + c];
        return inv;
    }

    // Column c: the largest pivot row swapped in, scaled to 1, and c cleared from the other rows.
    static void Eliminate(double[,] a, int c)
    {
        var p = Enumerable.Range(c, 4 - c).MaxBy(r => Math.Abs(a[r, c]));
        if (Math.Abs(a[p, c]) < 1e-12) throw new InvalidDataException("a joint's transform cannot be inverted (a scale of 0).");
        for (var k = 0; k < 8; k++) (a[c, k], a[p, k]) = (a[p, k], a[c, k]);
        var pivot = a[c, c];
        for (var k = 0; k < 8; k++) a[c, k] /= pivot;
        for (var r = 0; r < 4; r++)
        {
            if (r == c) continue;
            var f = a[r, c];
            for (var k = 0; k < 8; k++) a[r, k] -= f * a[c, k];
        }
    }
}
