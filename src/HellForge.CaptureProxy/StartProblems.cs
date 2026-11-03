using System.Diagnostics;
using System.Runtime.InteropServices;

// Who is in the capture's way when it cannot start: an earlier sync's capture still running, and the program listening on
// its port (iphlpapi's TCP table with owning process ids), as "name.exe (process 1234)" for proxy.log and the launcher.
// The programs holding the hosts file open are named by Program.cs's HostsLockers (Restart Manager).
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

    const int AfInet = 2, TcpTableOwnerPidListener = 3;

    [DllImport("iphlpapi.dll")]
    static extern uint GetExtendedTcpTable(IntPtr table, ref int size, bool sort, int family, int tableClass, uint reserved);
}
