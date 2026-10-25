using System.ComponentModel;
using System.Runtime.InteropServices;

namespace HellForge.Session;

/// <summary>
/// Low-integrity process tokens and folder labels for the sandboxed game session (docs/steam-bridge.md, "The rest of a
/// low-integrity session"). A low-integrity process cannot write to the player's files, registry or other processes
/// ("no write up"), even from native code calling the kernel directly. The folders the game writes are made new with a
/// low label (CreateLowFolder).
/// </summary>
static class LowIntegrityHelper
{
    /// <summary>
    /// Creates a primary token whose mandatory integrity level is Low (S-1-16-4096), duplicated from the current
    /// process's token. The caller must close the handle.
    /// </summary>
    public static nint CreateLowToken()
    {
        nint processToken = 0, lowToken = 0, sid = 0;
        try
        {
            if (!OpenProcessToken((nint)(-1), 0x0002 | 0x0008, out processToken))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken");
            if (!DuplicateTokenEx(processToken, 0x10000000, nint.Zero, 2, 1, out lowToken))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateTokenEx");

            byte[] authority = [0, 0, 0, 0, 0, 16];
            if (!AllocateAndInitializeSid(authority, 1, 0x1000, 0, 0, 0, 0, 0, 0, 0, out sid))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "AllocateAndInitializeSid");

            var label = new TOKEN_MANDATORY_LABEL { Label = new SID_AND_ATTRIBUTES { Sid = sid, Attributes = 0x20 } };
            int size = Marshal.SizeOf(label);
            nint ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(label, ptr, false);
                if (!SetTokenInformation(lowToken, 25, ptr, size))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation(TokenIntegrityLevel)");
            }
            finally { Marshal.FreeHGlobal(ptr); }

            nint result = lowToken;
            lowToken = 0;
            return result;
        }
        finally
        {
            if (sid != 0) FreeSid(sid);
            if (processToken != 0) CloseHandle(processToken);
            if (lowToken != 0) CloseHandle(lowToken);
        }
    }

    /// <summary>
    /// Creates a new folder whose security descriptor carries a Low mandatory label, inherited by everything created in it
    /// (OI and CI); its access list is inherited as usual. A label is set this way, at creation and at or below one's own
    /// integrity, without WRITE_OWNER, which relabelling an existing folder needs and a standard account seldom has. Throws
    /// when the folder exists already.
    /// </summary>
    public static string CreateLowFolder(string path)
    {
        var full = Path.GetFullPath(path);
        if (Directory.Exists(full) || File.Exists(full)) throw new IOException($"{full} exists already; a low folder is made new.");
        nint sd = 0;
        try
        {
            if (!ConvertStringSecurityDescriptorToSecurityDescriptor("S:(ML;OICI;NW;;;LW)", 1, out sd, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSecurityDescriptorToSecurityDescriptor");
            var attributes = new SECURITY_ATTRIBUTES { Length = Marshal.SizeOf<SECURITY_ATTRIBUTES>(), SecurityDescriptor = sd };
            if (!CreateDirectoryW(full, ref attributes))
            {
                var error = Marshal.GetLastWin32Error();
                throw new Win32Exception(error, $"Creating the low-integrity folder {full} failed: {new Win32Exception(error).Message} ({error})");
            }
            return full;
        }
        finally
        {
            if (sd != 0) LocalFree(sd);
        }
    }

    public static void CloseToken(nint token) { if (token != 0) CloseHandle(token); }

    /// <summary>This process's mandatory integrity level (0x1000 low, 0x2000 medium, 0x3000 high).</summary>
    public static int CurrentLevel()
    {
        if (!OpenProcessToken((nint)(-1), 0x0008, out var token)) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken");
        try
        {
            GetTokenInformation(token, 25, 0, 0, out var size);
            var buffer = Marshal.AllocHGlobal(size);
            try
            {
                if (!GetTokenInformation(token, 25, buffer, size, out _)) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetTokenInformation");
                var sid = Marshal.ReadIntPtr(buffer);
                return Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(Marshal.ReadByte(GetSidSubAuthorityCount(sid)) - 1)));
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        finally { CloseHandle(token); }
    }

    // --- P/Invoke ---

    [StructLayout(LayoutKind.Sequential)]
    struct SID_AND_ATTRIBUTES { public nint Sid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct TOKEN_MANDATORY_LABEL { public SID_AND_ATTRIBUTES Label; }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES { public int Length; public nint SecurityDescriptor; public int InheritHandle; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateDirectoryW(string path, ref SECURITY_ATTRIBUTES attributes);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(nint process, uint access, out nint token);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool DuplicateTokenEx(nint existing, uint access, nint attrs, int level, int type, out nint duplicate);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AllocateAndInitializeSid(byte[] authority, byte subAuthorityCount,
        uint a0, uint a1, uint a2, uint a3, uint a4, uint a5, uint a6, uint a7, out nint sid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern nint FreeSid(nint sid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetTokenInformation(nint token, int cls, nint info, int size);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string sddl, uint revision, out nint sd, out uint size);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(nint token, int cls, nint info, int size, out int returned);

    [DllImport("advapi32.dll")]
    static extern nint GetSidSubAuthority(nint sid, uint index);

    [DllImport("advapi32.dll")]
    static extern nint GetSidSubAuthorityCount(nint sid);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(nint handle);

    [DllImport("kernel32.dll")]
    static extern nint LocalFree(nint mem);
}
