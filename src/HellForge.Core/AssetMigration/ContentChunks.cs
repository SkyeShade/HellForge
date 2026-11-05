using System.Buffers.Binary;
using System.Security.Cryptography;

namespace HellForge.Core.AssetMigration;

// Content-defined anchors (FastCDC's gear hash) for the source index: the game's resources are keyed by them, and a
// mod's file gives the same anchors for any run of the game's bytes it holds, so the index says which resources to
// compare it with.
public static class ContentChunks
{
    static readonly ulong[] Gear = MakeGear();

    static ulong[] MakeGear()
    {
        var gear = new ulong[256];
        ulong state = 0;
        for (var i = 0; i < gear.Length; i++)
        {
            state += 0x9E3779B97F4A7C15;
            var z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
            gear[i] = z ^ (z >> 31);
        }
        return gear;
    }

    // Anchors: every 64-byte window whose gear hash has its top 11 bits clear (about one per 2 KiB), keyed by the
    // window's own hash. Which windows are anchors depends only on their bytes, never on where a file or chunk starts, so
    // a run of the game's bytes inside a mod gives the same anchors as it does in the game.
    public const int AnchorWindow = 64;
    const int AnchorBits = 11;

    public static List<(int Offset, ulong Hash)> Anchors(ReadOnlySpan<byte> data)
    {
        var anchors = new List<(int, ulong)>();
        ulong h = 0;
        for (var i = 0; i < data.Length; i++)
        {
            h = (h << 1) + Gear[data[i]];
            if (i + 1 < AnchorWindow || (h >> (64 - AnchorBits)) != 0) continue;
            var window = data.Slice(i + 1 - AnchorWindow, AnchorWindow);
            if (!Uniform(window)) anchors.Add((i + 1 - AnchorWindow, Hash(window)));
        }
        return anchors;
    }

    // A window's key in the index: the first 8 bytes of its SHA-256.
    public static ulong Hash(ReadOnlySpan<byte> chunk)
    {
        Span<byte> digest = stackalloc byte[32];
        SHA256.HashData(chunk, digest);
        return BinaryPrimitives.ReadUInt64LittleEndian(digest);
    }

    // A run of one byte value (padding, cleared buffers) says nothing about where a file came from.
    public static bool Uniform(ReadOnlySpan<byte> data) => data.Length == 0 || data.IndexOfAnyExcept(data[0]) < 0;
}
