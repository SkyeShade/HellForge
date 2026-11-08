using System.Text.Json;

namespace HellForge.Core.GameData;

// A mod's own animations for a unit, from a JSON file (a data spec's "animations"; the format is in
// docs/modding/animations.md): its moving joints, clips and state machine, built into the three resources the engine reads.
// Rest transforms default to the unit's own joints. Inputs are refused with the field named, never passed through.
public static class AnimationSet
{
    public sealed record Built(byte[] Bones, byte[] Machine, IReadOnlyList<(string Clip, byte[] Data)> Clips);
    // A unit joint as its clips see it: its thin name hash and its local rest transform (position, rotation x y z w, scale).
    public sealed record Joint(uint Name, double[] Position, double[] Rotation, double[] Scale);

    // A unit's joints (header +0x34: u32 count, 16 bytes, then per joint 64 bytes {f32 rotation 3x3 column-major,
    // f32 translation, f32 scale, f32}, then the world matrices, u16 pairs and thin name hashes).
    public static List<Joint> UnitJoints(byte[] unit)
    {
        var at = (int)AnimationBones.U32(unit, 0x34);
        if (at == 0) return [];
        var n = (int)AnimationBones.U32(unit, at);
        if (n is < 0 or > 4096) throw new InvalidDataException("the unit's joint count is not plausible.");
        var names = at + 16 + 64 * n + 64 * n + 4 * n;
        double F(int o) => AnimationBones.F32(unit, o);
        var list = new List<Joint>();
        for (var i = 0; i < n; i++)
        {
            var j = at + 16 + 64 * i;
            // column-major: column c is (F(j + 12c), F(j + 12c + 4), F(j + 12c + 8)); m[r, c]
            double M(int r, int c) => F(j + 12 * c + 4 * r);
            list.Add(new(AnimationBones.U32(unit, names + 4 * i), [F(j + 36), F(j + 40), F(j + 44)], Quaternion(M), [F(j + 48), F(j + 52), F(j + 56)]));
        }
        return list;
    }

    // A unit's joints, named where its bones resource names them (the joints its own clips move); the rest are known by hash only.
    public static List<(Joint Joint, string? Name)> Named(byte[] unit, byte[]? bones)
    {
        var names = bones == null ? [] : AnimationBones.Read(bones).Names.ToDictionary(AnimationBones.Thin, n => n);
        return UnitJoints(unit).Select(j => (j, names.TryGetValue(j.Name, out var n) ? n : null)).ToList();
    }

    // A game unit's joints by its name or 0x<hash>, named from the bones resource it points at (+0x08).
    public static List<(Joint Joint, string? Name)> Named(GameDataReader game, string unit)
    {
        var id = unit.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? ulong.TryParse(unit[2..], System.Globalization.NumberStyles.AllowHexSpecifier, null, out var h) ? h : throw new InvalidDataException($"{unit} is not 0x<up to 16 hex digits>.")
            : Hash(unit);
        var unitKey = new ResourceKey(ResourceTypes.Unit, id);
        var found = game.Find([unitKey]);
        if (!found.TryGetValue(unitKey, out var at)) throw new InvalidDataException($"unit {unit} is not in the game data.");
        var main = game.Read(at).Main;
        var bonesKey = new ResourceKey(ResourceTypes.Bones, main.Length >= 0x10 ? BitConverter.ToUInt64(main, 0x08) : 0);
        var bones = game.Find([bonesKey]).TryGetValue(bonesKey, out var b) ? game.Read(b).Main : null;
        return Named(main, bones);
    }

    // A rotation matrix m[r, c] as a quaternion (x, y, z, w).
    static double[] Quaternion(Func<int, int, double> m)
    {
        double tr = m(0, 0) + m(1, 1) + m(2, 2), x, y, z, w;
        if (tr > 0) { var s = Math.Sqrt(tr + 1) * 2; w = s / 4; x = (m(2, 1) - m(1, 2)) / s; y = (m(0, 2) - m(2, 0)) / s; z = (m(1, 0) - m(0, 1)) / s; }
        else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) { var s = Math.Sqrt(1 + m(0, 0) - m(1, 1) - m(2, 2)) * 2; w = (m(2, 1) - m(1, 2)) / s; x = s / 4; y = (m(0, 1) + m(1, 0)) / s; z = (m(0, 2) + m(2, 0)) / s; }
        else if (m(1, 1) > m(2, 2)) { var s = Math.Sqrt(1 + m(1, 1) - m(0, 0) - m(2, 2)) * 2; w = (m(0, 2) - m(2, 0)) / s; x = (m(0, 1) + m(1, 0)) / s; y = s / 4; z = (m(1, 2) + m(2, 1)) / s; }
        else { var s = Math.Sqrt(1 + m(2, 2) - m(0, 0) - m(1, 1)) * 2; w = (m(1, 0) - m(0, 1)) / s; x = (m(0, 2) + m(2, 0)) / s; y = (m(1, 2) + m(2, 1)) / s; z = s / 4; }
        return [x, y, z, w];
    }

    // 0xE62AFEFC: the end event of states nothing listens to, in every weapon's machine (its name is not known).
    public const uint Unheard = 0xE62AFEFC;

    // A clip's resource name: under the unit's own name, so units in one folder never share a clip.
    public static string ClipName(string unit, string clip) => unit + "/animation/" + clip;

    public static Built Build(string json, string unit, IReadOnlyList<Joint>? joints = null)
    {
        using var doc = JsonDocument.Parse(json, new() { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        var root = doc.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException("the animations file is a JSON object.");
        Fields(root, "", "bones", "rest", "clips", "layers");

        var bones = ReadBones(Get(root, "bones", ""));
        var index = bones.Names.Select((n, i) => (n, i)).ToDictionary(x => x.n, x => x.i);
        var rest = ReadRest(root.TryGetProperty("rest", out var rs) ? rs : default, bones.Names, index, joints);

        var clipsSpec = Get(root, "clips", "");
        if (clipsSpec.ValueKind != JsonValueKind.Object || !clipsSpec.EnumerateObject().Any()) throw new InvalidDataException("clips holds at least one clip.");
        var clips = clipsSpec.EnumerateObject().Select(c => (c.Name, AnimationClip.Write(Clip(c.Name, c.Value, rest, index)))).ToList();

        var machine = new MachineReader(clips.Select(c => c.Name).ToHashSet(), index, unit).Read(Get(root, "layers", ""));
        return new(AnimationBones.Write(bones), AnimationStateMachine.Write(machine), clips);
    }

    static AnimationBones.Bones ReadBones(JsonElement spec)
    {
        var moving = Strings(spec, "bones");
        if (moving.Count == 0) throw new InvalidDataException("bones lists the unit's moving joints.");
        if (moving.Distinct().Count() != moving.Count) throw new InvalidDataException("bones lists each joint once.");
        return AnimationBones.For(moving);
    }

    static List<AnimationClip.Pose> ReadRest(JsonElement spec, IReadOnlyList<string> names, Dictionary<string, int> index, IReadOnlyList<Joint>? joints)
    {
        if (spec.ValueKind is not (JsonValueKind.Object or JsonValueKind.Undefined)) throw new InvalidDataException("rest gives bones' rest transforms by name.");
        var given = spec.ValueKind == JsonValueKind.Object ? spec.EnumerateObject().ToDictionary(p => p.Name, p => p.Value) : [];
        foreach (var name in given.Keys)
            if (!index.ContainsKey(name)) throw new InvalidDataException($"rest names {name}, which is not in bones.");
        return names.Select(name => RestPose(name, index[name] < 2, given.TryGetValue(name, out var r) ? r : default, joints)).ToList();
    }

    // A bone's rest pose: from rest when given, else the unit's joint, else identity for the two root entries.
    static AnimationClip.Pose RestPose(string name, bool rootEntry, JsonElement given, IReadOnlyList<Joint>? joints)
    {
        var joint = joints?.FirstOrDefault(j => j.Name == AnimationBones.Thin(name));
        if (joints != null && joint == null && !rootEntry) throw new InvalidDataException($"bones names {name}, which is not a joint of the unit.");
        if (given.ValueKind == JsonValueKind.Undefined)
        {
            if (joint != null) return new(AnimationClip.Quantize(joint.Position), AnimationClip.QuantizeRotation(joint.Rotation), AnimationClip.Quantize(joint.Scale));
            if (rootEntry) return Identity();
            throw new InvalidDataException($"rest has no transform for {name} (needed when the unit is not known).");
        }
        var at = $"rest.{name}";
        Fields(given, at, "position", "rotation", "scale");
        return new(AnimationClip.Quantize(Numbers(given, "position", 3, at, [0, 0, 0])),
            AnimationClip.QuantizeRotation(Numbers(given, "rotation", 4, at, [0, 0, 0, 1])),
            AnimationClip.Quantize(Numbers(given, "scale", 3, at, [1, 1, 1])));
    }

    static AnimationClip.Pose Identity() => new([AnimationClip.Quantize(0), AnimationClip.Quantize(0), AnimationClip.Quantize(0)],
        AnimationClip.QuantizeRotation([0, 0, 0, 1]), [AnimationClip.Quantize(1), AnimationClip.Quantize(1), AnimationClip.Quantize(1)]);

    static AnimationClip.Clip Clip(string name, JsonElement c, List<AnimationClip.Pose> rest, Dictionary<string, int> index)
    {
        if (name.Length == 0 || name.Contains('/') || name.Contains('\\')) throw new InvalidDataException($"clip \"{name}\": a clip's name is one word, such as \"fire\".");
        var at = $"clips.{name}";
        Fields(c, at, "length", "keys", "events");
        var length = (float)Number(Get(c, "length", at), double.Epsilon, 1048, $"{at}: length is the clip's seconds, more than 0.");
        var keys = new List<AnimationClip.Key>();
        if (c.TryGetProperty("keys", out var keysSpec))
        {
            if (keysSpec.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at}.keys gives keys per joint.");
            foreach (var joint in keysSpec.EnumerateObject())
                keys.AddRange(JointKeys(joint, $"{at}.keys", index, length));
        }
        var triggers = c.TryGetProperty("events", out var events) ? Triggers(events, $"{at}.events", length) : [];
        return new(length, rest, keys, triggers);
    }

    static IEnumerable<AnimationClip.Key> JointKeys(JsonProperty joint, string at, Dictionary<string, int> index, float length)
    {
        if (!index.TryGetValue(joint.Name, out var bone)) throw new InvalidDataException($"{at} names {joint.Name}, which is not in bones.");
        Fields(joint.Value, $"{at}.{joint.Name}", "position", "rotation", "scale");
        var keys = new List<AnimationClip.Key>();
        foreach (var channel in new[] { AnimationClip.Channel.Position, AnimationClip.Channel.Rotation, AnimationClip.Channel.Scale })
        {
            var field = channel.ToString().ToLowerInvariant();
            if (joint.Value.TryGetProperty(field, out var list)) keys.AddRange(Curve(list, $"{at}.{joint.Name}.{field}", channel, bone, length));
        }
        return keys;
    }

    // One channel's keys: [time, values...] after 0 (the key at 0 is the rest pose), held to the clip's end.
    static List<AnimationClip.Key> Curve(JsonElement list, string at, AnimationClip.Channel channel, int bone, float length)
    {
        if (list.ValueKind != JsonValueKind.Array || list.GetArrayLength() == 0) throw new InvalidDataException($"{at}: a list of [time, values...].");
        var size = channel == AnimationClip.Channel.Rotation ? 4 : 3;
        var lastMs = (int)(length * 1000);
        var curve = new List<AnimationClip.Key>();
        foreach (var k in list.EnumerateArray())
        {
            var v = KeyValues(k, size, at);
            var ms = (int)Math.Round(v[0] * 1000);
            if (ms <= 0 || ms > lastMs) throw new InvalidDataException($"{at}: a key at {v[0]} s is not after 0 and within the clip's {length} s (the rest pose is the key at 0).");
            if (curve.Count > 0 && ms <= curve[^1].Ms) throw new InvalidDataException($"{at}: keys are in time order, at least 1 ms apart.");
            curve.Add(channel == AnimationClip.Channel.Rotation
                ? new(channel, bone, ms, null, AnimationClip.QuantizeRotation(v[1..]))
                : new(channel, bone, ms, AnimationClip.Quantize(v[1..]), 0));
        }
        if (curve[^1].Ms < lastMs) curve.Add(curve[^1] with { Ms = lastMs });
        return curve;
    }

    static double[] KeyValues(JsonElement k, int size, string at) =>
        k.ValueKind == JsonValueKind.Array && k.GetArrayLength() == size + 1 && k.EnumerateArray().All(x => x.ValueKind == JsonValueKind.Number)
            ? k.EnumerateArray().Select(x => x.GetDouble()).ToArray()
            : throw new InvalidDataException($"{at}: each key is [time, {size} values].");

    static List<AnimationClip.Trigger> Triggers(JsonElement events, string at, float length)
    {
        if (events.ValueKind != JsonValueKind.Array) throw new InvalidDataException($"{at} is a list of [time, \"event\"].");
        return events.EnumerateArray().Select(e =>
        {
            var (t, name) = TimedEvent(e, at);
            if (t > length) throw new InvalidDataException($"{at}: {t} s is outside the clip.");
            return new AnimationClip.Trigger((float)t, Event(name, at));
        }).ToList();
    }

    // [time, "event"] with time at least 0.
    static (double Time, string Name) TimedEvent(JsonElement e, string at) =>
        e.ValueKind == JsonValueKind.Array && e.GetArrayLength() == 2 && e[0].ValueKind == JsonValueKind.Number && e[1].ValueKind == JsonValueKind.String && e[0].GetDouble() >= 0
            ? (e[0].GetDouble(), e[1].GetString()!)
            : throw new InvalidDataException($"{at}: each is [time, \"event\"].");

    // A number in [min, max], or the message.
    static double Number(JsonElement e, double min, double max, string message) =>
        e.ValueKind == JsonValueKind.Number && e.GetDouble() is var v && v >= min && v <= max ? v : throw new InvalidDataException(message);

    static double Number(JsonElement parent, string name, double min, double max, double fallback, string message) =>
        parent.TryGetProperty(name, out var e) ? Number(e, min, max, message) : fallback;

    // The layers: every event they use goes in the machine's sorted event list, every bone mask in its mask list.
    sealed class MachineReader(HashSet<string> clipNames, Dictionary<string, int> index, string unit)
    {
        readonly SortedSet<uint> events = [Unheard];
        readonly List<float[]> masks = [];

        public AnimationStateMachine.Machine Read(JsonElement spec)
        {
            if (spec.ValueKind != JsonValueKind.Array || spec.GetArrayLength() == 0) throw new InvalidDataException("layers lists at least one layer.");
            var layers = spec.EnumerateArray().Select((layer, i) => Layer(layer, $"layers[{i}]")).ToList();
            return new(1, layers, [.. events], [], masks);
        }

        AnimationStateMachine.Layer Layer(JsonElement layer, string at)
        {
            Fields(layer, at, "default", "bones", "states");
            var statesSpec = Get(layer, "states", at);
            if (statesSpec.ValueKind != JsonValueKind.Object || !statesSpec.EnumerateObject().Any()) throw new InvalidDataException($"{at}.states holds at least one state.");
            var names = statesSpec.EnumerateObject().Select(s => s.Name).ToList();
            var defaultSpec = Get(layer, "default", at);
            var defaultIndex = defaultSpec.ValueKind == JsonValueKind.String ? names.IndexOf(defaultSpec.GetString()!) : -1;
            if (defaultIndex < 0) throw new InvalidDataException($"{at}: default names one of its states.");
            var mask = layer.TryGetProperty("bones", out var maskSpec) && maskSpec.ValueKind != JsonValueKind.Null ? Mask(maskSpec, $"{at}.bones") : -1;
            return new(defaultIndex, statesSpec.EnumerateObject().Select(s => State(s, $"{at}.states.{s.Name}", names, mask)).ToList());
        }

        int Mask(JsonElement spec, string at)
        {
            var weights = new float[index.Count];
            foreach (var b in Strings(spec, at))
                weights[index.TryGetValue(b, out var i) ? i : throw new InvalidDataException($"{at} names {b}, which is not in bones.")] = 1;
            masks.Add(weights);
            return masks.Count - 1;
        }

        AnimationStateMachine.State State(JsonProperty s, string at, List<string> names, int mask)
        {
            var v = s.Value;
            Fields(v, at, "clip", "empty", "loop", "speed", "end", "on", "events");
            var empty = v.TryGetProperty("empty", out var e) && e.ValueKind == JsonValueKind.True;
            var clip = v.TryGetProperty("clip", out var cs) ? cs.ValueKind == JsonValueKind.String ? cs.GetString() : throw new InvalidDataException($"{at}: clip names a clip.") : null;
            if (empty == (clip != null)) throw new InvalidDataException($"{at}: a state plays a clip or is \"empty\": true.");
            if (clip != null && !clipNames.Contains(clip)) throw new InvalidDataException($"{at}: clip {clip} is not in clips.");
            var (on, links) = v.TryGetProperty("on", out var onSpec) ? On(onSpec, $"{at}.on", names) : ([], []);
            var timed = v.TryGetProperty("events", out var timedSpec) ? Timed(timedSpec, $"{at}.events") : [];
            var end = End(v, at, empty);
            var speed = Number(v, "speed", 0, 100, 1, $"{at}: speed is 0 to 100.");
            var loop = v.TryGetProperty("loop", out var lp) && (lp.ValueKind is JsonValueKind.True or JsonValueKind.False ? lp.GetBoolean()
                : throw new InvalidDataException($"{at}: loop is true or false."));
            return new AnimationStateMachine.State(Hash(s.Name), empty ? 1u : 0u,
                clip == null ? [] : [Hash(ClipName(unit, clip))], clip == null ? [] : [1f], loop || empty, false,
                on, links, timed, end, 0, AnimationStateMachine.Speed((float)speed), [], [], mask);
        }

        // The event a state sends when its clip ends: end when given, else the unheard one; an empty state ends nothing.
        uint End(JsonElement v, string at, bool empty)
        {
            if (!v.TryGetProperty("end", out var spec)) return empty ? 0u : Unheard;
            var end = spec.ValueKind == JsonValueKind.String ? Event(spec.GetString()!, $"{at}.end") : throw new InvalidDataException($"{at}: end is an event name.");
            events.Add(end);
            return end;
        }

        // on: event -> state or [state, blend seconds]; links with the same target and blend are shared.
        (List<(uint, int)>, List<AnimationStateMachine.Link>) On(JsonElement spec, string at, List<string> names)
        {
            if (spec.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at} maps an event to a state.");
            var links = new List<AnimationStateMachine.Link>();
            var on = new List<(uint, int)>();
            foreach (var t in spec.EnumerateObject())
            {
                var (target, blend) = Transition(t.Value, $"{at}.{t.Name}");
                var ti = names.IndexOf(target);
                if (ti < 0) throw new InvalidDataException($"{at}.{t.Name}: {target} is not a state of this layer.");
                var link = links.FindIndex(x => x.Target == ti && Math.Abs(x.BlendTime - blend) < 1e-6);
                if (link < 0) { link = links.Count; links.Add(new(ti, (float)blend)); }
                var ev = Event(t.Name, at);
                events.Add(ev);
                on.Add((ev, link));
            }
            return (on, links);
        }

        static (string Target, double Blend) Transition(JsonElement v, string at)
        {
            var (target, blend) = v.ValueKind switch
            {
                JsonValueKind.String => (v.GetString()!, 0.0),
                JsonValueKind.Array when v.GetArrayLength() == 2 && v[0].ValueKind == JsonValueKind.String && v[1].ValueKind == JsonValueKind.Number
                    => (v[0].GetString()!, v[1].GetDouble()),
                _ => throw new InvalidDataException($"{at}: a state, or [state, blend seconds].")
            };
            return blend is >= 0 and <= 10 ? (target, blend) : throw new InvalidDataException($"{at}: the blend is 0 to 10 seconds.");
        }

        List<AnimationStateMachine.Timed> Timed(JsonElement spec, string at)
        {
            if (spec.ValueKind != JsonValueKind.Array) throw new InvalidDataException($"{at} is a list of [time, \"event\"].");
            return spec.EnumerateArray().Select(t =>
            {
                var (time, name) = TimedEvent(t, at);
                var ev = Event(name, at);
                events.Add(ev);
                return new AnimationStateMachine.Timed((float)time, ev, 1);
            }).ToList();
        }
    }

    static ulong Hash(string name) => EngineHash.Murmur64A(System.Text.Encoding.UTF8.GetBytes(name));

    static uint Event(string name, string at)
    {
        if (name.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return name.Length == 10 && uint.TryParse(name[2..], System.Globalization.NumberStyles.HexNumber, null, out var h) ? h : throw new InvalidDataException($"{at}: {name} is not 0x<8 hex digits>.");
        return name.Length > 0 ? AnimationBones.Thin(name) : throw new InvalidDataException($"{at}: an event has a name.");
    }

    static JsonElement Get(JsonElement e, string name, string at) =>
        e.TryGetProperty(name, out var v) ? v : throw new InvalidDataException($"{(at.Length > 0 ? at + ": " : "")}{name} is missing.");

    static void Fields(JsonElement e, string at, params string[] allowed)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{at}: an object.");
        foreach (var p in e.EnumerateObject())
            if (!allowed.Contains(p.Name)) throw new InvalidDataException($"{(at.Length > 0 ? at + ": " : "")}{p.Name} is not a field here ({string.Join(", ", allowed)}).");
    }

    static List<string> Strings(JsonElement e, string at) =>
        e.ValueKind == JsonValueKind.Array && e.EnumerateArray().All(x => x.ValueKind == JsonValueKind.String && x.GetString()!.Length > 0)
            ? e.EnumerateArray().Select(x => x.GetString()!).ToList()
            : throw new InvalidDataException($"{at} is a list of joint names.");

    static double[] Numbers(JsonElement e, string name, int n, string at, double[] fallback)
    {
        if (!e.TryGetProperty(name, out var v)) return fallback;
        return v.ValueKind == JsonValueKind.Array && v.GetArrayLength() == n && v.EnumerateArray().All(x => x.ValueKind == JsonValueKind.Number)
            ? v.EnumerateArray().Select(x => x.GetDouble()).ToArray()
            : throw new InvalidDataException($"{at}.{name} is {n} numbers.");
    }
}
