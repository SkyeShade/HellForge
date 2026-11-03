using System.Runtime.InteropServices;
using System.Text;

namespace HellForge.SdkShimGen;

// Loads the steam_api64.dll stand-in next to a copy of the original (as the session places them) and checks that
// forwarded exports resolve into the original, that both accessors hand out the HellForge storage interface, and that its
// methods keep files in HELLFORGE_CLOUD_DIR and refuse names that leave it. Steam itself is never started.
public static unsafe partial class CloudSmoke
{
    const string StorageVersion = "STEAMREMOTESTORAGE_INTERFACE_VERSION016";
    const int FileWrite = 0, FileRead = 1, FileForget = 5, FileDelete = 6, FileExists = 13, FilePersisted = 14, GetFileSize = 15,
        GetFileCount = 18, IsCloudEnabledForAccount = 21, IsCloudEnabledForApp = 22;

    [LibraryImport("kernel32.dll", EntryPoint = "SetDllDirectoryW", StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetDllDirectory(string path);

    [LibraryImport("kernel32.dll", EntryPoint = "GetModuleHandleW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint GetModuleHandle(string name);

    public static int Run(string standIn, string original)
    {
        var root = Path.Combine(Path.GetTempPath(), "hellforge-cloud-smoke-" + Guid.NewGuid().ToString("N"));
        var bin = Directory.CreateDirectory(Path.Combine(root, "bin")).FullName;
        var cloud = Directory.CreateDirectory(Path.Combine(root, "cloud")).FullName;
        var log = Path.Combine(root, "steam-cloud.log");
        File.Copy(standIn, Path.Combine(bin, "steam_api64.dll"));
        File.Copy(original, Path.Combine(bin, "steam_api64_hellforge.dll"));
        Environment.SetEnvironmentVariable("HELLFORGE_CLOUD_DIR", cloud);
        Environment.SetEnvironmentVariable("HELLFORGE_CLOUD_LOG", log);
        var problems = new List<string>();
        void Expect(bool condition, string description) { if (!condition) problems.Add(description); }

        // Forwarders are resolved by the loader's search, which the game's own folder satisfies; here it is set explicitly.
        SetDllDirectory(bin);
        var module = NativeLibrary.Load(Path.Combine(bin, "steam_api64.dll"));
        // one the stand-in does not write itself (it writes SteamAPI_GetHSteamUser, for bridge mode)
        var forwarded = NativeLibrary.GetExport(module, "SteamGameServer_GetHSteamUser");
        var real = GetModuleHandle("steam_api64_hellforge.dll");
        Expect(real != 0 && forwarded != 0 && !InModule(forwarded, module) && InModule(forwarded, real),
            "A forwarded export resolves into the original DLL");

        var accessor = (delegate* unmanaged<nint>)NativeLibrary.GetExport(module, "SteamAPI_SteamRemoteStorage_v016");
        var find = (delegate* unmanaged<int, byte*, nint>)NativeLibrary.GetExport(module, "SteamInternal_FindOrCreateUserInterface");
        var storage = accessor();
        nint found;
        fixed (byte* version = Encoding.ASCII.GetBytes(StorageVersion + "\0")) found = find(1, version);
        Expect(storage != 0 && found == storage && !InModule(storage, real), "Both accessors hand out the HellForge storage interface");
        if (storage == 0) return Report(problems);
        var vtable = *(nint**)storage;

        var write = (delegate* unmanaged<nint, byte*, byte*, int, byte>)vtable[FileWrite];
        var read = (delegate* unmanaged<nint, byte*, byte*, int, int>)vtable[FileRead];
        var forget = (delegate* unmanaged<nint, byte*, byte>)vtable[FileForget];
        var delete = (delegate* unmanaged<nint, byte*, byte>)vtable[FileDelete];
        var exists = (delegate* unmanaged<nint, byte*, byte>)vtable[FileExists];
        var persisted = (delegate* unmanaged<nint, byte*, byte>)vtable[FilePersisted];
        var size = (delegate* unmanaged<nint, byte*, int>)vtable[GetFileSize];
        var count = (delegate* unmanaged<nint, int>)vtable[GetFileCount];
        var accountEnabled = (delegate* unmanaged<nint, byte>)vtable[IsCloudEnabledForAccount];
        var appEnabled = (delegate* unmanaged<nint, byte>)vtable[IsCloudEnabledForApp];

        var save = Encoding.ASCII.GetBytes("synthetic cloud save");
        var buffer = new byte[64];
        Expect(accountEnabled(storage) != 0 && appEnabled(storage) != 0, "Cloud storage reports itself enabled");
        fixed (byte* name = "testament_new.sav\0"u8, data = save, into = buffer)
        {
            Expect(exists(storage, name) == 0 && size(storage, name) == 0 && read(storage, name, into, buffer.Length) == 0,
                "A missing file does not exist and reads nothing");
            Expect(write(storage, name, data, save.Length) != 0, "FileWrite succeeds");
            Expect(File.Exists(Path.Combine(cloud, "testament_new.sav")) && File.ReadAllBytes(Path.Combine(cloud, "testament_new.sav")).SequenceEqual(save),
                "The file lands in HELLFORGE_CLOUD_DIR");
            Expect(exists(storage, name) != 0 && persisted(storage, name) != 0 && size(storage, name) == save.Length, "The file exists with its size");
            var got = read(storage, name, into, buffer.Length);
            Expect(got == save.Length && buffer.AsSpan(0, got).SequenceEqual(save), "FileRead returns the bytes written");
            Expect(read(storage, name, into, 4) == 4, "FileRead stops at the buffer size");
            Expect(forget(storage, name) != 0 && exists(storage, name) != 0, "FileForget keeps the file");
            Expect(delete(storage, name) != 0 && exists(storage, name) == 0 && size(storage, name) == 0, "FileDelete removes it");
        }
        foreach (var unsafeName in new[] { "../escape.sav", "..\\escape.sav", "sub/file.sav", "C:file.sav", "..", "" })
            fixed (byte* name = Encoding.UTF8.GetBytes(unsafeName + "\0"), data = save)
                Expect(write(storage, name, data, save.Length) == 0, "FileWrite refuses " + (unsafeName.Length > 0 ? unsafeName : "an empty name"));
        Expect(!File.Exists(Path.Combine(root, "escape.sav")) && Directory.GetFiles(cloud).Length == 0, "Nothing is written outside HELLFORGE_CLOUD_DIR");
        Expect(count(storage) == 0, "An unused method returns zero");
        Expect(File.Exists(log) && ReadShared(log).Contains("FileWrite testament_new.sav 20 bytes ok"), "Calls are logged to HELLFORGE_CLOUD_LOG");
        Expect(OpensDated(log), "The log opens with its dated line");
        return Report(problems);
    }

    // Whether a native log exists and its first line is its dated one (native/common/hf_log_header.h).
    internal static bool OpensDated(string path) => File.Exists(path) && System.Text.RegularExpressions.Regex.IsMatch(ReadShared(path).Split('\n')[0],
        @"^\d\d:\d\d:\d\d\.\d{3} log opened \d{4}-\d\d-\d\d \d\d:\d\d:\d\d local time \(UTC[+-]\d\d:\d\d\); the times below are UTC$");

    static bool InModule(nint address, nint module) =>
        module != 0 && address >= module && address < module + *(int*)(module + *(int*)(module + 0x3c) + 0x50);

    static string ReadShared(string path)
    {
        using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
        return reader.ReadToEnd();
    }

    static int Report(List<string> problems)
    {
        Console.WriteLine($"steam_api64 stand-in smoke: {(problems.Count == 0 ? "passed" : problems.Count + " problem(s)")}");
        foreach (var problem in problems) Console.Error.WriteLine("  " + problem);
        return problems.Count == 0 ? 0 : 1;
    }
}
