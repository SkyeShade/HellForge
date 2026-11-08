using System.Buffers.Binary;
using System.Text;

namespace HellForge.Core.GameData;

// The engine's animation resources for a unit's moving parts: the bones resource (which joints its clips animate), clips
// (quantized keys in the engine's streamed order) and the state machine (layers of states played on events). Checked by
// re-emitting the supported build's weapon bones, state machines and clips byte for byte. What weapons never use (float
// keys, beats, hash tracks, vector blocks, extra header lists) is refused, never guessed.
public static class AnimationBones
{
    // The six LOD thresholds every weapon's and the Helldiver's bones resource has.
    public static readonly float[] LodThresholds = [0.25f, 0.15f, 0.07f, 0.04f, 0.02f, 0.01f];

    // Names in order (entry i is bone index i of the unit's clips; each binds to the unit joint of that name), the LOD
    // thresholds and how many of the first bones animate at each LOD.
    public sealed record Bones(IReadOnlyList<string> Names, IReadOnlyList<float> Lod, IReadOnlyList<uint> LodCounts);

    // A unit's bones: the root entries every unit starts with, then its moving joints, animated at every LOD.
    public static Bones For(IEnumerable<string> movingJoints)
    {
        var names = new List<string> { "StingrayEntityRoot", "FbxAxisSystem_ConvertNode" };
        foreach (var joint in movingJoints)
            if (!names.Contains(joint)) names.Add(joint);
        return new(names, LodThresholds, Enumerable.Repeat((uint)names.Count, LodThresholds.Length).ToArray());
    }

    public static byte[] Write(Bones bones)
    {
        if (bones.Names.Count is 0 or > 1024) throw new InvalidDataException("a unit animates 1 to 1024 bones.");
        if (bones.Lod.Count != bones.LodCounts.Count) throw new InvalidDataException("every LOD threshold needs its bone count.");
        if (bones.LodCounts.Any(c => c > bones.Names.Count)) throw new InvalidDataException("a LOD animates more bones than there are.");
        var o = new MemoryStream();
        var w = new BinaryWriter(o);
        w.Write((uint)bones.Names.Count);
        w.Write((uint)bones.Lod.Count);
        foreach (var t in bones.Lod) w.Write(t);
        foreach (var name in bones.Names) w.Write(Thin(name));
        foreach (var c in bones.LodCounts) w.Write(c);
        foreach (var name in bones.Names)
        {
            if (name.Length == 0 || name.Contains('\0')) throw new InvalidDataException("a bone has a name.");
            w.Write(Encoding.UTF8.GetBytes(name));
            w.Write((byte)0);
        }
        return o.ToArray();
    }

    public static Bones Read(byte[] d)
    {
        var n = (int)U32(d, 0);
        var nlod = (int)U32(d, 4);
        if (n is 0 or > 1024 || nlod is 0 or > 16) throw new InvalidDataException("not a bones resource.");
        var at = 8;
        var lod = Enumerable.Range(0, nlod).Select(k => F32(d, at + 4 * k)).ToArray();
        at += 4 * nlod;
        var hashes = Enumerable.Range(0, n).Select(k => U32(d, at + 4 * k)).ToArray();
        at += 4 * n;
        var counts = Enumerable.Range(0, nlod).Select(k => U32(d, at + 4 * k)).ToArray();
        at += 4 * nlod;
        var names = new List<string>();
        for (var k = 0; k < n; k++)
        {
            var end = Array.IndexOf(d, (byte)0, at);
            if (end < 0) throw new InvalidDataException("a bone name is not ended.");
            names.Add(Encoding.UTF8.GetString(d, at, end - at));
            at = end + 1;
        }
        if (at != d.Length) throw new InvalidDataException("the bones resource has data after its names.");
        for (var k = 0; k < n; k++)
            if (hashes[k] != Thin(names[k])) throw new InvalidDataException($"bone {names[k]}'s hash is not its name's.");
        return new(names, lod, counts);
    }

    // The engine's 32-bit name hash: the top half of MurmurHash64A.
    public static uint Thin(string name) => (uint)(EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name)) >> 32);

    internal static uint U32(byte[] d, int at) =>
        at >= 0 && at + 4 <= d.Length ? BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at)) : throw new InvalidDataException("the resource ends early.");
    internal static ushort U16(byte[] d, int at) =>
        at >= 0 && at + 2 <= d.Length ? BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at)) : throw new InvalidDataException("the resource ends early.");
    internal static float F32(byte[] d, int at) => BitConverter.UInt32BitsToSingle(U32(d, at));
}

public static class AnimationClip
{
    public enum Channel { Scale = 1, Position = 2, Rotation = 3 }

    // A bone's quantized transform: position and scale as 3 x u16, rotation as a smallest-three u32.
    public sealed record Pose(ushort[] Position, uint Rotation, ushort[] Scale);
    // One key of one bone's channel at a time in whole milliseconds: Vector for position and scale, Rotation for rotation.
    public sealed record Key(Channel Channel, int Bone, int Ms, ushort[]? Vector, uint Rotation);
    // A named event the clip sends at a time in seconds (the engine's thin hash of its name).
    public sealed record Trigger(float Time, uint Hash);
    public sealed record Clip(float Length, IReadOnlyList<Pose> Rest, IReadOnlyList<Key> Keys, IReadOnlyList<Trigger> Triggers);

    // Positions and scales: (u - 32767) * 10 / 32767, so +-10 (metres, or a scale factor), truncated like the engine's
    // own clips (a scale of 1 is 36043).
    public static ushort Quantize(double v)
    {
        if (!double.IsFinite(v) || Math.Abs(v) > 10) throw new InvalidDataException($"{v} is outside the +-10 a clip can hold.");
        return (ushort)Math.Clamp(Math.Floor(v * 3276.7 + 32767), 0, 65535);
    }
    public static double Dequantize(ushort u) => (u - 32767) * (10.0 / 32767);
    public static ushort[] Quantize(double[] v) => v.Length == 3 ? [Quantize(v[0]), Quantize(v[1]), Quantize(v[2])] : throw new InvalidDataException("a position or scale has 3 values.");

    // A rotation (x, y, z, w): the largest component is dropped (its index in bits 1..0, rebuilt as positive), the other
    // three follow it in cyclic order as 10-bit (c / 0.75 * 512 + 512).
    public static uint QuantizeRotation(double[] q)
    {
        if (q.Length != 4) throw new InvalidDataException("a rotation is a quaternion (x, y, z, w).");
        var len = Math.Sqrt(q.Sum(c => c * c));
        if (!double.IsFinite(len) || len < 1e-6) throw new InvalidDataException("a rotation is not a quaternion.");
        var n = q.Select(c => c / len).ToArray();
        var k = 0;
        for (var j = 1; j < 4; j++) if (Math.Abs(n[j]) > Math.Abs(n[k])) k = j;
        if (n[k] < 0) n = n.Select(c => -c).ToArray();
        uint F(int j) => (uint)Math.Clamp((int)Math.Round(n[(k + j) % 4] / 0.75 * 512 + 512), 0, 1023);
        return (uint)k | (F(1) << 2) | (F(2) << 12) | (F(3) << 22);
    }
    public static double[] DequantizeRotation(uint p)
    {
        var k = (int)(p & 3);
        double C(int shift) => ((int)((p >> shift) & 0x3FF) - 512) / 512.0 * 0.75;
        double a = C(2), b = C(12), c = C(22);
        var l = Math.Sqrt(Math.Max(0, 1 - a * a - b * b - c * c));
        var q = new double[4];
        q[k] = l; q[(k + 1) % 4] = a; q[(k + 2) % 4] = b; q[(k + 3) % 4] = c;
        return q;
    }

    // The file: the clip body twice, the first copy holding the file's size at +0x0C.
    public static byte[] Write(Clip clip)
    {
        var body = Body(clip);
        var file = new byte[body.Length * 2];
        body.CopyTo(file, 0);
        body.CopyTo(file, body.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(file.AsSpan(12), (uint)file.Length);
        return file;
    }

    static byte[] Body(Clip clip)
    {
        var n = clip.Rest.Count;
        if (n is 0 or > 1023) throw new InvalidDataException("a clip animates 1 to 1023 bones.");
        if (!float.IsFinite(clip.Length) || clip.Length < 0 || clip.Length > 1048) throw new InvalidDataException("a clip lasts 0 to 1048 seconds.");
        var o = new MemoryStream();
        var w = new BinaryWriter(o);
        w.Write(0u); w.Write((uint)n); w.Write(clip.Length); w.Write(0u); w.Write(0u); w.Write(0u);
        w.Write((ushort)1);
        var bits = new byte[((3 * n + 7) / 8 + 1) / 2 * 2];
        for (var i = 0; i < 3 * n; i++) bits[i / 8] |= (byte)(1 << (i % 8)); // every channel quantized
        w.Write(bits);
        foreach (var pose in clip.Rest)
        {
            WriteVector(w, pose.Position);
            w.Write(pose.Rotation);
            WriteVector(w, pose.Scale);
        }
        foreach (var (_, write) in Stream(clip)) write(w);
        w.Write((ushort)3);
        w.Write((uint)o.Length);
        return o.ToArray();
    }

    static void WriteVector(BinaryWriter w, ushort[]? v)
    {
        if (v is not { Length: 3 }) throw new InvalidDataException("a position or scale has 3 values.");
        foreach (var x in v) w.Write(x);
    }

    // The key stream in the engine's order: each bone channel is a curve whose rest pose is key 0; its key n goes out
    // when key n-2 is due (keys 1 and 2 first), sorted by that time, ties scale, rotation, position, then bone, then n.
    // A trigger goes out at its own time, ahead of keys due within 1 ms of it.
    static List<((double Due, int Tie, int Bone, int N) Order, Action<BinaryWriter> Write)> Stream(Clip clip)
    {
        var entries = new List<((double, int, int, int), Action<BinaryWriter>)>();
        foreach (var curve in clip.Keys.GroupBy(k => (k.Channel, k.Bone)))
            entries.AddRange(CurveEntries(clip, curve.ToList()));
        foreach (var t in clip.Triggers)
        {
            if (!float.IsFinite(t.Time) || t.Time < 0 || t.Time > 1048) throw new InvalidDataException($"a trigger at {t.Time} s is outside what a clip can hold.");
            entries.Add(((t.Time * 1000.0 - 1.0, -1, -1, 0), w => { w.Write((ushort)2); w.Write(t.Hash); w.Write(t.Time); }));
        }
        return entries.OrderBy(e => e.Item1).ToList();
    }

    static IEnumerable<((double, int, int, int), Action<BinaryWriter>)> CurveEntries(Clip clip, List<Key> keys)
    {
        var (channel, bone) = (keys[0].Channel, keys[0].Bone);
        CheckCurve(clip, keys);
        int Time(int n) => n < 1 ? 0 : keys[n - 1].Ms; // key 0 is the rest pose at 0
        var tie = channel switch { Channel.Scale => 0, Channel.Rotation => 1, _ => 2 };
        for (var n = 1; n <= keys.Count; n++)
        {
            var key = keys[n - 1];
            yield return ((n >= 2 ? Time(n - 2) : 0, tie, bone, n), w =>
            {
                w.Write((ushort)(((int)key.Channel << 14) | (key.Bone << 4) | (key.Ms >> 16)));
                w.Write((ushort)(key.Ms & 0xFFFF));
                if (key.Channel == Channel.Rotation) w.Write(key.Rotation);
                else WriteVector(w, key.Vector);
            });
        }
    }

    static void CheckCurve(Clip clip, List<Key> keys)
    {
        var (channel, bone) = (keys[0].Channel, keys[0].Bone);
        if (bone < 0 || bone >= clip.Rest.Count) throw new InvalidDataException($"a key animates bone {bone}, which the clip does not have.");
        if (channel is not (Channel.Scale or Channel.Position or Channel.Rotation)) throw new InvalidDataException("a key is a scale, position or rotation.");
        var lastMs = (int)(clip.Length * 1000);
        for (var i = 0; i < keys.Count; i++)
        {
            if (keys[i].Ms < 0 || keys[i].Ms > lastMs) throw new InvalidDataException($"a key at {keys[i].Ms} ms is outside the clip's {lastMs} ms.");
            if (i > 0 && keys[i].Ms <= keys[i - 1].Ms) throw new InvalidDataException($"bone {bone}'s {channel} keys are not in time order.");
        }
    }

    public static Clip Read(byte[] file)
    {
        var (clip, size) = ReadBody(file);
        if (size * 2 != file.Length || !file.AsSpan(0, 12).SequenceEqual(file.AsSpan(size, 12)) || !file.AsSpan(16, size - 16).SequenceEqual(file.AsSpan(size + 16, size - 16))
            || AnimationBones.U32(file, 12) != file.Length || AnimationBones.U32(file, size + 12) != 0)
            throw new InvalidDataException("the clip is not two copies of its body.");
        return clip;
    }

    static (Clip, int Size) ReadBody(byte[] d)
    {
        var n = (int)AnimationBones.U32(d, 4);
        var length = AnimationBones.F32(d, 8);
        var o = ReadHeader(d, n);
        var rest = new List<Pose>();
        for (var i = 0; i < n; i++, o += 16) rest.Add(new(Vec(d, o), AnimationBones.U32(d, o + 6), Vec(d, o + 10)));
        var (keys, triggers, end) = ReadStream(d, o);
        if (AnimationBones.U32(d, end) != end) throw new InvalidDataException("the clip's end is not where it says.");
        // curves in time order (the stream interleaves them)
        var ordered = keys.GroupBy(k => (k.Channel, k.Bone)).SelectMany(g => g.OrderBy(k => k.Ms)).ToList();
        return (new Clip(length, rest, ordered, triggers), end + 4);
    }

    // Checks what HellForge reads (no hash tracks, beats or float channels); returns where the rest poses start.
    static int ReadHeader(byte[] d, int n)
    {
        if (AnimationBones.U32(d, 0) != 0 || n is 0 or > 1023) throw new InvalidDataException("not an animation clip.");
        if (AnimationBones.U32(d, 16) != 0 || AnimationBones.U32(d, 20) != 0) throw new InvalidDataException("the clip has hash tracks or beats; HellForge does not read them.");
        if (AnimationBones.U16(d, 24) != 1) throw new InvalidDataException("the clip's header is not one HellForge knows.");
        var bits = 26;
        if (bits + (3 * n + 7) / 8 > d.Length) throw new InvalidDataException("the clip ends early.");
        for (var i = 0; i < 3 * n; i++)
            if ((d[bits + i / 8] >> (i % 8) & 1) == 0) throw new InvalidDataException("the clip has float channels; HellForge does not read them.");
        return bits + ((3 * n + 7) / 8 + 1) / 2 * 2;
    }

    static (List<Key> Keys, List<Trigger> Triggers, int End) ReadStream(byte[] d, int o)
    {
        var keys = new List<Key>();
        var triggers = new List<Trigger>();
        while (o + 2 <= d.Length)
        {
            var h = AnimationBones.U16(d, o);
            o += 2;
            var type = h >> 14;
            if (h == 3) return (keys, triggers, o);
            if (type == 0)
            {
                if (h != 2) throw new InvalidDataException("the clip has float keys; HellForge does not read them.");
                triggers.Add(new(AnimationBones.F32(d, o + 4), AnimationBones.U32(d, o)));
                o += 8;
                continue;
            }
            var ms = ((h & 0xF) << 16) | AnimationBones.U16(d, o);
            var bone = (h >> 4) & 0x3FF;
            o += 2;
            if (type == 3) { keys.Add(new(Channel.Rotation, bone, ms, null, AnimationBones.U32(d, o))); o += 4; }
            else { keys.Add(new((Channel)type, bone, ms, Vec(d, o), 0)); o += 6; }
        }
        throw new InvalidDataException("the clip's keys are not ended.");
    }

    static ushort[] Vec(byte[] d, int o)
    {
        if (o + 6 > d.Length) throw new InvalidDataException("the clip ends early.");
        return [AnimationBones.U16(d, o), AnimationBones.U16(d, o + 2), AnimationBones.U16(d, o + 4)];
    }
}

public static class AnimationStateMachine
{
    // A state's link: the target state in its layer, the blend time in seconds, the link type and a beat (0 on weapons).
    public sealed record Link(int Target, float BlendTime, uint Type = 0, uint Beat = 0);
    // An event a state sends while it plays; Flag 1 when the event is in the machine's event table.
    public sealed record Timed(float Time, uint Event, uint Flag);
    // A state (0xA8 bytes, then its blocks). Type 0 clip, 1 empty, 2 blend. Functions are the blend functions' postfix
    // words (the first is the playback speed, each ends with 0x7FA00000), FunctionStarts the word index of each after the
    // first. The remaining fields are kept as the engine's own states have them.
    public sealed record State(ulong Name, uint Type, IReadOnlyList<ulong> Clips, IReadOnlyList<float> Weights, bool Loop, bool Additive,
        IReadOnlyList<(uint Event, int Link)> Events, IReadOnlyList<Link> Links, IReadOnlyList<Timed> TimedEvents, uint EmitEnd, float EndTime,
        IReadOnlyList<uint> Functions, IReadOnlyList<uint> FunctionStarts, IReadOnlyList<uint> Ui2, int BlendMask = -1, uint BlendVariable = 0,
        uint Flags = 0, uint Unk1C = 1, uint Unk60 = 0, uint Unk64 = 0, uint Unk68 = 3, int Unk80 = -1, uint Ragdoll = 0);
    public sealed record Layer(int Default, IReadOnlyList<State> States, uint Magic = 300);
    // MaxClips: the most clips any state plays. Events: every event thin hash the machine handles, sorted.
    public sealed record Machine(uint MaxClips, IReadOnlyList<Layer> Layers, IReadOnlyList<uint> Events,
        IReadOnlyList<(uint Name, float Value)> Variables, IReadOnlyList<float[]> Masks);

    // The constant playback speed function (1 = the clip's own speed, 0 = frozen on its first frame).
    public static uint[] Speed(float speed) => [BitConverter.SingleToUInt32Bits(speed), 0x7FA00000];

    public static byte[] Write(Machine m)
    {
        if (m.Layers.Count == 0) throw new InvalidDataException("a state machine has at least one layer.");
        var sorted = m.Events.Order().ToList();
        if (!sorted.SequenceEqual(m.Events) || sorted.Distinct().Count() != sorted.Count) throw new InvalidDataException("the machine's events are sorted and each is listed once.");
        var b = new Buf();
        b.Zero(0x50);
        var lo = b.Length;
        WriteLayers(b, m);
        b.Pad(4);
        var evOff = m.Events.Count > 0 ? b.Length : 0;
        foreach (var e in m.Events) b.U32(e);
        var varOff = m.Variables.Count > 0 ? b.Length : 0;
        foreach (var v in m.Variables) b.U32(v.Name);
        foreach (var v in m.Variables) b.F32(v.Value);
        var maskOff = WriteMasks(b, m.Masks);
        b.Pad(8); var u1 = b.Length; b.U32(0);
        b.Pad(8); var u2 = b.Length; b.U32(0);
        uint[] header = [m.MaxClips, (uint)m.Layers.Count, (uint)lo, (uint)m.Events.Count, (uint)evOff, (uint)m.Variables.Count, (uint)varOff,
            (uint)m.Masks.Count, (uint)maskOff, 0, 0, 4, (uint)u1, 4, (uint)u2];
        for (var k = 0; k < header.Length; k++) b.At(4 * k, header[k]);
        return b.ToArray();
    }

    // The layer list: a count and offsets (from the list), then each layer (8-aligned): magic, default, state count, state
    // offsets (from the layer), then each state (8-aligned).
    static void WriteLayers(Buf b, Machine m)
    {
        var lo = b.Length;
        b.U32((uint)m.Layers.Count);
        b.Zero(4 * m.Layers.Count);
        for (var i = 0; i < m.Layers.Count; i++)
        {
            var layer = m.Layers[i];
            if (layer.Default < 0 || layer.Default >= layer.States.Count) throw new InvalidDataException("a layer's default state is one of its states.");
            b.Pad(8);
            var ls = b.Length;
            b.At(lo + 4 + 4 * i, (uint)(ls - lo));
            b.U32(layer.Magic); b.U32((uint)layer.Default); b.U32((uint)layer.States.Count);
            var table = b.Length;
            b.Zero(4 * layer.States.Count);
            for (var j = 0; j < layer.States.Count; j++)
            {
                b.Pad(8);
                b.At(table + 4 * j, (uint)(b.Length - ls));
                b.Bytes(StateBytes(layer.States[j], layer.States.Count, m.Masks.Count));
            }
        }
    }

    // Blend masks: a count and offsets (from the list), then each mask's count and one weight per bone. 0 without masks.
    static int WriteMasks(Buf b, IReadOnlyList<float[]> masks)
    {
        if (masks.Count == 0) return 0;
        var at = b.Length;
        b.U32((uint)masks.Count);
        var table = b.Length;
        b.Zero(4 * masks.Count);
        for (var i = 0; i < masks.Count; i++)
        {
            b.At(table + 4 * i, (uint)(b.Length - at));
            b.U32((uint)masks[i].Length);
            foreach (var f in masks[i]) b.F32(f);
        }
        return at;
    }

    static byte[] StateBytes(State s, int states, int masks)
    {
        if (s.Links.Any(l => l.Target < 0 || l.Target >= states)) throw new InvalidDataException("a link targets a state outside its layer.");
        if (s.Events.Any(e => e.Link < 0 || e.Link >= s.Links.Count)) throw new InvalidDataException("a state's event names a link it does not have.");
        if (s.BlendMask < -1 || s.BlendMask >= masks) throw new InvalidDataException("a state names a blend mask the machine does not have.");
        var b = new Buf();
        b.Zero(0xA8);
        int Block(int align, Action<Buf> write, bool any)
        {
            if (!any) return 0;
            b.Pad(align);
            var at = b.Length;
            write(b);
            return at;
        }
        var clipOff = Block(8, x => { foreach (var c in s.Clips) x.U64(c); }, s.Clips.Count > 0);
        var wOff = Block(4, x => { foreach (var f in s.Weights) x.F32(f); }, s.Weights.Count > 0);
        var evOff = Block(4, x => { foreach (var (e, l) in s.Events) { x.U32(e); x.U32((uint)l); } }, s.Events.Count > 0);
        var linkOff = Block(4, x => { foreach (var l in s.Links) { x.U32((uint)l.Target); x.F32(l.BlendTime); x.U32(l.Type); x.U32(l.Beat); } }, s.Links.Count > 0);
        var timedOff = Block(4, x => { foreach (var t in s.TimedEvents) { x.F32(t.Time); x.U32(t.Event); x.U32(t.Flag); } }, s.TimedEvents.Count > 0);
        var fnOff = Block(4, x => { foreach (var f in s.Functions) x.U32(f); }, s.Functions.Count > 0);
        var startOff = Block(4, x => { foreach (var f in s.FunctionStarts) x.U32(f); }, s.FunctionStarts.Count > 0);
        var ui2Off = Block(4, x => { foreach (var f in s.Ui2) x.U32(f); }, s.Ui2.Count > 0);
        b.At64(0x00, s.Name);
        b.At(0x08, s.Type);
        b.At(0x0C, (uint)s.Clips.Count); b.At(0x10, (uint)clipOff);
        b.At(0x14, (uint)s.Weights.Count); b.At(0x18, (uint)wOff);
        b.At(0x1C, s.Unk1C);
        b.At(0x20, s.Loop ? 1u : 0u);
        b.At(0x24, s.Additive ? 1u : 0u);
        b.At(0x28, (uint)s.Events.Count); b.At(0x2C, (uint)evOff);
        b.At(0x30, (uint)s.Links.Count); b.At(0x34, (uint)linkOff);
        b.At(0x40, (uint)s.TimedEvents.Count); b.At(0x44, (uint)timedOff);
        b.At(0x48, s.EmitEnd);
        b.At(0x4C, BitConverter.SingleToUInt32Bits(s.EndTime));
        b.At(0x50, (uint)s.Functions.Count); b.At(0x54, (uint)fnOff);
        b.At(0x58, (uint)s.FunctionStarts.Count); b.At(0x5C, (uint)startOff);
        b.At(0x60, s.Unk60); b.At(0x64, s.Unk64);
        b.At(0x68, s.Unk68);
        b.At(0x6C, (uint)s.BlendMask);
        b.At(0x78, s.BlendVariable);
        b.At(0x7C, s.Flags);
        b.At(0x80, (uint)s.Unk80);
        b.At(0x84, s.Ragdoll);
        b.At(0x88, (uint)s.Ui2.Count); b.At(0x8C, (uint)ui2Off);
        return b.ToArray();
    }

    public static Machine Read(byte[] d)
    {
        var r = new Reader(d);
        if (new[] { 0x24, 0x28, 0x3C, 0x40, 0x44, 0x48, 0x4C }.Any(at => r.U(at) != 0))
            throw new InvalidDataException("the state machine has lists HellForge does not read (dynamic bones).");
        var lo = r.I(0x08);
        var nl = r.I(0x04);
        if (lo != 0x50 || nl is 0 or > 64 || r.I(lo) != nl) throw new InvalidDataException("not a state machine.");
        var layers = Enumerable.Range(0, nl).Select(i => ReadLayer(r, lo + r.I(lo + 4 + 4 * i))).ToList();
        var events = r.List(0x0C, 0x10, 4, 0, r.U);
        var nv = r.I(0x14);
        var variables = Enumerable.Range(0, nv).Select(k => (r.U(r.I(0x18) + 4 * k), r.F(r.I(0x18) + 4 * nv + 4 * k))).ToList();
        var masks = new List<float[]>();
        if (r.I(0x1C) > 0)
        {
            var mo = r.I(0x20);
            for (var k = 0; k < r.I(mo); k++)
            {
                var at = mo + r.I(mo + 4 + 4 * k);
                masks.Add(Enumerable.Range(0, r.I(at)).Select(q => r.F(at + 4 + 4 * q)).ToArray());
            }
        }
        return new Machine(r.U(0), layers, events, variables, masks);
    }

    static Layer ReadLayer(Reader r, int ls)
    {
        var ns = r.I(ls + 8);
        if (ns is < 0 or > 4096) throw new InvalidDataException("a layer's state count is not plausible.");
        return new Layer(r.I(ls + 4), Enumerable.Range(0, ns).Select(j => ReadState(r, ls + r.I(ls + 12 + 4 * j))).ToList(), r.U(ls));
    }

    static State ReadState(Reader r, int s)
    {
        if (r.U(s + 0x38) != 0 || r.U(s + 0x3C) != 0) throw new InvalidDataException("a state has vectors; HellForge does not read them.");
        if (new[] { 0x70, 0x74, 0x90, 0x94, 0x98, 0x9C, 0xA0, 0xA4 }.Any(at => r.U(s + at) != 0)) throw new InvalidDataException("a state has fields HellForge does not know.");
        return new State(r.U64(s), r.U(s + 0x08),
            r.List(s + 0x0C, s + 0x10, 8, s, r.U64), r.List(s + 0x14, s + 0x18, 4, s, r.F), r.U(s + 0x20) != 0, r.U(s + 0x24) != 0,
            r.List(s + 0x28, s + 0x2C, 8, s, at => (r.U(at), r.I(at + 4))),
            r.List(s + 0x30, s + 0x34, 16, s, at => new Link(r.I(at), r.F(at + 4), r.U(at + 8), r.U(at + 12))),
            r.List(s + 0x40, s + 0x44, 12, s, at => new Timed(r.F(at), r.U(at + 4), r.U(at + 8))),
            r.U(s + 0x48), r.F(s + 0x4C),
            r.List(s + 0x50, s + 0x54, 4, s, r.U), r.List(s + 0x58, s + 0x5C, 4, s, r.U), r.List(s + 0x88, s + 0x8C, 4, s, r.U),
            r.I(s + 0x6C), r.U(s + 0x78), r.U(s + 0x7C), r.U(s + 0x1C), r.U(s + 0x60), r.U(s + 0x64), r.U(s + 0x68), r.I(s + 0x80), r.U(s + 0x84));
    }

    sealed class Reader(byte[] d)
    {
        public uint U(int at) => AnimationBones.U32(d, at);
        public int I(int at) => (int)U(at);
        public float F(int at) => AnimationBones.F32(d, at);
        public ulong U64(int at) => at >= 0 && at + 8 <= d.Length ? BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(at)) : throw new InvalidDataException("the resource ends early.");
        // a list: its count at countAt, its offset (from base) at offsetAt, items of size bytes
        public List<T> List<T>(int countAt, int offsetAt, int size, int @base, Func<int, T> item)
        {
            var count = I(countAt);
            if (count is < 0 or > 65536) throw new InvalidDataException("a list's count is not plausible.");
            var start = @base + I(offsetAt);
            return Enumerable.Range(0, count).Select(k => item(start + size * k)).ToList();
        }
    }

    sealed class Buf
    {
        readonly List<byte> b = [];
        public int Length => b.Count;
        public void Zero(int n) { for (var i = 0; i < n; i++) b.Add(0); }
        public void Pad(int a) { while (b.Count % a != 0) b.Add(0); }
        public void U32(uint v) { Span<byte> t = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(t, v); b.AddRange(t.ToArray()); }
        public void U64(ulong v) { Span<byte> t = stackalloc byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(t, v); b.AddRange(t.ToArray()); }
        public void F32(float v) => U32(BitConverter.SingleToUInt32Bits(v));
        public void Bytes(byte[] v) => b.AddRange(v);
        public void At(int at, uint v) { for (var k = 0; k < 4; k++) b[at + k] = (byte)(v >> (8 * k)); }
        public void At64(int at, ulong v) { for (var k = 0; k < 8; k++) b[at + k] = (byte)(v >> (8 * k)); }
        public byte[] ToArray() => [.. b];
    }
}
