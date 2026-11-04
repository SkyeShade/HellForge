using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using HellForge.Core;

// HellForge.Cli crash-dump: a memory dump of a crashed game, written from outside it. The mod loader's crash note
// (native/mod-loader/crash_notes.c WriteCrashDump) starts this with the game's process, the crashed thread and the
// address of its EXCEPTION_POINTERS, and waits: a process dumping itself from its own exception handler can deadlock on
// a lock one of its suspended threads holds. A full-memory dump is written unless it would leave the session folder's
// file system with less than Reserve free; then the small dump (threads and modules, no game memory) is written and said
// so in the session's modloader.log.
static class CrashDump
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct ExceptionInformation
    {
        public uint ThreadId;
        public IntPtr ExceptionPointers;
        public int ClientPointers;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr process, uint processId, SafeHandle file, uint type, ref ExceptionInformation exception, IntPtr user, IntPtr callback);

    // Full memory, handle data, thread info, unloaded modules.
    const uint DumpType = 0x2 | 0x4 | 0x1000 | 0x20;

    // The same without full memory: the threads' stacks, handles and the module list.
    const uint SmallDumpType = 0x4 | 0x1000 | 0x20;

    // Kept free besides the dump, so one crash cannot fill the disk.
    internal const long Reserve = 2L << 30;

    // A full dump is about the game's committed memory: its private bytes, or its working set where that is larger.
    internal static long FullDumpBytes(long privateBytes, long workingSet) => Math.Max(privateBytes, workingSet);

    // Whether a full dump of fullBytes would leave less than Reserve free. An unreadable file system keeps the full dump,
    // as DiskSpace.Require does.
    internal static bool SmallDump(long? freeBytes, long fullBytes) =>
        freeBytes is { } free && free - fullBytes < Reserve;

    // One line in the session's mod loader log, which the loader has open for append while it waits for this process.
    // Another writer's failure must not cost the dump.
    static void SessionNote(string folder, string line)
    {
        try
        {
            using var stream = new FileStream(Path.Combine(folder, "modloader.log"), FileMode.Append, FileAccess.Write,
                FileShare.ReadWrite | FileShare.Delete);
            using var writer = new StreamWriter(stream, new UTF8Encoding(false));
            writer.WriteLine(Privacy.Scrub(line));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    public static object Write(int processId, uint threadId, ulong pointers, string output)
    {
        using var process = Process.GetProcessById(processId);
        var folder = Path.GetDirectoryName(output)!;
        var space = DiskSpace.ForFolder(folder);
        var fullBytes = FullDumpBytes(process.PrivateMemorySize64, process.WorkingSet64);
        var small = SmallDump(space.FreeBytes, fullBytes);
        using var file = File.Create(output);
        var exception = new ExceptionInformation { ThreadId = threadId, ExceptionPointers = (IntPtr)(long)pointers, ClientPointers = 1 };
        if (!MiniDumpWriteDump(process.Handle, (uint)processId, file.SafeFileHandle, small ? SmallDumpType : DumpType,
            ref exception, IntPtr.Zero, IntPtr.Zero))
            throw new IOException("MiniDumpWriteDump failed: " + new Win32Exception(Marshal.GetLastWin32Error()).Message);
        file.Flush();
        if (small)
            SessionNote(folder, $"crash dump: small dump: a full dump of {DiskSpace.GB(fullBytes)} would leave less than " +
                $"{Reserve / (1L << 30)} GB free on {space.Name} ({DiskSpace.GB(space.FreeBytes ?? 0)} free)");
        return new { dump = output, megabytes = file.Length / (1024 * 1024), small };
    }
}
