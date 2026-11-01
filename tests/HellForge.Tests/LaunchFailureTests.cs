using HellForge.Session;

// A game that closes while the session runner prepares or connects it (src/HellForge.Session/LaunchFailure.cs and
// WindowsCrash.cs): the player is told the game closed, with its exit code and where Windows recorded the crash, not that
// it "could not be connected to the local backend" (#203, #113); Windows' record is read from wevtutil's XML.
static class LaunchFailureTests
{
    const string Root = @"C:\Users\Alex\Documents\HellForge", Windows = @"C:\Windows";
    const string Rtss = @"C:\Program Files (x86)\RivaTuner Statistics Server\RTSSHooks64.dll";

    // One event 1000 as wevtutil qe Application /f:xml writes it on Windows 10 and 11 (named data)
    static string Event(string app, string pid, string module, string offset, string modulePath) =>
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Application Error' " +
        "Guid='{a0e9b465-b939-57d7-b27d-95d8e925ff57}'/><EventID>1000</EventID><Level>2</Level><TimeCreated SystemTime='2026-10-31T06:03:09.8754123Z'/>" +
        "<Channel>Application</Channel><Computer>DESKTOP-K2Q7R4T</Computer></System><EventData>" +
        $"<Data Name='AppName'>{app}</Data><Data Name='AppVersion'>0.0.0.0</Data><Data Name='AppTimeStamp'>68e4b7a1</Data>" +
        $"<Data Name='ModuleName'>{module}</Data><Data Name='ModuleVersion'>7.3.6.0</Data><Data Name='ModuleTimeStamp'>66a1c2d3</Data>" +
        $"<Data Name='ExceptionCode'>c0000005</Data><Data Name='FaultingOffset'>{offset}</Data><Data Name='ProcessId'>{pid}</Data>" +
        $"<Data Name='ProcessCreationTime'>0x1db4a1b2c3d4e5f</Data><Data Name='AppPath'>{Root}\\game\\bin\\{app}</Data>" +
        $"<Data Name='ModulePath'>{modulePath}</Data><Data Name='IntegratorReportId'>1d2e3f40-1111-2222-3333-444455556666</Data>" +
        "<Data Name='PackageFullName'></Data><Data Name='PackageRelativeAppId'/></EventData></Event>";

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Windows' crash record: the game's own event 1000 among others, its module, offset and code, in either data form", () =>
        {
            // newest first, as /rd:true gives them: another program's crash, then the game's (process 0x35f8 = 13816)
            var xml = Event("chrome.exe", "0x1111", "ntdll.dll", "0000000000001234", @"C:\Windows\SYSTEM32\ntdll.dll") + "\r\n" +
                Event("helldivers2.exe", "0x35f8", "RTSSHooks64.dll", "000000000002f3c1", Rtss) + "\r\n";
            var crash = WindowsCrash.Parse(xml, 13816);
            check(crash == new WindowsCrash.Record("RTSSHooks64.dll", "0x2f3c1", "0xC0000005", Rtss), "The game's crash: " + crash);
            check(WindowsCrash.Parse(xml, 4369) == null, "Another program's crash under the game's name is not taken (0x1111 is chrome.exe).");
            check(WindowsCrash.Parse(xml, 777) == null && WindowsCrash.Parse("", 13816) == null, "No record for another process, or with no events.");
            // older systems: the same data in order, without names; a path's & comes escaped
            var older = "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID Qualifiers='0'>1000</EventID></System><EventData>" +
                "<Data>helldivers2.exe</Data><Data>0.0.0.0</Data><Data>68e4b7a1</Data><Data>hook.dll</Data><Data>1.0.0.0</Data><Data>5f000000</Data>" +
                "<Data>c0000409</Data><Data>0x0000000000000a10</Data><Data>13816</Data><Data>0x1db</Data><Data>C:\\game\\helldivers2.exe</Data>" +
                @"<Data>C:\Program Files\R&amp;D Tools\hook.dll</Data><Data>id</Data><Data></Data><Data></Data></EventData></Event>";
            check(WindowsCrash.Parse(older, 13816) == new WindowsCrash.Record("hook.dll", "0xa10", "0xC0000409", @"C:\Program Files\R&D Tools\hook.dll"),
                "Unnamed data: " + WindowsCrash.Parse(older, 13816));
            var nowhere = WindowsCrash.Parse(Event("helldivers2.exe", "0x35f8", "", "0000000000000000", ""), 13816);
            check(nowhere == new WindowsCrash.Record("unknown", "0x0", "0xC0000005", ""), "A crash outside any module: " + nowhere);
            check(WindowsCrash.IsCrash(unchecked((int)0xC0000005)) && WindowsCrash.IsCrash(unchecked((int)0xC0000409)), "Exceptions are crashes.");
            check(!WindowsCrash.IsCrash(0) && !WindowsCrash.IsCrash(1) && !WindowsCrash.IsCrash(unchecked((int)0xB7520003)), "A chosen exit or HellForge's own stop is not.");
        });

        test("Windows' crash record: the crashed module is Windows', the installation's or another program's", () =>
        {
            WindowsCrash.Owner Of(string module, string path) => WindowsCrash.OwnerOf(new(module, "0x1", "0xC0000005", path), Root, Windows);
            check(Of("RTSSHooks64.dll", Rtss) == WindowsCrash.Owner.Other, "An overlay's hook.");
            check(Of("hook.dll", Root + @"Other\hook.dll") == WindowsCrash.Owner.Other, "A folder whose name only starts like the installation's.");
            check(Of("ntdll.dll", @"C:\WINDOWS\SYSTEM32\ntdll.dll") == WindowsCrash.Owner.Windows, "Windows' own, in any case.");
            check(Of("nvwgf2umx.dll", @"C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_1\nvwgf2umx.dll") == WindowsCrash.Owner.Windows, "A driver.");
            check(Of("steam_api64.dll", Root + @"\game\bin\steam_api64.dll") == WindowsCrash.Owner.Installation, "HellForge's stand-in in the copy.");
            check(Of("some_mod.dll", Root + @"\mods\.unpacked\some-mod-1a2b\some_mod.dll") == WindowsCrash.Owner.Installation, "A code mod.");
            check(Of("game.dll", @"D:\Linked\HellForge\game\data\game\game.dll") == WindowsCrash.Owner.Installation, "The game's own, by name, through a link.");
            check(Of("unknown", "") == WindowsCrash.Owner.Unknown, "A crash outside any module.");
            var json = WindowsCrash.ToJson(new("RTSSHooks64.dll", "0x2f3c1", "0xC0000005", Rtss), WindowsCrash.Owner.Other);
            check(json.ToJsonString() == """{"module":"RTSSHooks64.dll","offset":"0x2f3c1","code":"0xC0000005","modulePath":"C:\\Program Files (x86)\\RivaTuner Statistics Server\\RTSSHooks64.dll","owner":"other"}""",
                "As result.json keeps it: " + json.ToJsonString());
        });

        test("A game that closed while starting is said to have closed, with its exit code and Windows' record; not as could not be connected (#203)", () =>
        {
            const int AccessViolation = unchecked((int)0xC0000005);
            var rtss = new WindowsCrash.Record("RTSSHooks64.dll", "0x2f3c1", "0xC0000005", Rtss);
            var other = LaunchFailure.Closed(LaunchFailure.Connect, AccessViolation, rtss, WindowsCrash.Owner.Other);
            foreach (var expected in new[] { "The game closed while starting, before HellForge could connect it to the local backend",
                "exit code 0xC0000005: the game crashed (access violation); Windows recorded the crash in RTSSHooks64.dll+0x2f3c1)",
                "RTSSHooks64.dll is not part of the game, HellForge or Windows", "Close that program" })
                check(other.Contains(expected), $"Lacks \"{expected}\": {other}");
            check(!other.Contains("could not be connected") && !other.Contains(Rtss), "Neither the old message nor the module's path: " + other);
            var windows = LaunchFailure.Closed(LaunchFailure.Connect, AccessViolation, rtss with { Module = "ntdll.dll" }, WindowsCrash.Owner.Windows);
            check(windows.Contains("in ntdll.dll+0x2f3c1") && !windows.Contains("not part of") && windows.Contains("save the crash report"), "A crash in Windows' code: " + windows);
            var unrecorded = LaunchFailure.Closed(LaunchFailure.Prepare, AccessViolation, null, WindowsCrash.Owner.Unknown);
            check(unrecorded.StartsWith("The game closed while starting, before HellForge could prepare it for the local backend " +
                "(exit code 0xC0000005: the game crashed (access violation)). Press PLAY", StringComparison.Ordinal), "No record: " + unrecorded);
            check(LaunchFailure.Closed(LaunchFailure.Connect, 0, null, WindowsCrash.Owner.Unknown).Contains("(exit code 0x00000000)."), "A plain exit has no meaning.");
            // the game still runs: what part of the connect step failed
            check(LaunchFailure.NotConnected("module_not_observed").StartsWith("The game could not be connected to the local backend: it had not loaded its code " +
                "(game.dll) after 45 seconds.", StringComparison.Ordinal), LaunchFailure.NotConnected("module_not_observed"));
            check(LaunchFailure.NotConnected("decoded_bytes_not_observed").Contains("not what HellForge expects") &&
                LaunchFailure.NotConnected("patch_failed").Contains("did not go through") && LaunchFailure.NotConnected("odd_outcome").Contains("odd_outcome"),
                "Each outcome in words.");
        });
    }
}
