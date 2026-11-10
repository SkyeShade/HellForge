using System.Buffers.Binary;
using System.Text;
using HellForge.Core;
using HellForge.Core.GameData;

// Sound banks of a mod's own (SoundBank, a data spec's "sounds") against a synthetic game bank with a root mixer, the
// attenuation it names and a nested mixer, laid out as the game's version 154 banks are.
static class SoundBankTests
{
    const uint Attenuation = 0x0F1679CE, Root = 0x2D200575, Nested = 0x3958659E, Bus = 0x78DBF1F3;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A sound bank is the game's bank version: header key, PCM sounds under a copy of a root mixer, events by name", () =>
        {
            var (bank, dep) = SoundBank.Build("test/mod/rifle_sounds", [new("test_rifle_fire", Wav(1, 48000, 16), -3), new("test_rifle_click", Wav(2, 44100, 16), 0)],
                GameBank(), Root);
            uint U32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at));
            check(U32(bank, 0) == 0x78A3F4A5 && U32(bank, 4) == bank.Length - 16 && BinaryPrimitives.ReadUInt64LittleEndian(bank.AsSpan(8)) == EngineHash.Murmur64A("test/mod/rifle_sounds"u8),
                "The Stingray header is not {magic, size, name}.");
            check(Encoding.ASCII.GetString(bank, 16, 4) == "BKHD" && (U32(bank, 0x18) ^ 0x9211BCAC) == 154 && (U32(bank, 0x1C) ^ 0xA3107038) == SoundBank.Id("test/mod/rifle_sounds") &&
                (U32(bank, 0x20) ^ 0xC9CF6298) == SoundBank.Id("SFX"), "The header is not version 154 with the bank's id under the plugin's key.");
            var objects = SoundBank.Objects(bank);
            check(objects.Select(o => o.Type).SequenceEqual(new byte[] { 14, 2, 2, 7, 3, 3, 4, 4 }), "Not attenuation, sounds, mixer, actions, events: " + string.Join(",", objects.Select(o => o.Type)));
            var mixer = objects.Single(o => o.Type == 7);
            var sounds = objects.Where(o => o.Type == 2).ToList();
            check(objects[0].Id == Attenuation && mixer.Id == SoundBank.Id("test/mod/rifle_sounds/mixer") && U32(mixer.Data, 4) == Bus && U32(mixer.Data, 8) == 0,
                "The mixer copy lost its bus, gained a parent, or its attenuation is missing.");
            var children = mixer.Data.AsSpan(mixer.Data.Length - 12);
            check(BinaryPrimitives.ReadUInt32LittleEndian(children) == 2 && sounds.All(s => U32(s.Data, 0) == 0x00010001 && U32(s.Data, 26) == mixer.Id) &&
                BinaryPrimitives.ReadUInt32LittleEndian(children[4..]) == sounds[0].Id, "The sounds are not PCM children of the mixer.");
            check(sounds[0].Data[31] == 1 && sounds[0].Data[32] == 0x05 && BitConverter.ToSingle(sounds[0].Data, 33) == -3f && sounds[1].Data[31] == 0, "The gain is not make-up gain.");
            var events = objects.Where(o => o.Type == 4).Select(o => o.Id).ToList();
            check(events.SequenceEqual(new[] { SoundBank.Id("test_rifle_fire"), SoundBank.Id("TEST_RIFLE_CLICK") }), "Event ids are not their names' short ids.");
            check(objects.Where(o => o.Type == 3).All(a => a.Data[0] == 3 && a.Data[1] == 4 && U32(a.Data, 10) == SoundBank.Id("test/mod/rifle_sounds")), "An action is not a Play in this bank.");
            check(Encoding.ASCII.GetString(dep, 8, dep.Length - 9) == "test/mod/rifle_sounds" && U32(dep, 4) == dep.Length - 8 && dep[^1] == 0, "The dependency does not name the bank.");
            var data = Chunk(bank, "DATA");
            check(Encoding.ASCII.GetString(data, 0, 4) == "RIFF" && BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(20)) == 0xFFFE &&
                (U32(data, 40) == 0x4101 || U32(data, 40) == 0x3102), "The media is not a Wwise PCM file with its channel configuration.");

            reject(() => SoundBank.Build("test/mod/b", [new("a", Wav(1, 48000, 16), 0)], GameBank(), Nested));
            reject(() => SoundBank.Build("test/mod/b", [new("a", Wav(1, 48000, 16), 0)], GameBank(), 0x1234));
            reject(() => SoundBank.Build("test/mod/b", [new("a", Wav(1, 48000, 8), 0)], GameBank(), Root));
            reject(() => SoundBank.Build("test/mod/b", [new("a", Wav(1, 48000, 16), 0), new("A", Wav(1, 48000, 16), 0)], GameBank(), Root));
            reject(() => SoundBank.Build("test/mod/b", [new("a", Wav(1, 48000, 16), 0)], GameBank(attenuation: false), Root));
        });

        test("A data spec's sounds build a bank and its dependency from the mod's WAVs, with a game bank's mixer", () =>
        {
            var game = Path.Combine(root, "sound-game");
            var archive = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.WwiseBank, EngineHash.Murmur64A("content/audio/test_weapon"u8))] = new(GameBank(), [], []),
            });
            DataBuilderTests.WriteBundles(game, new Dictionary<string, byte[]> { ["1111111111111111"] = archive.Archive, ["1111111111111111.stream"] = archive.Stream });
            var mod = Directory.CreateDirectory(Path.Combine(root, "sound-mod", "sounds")).Parent!.FullName;
            File.WriteAllBytes(Path.Combine(mod, "sounds", "fire.wav"), Wav(1, 48000, 16));
            DataSpec Spec(string sounds)
            {
                File.WriteAllText(Path.Combine(mod, DataSpecs.FileName), $$"""{"format":1,"patches":[{"archives":["1111111111111111"],"resources":[{{sounds}}]}]}""");
                return DataSpecs.Load(mod)!;
            }
            var spec = Spec("""{"sounds":[{"event":"test_rifle_fire","wav":"sounds/fire.wav","gain":-6}],"name":"test/mod/rifle_sounds","mixer_bank":"content/audio/test_weapon","mixer":"0x2D200575"}""");
            check(spec.Inputs.Contains("sounds/fire.wav") && spec.Patches[0].Resources[0] is SoundsResource { Mixer: Root, Sounds: [{ Gain: -6 }] }, "The sounds are not part of the spec.");
            foreach (var (bad, reason) in new[] {
                ("""{"sounds":[{"event":"a b","wav":"sounds/fire.wav"}],"name":"test/mod/s","mixer_bank":"x/y","mixer":"0x1"}""", "event is a name"),
                ("""{"sounds":[{"event":"a","wav":"sounds/none.wav"}],"name":"test/mod/s","mixer_bank":"x/y","mixer":"0x1"}""", "is missing"),
                ("""{"sounds":[{"event":"a","wav":"sounds/fire.wav","gain":40}],"name":"test/mod/s","mixer_bank":"x/y","mixer":"0x1"}""", "gain"),
                ("""{"sounds":[],"name":"test/mod/s","mixer_bank":"x/y","mixer":"0x1"}""", "no sounds"),
                ("""{"sounds":[{"event":"a","wav":"sounds/fire.wav"}],"name":"test/mod/s","mixer_bank":"x/y","mixer":"root"}""", "mixer is") })
            {
                try { Spec(bad); throw new Exception("Accepted: " + bad); }
                catch (InvalidDataException error) { check(error.Message.Contains(reason), "Refused for another reason: " + error.Message); }
            }

            using var reader = new GameDataReader(game);
            var output = Path.Combine(root, "sound-out");
            ModDataBuilder.Build(spec, mod, reader, output);
            var built = ResourceArchive.Read(File.ReadAllBytes(Path.Combine(output, "1111111111111111.patch_0")), [], []);
            var name = EngineHash.Murmur64A("test/mod/rifle_sounds"u8);
            check(built.TryGetValue(new(ResourceTypes.WwiseBank, name), out var bank) && SoundBank.Objects(bank.Main).Count(o => o.Type == 4) == 1 &&
                built.ContainsKey(new(ResourceTypes.WwiseDependency, name)), "The patch lacks the bank or its dependency.");
        });
    }

    // A 16-bit or 8-bit PCM WAV of a decaying tone.
    static byte[] Wav(int channels, int rate, int bits)
    {
        var frames = rate / 50;
        using var m = new MemoryStream();
        using var w = new BinaryWriter(m);
        var size = frames * channels * bits / 8;
        w.Write("RIFF"u8); w.Write(36 + size); w.Write("WAVEfmt "u8); w.Write(16); w.Write((short)1); w.Write((short)channels); w.Write(rate);
        w.Write(rate * channels * bits / 8); w.Write((short)(channels * bits / 8)); w.Write((short)bits); w.Write("data"u8); w.Write(size);
        for (var i = 0; i < frames * channels; i++)
            if (bits == 16) w.Write((short)(8000 * Math.Sin(i * 0.05) * (1 - (double)i / (frames * channels))));
            else w.Write((byte)(128 + 60 * Math.Sin(i * 0.05)));
        return m.ToArray();
    }

    static byte[] Chunk(byte[] bank, string tag)
    {
        for (var at = 16; at + 8 <= bank.Length; at += 8 + BinaryPrimitives.ReadInt32LittleEndian(bank.AsSpan(at + 4)))
            if (Encoding.ASCII.GetString(bank, at, 4) == tag) return bank.AsSpan(at + 8, BinaryPrimitives.ReadInt32LittleEndian(bank.AsSpan(at + 4))).ToArray();
        throw new InvalidDataException("no " + tag);
    }

    // A game bank: its attenuation, a root mixer (no parent, its own bus, the attenuation as property 0x4B, one
    // game-parameter curve) and a mixer nested under it.
    static byte[] GameBank(bool attenuation = true)
    {
        static byte[] Mixer(uint bus, uint parent, bool curve)
        {
            using var m = new MemoryStream();
            using var w = new BinaryWriter(m);
            w.Write(0); w.Write(bus); w.Write(parent); w.Write((byte)0);
            w.Write((byte)1); w.Write((byte)0x4B); w.Write(Attenuation);
            w.Write((byte)0); w.Write((byte)0x03); w.Write((byte)0x01); w.Write((byte)0); w.Write(0);
            w.Write(new byte[] { 0, 1, 0, 0, 3, 0 });
            w.Write((byte)0); w.Write((byte)0);
            w.Write((ushort)(curve ? 1 : 0));
            if (curve) { w.Write(0x11111111); w.Write((byte)0); w.Write((byte)0); w.Write((byte)5); w.Write(0x22222222); w.Write((byte)0); w.Write((ushort)1); w.Write(0f); w.Write(1f); w.Write(4); }
            w.Write(1); w.Write(parent == 0 ? Nested : 0x44444444u);
            return m.ToArray();
        }
        var objects = new List<(byte, uint, byte[])> { (7, Root, Mixer(Bus, 0, true)), (7, Nested, Mixer(0, Root, false)) };
        if (attenuation) objects.Insert(0, (14, Attenuation, Enumerable.Repeat((byte)0x5A, 40).ToArray()));
        using var hirc = new MemoryStream();
        hirc.Write(BitConverter.GetBytes(objects.Count));
        foreach (var (type, id, data) in objects) { hirc.WriteByte(type); hirc.Write(BitConverter.GetBytes(data.Length + 4)); hirc.Write(BitConverter.GetBytes(id)); hirc.Write(data); }
        using var body = new MemoryStream();
        body.Write("BKHD"u8); body.Write(BitConverter.GetBytes(44)); body.Write(new byte[44]);
        body.Write("HIRC"u8); body.Write(BitConverter.GetBytes((int)hirc.Length)); body.Write(hirc.ToArray());
        return [.. BitConverter.GetBytes(0x78A3F4A5u), .. BitConverter.GetBytes((uint)body.Length), .. BitConverter.GetBytes(0x5555ul), .. body.ToArray()];
    }
}
