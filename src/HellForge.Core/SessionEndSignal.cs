using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace HellForge.Core;

// The session's end, told to the game's mods before the session runner stops the game (SDK 16, HF_EVENT_SESSION_END):
// two named events for one game process, made by the runner before the game starts. The mod loader waits on
// Local\HellForge-session-end-<pid> between its passes, delivers the event to the mods, then sets
// Local\HellForge-session-ended-<pid> (native/mod-loader/mod_status.c). Only this user may open them, and they carry a
// low mandatory label, so a sandboxed game at low integrity can (as PipeServer's pipes). Windows only.
public sealed class SessionEndSignal : IDisposable
{
    readonly nint end, ended;

    SessionEndSignal(nint end, nint ended) => (this.end, this.ended) = (end, ended);

    public static string EndName(int processId) => $@"Local\HellForge-session-end-{processId}";
    public static string EndedName(int processId) => $@"Local\HellForge-session-ended-{processId}";

    // The two events for the game process `processId`, made before it runs; null where there are none (not Windows).
    public static SessionEndSignal? Create(int processId)
    {
        if (!OperatingSystem.IsWindows()) return null;
        var user = WindowsIdentity.GetCurrent().User ?? throw new InvalidOperationException("No user SID for the session's end events.");
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW($"D:P(A;;GA;;;{user.Value})S:(ML;;NW;;;LW)", 1, out var descriptor, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSecurityDescriptorToSecurityDescriptor");
        try
        {
            var attributes = new SECURITY_ATTRIBUTES { Length = Marshal.SizeOf<SECURITY_ATTRIBUTES>(), SecurityDescriptor = descriptor };
            var end = CreateEventW(ref attributes, true, false, EndName(processId));
            if (end == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateEvent " + EndName(processId));
            var ended = CreateEventW(ref attributes, true, false, EndedName(processId));
            if (ended != 0) return new(end, ended);
            var error = Marshal.GetLastWin32Error();
            CloseHandle(end);
            throw new Win32Exception(error, "CreateEvent " + EndedName(processId));
        }
        finally { LocalFree(descriptor); }
    }

    // Tells the game's mods the session ends, then waits for them at most `wait`: true when the loader said they heard.
    public bool Signal(TimeSpan wait)
    {
        if (!SetEvent(end)) throw new Win32Exception(Marshal.GetLastWin32Error(), "SetEvent");
        return WaitForSingleObject(ended, (uint)Math.Clamp(wait.TotalMilliseconds, 0, uint.MaxValue - 1)) == 0;
    }

    public void Dispose()
    {
        CloseHandle(end);
        CloseHandle(ended);
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES { public int Length; public nint SecurityDescriptor; public int InheritHandle; }

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out nint descriptor, out uint size);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern nint CreateEventW(ref SECURITY_ATTRIBUTES attributes, bool manualReset, bool initialState, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetEvent(nint handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(nint handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(nint handle);

    [DllImport("kernel32.dll")]
    static extern nint LocalFree(nint memory);
}
