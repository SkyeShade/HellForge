using System.Diagnostics;
using System.Runtime.InteropServices;

namespace HellForge.Session;

// The session runner's own helpers (the local backend, the Steam broker) in a job that ends them when the runner ends,
// however it ends. The game's job already does that for the game (HostBoundedProcess); without this, a runner ended
// from Task Manager left the backend holding HellForge's port (the next session refused to start) and the broker holding
// Steam's "in game" status. The handle is never closed before the runner exits: closing it is what ends them.
sealed class HelperJob
{
    nint job;

    // The processes a job could not take (recorded by the caller), or null.
    public string? Error { get; private set; }

    public void Add(Process process)
    {
        try
        {
            if (job == 0)
            {
                var created = CreateJobObjectW(0, null);
                if (created == 0) { Error = "CreateJobObject " + Marshal.GetLastPInvokeError(); return; }
                var limits = new LIMIT();
                limits.Basic.Flags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (!SetInformationJobObject(created, 9, ref limits, Marshal.SizeOf<LIMIT>())) { Error = "SetInformationJobObject " + Marshal.GetLastPInvokeError(); CloseHandle(created); return; }
                job = created;
            }
            if (!AssignProcessToJobObject(job, process.Handle)) Error = process.ProcessName + ": AssignProcessToJobObject " + Marshal.GetLastPInvokeError();
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { Error = error.Message; }
    }

    [StructLayout(LayoutKind.Sequential)] struct BASIC { public long ProcessTime, JobTime; public uint Flags; public nuint MinWorkingSet, MaxWorkingSet; public uint ActiveLimit; public nuint Affinity; public uint Priority, Scheduling; }
    [StructLayout(LayoutKind.Sequential)] struct IO { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)] struct LIMIT { public BASIC Basic; public IO Io; public nuint ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern nint CreateJobObjectW(nint attributes, string? name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(nint job, int infoClass, ref LIMIT info, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(nint job, nint process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(nint handle);
}
