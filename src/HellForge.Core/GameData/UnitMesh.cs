using System.Globalization;

namespace HellForge.Core.GameData;

// A mod's own mesh in a copy of one of the game's models (a data spec's "mesh" on a unit copy): every render and shadow
// mesh of the copied unit, in every LOD, gets the mod's geometry, encoded in each vertex stream's own layout; the unit
// keeps its joints, skeleton, materials, LOD entries and mesh and group counts, so nothing else in it moves. The mesh is
// rigid on the unit's first bone (bone index 0 -> the skeleton map's first joint: a weapon's main bone), and the boxes and
// radii the engine culls with are rebuilt around it. The layout is HellForge's research (artifacts/models/FORMAT.md in
// the research repository), checked against every unit of the supported build; what it does not cover is refused, never
// guessed: units whose geometry lives in another resource, data after the vertex streams, rotated mesh joints, meshes
// culled by another body (a sentry's c_culling), vertex formats without an encoder. Physics, hitboxes and attachment
// points stay the base's: only what is drawn changes.
public static class UnitMesh
{
    public sealed record Mesh(IReadOnlyList<double[]> Positions, IReadOnlyList<double[]> Normals, IReadOnlyList<double[]> Uvs, IReadOnlyList<int> Triangles);

    const int RenderMesh = 0x9, ShadowMesh = 0xA;

    sealed record Item(uint Type, uint Format, uint Layer);
    sealed record Stream(int At, IReadOnlyList<Item> Items, uint Vertices, uint Stride, uint Indices, uint IndexType, uint VbOffset, uint VbSize, uint IbOffset, uint IbSize);
    sealed record Group(uint Material, uint VertexOffset, uint Vertices, uint IndexOffset, uint IndexCount, uint Id);
    sealed record UnitMeshRecord(int At, uint Type, uint AabbJoint, uint Joint, int Stream, IReadOnlyList<Group> Groups, int GroupsAt);
    // The copied unit as Replace rewrites it: its main part (written in place), its GPU part, its meshes and the ones that
    // get the mod's geometry.
    sealed record Model(byte[] Main, byte[] Gpu, List<UnitMeshRecord> Meshes, HashSet<int> Replaced);
    // The mod's geometry as each replaced mesh gets it: with its tangents, its box and its radius.
    sealed record Geometry(Mesh Mesh, List<(double[] T, int Sign)> Tangents, double[] Lo, double[] Hi, double Radius);
    // A mesh's vertices and its groups' index lists, as they go into the rewritten stream.
    sealed record Block(int Mesh, byte[] Vertices, int Count, List<List<uint>> Lists);

    static uint U32(byte[] b, int at) => BitConverter.ToUInt32(b, at);
    static int I32(byte[] b, int at) => BitConverter.ToInt32(b, at);
    static float F32(byte[] b, int at) => BitConverter.ToSingle(b, at);

    // Format id -> size in bytes (FORMAT.md's vertex item formats).
    static int FormatSize(uint format) => format switch
    {
        0 or 4 or 21 or 28 or 29 or 30 => 4, 1 or 22 => 8, 2 or 23 => 12, 3 or 24 => 16, 25 => 1, 26 or 32 => 2, 27 => 3,
        33 => 4, 34 => 6, 35 => 8,
        _ => throw new InvalidDataException($"vertex format {format} is not one HellForge knows.")
    };

    public static (byte[] Main, byte[] Gpu) Replace(byte[] mainIn, byte[] gpu, Mesh mesh)
    {
        if (mesh.Positions.Count == 0 || mesh.Triangles.Count == 0) throw new InvalidDataException("the mesh has no triangles.");
        var main = mainIn.ToArray();
        if (BitConverter.ToUInt64(main, 0x10) != 0) throw new InvalidDataException("this model keeps its geometry in another resource; HellForge cannot replace it yet.");
        var streams = Streams(main);
        var meshes = Meshes(main);
        var joints = JointTranslations(main);
        if (streams.Count == 0 || meshes.Count == 0) throw new InvalidDataException("the model has no meshes.");
        var end = streams.Max(s => (long)s.IbOffset + s.IbSize);
        if ((end + 15) / 16 * 16 < gpu.Length) throw new InvalidDataException("the model has GPU data after its vertex streams; HellForge cannot replace it yet.");

        var model = new Model(main, gpu, meshes, Replaced(meshes, joints));
        var geometry = Shape(mesh);
        var output = new MemoryStream();
        for (var si = 0; si < streams.Count; si++) WriteStream(model, streams[si], si, geometry, output);
        RebuildLodFooters(main, Meshes(main), joints);
        return (main, output.ToArray());
    }

    // The render and shadow meshes that get the mod's geometry, each culled by its own joint, which is not rotated or scaled.
    static HashSet<int> Replaced(List<UnitMeshRecord> meshes, List<(double[] T, bool Plain)> joints)
    {
        var replace = Enumerable.Range(0, meshes.Count).Where(i => meshes[i].Type is RenderMesh or ShadowMesh && meshes[i].Stream >= 0).ToHashSet();
        if (replace.Count == 0) throw new InvalidDataException("the model has no render meshes.");
        foreach (var i in replace)
        {
            if (meshes[i].AabbJoint != meshes[i].Joint) throw new InvalidDataException("the model is culled by a body of its own (a c_culling mesh); HellForge cannot replace it yet.");
            if (!joints[(int)meshes[i].Joint].Plain) throw new InvalidDataException("a mesh joint of the model is rotated or scaled; HellForge cannot replace it yet.");
        }
        return replace;
    }

    static Geometry Shape(Mesh mesh)
    {
        var tangents = Tangents(mesh);
        double[] lo = [double.MaxValue, double.MaxValue, double.MaxValue], hi = [double.MinValue, double.MinValue, double.MinValue];
        foreach (var p in mesh.Positions) for (var k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); }
        var radius = mesh.Positions.Max(p => Math.Sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
        return new(mesh, tangents, lo, hi, radius);
    }

    // One vertex stream rewritten into output: its meshes' vertices, then their indices (u16, or u32 when a block needs
    // it), with the stream's and its groups' counts and offsets set in the model's main part.
    static void WriteStream(Model model, Stream s, int si, Geometry geometry, MemoryStream output)
    {
        var main = model.Main;
        var blocks = StreamBlocks(model, s, si, geometry, ItemOffsets(s));
        var wide = blocks.Any(b => b.Count > 0xFFFF);
        Align(output);
        var vbOffset = (uint)output.Length;
        var first = new Dictionary<int, uint>();
        uint vertexCount = 0;
        foreach (var b in blocks) { first[b.Mesh] = vertexCount; output.Write(b.Vertices); vertexCount += (uint)b.Count; }
        var vbSize = (uint)output.Length - vbOffset;
        Align(output);
        var ibOffset = (uint)output.Length;
        uint indexCount = 0;
        foreach (var b in blocks)
        {
            var m = model.Meshes[b.Mesh];
            indexCount = WriteIndices(main, m, b, first[b.Mesh], indexCount, wide, output);
            if (model.Replaced.Contains(b.Mesh)) WriteBox(main, m.At, geometry);
        }
        var ibSize = (uint)output.Length - ibOffset;
        Write(main, s.At + 352, vertexCount);
        Write(main, s.At + 392, indexCount);
        Write(main, s.At + 396, wide ? 1u : 0u);
        Write(main, s.At + 416, vbOffset);
        Write(main, s.At + 420, vbSize);
        Write(main, s.At + 424, ibOffset);
        Write(main, s.At + 428, ibSize);
    }

    // Where each item of a stream's vertex starts; the items must fill its stride.
    static int[] ItemOffsets(Stream s)
    {
        var offsets = new int[s.Items.Count];
        var size = 0;
        for (var k = 0; k < s.Items.Count; k++) { offsets[k] = size; size += FormatSize(s.Items[k].Format); }
        if (size != s.Stride) throw new InvalidDataException($"a vertex stream's items take {size} bytes, not its stride {s.Stride}.");
        return offsets;
    }

    // The stream's meshes in vertex order: a replaced one with the mod's geometry, the others as they are.
    static List<Block> StreamBlocks(Model model, Stream s, int si, Geometry geometry, int[] offsets)
    {
        var meshes = model.Meshes;
        var own = Enumerable.Range(0, meshes.Count).Where(i => meshes[i].Stream == si).OrderBy(i => meshes[i].Groups[0].VertexOffset).ToList();
        var blocks = new List<Block>();
        foreach (var i in own)
            blocks.Add(model.Replaced.Contains(i) ? NewBlock(i, meshes[i], s, geometry, offsets) : OldBlock(i, meshes[i], s, model.Gpu));
        return blocks;
    }

    static Block NewBlock(int i, UnitMeshRecord m, Stream s, Geometry geometry, int[] offsets)
    {
        var mesh = geometry.Mesh;
        var vertices = new byte[mesh.Positions.Count * s.Stride];
        for (var v = 0; v < mesh.Positions.Count; v++)
            EncodeVertex(vertices.AsSpan(v * (int)s.Stride, (int)s.Stride), s.Items, offsets, mesh.Positions[v], mesh.Normals[v], geometry.Tangents[v], mesh.Uvs[v]);
        var lists = new List<List<uint>> { mesh.Triangles.Select(t => (uint)t).ToList() };
        for (var g = 1; g < m.Groups.Count; g++) lists.Add([0, 0, 0]); // a group left over: one degenerate triangle
        return new(i, vertices, mesh.Positions.Count, lists);
    }

    static Block OldBlock(int i, UnitMeshRecord m, Stream s, byte[] gpu)
    {
        var oldWide = s.IndexType == 1;
        var g0 = m.Groups[0];
        var from = (int)(s.VbOffset + g0.VertexOffset * s.Stride);
        var lists = m.Groups.Select(g => Enumerable.Range(0, (int)g.IndexCount).Select(q => oldWide
            ? U32(gpu, (int)(s.IbOffset + (g.IndexOffset + q) * 4)) : BitConverter.ToUInt16(gpu, (int)(s.IbOffset + (g.IndexOffset + q) * 2))).Select(x => (uint)x).ToList()).ToList();
        return new(i, gpu.AsSpan(from, (int)(g0.Vertices * s.Stride)).ToArray(), (int)g0.Vertices, lists);
    }

    // A block's groups: their vertex and index ranges set in main, their indices into output. Returns the stream's index
    // count after them.
    static uint WriteIndices(byte[] main, UnitMeshRecord m, Block b, uint firstVertex, uint indexCount, bool wide, MemoryStream output)
    {
        for (var g = 0; g < m.Groups.Count; g++)
        {
            var list = b.Lists[g];
            var at = m.GroupsAt + 24 * g;
            Write(main, at + 4, firstVertex);
            Write(main, at + 8, (uint)b.Count);
            Write(main, at + 12, indexCount);
            Write(main, at + 16, (uint)list.Count);
            foreach (var index in list)
                if (wide) output.Write(BitConverter.GetBytes(index)); else output.Write(BitConverter.GetBytes((ushort)index));
            indexCount += (uint)list.Count;
        }
        return indexCount;
    }

    // A replaced mesh's box and radius, the mod's geometry's.
    static void WriteBox(byte[] main, int at, Geometry geometry)
    {
        for (var k = 0; k < 3; k++) { WriteF(main, at + 8 + 4 * k, geometry.Lo[k]); WriteF(main, at + 20 + 4 * k, geometry.Hi[k]); }
        WriteF(main, at + 0x20, geometry.Radius);
    }

    static void Align(MemoryStream output) { while (output.Length % 16 != 0) output.WriteByte(0); }
    static void Write(byte[] b, int at, uint value) => BitConverter.TryWriteBytes(b.AsSpan(at, 4), value);
    static void WriteF(byte[] b, int at, double value) => BitConverter.TryWriteBytes(b.AsSpan(at, 4), (float)value);

    static List<Stream> Streams(byte[] m)
    {
        var list = new List<Stream>();
        var base0 = (int)U32(m, 0x5C);
        if (base0 == 0) return list;
        var n = (int)U32(m, base0);
        for (var i = 0; i < n; i++)
        {
            var at = base0 + (int)U32(m, base0 + 4 + 4 * i);
            var count = (int)U32(m, at + 328);
            if (count > 16) throw new InvalidDataException("a vertex stream lists more than 16 items.");
            var items = Enumerable.Range(0, count).Select(k => new Item(U32(m, at + 8 + 20 * k), U32(m, at + 12 + 20 * k), U32(m, at + 16 + 20 * k))).ToList();
            list.Add(new(at, items, U32(m, at + 352), U32(m, at + 356), U32(m, at + 392), U32(m, at + 396), U32(m, at + 416), U32(m, at + 420), U32(m, at + 424), U32(m, at + 428)));
        }
        return list;
    }

    static List<UnitMeshRecord> Meshes(byte[] m)
    {
        var list = new List<UnitMeshRecord>();
        var base0 = (int)U32(m, 0x64);
        if (base0 == 0) return list;
        var n = (int)U32(m, base0);
        for (var i = 0; i < n; i++)
        {
            var at = base0 + (int)U32(m, base0 + 4 + 4 * i);
            var groupCount = (int)U32(m, at + 0x78);
            var groupsAt = at + (int)U32(m, at + 0x7C);
            var groups = Enumerable.Range(0, groupCount).Select(g =>
            {
                var o = groupsAt + 24 * g;
                return new Group(U32(m, o), U32(m, o + 4), U32(m, o + 8), U32(m, o + 12), U32(m, o + 16), U32(m, o + 20));
            }).ToList();
            if (groups.Count == 0) throw new InvalidDataException("a mesh of the model has no groups.");
            list.Add(new(at, U32(m, at + 0x24), U32(m, at + 0x2C), U32(m, at + 0x30), I32(m, at + 0x3C), groups, groupsAt));
        }
        return list;
    }

    // Each joint's world translation, and whether its world matrix is a translation only (rotation and scale identity).
    static List<(double[] T, bool Plain)> JointTranslations(byte[] m)
    {
        var list = new List<(double[], bool)>();
        var base0 = (int)U32(m, 0x34);
        if (base0 == 0) return list;
        var n = (int)U32(m, base0);
        var world = base0 + 16 + 64 * n;
        for (var i = 0; i < n; i++)
        {
            var at = world + 64 * i;
            var plain = true;
            for (var k = 0; k < 12; k++)
                if (Math.Abs(F32(m, at + 4 * k) - (k is 0 or 5 or 10 ? 1.0 : 0.0)) > 1e-6) plain = false;
            if (Math.Abs(F32(m, at + 60) - 1.0) > 1e-6) plain = false;
            list.Add(([F32(m, at + 48), F32(m, at + 52), F32(m, at + 56)], plain));
        }
        return list;
    }

    // Each LOD group's footer: the union of its render meshes' boxes (in the unit's space: plus their joints' translation)
    // and half its diagonal, which the engine picks the LOD and culls with.
    static void RebuildLodFooters(byte[] m, List<UnitMeshRecord> meshes, List<(double[] T, bool Plain)> joints)
    {
        var base0 = (int)U32(m, 0x30);
        if (base0 == 0) return;
        var n = (int)U32(m, base0);
        for (var i = 0; i < n; i++)
        {
            var at = base0 + (int)U32(m, base0 + 4 + 4 * i);
            int footers = (int)U32(m, at + 12), entries = (int)U32(m, at + 16);
            if (LodBox(m, at, entries, meshes, joints) is not { } box) continue;
            var (lo, hi) = box;
            var half = Math.Sqrt(Enumerable.Range(0, 3).Sum(k => (hi[k] - lo[k]) * (hi[k] - lo[k]))) / 2;
            for (var f = 0; f < footers; f++)
            {
                var footer = at + (int)U32(m, at + 20 + 4 * entries + 4 * f);
                for (var k = 0; k < 3; k++) { WriteF(m, footer + 4 * k, lo[k]); WriteF(m, footer + 12 + 4 * k, hi[k]); }
                WriteF(m, footer + 24, half);
            }
        }
    }

    // The union of the boxes of a LOD group's render meshes (at, with entries mesh lists), each moved by its joint's
    // translation; null when it lists none.
    static (double[] Lo, double[] Hi)? LodBox(byte[] m, int at, int entries, List<UnitMeshRecord> meshes, List<(double[] T, bool Plain)> joints)
    {
        double[] lo = [double.MaxValue, double.MaxValue, double.MaxValue], hi = [double.MinValue, double.MinValue, double.MinValue];
        var any = false;
        for (var e = 0; e < entries; e++)
        {
            var entry = at + (int)U32(m, at + 20 + 4 * e);
            var count = (int)U32(m, entry + 8);
            for (var q = 0; q < count; q++)
            {
                var index = (int)U32(m, entry + 12 + 4 * q);
                if (index >= meshes.Count || meshes[index].Type != RenderMesh) continue;
                var me = meshes[index];
                var t = joints[(int)me.Joint].T;
                for (var k = 0; k < 3; k++)
                {
                    lo[k] = Math.Min(lo[k], F32(m, me.At + 8 + 4 * k) + t[k]);
                    hi[k] = Math.Max(hi[k], F32(m, me.At + 20 + 4 * k) + t[k]);
                }
                any = true;
            }
        }
        return any ? (lo, hi) : null;
    }

    static void EncodeVertex(Span<byte> out_, IReadOnlyList<Item> items, int[] offsets, double[] p, double[] n, (double[] T, int Sign) t, double[] uv)
    {
        for (var k = 0; k < items.Count; k++)
        {
            var (type, format) = (items[k].Type, items[k].Format);
            var at = out_[offsets[k]..];
            switch (type, format)
            {
                case (0, 2): for (var q = 0; q < 3; q++) BitConverter.TryWriteBytes(at[(4 * q)..], (float)p[q]); break;
                case (1, 30): BitConverter.TryWriteBytes(at, PackNormal(n, t.T, t.Sign)); break;
                case (4, 33): BitConverter.TryWriteBytes(at, (Half)uv[0]); BitConverter.TryWriteBytes(at[2..], (Half)uv[1]); break;
                case (4, 1): BitConverter.TryWriteBytes(at, (float)uv[0]); BitConverter.TryWriteBytes(at[4..], (float)uv[1]); break;
                case (5, 4): at[..4].Fill(255); break;
                case (7, 0): BitConverter.TryWriteBytes(at, 1.0f); break;
                case (7, 35): BitConverter.TryWriteBytes(at, (Half)1.0); break; // weights 1, 0, 0, 0 (the rest stay 0)
                case (6, 28) or (6, 24): break; // bone index 0: the skeleton map's first joint
                default: throw new InvalidDataException($"the model's vertices have an item (type {type}, format {format}) HellForge cannot write yet.");
            }
        }
    }

    static double[] Normalize(double[] v)
    {
        var l = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return l == 0 ? [0, 0, 1] : [v[0] / l, v[1] / l, v[2] / l];
    }

    static double[] Cross(double[] a, double[] b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    static double Dot(double[] a, double[] b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

    // R10G10B10A2: R, G the octahedral normal; B the tangent's angle about it from a reference tangent (diamond encoded);
    // A = 3 when the bitangent is flipped.
    static uint PackNormal(double[] normal, double[] tangent, int sign)
    {
        var n = Normalize(normal);
        var l1 = Math.Abs(n[0]) + Math.Abs(n[1]) + Math.Abs(n[2]);
        double x = n[0] / l1, y = n[1] / l1;
        if (n[2] < 0) (x, y) = ((1 - Math.Abs(y)) * Math.CopySign(1, x), (1 - Math.Abs(x)) * Math.CopySign(1, y));
        var r = (uint)Math.Clamp((int)Math.Round((x + 1) * 1023 / 2), 0, 1023);
        var g = (uint)Math.Clamp((int)Math.Round((y + 1) * 1023 / 2), 0, 1023);
        var useXy = Math.Abs(n[2]) < Math.Abs(n[1]);
        if (Math.Abs(Math.Abs(n[0]) - 1) < 1e-6) useXy = !useXy;
        var basis = Normalize(useXy ? [n[1], -n[0], 0] : [n[2], 0, -n[0]]);
        var bxn = Cross(basis, n);
        double c = Dot(tangent, basis), s = Dot(tangent, bxn);
        var l = Math.Abs(c) + Math.Abs(s);
        c /= l == 0 ? 1 : l;
        var rotation = s <= 0 ? (c + 1) / 4 : (3 - c) / 4;
        var b = (uint)Math.Clamp((int)Math.Round(rotation * 1023), 0, 1023);
        return r | g << 10 | b << 20 | (sign < 0 ? 3u : 0u) << 30;
    }

    // Per-vertex tangents and handedness from the UV derivatives of their triangles.
    static List<(double[] T, int Sign)> Tangents(Mesh mesh)
    {
        int count = mesh.Positions.Count;
        var t = new double[count][];
        var b = new double[count][];
        for (var i = 0; i < count; i++) { t[i] = new double[3]; b[i] = new double[3]; }
        for (var f = 0; f + 2 < mesh.Triangles.Count; f += 3)
        {
            int i0 = mesh.Triangles[f], i1 = mesh.Triangles[f + 1], i2 = mesh.Triangles[f + 2];
            var p0 = mesh.Positions[i0];
            double[] e1 = [mesh.Positions[i1][0] - p0[0], mesh.Positions[i1][1] - p0[1], mesh.Positions[i1][2] - p0[2]];
            double[] e2 = [mesh.Positions[i2][0] - p0[0], mesh.Positions[i2][1] - p0[1], mesh.Positions[i2][2] - p0[2]];
            double du1 = mesh.Uvs[i1][0] - mesh.Uvs[i0][0], dv1 = mesh.Uvs[i1][1] - mesh.Uvs[i0][1];
            double du2 = mesh.Uvs[i2][0] - mesh.Uvs[i0][0], dv2 = mesh.Uvs[i2][1] - mesh.Uvs[i0][1];
            var det = du1 * dv2 - du2 * dv1;
            if (Math.Abs(det) < 1e-12) continue;
            var r = 1.0 / det;
            foreach (var v in new[] { i0, i1, i2 })
                for (var k = 0; k < 3; k++)
                {
                    t[v][k] += (e1[k] * dv2 - e2[k] * dv1) * r;
                    b[v][k] += (e2[k] * du1 - e1[k] * du2) * r;
                }
        }
        var list = new List<(double[], int)>(count);
        for (var i = 0; i < count; i++)
        {
            var n = Normalize(mesh.Normals[i]);
            var d = Dot(t[i], n);
            double[] tv = [t[i][0] - n[0] * d, t[i][1] - n[1] * d, t[i][2] - n[2] * d];
            tv = Dot(tv, tv) < 1e-20 ? Normalize(Math.Abs(n[2]) < Math.Abs(n[1]) ? [n[1], -n[0], 0] : [n[2], 0, -n[0]]) : Normalize(tv);
            list.Add((tv, Dot(Cross(n, tv), b[i]) >= 0 ? 1 : -1));
        }
        return list;
    }

    // A Wavefront OBJ (triangulated on read; one vertex per distinct position/UV/normal corner). yUp: the file is Y-up,
    // as Blender and most tools write it, turned into the game's Z-up (x, -z, y), which keeps the winding. Its V is up,
    // the game's down (v -> 1 - v). Corners without a normal take their faces' normal.
    public static Mesh ReadObj(string text, double scale, bool yUp)
    {
        var positions = new List<double[]>();
        var uvs = new List<double[]>();
        var normals = new List<double[]>();
        var corners = new Dictionary<(int P, int T, int N), int>();
        var order = new List<(int P, int T, int N)>();
        var triangles = new List<int>();
        double[] Axes(double x, double y, double z) => yUp ? [x, -z, y] : [x, y, z];
        static double Number(string s) => double.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);
        // A face's corners (each distinct position/UV/normal corner one vertex), fanned into triangles.
        void Face(string[] parts)
        {
            var face = new List<int>();
            foreach (var corner in parts.Skip(1))
            {
                var q = corner.Split('/');
                int Index(int k, int count) => k < q.Length && q[k].Length > 0
                    ? (int.Parse(q[k], CultureInfo.InvariantCulture) is var i && i < 0 ? count + i : i - 1) : -1;
                var key = (Index(0, positions.Count), Index(1, uvs.Count), Index(2, normals.Count));
                if (key.Item1 < 0 || key.Item1 >= positions.Count || key.Item2 >= uvs.Count || key.Item3 >= normals.Count)
                    throw new InvalidDataException("a face names a vertex the file does not have.");
                if (!corners.TryGetValue(key, out var index)) { index = order.Count; corners[key] = index; order.Add(key); }
                face.Add(index);
            }
            if (face.Count < 3) throw new InvalidDataException("a face has fewer than 3 corners.");
            for (var k = 1; k + 1 < face.Count; k++) triangles.AddRange([face[0], face[k], face[k + 1]]);
        }
        var line = 0;
        foreach (var raw in text.Split('\n'))
        {
            line++;
            var parts = raw.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0 || parts[0].StartsWith('#')) continue;
            try
            {
                switch (parts[0])
                {
                    case "v":
                    {
                        var p = Axes(Number(parts[1]) * scale, Number(parts[2]) * scale, Number(parts[3]) * scale);
                        positions.Add(p);
                        break;
                    }
                    case "vt": uvs.Add([Number(parts[1]), parts.Length > 2 ? Number(parts[2]) : 0]); break;
                    case "vn": normals.Add(Axes(Number(parts[1]), Number(parts[2]), Number(parts[3]))); break;
                    case "f": Face(parts); break;
                }
            }
            catch (Exception error) when (error is FormatException or IndexOutOfRangeException or OverflowException)
            {
                throw new InvalidDataException($"OBJ line {line}: {error.Message}");
            }
            catch (InvalidDataException error) { throw new InvalidDataException($"OBJ line {line}: {error.Message}"); }
        }
        return Vertices(positions, uvs, normals, order, triangles);
    }

    // The corners as the mesh's vertices: their positions, UVs (V flipped to the game's) and normals, a corner without a
    // normal taking its faces'.
    static Mesh Vertices(List<double[]> positions, List<double[]> uvs, List<double[]> normals, List<(int P, int T, int N)> order, List<int> triangles)
    {
        var pos = order.Select(c => positions[c.P]).ToList();
        var uv = order.Select(c => c.T >= 0 ? new[] { uvs[c.T][0], 1 - uvs[c.T][1] } : [0.0, 0.0]).ToList();
        var nrm = order.Select(c => c.N >= 0 ? normals[c.N] : null).ToList();
        if (nrm.Any(n => n == null)) FaceNormals(pos, nrm, triangles);
        if (triangles.Count == 0) throw new InvalidDataException("the OBJ has no faces.");
        if (pos.Count > 1_000_000) throw new InvalidDataException("the OBJ has over a million vertices.");
        return new(pos, nrm!, uv, triangles);
    }

    // Each vertex without a normal: the sum of its faces' normals (unnormalized: larger faces weigh more).
    static void FaceNormals(List<double[]> pos, List<double[]?> nrm, List<int> triangles)
    {
        var sum = pos.Select(_ => new double[3]).ToList();
        for (var f = 0; f + 2 < triangles.Count; f += 3)
        {
            int a = triangles[f], b = triangles[f + 1], c = triangles[f + 2];
            var face = Cross([pos[b][0] - pos[a][0], pos[b][1] - pos[a][1], pos[b][2] - pos[a][2]], [pos[c][0] - pos[a][0], pos[c][1] - pos[a][1], pos[c][2] - pos[a][2]]);
            foreach (var v in new[] { a, b, c }) for (var k = 0; k < 3; k++) sum[v][k] += face[k];
        }
        for (var i = 0; i < nrm.Count; i++) nrm[i] ??= sum[i];
    }
}
