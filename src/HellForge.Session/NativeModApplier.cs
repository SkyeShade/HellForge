using System.Buffers.Binary;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace HellForge.Session;

using HellForge.Core;

// Applies network field type edits to the running copied game until it exits (docs/modding/data.md, "Native edits in the manifest").
// Component writes now run only in the native loader's entity-load pass. The component helpers below remain
// available for existing diagnostic fixtures; Run never invokes them or periodically restores component fields.
public static class NativeModApplier
{
    const int NetworkConfigSlot = 0x1A10268; // helldivers2.exe: [[slot]] is the loaded network configuration
    public const uint ProcessAccess = 0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x00100000;

    // folders: the session's mods with network type edits as mods-stage staged them ("nativeFolders"; after a mod sync the
    // host's, some in mods/.synced); null reads the installation's enabled mods (a mods-stage from before it).
    public static int Run(InstallLayout install, IReadOnlyList<string>? folders, int pid, string expectedExe, string logPath, string statusPath)
    {
        using var log = new StreamWriter(logPath, append: true) { AutoFlush = true };
        void Log(string text) => log.WriteLine($"{DateTime.UtcNow:HH\\:mm\\:ss.fff} {Privacy.Scrub(text)}");
        var native = new List<ModManifest>();
        if (folders == null)
        {
            var (mods, problems) = ModCatalog.Load(Path.Combine(install.Root, "mods"));
            foreach (var problem in problems) Log("Mod not loaded: " + problem);
            native.AddRange(ModCatalog.Active(mods));
        }
        else
            foreach (var folder in folders)
            {
                try { native.Add(ModCatalog.Parse(folder, EntityProfile.Current)); }
                catch (Exception error) when (error is InvalidDataException or JsonException or IOException) { Log($"Mod not loaded: {folder}: {error.Message}"); }
            }
        native = native.Where(m => m.NetworkFieldTypes.Count > 0).ToList();
        if (native.Count == 0) { Log("No native mods."); return 0; }
        var process = OpenProcess(ProcessAccess, false, pid);
        if (process == 0) { Log($"Could not open the game process ({Marshal.GetLastWin32Error()})."); return 1; }
        try
        {
            if (!string.Equals(ImagePath(process), Path.GetFullPath(expectedExe), StringComparison.OrdinalIgnoreCase))
            { Log("The process is not the copied game."); return 1; }
            var types = native.SelectMany(m => m.NetworkFieldTypes.Select(t => (Mod: m.Id, Edit: t))).ToList();
            var state = new SortedDictionary<string, string>(StringComparer.Ordinal);
            void Report(string key, string value)
            {
                if (state.TryGetValue(key, out var old) && old == value) return;
                state[key] = value;
                Log($"{key}: {value}");
                File.WriteAllText(statusPath, JsonSerializer.Serialize(state, SessionJson.Default.SortedDictionaryStringString));
            }
            Log($"Applying {types.Count} network type edit(s) from " +
                string.Join(", ", native.Select(m => $"{m.Id} {m.Version}")) + ".");
            long exe = 0;
            var clock = Stopwatch.StartNew();
            while (WaitForSingleObject(process, 0) != 0)
            {
                try
                {
                    if (exe == 0) exe = MainModule(process, expectedExe);
                    if (exe != 0) foreach (var (mod, edit) in types) Report($"{mod}: network type {edit.Name}", NetworkType(process, exe, edit));
                }
                catch (Win32Exception) { } // The game is changing a page right now; try again.
                Thread.Sleep(clock.Elapsed.TotalMinutes < 3 ? 100 : 1000);
            }
            Log("The game exited.");
            return 0;
        }
        finally { CloseHandle(process); }
    }

    // "applied", "waiting" (not loaded yet) or why it was left alone.
    public static string NetworkType(nint process, long exe, NetworkFieldTypeEdit edit)
    {
        var holder = ReadPointer(process, exe + NetworkConfigSlot);
        var config = holder == 0 ? 0 : ReadPointer(process, holder);
        if (config == 0) return "waiting";
        var count = BinaryPrimitives.ReadUInt32LittleEndian(Read(process, config + 0x10, 4));
        var types = ReadPointer(process, config + 0x18);
        if (count is 0 or > 8192 || types == 0) return "waiting";
        var rows = Read(process, types, (int)count * 0x18);
        var hash = EngineHash.Id32(edit.Name);
        var matches = Enumerable.Range(0, (int)count).Where(i => BinaryPrimitives.ReadUInt32LittleEndian(rows.AsSpan(i * 0x18)) == hash).ToList();
        if (matches.Count != 1) return matches.Count == 0 ? "waiting" : "left alone: the name is not unique";
        var row = matches[0] * 0x18;
        int kind = rows[row + 12], bits = rows[row + 13], min = BinaryPrimitives.ReadInt32LittleEndian(rows.AsSpan(row + 16));
        if (kind == edit.ExpectKind && min == edit.ExpectMin && bits == edit.Bits) return $"applied ({edit.ExpectBits} -> {edit.Bits} bits)";
        if (kind != edit.ExpectKind || min != edit.ExpectMin || bits != edit.ExpectBits)
            return $"left alone: found kind {kind}, bits {bits}, min {min}";
        WriteData(process, types + row + 13, [(byte)edit.Bits]);
        return Read(process, types + row + 13, 1)[0] == edit.Bits ? $"applied ({edit.ExpectBits} -> {edit.Bits} bits)" : "write did not stick";
    }

    public static IEnumerable<(string Field, string Outcome)> Component(nint process, long region, EntityProfile profile, ComponentWrite write)
    {
        if (region == 0) return write.Fields.Select(f => (f.Name, "waiting"));
        var schema = profile.Components[write.Component];
        var index = Read(process, region + schema.Offset - 4, 32 + schema.RecordOffset);
        if (BinaryPrimitives.ReadUInt32LittleEndian(index) != schema.Index || Convert.ToHexString(index, 4, 28) != schema.Header.ToUpperInvariant() ||
            schema.RecordOffset != schema.Indices * 16 || schema.RecordOffset + (long)schema.Records * schema.Stride > BinaryPrimitives.ReadUInt32LittleEndian(index.AsSpan(20)))
            return write.Fields.Select(f => (f.Name, "left alone: the component section is not as reviewed"));
        int selected = -1, owners = 0;
        var records = new List<int>();
        for (var row = 0; row < schema.Indices; row++)
        {
            var at = 32 + row * 16;
            var resource = BinaryPrimitives.ReadUInt64LittleEndian(index.AsSpan(at));
            var record = (int)BinaryPrimitives.ReadUInt32LittleEndian(index.AsSpan(at + 8));
            if (record >= schema.Records || BinaryPrimitives.ReadUInt32LittleEndian(index.AsSpan(at + 12)) != 0)
                return write.Fields.Select(f => (f.Name, "left alone: the component index is not as reviewed"));
            if (resource == write.Resource) { owners++; selected = record; }
            if (resource != 0) records.Add(record);
        }
        if (owners != 1) return write.Fields.Select(f => (f.Name, owners == 0 ? "waiting" : "left alone: the entity owns several records"));
        // The record must belong to this entity alone, or the edit would change others too.
        if (records.Count(r => r == selected) != 1) return write.Fields.Select(f => (f.Name, "left alone: the record is shared with another entity"));
        var address = region + schema.Offset + 28 + schema.RecordOffset + (long)selected * schema.Stride;
        var results = new List<(string, string)>();
        foreach (var field in write.Fields)
        {
            var value = BinaryPrimitives.ReadUInt32LittleEndian(Read(process, address + field.Offset, 4));
            if (value == field.Value) { results.Add((field.Name, $"applied ({field.Expect} -> {field.Value})")); continue; }
            if (value != field.Expect) { results.Add((field.Name, $"left alone: found {value}, expected {field.Expect}")); continue; }
            var bytes = new byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(bytes, field.Value);
            WriteData(process, address + field.Offset, bytes);
            results.Add((field.Name, BinaryPrimitives.ReadUInt32LittleEndian(Read(process, address + field.Offset, 4)) == field.Value
                ? $"applied ({field.Expect} -> {field.Value})" : "write did not stick"));
        }
        return results;
    }

    // The entity region: one private, committed allocation of exactly the profile's size starting with its map header.
    public static long FindEntityRegion(nint process, EntityProfile profile)
    {
        long found = 0, address = 0x10000;
        var header = Convert.FromHexString(profile.MapHeader);
        while (VirtualQueryEx(process, address, out var info, Marshal.SizeOf<MemoryInfo>()) != 0)
        {
            var next = info.BaseAddress + info.RegionSize;
            if (info.State == 0x1000 && info.Type == 0x20000 && info.BaseAddress == info.AllocationBase && info.Protect is 2 or 4 &&
                info.RegionSize == profile.EntityRegionSize)
            {
                try
                {
                    if (Read(process, info.BaseAddress, 28).AsSpan().SequenceEqual(header))
                    {
                        if (found != 0) return 0; // Two candidates: not as reviewed.
                        found = info.BaseAddress;
                    }
                }
                catch (Win32Exception) { }
            }
            if (next <= address) break;
            address = next;
        }
        return found;
    }

    static bool IsEntityRegion(nint process, long region, EntityProfile profile)
    {
        try { return Read(process, region, 28).AsSpan().SequenceEqual(Convert.FromHexString(profile.MapHeader)); }
        catch (Win32Exception) { return false; }
    }

    static long MainModule(nint process, string expectedExe)
    {
        var modules = new nint[1024];
        if (!EnumProcessModulesEx(process, modules, modules.Length * IntPtr.Size, out var needed, 0x02)) return 0;
        var name = new StringBuilder(1024);
        for (var i = 0; i < Math.Min(needed / IntPtr.Size, modules.Length); i++)
        {
            name.Clear();
            if (GetModuleFileNameEx(process, modules[i], name, name.Capacity) != 0 &&
                string.Equals(name.ToString(), Path.GetFullPath(expectedExe), StringComparison.OrdinalIgnoreCase)) return modules[i];
        }
        return 0;
    }

    static string ImagePath(nint process)
    {
        var name = new StringBuilder(1024);
        var size = name.Capacity;
        return QueryFullProcessImageName(process, 0, name, ref size) ? name.ToString() : "";
    }

    static byte[] Read(nint process, long address, int size)
    {
        var buffer = new byte[size];
        if (!ReadProcessMemory(process, address, buffer, size, out var read) || read != size) throw new Win32Exception(Marshal.GetLastWin32Error());
        return buffer;
    }

    static long ReadPointer(nint process, long address) => BinaryPrimitives.ReadInt64LittleEndian(Read(process, address, 8));

    // Data pages only: a read-only page is made writable for the write and restored.
    static void WriteData(nint process, long address, byte[] bytes)
    {
        if (!VirtualProtectEx(process, address, bytes.Length, 0x04, out var previous)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            if (!WriteProcessMemory(process, address, bytes, bytes.Length, out var written) || written != bytes.Length)
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally { VirtualProtectEx(process, address, bytes.Length, previous, out _); }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MemoryInfo
    {
        public long BaseAddress, AllocationBase;
        public uint AllocationProtect, Alignment1;
        public long RegionSize;
        public uint State, Protect, Type, Alignment2;
    }

    [DllImport("kernel32.dll", SetLastError = true)] public static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(nint handle);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(nint handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadProcessMemory(nint process, long address, byte[] buffer, int size, out int read);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteProcessMemory(nint process, long address, byte[] buffer, int size, out int written);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool VirtualProtectEx(nint process, long address, int size, uint protection, out uint previous);
    [DllImport("kernel32.dll", SetLastError = true)] static extern nint VirtualQueryEx(nint process, long address, out MemoryInfo info, int size);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageName(nint process, int flags, StringBuilder name, ref int size);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool EnumProcessModulesEx(nint process, [Out] nint[] modules, int size, out int needed, uint filter);
    [DllImport("psapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern uint GetModuleFileNameEx(nint process, nint module, StringBuilder name, int size);
}
