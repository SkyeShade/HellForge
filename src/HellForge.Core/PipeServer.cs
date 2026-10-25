using System.ComponentModel;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace HellForge.Core;

// The pipes the game opens (the backend's lobby and party pipe, the live pipe): only this user may open them, and on
// this PC only. A sandboxed session's game runs at low integrity, where "no write up" refuses it a pipe with the usual
// (medium) label, so LowIntegrityClients gives them a low mandatory label too (docs/steam-bridge.md). The backend sets
// it from --sandbox; it is one process per session.
public static class PipeServer
{
    public static bool LowIntegrityClients { get; set; }

    public static NamedPipeServerStream Create(string name, int maxInstances)
    {
        if (!LowIntegrityClients || !OperatingSystem.IsWindows())
            return new NamedPipeServerStream(name, PipeDirection.InOut, maxInstances, PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        var user = WindowsIdentity.GetCurrent().User ?? throw new InvalidOperationException("No user SID for the pipe.");
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW($"D:P(A;;GA;;;{user.Value})S:(ML;;NW;;;LW)", 1, out var descriptor, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSecurityDescriptorToSecurityDescriptor");
        try
        {
            var attributes = new SECURITY_ATTRIBUTES { Length = Marshal.SizeOf<SECURITY_ATTRIBUTES>(), SecurityDescriptor = descriptor };
            const uint Duplex = 0x3, Overlapped = 0x40000000, RejectRemote = 0x8;
            var handle = CreateNamedPipeW(@"\\.\pipe\" + name, Duplex | Overlapped, RejectRemote, maxInstances < 0 ? 255u : (uint)maxInstances,
                4096, 4096, 0, ref attributes);
            if (handle.IsInvalid) throw new IOException($"The pipe {name} could not be made.", new Win32Exception(Marshal.GetLastWin32Error()));
            return new NamedPipeServerStream(PipeDirection.InOut, isAsync: true, isConnected: false, handle);
        }
        finally { LocalFree(descriptor); }
    }

    // The integrity level a pipe's label grants writes from (0x1000 low, 0x2000 medium; 0 when it has none), for tests.
    public static int LabelLevel(SafePipeHandle pipe)
    {
        var error = GetSecurityInfo(pipe, 6, 0x10, 0, 0, 0, out var sacl, out var descriptor);
        if (error != 0) throw new Win32Exception((int)error, "GetSecurityInfo");
        try
        {
            if (sacl == 0 || !GetAce(sacl, 0, out var ace)) return 0;
            // SYSTEM_MANDATORY_LABEL_ACE: header (4), mask (4), then the SID S-1-16-<level>: its last sub-authority
            var sid = ace + 8;
            return Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(Marshal.ReadByte(GetSidSubAuthorityCount(sid)) - 1)));
        }
        finally { LocalFree(descriptor); }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES { public int Length; public nint SecurityDescriptor; public int InheritHandle; }

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out nint descriptor, out uint size);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafePipeHandle CreateNamedPipeW(string name, uint openMode, uint pipeMode, uint maxInstances, uint outBuffer, uint inBuffer,
        uint timeout, ref SECURITY_ATTRIBUTES attributes);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern uint GetSecurityInfo(SafeHandle handle, int objectType, uint info, nint owner, nint group, nint dacl, out nint sacl, out nint descriptor);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetAce(nint acl, int index, out nint ace);

    [DllImport("advapi32.dll")]
    static extern nint GetSidSubAuthority(nint sid, uint index);

    [DllImport("advapi32.dll")]
    static extern nint GetSidSubAuthorityCount(nint sid);

    [DllImport("kernel32.dll")]
    static extern nint LocalFree(nint memory);
}
