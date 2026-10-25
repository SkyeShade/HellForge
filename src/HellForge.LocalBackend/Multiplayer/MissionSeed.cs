namespace HellForge.LocalBackend.Multiplayer;

// A lobby record's "seed": the host's selected mission as fixed-width lowercase hex, which the game uploads once a mission
// is selected (game.dll encoder 0xFC3E20, version 0xF3FB; the host's lobby key 10 holds the same string). The Galactic
// Map draws a scanned lobby's marker from it. The game's decoder (0xFC39F0) trusts the counts in the string and writes the
// arrays into a 0xE8-byte struct on its stack, so a record from another player keeps its seed only when it has exactly
// this layout and every count fits its slot.
public static class MissionSeed
{
    public const string Version = "f3fb";

    // Hex digits of the fields before the first count: two u32, three bytes, a u32, a u64, a byte, the mission type's
    // hash (u32) and a byte.
    const int Fixed = 8 + 8 + 2 + 2 + 2 + 8 + 16 + 2 + 8 + 2;

    // The longest valid seed: every array full (the encoder's 40 bytes include the version).
    public const int MaxLength = 2 * (40 + 4 * (32 + 4 + 8) + 8 + 3 + 2);

    public static bool Valid(string? seed)
    {
        if (seed is not { Length: >= 4 and <= MaxLength } || !seed.StartsWith(Version, StringComparison.Ordinal) ||
            !seed.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f')) return false;
        var at = Version.Length + Fixed;
        // u32 array (struct +0x20, room for 32), three byte arrays (+0xA1: 8, +0xAA: 3, +0xAE: 2)
        if (!Array(seed, ref at, 32, 8) || !Array(seed, ref at, 8, 2) || !Array(seed, ref at, 3, 2) || !Array(seed, ref at, 2, 2)) return false;
        at += 2;
        // u32 array (+0xB4, room for 4), two bytes, u32 array (+0xC8, room for 8: the struct ends at 0xE8)
        if (!Array(seed, ref at, 4, 8)) return false;
        at += 4;
        return Array(seed, ref at, 8, 8) && at == seed.Length;
    }

    // A count byte and its entries of `digits` hex digits each, at most `room` of them.
    static bool Array(string seed, ref int at, int room, int digits)
    {
        if (at + 2 > seed.Length) return false;
        var count = Convert.ToInt32(seed.Substring(at, 2), 16);
        if (count > room) return false;
        at += 2 + count * digits;
        return at <= seed.Length;
    }
}
