using HellForge.Core;

// Setup's check for a Windows that stops .NET as it starts (#14: Windows 10 21H1, ntdll 10.0.19041.2130, Alder Lake).
static class WindowsSupportTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Setup refuses Windows 10 21H1 on a CET processor and says to update to 22H2 or Windows 11 (#14)", () =>
        {
            var problem = WindowsSupport.Problem(19043, new Version(10, 0, 19041, 2130), shadowStacks: true);
            check(problem != null && problem.Contains("21H1") && problem.Contains("build 19043") && problem.Contains("22H2") &&
                problem.Contains("Windows 11") && problem.Contains("no longer gets updates"), "The #14 PC: " + problem);
            check(WindowsSupport.Problem(19042, new Version(10, 0, 19041, 2130), true)?.Contains("20H2") == true, "20H2 passed.");
        });

        test("An unpatched 22H2 is told to install updates; an updated one, Windows 11, older builds and other processors pass", () =>
        {
            var old22H2 = WindowsSupport.Problem(19045, new Version(10, 0, 19041, 2788), true);
            check(old22H2 != null && old22H2.Contains("22H2") && old22H2.Contains("without the latest updates"), "Unpatched 22H2: " + old22H2);
            check(WindowsSupport.Problem(19045, WindowsSupport.FixedNtdll, true) == null, "22H2 with the CET fixes refused.");
            check(WindowsSupport.Problem(19045, new Version(10, 0, 19041, 6456), true) == null, "Current 22H2 refused.");
            check(WindowsSupport.Problem(19043, new Version(10, 0, 19041, 2130), false) == null, "A processor without CET refused.");
            check(WindowsSupport.Problem(22631, new Version(10, 0, 22621, 1000), true) == null, "Windows 11 refused.");
            check(WindowsSupport.Problem(17763, new Version(10, 0, 17763, 100), true) == null, "1809 (no user shadow stacks) refused.");
            check(WindowsSupport.Problem(19043, null, true) == null, "An unreadable ntdll refused.");
            check(WindowsSupport.Problem(19045, new Version(6, 1, 7601, 24059), true) == null, "Proton 11's ntdll (live test) refused.");
            check(WindowsSupport.Problem(19043, new Version(6, 1, 7601, 24059), true) == null, "Wine set to 19043 refused.");
        });
    }
}
