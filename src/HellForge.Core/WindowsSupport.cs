namespace HellForge.Core;

// Whether this Windows can start HellForge's .NET programs (#14). .NET 9 and later mark every program for
// hardware-enforced stack protection (CET shadow stacks). On a processor that has it (Intel 11th generation and AMD
// Ryzen 5000 and later), Windows 10 whose system files predate Microsoft's CET fixes stops the .NET runtime while it
// starts, before any of the program's own code: "The process was terminated due to an internal error in the .NET
// Runtime ... with exit code 0x80131506" in the Application event log, and nothing on screen. The fixes reached
// Windows 10 in ntdll.dll 10.0.19041.5007 (KB5044273, October 2024; dotnet/runtime#110920); 2004, 20H2 and 21H1 were
// out of support by then and never got them. HellForge-Setup is built without the CET mark so it can start and say so
// (its csproj); the launcher and the rest of the app keep it.
public static class WindowsSupport
{
    // The first Windows 10 build with user-mode shadow stacks (2004) and Windows 11's first: in between, CET can stop .NET.
    const int FirstCetBuild = 19041, Windows11 = 22000;
    public static readonly Version FixedNtdll = new(10, 0, 19041, 5007);

    // A plain message when HellForge's programs would close as they start on this Windows, null when they run.
    // build: the Windows build (Environment.OSVersion); ntdll: System32\ntdll.dll's file version; shadowStacks: the
    // processor has CET shadow stacks (CPUID 7, ECX bit 7).
    public static string? Problem(int build, Version? ntdll, bool shadowStacks)
    {
        if (!shadowStacks || build < FirstCetBuild || build >= Windows11 || ntdll == null || ntdll >= FixedNtdll) return null;
        var windows = $"Windows 10 version {Release(build)} (build {build})";
        const string why = "HellForge's programs need Windows updates from October 2024 or later on this PC's processor: without them they " +
            "close as soon as they start, before they can show anything.";
        return build <= 19043
            ? $"This PC runs {windows}, which no longer gets updates from Microsoft. {why}\n\nUpdate to Windows 10 version 22H2 " +
                "or Windows 11 in Settings > Update & Security > Windows Update, install the updates it offers after that, then run " +
                "HellForge Setup again."
            : $"This PC runs {windows} without the latest updates. {why}\n\nInstall the updates in Settings > Update & Security > " +
                "Windows Update (if it offers none, update to Windows 10 version 22H2 or Windows 11), then run HellForge Setup again.";
    }

    // Windows 10's release names, by build (all share ntdll's 19041 numbering).
    public static string Release(int build) => build switch
    {
        19041 => "2004", 19042 => "20H2", 19043 => "21H1", 19044 => "21H2", 19045 => "22H2", _ => build.ToString(),
    };
}
