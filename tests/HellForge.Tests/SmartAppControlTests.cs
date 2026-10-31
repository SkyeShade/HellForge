using System.ComponentModel;
using HellForge.Core;

// Windows 11's Smart App Control blocking HellForge's unsigned programs and DLLs (SmartAppControl): its state as the
// registry gives it, which Windows errors are its blocks, and what the player is told.
static class SmartAppControlTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Smart App Control's state reads 0 as off, 1 as on, 2 as evaluation, and anything else as not present", () =>
        {
            check(SmartAppControl.FromValue(0) == SmartAppControlState.Off, "0 is off");
            check(SmartAppControl.FromValue(1) == SmartAppControlState.On, "1 is on");
            check(SmartAppControl.FromValue(2) == SmartAppControlState.Evaluation, "2 is evaluation");
            foreach (var other in new object?[] { null, 3, -1, "1", 1L })
                check(SmartAppControl.FromValue(other) == SmartAppControlState.NotPresent, $"{other ?? "null"} is not present");
            check(SmartAppControl.Describe(SmartAppControlState.Evaluation) == "evaluation" && SmartAppControl.Describe(SmartAppControlState.NotPresent) == "not present", "described");
            // off Windows (this suite also runs on Linux) there is none
            if (!OperatingSystem.IsWindows()) check(SmartAppControl.Current == SmartAppControlState.NotPresent, "none off Windows");
        });

        test("An App Control block is told apart from other start failures, as a Win32 code or its HRESULT", () =>
        {
            // "An Application Control policy has blocked this file." (ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION), as Process.Start throws it
            check(SmartAppControl.IsBlock(new Win32Exception(4551).NativeErrorCode), "4551");
            check(SmartAppControl.IsBlock(unchecked((int)0x800711C7)), "0x800711C7, its HRESULT");
            foreach (var code in new[] { 577, 4556, 4557, 4558, 4559, 4580, 4581, 4582 }) check(SmartAppControl.IsBlock(code), code + " is a block");
            // not found, access denied, UAC declined, a bad image: other problems with other messages
            foreach (var code in new[] { 0, 2, 5, 193, 1223, 4550, 4552, 4560, unchecked((int)0x80070005) }) check(!SmartAppControl.IsBlock(code), code + " is not a block");
            check(SmartAppControl.IsBlockStatus(unchecked((int)0xC0000428)) && !SmartAppControl.IsBlockStatus(unchecked((int)0xC0000135)), "STATUS_INVALID_IMAGE_HASH only");
        });

        test("A Smart App Control block names the file, says HellForge is unsigned, and is honest that turning it off may be for good", () =>
        {
            var file = Path.Combine(root, "sac", "app", "session", "HellForge.Session.exe");
            var on = SmartAppControl.BlockedMessage(file, SmartAppControlState.On);
            check(on.StartsWith("Windows blocked HellForge.Session.exe (\"An Application Control policy has blocked this file\"", StringComparison.Ordinal), on);
            check(on.Contains("Smart App Control is on", StringComparison.Ordinal) && on.Contains("not signed yet", StringComparison.Ordinal), "why, when on");
            check(on.Contains(SmartAppControl.SettingsPath, StringComparison.Ordinal), "where to turn it off");
            check(on.Contains("may not let you turn Smart App Control back on", StringComparison.Ordinal), "turning it off may not be reversible");
            check(on.Contains("cannot add an exception", StringComparison.Ordinal), "no exception exists");
            // off or not present, the block is another App Control policy: it may be an organisation's
            var other = SmartAppControl.BlockedMessage(file, SmartAppControlState.Off);
            check(!other.Contains("Smart App Control is on", StringComparison.Ordinal) && other.Contains("set by your organisation", StringComparison.Ordinal), other);
        });

        test("The launcher and setup warn while Smart App Control is on or evaluating, and say nothing when it is off", () =>
        {
            check(SmartAppControl.Warning(SmartAppControlState.Off) == null && SmartAppControl.Warning(SmartAppControlState.NotPresent) == null, "quiet when off");
            var on = SmartAppControl.Warning(SmartAppControlState.On)!;
            check(on.StartsWith("Smart App Control is on.", StringComparison.Ordinal) && on.Contains("may not let you turn Smart App Control back on", StringComparison.Ordinal), on);
            var evaluation = SmartAppControl.Warning(SmartAppControlState.Evaluation)!;
            check(evaluation.Contains("evaluation mode", StringComparison.Ordinal) && evaluation.Contains("blocks nothing yet", StringComparison.Ordinal), evaluation);
        });

        test("The mod loader names the same Windows errors as App Control blocks", () =>
        {
            var loader = File.ReadAllText(Path.Combine(Repository(), "native", "mod-loader", "hellforge_modloader.c"));
            foreach (var code in new[] { "4551", "577", "4556", "4559", "4580", "4582" }) check(loader.Contains("loadError == " + code, StringComparison.Ordinal) ||
                loader.Contains("loadError >= " + code, StringComparison.Ordinal) || loader.Contains("loadError <= " + code, StringComparison.Ordinal), code + " in the mod loader");
        });
    }

    static string Repository()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        throw new InvalidOperationException("The repository was not found.");
    }
}
