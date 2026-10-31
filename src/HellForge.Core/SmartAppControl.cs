using System.Runtime.Versioning;
using Microsoft.Win32;

namespace HellForge.Core;

// Windows 11's Smart App Control (docs/code-signing.md). When it is on, Windows refuses to start a program, or load a DLL,
// that is neither signed by a publisher it trusts nor known to its cloud reputation service. HellForge's programs and DLLs
// are not signed yet, so Smart App Control can block any of them: the session runner, the local backend, the Steam
// broker, the mod loader and SDK stand-ins the game loads, and mods' DLLs built on the player's own PC. HellForge cannot
// add an exception (Smart App Control has none) and must not turn it off: it says what is happening, in plain words.
public enum SmartAppControlState { NotPresent, Off, On, Evaluation }

public static class SmartAppControl
{
    // VerifiedAndReputablePolicyState: 0 off, 1 on (enforcing), 2 evaluation (watching, not blocking). Any user may read it.
    public const string PolicyKey = @"SYSTEM\CurrentControlSet\Control\CI\Policy";
    public const string PolicyValue = "VerifiedAndReputablePolicyState";
    public const string SettingsPath = "Windows Security > App & browser control > Smart App Control settings";

    public static SmartAppControlState FromValue(object? value) => value switch
    {
        0 => SmartAppControlState.Off,
        1 => SmartAppControlState.On,
        2 => SmartAppControlState.Evaluation,
        _ => SmartAppControlState.NotPresent
    };

    // This PC's state; NotPresent off Windows, under Wine, or on a Windows without Smart App Control.
    public static SmartAppControlState Current
    {
        get
        {
            if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine) return SmartAppControlState.NotPresent;
            try { return Read(); }
            catch (Exception error) when (error is System.Security.SecurityException or IOException or UnauthorizedAccessException)
            { return SmartAppControlState.NotPresent; }
        }
    }

    [SupportedOSPlatform("windows")]
    static SmartAppControlState Read()
    {
        using var key = Registry.LocalMachine.OpenSubKey(PolicyKey);
        return FromValue(key?.GetValue(PolicyValue));
    }

    // For logs and reports: "on", "off", "evaluation" or "not present".
    public static string Describe(SmartAppControlState state) => state switch
    {
        SmartAppControlState.On => "on",
        SmartAppControlState.Off => "off",
        SmartAppControlState.Evaluation => "evaluation",
        _ => "not present"
    };

    // Whether a Windows error (a Win32 code, or its HRESULT 0x8007xxxx) means code integrity refused the file: Smart App
    // Control or another App Control (WDAC) policy. 4551 is ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION, "An Application Control
    // policy has blocked this file."; 4556-4559 and 4580-4582 are its reputation variants; 577 is ERROR_INVALID_IMAGE_HASH
    // ("Windows cannot verify the digital signature for this file").
    public static bool IsBlock(int code)
    {
        if ((unchecked((uint)code) & 0xFFFF0000) == 0x80070000) code &= 0xFFFF;
        return code is 4551 or (>= 4556 and <= 4559) or (>= 4580 and <= 4582) or 577;
    }

    // An NTSTATUS a process exits with when Windows refused a DLL it loads: STATUS_INVALID_IMAGE_HASH.
    public static bool IsBlockStatus(int status) => unchecked((uint)status) == 0xC0000428;

    // What a player is told when Windows blocked `file` (a HellForge program or DLL) from starting or loading.
    public static string BlockedMessage(string file, SmartAppControlState state)
    {
        var name = Path.GetFileName(file);
        var why = state switch
        {
            SmartAppControlState.On => "Smart App Control is on, and it blocks programs and DLLs that are not signed by a publisher Windows knows. ",
            _ => "An App Control policy on this PC (Smart App Control, or one set by your organisation) blocks programs and DLLs that are not signed. "
        };
        return $"Windows blocked {name} (\"An Application Control policy has blocked this file\"; {Privacy.Scrub(file)}). " + why +
            "HellForge's files are not signed yet, so HellForge cannot run while it is on, and HellForge cannot add an exception. " +
            Options(state);
    }

    // What a player is told before installing or playing while Smart App Control is on or evaluating, or null.
    public static string? Warning(SmartAppControlState state) => state switch
    {
        SmartAppControlState.On =>
            "Smart App Control is on. It blocks programs that are not signed by a publisher Windows knows, and HellForge's are " +
            "not signed yet, so Windows will most likely stop HellForge with \"An Application Control policy has blocked this file\". " +
            Options(state),
        SmartAppControlState.Evaluation =>
            "Smart App Control is in evaluation mode: it blocks nothing yet, but Windows may turn it on by itself, and then it " +
            "would block HellForge's unsigned files. If it does, HellForge says so when a session starts.",
        _ => null
    };

    static string Options(SmartAppControlState state) =>
        "Your options: play HELLDIVERS 2 without HellForge for now, or turn Smart App Control off (" + SettingsPath + "). " +
        "Be aware that Windows may not let you turn Smart App Control back on afterwards without resetting or reinstalling Windows" +
        (state == SmartAppControlState.On ? "." : ", and that a policy set by your organisation is for its administrator to change.");
}
