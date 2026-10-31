using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

// Who is in the capture's way when it cannot start: the program listening on its port (iphlpapi's TCP table with owning
// process ids) and the programs holding a file open (the Restart Manager, which Windows' own installers use to find
// them). Each answer is a short "name.exe (process 1234)" list for proxy.log and the launcher's message; empty when
// Windows does not say.
static class StartProblems
{
    public const string ProcessName = "HellForge.CaptureProxy";

    // Other capture processes: one from an earlier sync whose launcher closed or crashed still holds port 443 and, when
    // it times out, removes the hosts line this run adds. Being elevated, this run can end them. Null when every one
    // ended, else what is still running.
    public static string? StopOtherCaptures(Action<string> log)
    {
        var left = new List<string>();
        foreach (var process in Process.GetProcessesByName(ProcessName))
        {
            using (process)
            {
                if (process.Id == Environment.ProcessId) continue;
                try
                {
                    log($"an earlier capture is still running (process {process.Id}): stopping it");
                    process.Kill();
                    if (!process.WaitForExit(5000)) left.Add($"process {process.Id}");
                }
                catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
                {
                    if (!process.HasExited) left.Add($"process {process.Id}: {error.Message}");
                }
            }
        }
        return left.Count == 0 ? null : string.Join(", ", left);
    }

    public static string Describe(int pid)
    {
        if (pid == 4) return "Windows' web service (process 4), used by IIS and some programs' own web servers";
        try
        {
            using var process = Process.GetProcessById(pid);
            return $"{process.ProcessName}.exe (process {pid})";
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException)
        {
            return $"process {pid}";
        }
    }

    // The process listening on an IPv4 TCP port, on any address; null when none is found.
    public static int? PortOwner(int port)
    {
        try
        {
            var size = 0;
            GetExtendedTcpTable(IntPtr.Zero, ref size, false, AfInet, TcpTableOwnerPidListener, 0);
            var table = Marshal.AllocHGlobal(size);
            try
            {
                if (GetExtendedTcpTable(table, ref size, false, AfInet, TcpTableOwnerPidListener, 0) != 0) return null;
                var rows = Marshal.ReadInt32(table);
                // MIB_TCPROW_OWNER_PID: state, local address, local port, remote address, remote port, owning pid (6 DWORDs).
                for (var i = 0; i < rows; i++)
                {
                    var row = table + 4 + i * 24;
                    var raw = (uint)Marshal.ReadInt32(row + 8);
                    var local = (int)(((raw & 0xFF) << 8) | ((raw >> 8) & 0xFF));
                    if (local == port) return Marshal.ReadInt32(row + 20);
                }
                return null;
            }
            finally { Marshal.FreeHGlobal(table); }
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or OutOfMemoryException)
        {
            return null;
        }
    }

    // The programs that have a file open; empty when none is found or the Restart Manager does not answer.
    public static IReadOnlyList<string> FileHolders(string path)
    {
        try
        {
            var key = new StringBuilder(33);
            if (RmStartSession(out var session, 0, key) != 0) return [];
            try
            {
                if (RmRegisterResources(session, 1, [path], 0, null, 0, null) != 0) return [];
                uint count = 0, reasons = 0;
                var result = RmGetList(session, out var needed, ref count, null, ref reasons);
                if (result != ErrorMoreData || needed == 0) return [];
                var found = new RmProcessInfo[needed];
                count = needed;
                if (RmGetList(session, out needed, ref count, found, ref reasons) != 0) return [];
                return [.. found.Take((int)count).Select(info => Describe(info.Process.ProcessId) +
                    (string.IsNullOrWhiteSpace(info.AppName) ? "" : ", " + info.AppName.Trim()))];
            }
            finally { RmEndSession(session); }
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException)
        {
            return [];
        }
    }

    const int AfInet = 2, TcpTableOwnerPidListener = 3, ErrorMoreData = 234;

    [DllImport("iphlpapi.dll")]
    static extern uint GetExtendedTcpTable(IntPtr table, ref int size, bool sort, int family, int tableClass, uint reserved);

    [StructLayout(LayoutKind.Sequential)]
    struct RmUniqueProcess
    {
        public int ProcessId;
        // FILETIME: two DWORDs, so the struct stays 12 bytes.
        public uint StartTimeLow, StartTimeHigh;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct RmProcessInfo
    {
        public RmUniqueProcess Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string AppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string ServiceShortName;
        public int ApplicationType;
        public uint AppStatus;
        public uint SessionId;
        [MarshalAs(UnmanagedType.Bool)] public bool Restartable;
    }

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    static extern int RmStartSession(out uint session, int flags, StringBuilder key);

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    static extern int RmRegisterResources(uint session, uint files, string[] names, uint applications, RmUniqueProcess[]? processes,
        uint services, string[]? serviceNames);

    [DllImport("rstrtmgr.dll")]
    static extern int RmGetList(uint session, out uint needed, ref uint count, [In, Out] RmProcessInfo[]? processes, ref uint rebootReasons);

    [DllImport("rstrtmgr.dll")]
    static extern int RmEndSession(uint session);
}
