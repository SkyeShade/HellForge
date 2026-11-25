using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core.GameData;

// A Wwise sound bank of a mod's own (a data spec's "sounds"; docs/modding/data.md "Sounds of your own"): one event per
// WAV file, each a Play action on a sound with the WAV as 16-bit PCM, all sounds under one actor-mixer copied from a
// root mixer of a game bank (its bus, positioning, attenuation and game-parameter curves). The game's Wwise is bank
// version 154 and XORs the header's first words with a fixed key; the plugin registers the PCM codec itself.
public static class SoundBank
{
    public const uint Version = 154;
    const uint Magic = 0x78A3F4A5; // a Stingray wwise_bank's and wwise_dep's first word
    static readonly uint[] HeaderKey = [0x9211BCAC, 0xA3107038, 0xC9CF6298];
    const uint Language = 0x17705D3E; // FNV-1 of "SFX": no language
    const uint Project = 0xD16;
    const uint Pcm = 0x00010001;
    const byte MakeUpGain = 0x05, AttenuationId = 0x4B;
    const byte SoundType = 2, ActionType = 3, EventType = 4, MixerType = 7, AttenuationType = 14;

    public sealed record Sound(string Event, byte[] Wav, double Gain);

    // Wwise's short id: FNV-1 (32-bit) of the lower-case name. Event ids are this of the event's name.
    public static uint Id(string name)
    {
        var h = 2166136261u;
        foreach (var b in Encoding.UTF8.GetBytes(name.ToLowerInvariant())) h = (h * 16777619) ^ b;
        return h;
    }

    // The bank's Wwise id. The game's banks use their path's last part; a mod's uses its whole path, so two mods'
    // "rifle" banks differ. The plugin reads the id from the header and the game never derives it from the name.
    public static uint BankId(string path) => Id(path);

    // The bank (its wwise_bank main part) and its wwise_dep main part. mixerBank is the game bank holding mixer.
    public static (byte[] Bank, byte[] Dependency) Build(string path, IReadOnlyList<Sound> sounds, byte[] mixerBank, uint mixer)
    {
        if (sounds.Count == 0) throw new InvalidDataException("a sound bank needs a sound.");
        var names = sounds.Select(s => s.Event.ToLowerInvariant()).ToList();
        if (names.Distinct().Count() != names.Count) throw new InvalidDataException("each sound's event name is unique.");
        var bankId = BankId(path);
        var mixerId = Id(path + "/mixer");
        var objects = MixerObjects(mixerBank, mixer, mixerId, sounds.Select(s => Id(s.Event + "/sound")).ToList());
        var media = new List<(uint Id, byte[] Data)>();
        foreach (var s in sounds)
        {
            var wem = Wem(s.Wav, s.Event);
            var source = Id(s.Event + "/media");
            media.Add((source, wem));
            objects.Insert(objects.Count - 1, (SoundType, Id(s.Event + "/sound"), SoundObject(source, wem, mixerId, s.Gain)));
        }
        foreach (var s in sounds) objects.Add((ActionType, Id(s.Event + "/play"), PlayAction(Id(s.Event + "/sound"), bankId)));
        foreach (var s in sounds) objects.Add((EventType, Id(s.Event), [1, .. U32(Id(s.Event + "/play"))]));
        var ids = objects.Select(o => o.Id).ToList();
        if (ids.Distinct().Count() != ids.Count) throw new InvalidDataException("two of the bank's objects have the same id; rename an event.");
        return (Container(path, bankId, media, objects), Dependency(path));
    }

    static byte[] U32(uint v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, v);
        return b;
    }

    // The game mixer under the new id with the sounds as its children, and the attenuation it names (a bank carries its
    // own copy of each attenuation it uses, as the game's do). A root mixer only: its parent would be in another bank.
    static List<(byte Type, uint Id, byte[] Data)> MixerObjects(byte[] bank, uint mixer, uint mixerId, IReadOnlyList<uint> children)
    {
        var all = Objects(bank);
        var found = all.FirstOrDefault(o => o.Id == mixer && o.Type == MixerType);
        if (found.Data == null) throw new InvalidDataException($"the bank has no actor-mixer 0x{mixer:X8}.");
        var node = NodeBase.Read(found.Data);
        if (node.Parent != 0 || node.Bus == 0) throw new InvalidDataException($"mixer 0x{mixer:X8} is not a root mixer (no parent, its own bus).");
        var list = new List<(byte, uint, byte[])>();
        if (node.Props.TryGetValue(AttenuationId, out var attenuation) && attenuation != 0)
        {
            var att = all.FirstOrDefault(o => o.Id == attenuation && o.Type == AttenuationType);
            if (att.Data == null) throw new InvalidDataException($"mixer 0x{mixer:X8}'s attenuation 0x{attenuation:X8} is not in its bank.");
            list.Add(att);
        }
        list.Add((MixerType, mixerId, [.. found.Data.AsSpan(0, node.End), .. U32((uint)children.Count), .. children.SelectMany(U32)]));
        return list;
    }

    // A sound playing in-memory PCM media under the mixer; gain in dB as make-up gain, as the game's weapon sounds set
    // their level. Everything else comes from the mixer (positioning 0: inherited).
    static byte[] SoundObject(uint source, byte[] wem, uint parent, double gain)
    {
        using var o = new MemoryStream();
        o.Write(U32(Pcm));
        o.WriteByte(0); // in memory
        o.Write(U32(source));
        o.Write(U32(CacheId(wem)));
        o.Write(U32((uint)wem.Length));
        o.WriteByte(0);
        o.Write([0, 0, 0, 0]); // no effects, no metadata
        o.Write(U32(0)); // the parent's bus
        o.Write(U32(parent));
        o.WriteByte(0);
        if (gain != 0) { o.Write([1, MakeUpGain]); o.Write(BitConverter.GetBytes((float)gain)); }
        else o.WriteByte(0);
        o.Write([0, 0, 0]); // no ranged props, positioning and aux from the parent
        o.Write(U32(0)); // reflections bus
        o.Write([0x00, 0x01, 0x00, 0x00, 0x03, 0x00]); // advanced settings as most game sounds
        o.Write([0, 0, 0, 0]); // no state props, state groups or curves
        return o.ToArray();
    }

    static byte[] PlayAction(uint target, uint bank) =>
        [0x03, 0x04, .. U32(target), 0, 0, 0, 0x04, .. U32(bank), .. U32(0)];

    // The media cache key: one per media content (the game's banks share it only between identical media), never 0.
    static uint CacheId(byte[] wem) => BinaryPrimitives.ReadUInt32LittleEndian(SHA256.HashData(wem)) | 1;

    static byte[] Dependency(string path)
    {
        var name = Encoding.UTF8.GetBytes(path);
        return [.. U32(Magic), .. U32((uint)name.Length + 1), .. name, 0];
    }

    static byte[] Container(string path, uint bankId, List<(uint Id, byte[] Data)> media, List<(byte Type, uint Id, byte[] Data)> objects)
    {
        using var body = new MemoryStream();
        media.Sort((a, b) => a.Id.CompareTo(b.Id));
        var didx = new MemoryStream();
        var data = new MemoryStream();
        foreach (var (id, bytes) in media)
        {
            while (data.Length % 16 != 0) data.WriteByte(0);
            didx.Write(U32(id)); didx.Write(U32((uint)data.Length)); didx.Write(U32((uint)bytes.Length));
            data.Write(bytes);
        }
        var hirc = new MemoryStream();
        hirc.Write(U32((uint)objects.Count));
        foreach (var (type, id, bytes) in objects) { hirc.WriteByte(type); hirc.Write(U32((uint)bytes.Length + 4)); hirc.Write(U32(id)); hirc.Write(bytes); }
        Chunk(body, "BKHD", Header(bankId, hirc.ToArray()));
        Chunk(body, "DIDX", didx.ToArray());
        Chunk(body, "DATA", data.ToArray());
        Chunk(body, "HIRC", hirc.ToArray());
        return [.. U32(Magic), .. U32((uint)body.Length), .. BitConverter.GetBytes(EngineHash.Murmur64A(Encoding.UTF8.GetBytes(path))), .. body.ToArray()];
    }

    static void Chunk(Stream s, string tag, byte[] data)
    {
        s.Write(Encoding.ASCII.GetBytes(tag));
        s.Write(U32((uint)data.Length));
        s.Write(data);
    }

    // version, bank id, language, alternate values (the first three and the two u16 XORed with the plugin's key),
    // project, bank type 0, a 16-byte hash and 4 zero bytes.
    static byte[] Header(uint bankId, byte[] hirc)
    {
        var h = new byte[44];
        BinaryPrimitives.WriteUInt32LittleEndian(h, Version ^ HeaderKey[0]);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(4), bankId ^ HeaderKey[1]);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(8), Language ^ HeaderKey[2]);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(12), 0x10 ^ 0x4799);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(14), 0x09E0);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(16), Project);
        SHA256.HashData(hirc).AsSpan(0, 16).CopyTo(h.AsSpan(24));
        return h;
    }

    // Where an actor-mixer's NodeBaseParams end (its child list follows).
    internal static int MixerNodeEnd(byte[] mixer) => NodeBase.Read(mixer).End;

    // A bank's objects (type, id, data after the id). Takes a wwise_bank main part, with its 16-byte Stingray header.
    public static List<(byte Type, uint Id, byte[] Data)> Objects(byte[] bank)
    {
        var at = 16;
        while (at + 8 <= bank.Length)
        {
            var tag = Encoding.ASCII.GetString(bank, at, 4);
            var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(bank.AsSpan(at + 4));
            if (tag == "HIRC") return HircObjects(bank.AsSpan(at + 8, Math.Min(size, bank.Length - at - 8)));
            at += 8 + size;
        }
        throw new InvalidDataException("the bank has no HIRC chunk.");
    }

    static List<(byte, uint, byte[])> HircObjects(ReadOnlySpan<byte> h)
    {
        var n = BinaryPrimitives.ReadUInt32LittleEndian(h);
        var list = new List<(byte, uint, byte[])>();
        var p = 4;
        for (var i = 0; i < n; i++)
        {
            var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(h[(p + 1)..]);
            if (size < 4 || p + 5 + size > h.Length) throw new InvalidDataException("the bank's HIRC chunk ends early.");
            list.Add((h[p], BinaryPrimitives.ReadUInt32LittleEndian(h[(p + 5)..]), h.Slice(p + 9, size - 4).ToArray()));
            p += 5 + size;
        }
        return list;
    }

    // The WAV's 16-bit PCM samples as a Wwise media file: RIFF/WAVE, a 24-byte fmt whose last word is Wwise's channel
    // configuration (count, standard type, speaker mask), then data.
    static byte[] Wem(byte[] wav, string name)
    {
        var (channels, rate, samples) = Wav.Read(wav, name);
        var fmt = new byte[24];
        BinaryPrimitives.WriteUInt16LittleEndian(fmt, 0xFFFE);
        BinaryPrimitives.WriteUInt16LittleEndian(fmt.AsSpan(2), (ushort)channels);
        BinaryPrimitives.WriteUInt32LittleEndian(fmt.AsSpan(4), (uint)rate);
        BinaryPrimitives.WriteUInt32LittleEndian(fmt.AsSpan(8), (uint)(rate * channels * 2));
        BinaryPrimitives.WriteUInt16LittleEndian(fmt.AsSpan(12), (ushort)(channels * 2));
        BinaryPrimitives.WriteUInt16LittleEndian(fmt.AsSpan(14), 16);
        BinaryPrimitives.WriteUInt16LittleEndian(fmt.AsSpan(16), 6);
        BinaryPrimitives.WriteUInt16LittleEndian(fmt.AsSpan(18), 16);
        BinaryPrimitives.WriteUInt32LittleEndian(fmt.AsSpan(20), (uint)channels | 0x100u | (channels == 1 ? 0x4u : 0x3u) << 12);
        using var o = new MemoryStream();
        o.Write("RIFF"u8); o.Write(U32((uint)(4 + 8 + fmt.Length + 8 + samples.Length)));
        o.Write("WAVE"u8);
        Chunk(o, "fmt ", fmt);
        Chunk(o, "data", samples);
        return o.ToArray();
    }

    // A WAV file's samples: 16-bit PCM, mono or stereo (plain or WAVE_FORMAT_EXTENSIBLE), 8 to 96 kHz.
    static class Wav
    {
        public static (int Channels, int Rate, byte[] Samples) Read(byte[] wav, string name)
        {
            if (wav.Length < 12 || Encoding.ASCII.GetString(wav, 0, 4) != "RIFF" || Encoding.ASCII.GetString(wav, 8, 4) != "WAVE")
                throw new InvalidDataException($"{name}: not a WAV file.");
            int channels = 0, rate = 0;
            for (var at = 12; at + 8 <= wav.Length;)
            {
                var tag = Encoding.ASCII.GetString(wav, at, 4);
                var size = (int)Math.Min(BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(at + 4)), (uint)(wav.Length - at - 8));
                if (tag == "fmt ") (channels, rate) = Format(wav.AsSpan(at + 8, size), name);
                if (tag == "data")
                {
                    if (channels == 0) throw new InvalidDataException($"{name}: its fmt chunk comes after its data.");
                    if (size == 0) throw new InvalidDataException($"{name}: it has no samples.");
                    return (channels, rate, wav.AsSpan(at + 8, size - size % (2 * channels)).ToArray());
                }
                at += 8 + size + (size & 1);
            }
            throw new InvalidDataException($"{name}: it has no data chunk.");
        }

        static (int, int) Format(ReadOnlySpan<byte> f, string name)
        {
            if (f.Length < 16) throw new InvalidDataException($"{name}: its fmt chunk is too short.");
            var tag = BinaryPrimitives.ReadUInt16LittleEndian(f);
            var channels = BinaryPrimitives.ReadUInt16LittleEndian(f[2..]);
            var rate = (int)BinaryPrimitives.ReadUInt32LittleEndian(f[4..]);
            var bits = BinaryPrimitives.ReadUInt16LittleEndian(f[14..]);
            var pcm = tag == 1 || tag == 0xFFFE && f.Length >= 26 && BinaryPrimitives.ReadUInt16LittleEndian(f[24..]) == 1;
            if (!pcm || bits != 16 || channels is < 1 or > 2 || rate is < 8000 or > 96000)
                throw new InvalidDataException($"{name}: a sound is a 16-bit PCM WAV, mono or stereo, 8 to 96 kHz.");
            return (channels, rate);
        }
    }

    // What the writer needs of an actor-mixer's NodeBaseParams (v154): its bus, parent, props and where it ends.
    sealed record NodeBase(uint Bus, uint Parent, Dictionary<byte, uint> Props, int End)
    {
        public static NodeBase Read(byte[] d)
        {
            var r = new Reader(d);
            r.SkipEffects(true);
            r.SkipEffects(false);
            var bus = r.U32();
            var parent = r.U32();
            r.U8();
            var props = r.Props();
            r.SkipRanged();
            r.SkipPositioning();
            r.SkipAux();
            r.Skip(6);
            r.SkipStates();
            r.SkipCurves();
            return new(bus, parent, props, r.At);
        }
    }

    sealed class Reader(byte[] d)
    {
        public int At { get; private set; }
        void Need(int n) { if (At + n > d.Length) throw new InvalidDataException("the mixer's data ends early."); }
        public byte U8() { Need(1); return d[At++]; }
        public ushort U16() { Need(2); var v = BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(At)); At += 2; return v; }
        public uint U32() { Need(4); var v = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(At)); At += 4; return v; }
        public void Skip(int n) { Need(n); At += n; }
        public int Var()
        {
            int b = U8(), v = b & 0x7F;
            while ((b & 0x80) != 0) { b = U8(); v = (v << 7) | (b & 0x7F); }
            return v;
        }

        // Effects {override, count, [bypass,] count x {index, id, bits}}; metadata the same without the bypass byte.
        public void SkipEffects(bool effects)
        {
            U8();
            var n = U8();
            if (n > 0 && effects) U8();
            Skip(6 * n);
        }

        public Dictionary<byte, uint> Props()
        {
            var n = U8();
            var ids = new byte[n];
            for (var i = 0; i < n; i++) ids[i] = U8();
            var props = new Dictionary<byte, uint>();
            foreach (var id in ids) props[id] = U32();
            return props;
        }

        public void SkipRanged() { var n = U8(); Skip(n + 8 * n); }

        public void SkipPositioning()
        {
            var bits = U8();
            if ((bits & 1) == 0 || (bits & 2) == 0) return;
            U8();
            if (((bits >> 5) & 3) == 0) return;
            Skip(5);
            Skip(16 * (int)U32());
            var items = (int)U32();
            Skip(8 * items + 12 * items);
        }

        public void SkipAux() { if ((U8() & 8) != 0) Skip(16); Skip(4); }

        public void SkipStates()
        {
            for (int i = 0, n = Var(); i < n; i++) { Var(); Skip(2); }
            for (int i = 0, n = Var(); i < n; i++)
            {
                Skip(5);
                for (int k = 0, m = Var(); k < m; k++) { Skip(4); Skip(6 * U16()); }
            }
        }

        public void SkipCurves()
        {
            for (int i = 0, n = U16(); i < n; i++)
            {
                Skip(6);
                Var();
                Skip(5);
                Skip(12 * U16());
            }
        }
    }
}
