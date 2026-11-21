using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// A unit's material slots renamed in place (a unit edit's "slots"). The game's weapon paints override materials by slot
// id, so a body on a slot of its own takes them once it carries a slot id the paints name. A slot id stands in the
// unit's material table (ids sorted, each with its material) and in each mesh's material list, and nowhere else in the
// game's weapon units; groups name a mesh's materials by index, so the lists keep their order.
public static class UnitSlots
{
    public static byte[] Rename(byte[] unit, IReadOnlyDictionary<uint, uint> renames)
    {
        var main = unit.ToArray();
        RenameTable(main, renames);
        var meshes = Offset(main, 0x64, 4);
        if (meshes == 0) return main;
        var m = Count(main, meshes, 4);
        for (var i = 0; i < m; i++) RenameMesh(main, meshes + (int)U32(main, meshes + 4 + 4 * i), i, renames);
        return main;
    }

    static void RenameTable(byte[] main, IReadOnlyDictionary<uint, uint> renames)
    {
        var table = Offset(main, 0x70, 4);
        var n = Count(main, table, 12);
        var ids = Enumerable.Range(0, n).Select(i => U32(main, table + 4 + 4 * i)).ToArray();
        var materials = Enumerable.Range(0, n).Select(i => BinaryPrimitives.ReadUInt64LittleEndian(main.AsSpan(table + 4 + 4 * n + 8 * i))).ToArray();
        var missing = renames.Keys.Where(from => !ids.Contains(from)).ToList();
        if (missing.Count > 0) throw new InvalidDataException($"the unit has no material slot 0x{missing[0]:X8}.");
        var renamed = ids.Select(id => renames.GetValueOrDefault(id, id)).ToArray();
        if (renamed.Distinct().Count() != n) throw new InvalidDataException("a slot would take an id the unit already has, or two slots one id.");
        var order = Enumerable.Range(0, n).OrderBy(i => renamed[i]).ToArray();
        for (var k = 0; k < n; k++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(main.AsSpan(table + 4 + 4 * k), renamed[order[k]]);
            BinaryPrimitives.WriteUInt64LittleEndian(main.AsSpan(table + 4 + 4 * n + 8 * k), materials[order[k]]);
        }
    }

    static void RenameMesh(byte[] main, int at, int index, IReadOnlyDictionary<uint, uint> renames)
    {
        if (at < 0 || at + 0x70 > main.Length) throw new InvalidDataException($"mesh {index} of the unit is past its end.");
        var list = at + (int)U32(main, at + 0x6C);
        var count = (int)U32(main, at + 0x68);
        if (count < 0 || list < at || (long)list + 4L * count > main.Length) throw new InvalidDataException($"mesh {index}'s material list is past the unit's end.");
        for (var k = 0; k < count; k++)
            if (renames.TryGetValue(U32(main, list + 4 * k), out var to)) BinaryPrimitives.WriteUInt32LittleEndian(main.AsSpan(list + 4 * k), to);
    }

    static uint U32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at));

    static int Offset(byte[] main, int field, int size)
    {
        if (main.Length < 0x80) throw new InvalidDataException("the unit is shorter than its header.");
        var at = (int)U32(main, field);
        return at >= 0 && (long)at + size <= main.Length ? at : throw new InvalidDataException($"the unit's section at header +0x{field:X} is past its end.");
    }

    static int Count(byte[] main, int at, int each)
    {
        var n = (int)U32(main, at);
        return n >= 0 && at + 4L + (long)each * n <= main.Length ? n : throw new InvalidDataException("a count in the unit runs past its end.");
    }
}
