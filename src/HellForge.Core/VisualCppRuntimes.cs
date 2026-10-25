namespace HellForge.Core;

// The Microsoft Visual C++ runtimes the supported build loads from Windows' system folder. helldivers2.exe imports
// lua51.dll, which needs the 2012 runtime's MSVCR110.dll; PartyWin, PlayFab, libcurl, XeSS and the upscaler need the
// 2015-2022 runtime. The game ships neither, and the live build's Steam install no longer installs the 2012 one, so a
// fresh PC has the game stop at start (0xC0000135, a DLL not found). Setup checks before the long copy, and every
// session before it starts the game.
public static class VisualCppRuntimes
{
    public sealed record Runtime(string Name, string[] Files, string Download);

    public static readonly Runtime[] Required =
    [
        new("Microsoft Visual C++ 2012 Redistributable (x64)", ["msvcr110.dll"],
            "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe"),
        new("Microsoft Visual C++ 2015-2022 Redistributable (x64)", ["vcruntime140.dll", "msvcp140.dll"],
            "https://aka.ms/vc14/vc_redist.x64.exe"),
    ];

    // The 64-bit system folder, also from a 32-bit process (whose System32 Windows redirects to SysWOW64).
    public static string SystemFolder => !Environment.Is64BitProcess && Environment.Is64BitOperatingSystem
        ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Sysnative")
        : Environment.SystemDirectory;

    public static List<Runtime> Missing(string systemFolder) =>
        Required.Where(runtime => runtime.Files.Any(file => !File.Exists(Path.Combine(systemFolder, file)))).ToList();

    // What to tell the player, or null when every runtime is there. Under Wine (Linux), Wine provides the runtimes itself.
    public static string? Problem(string then) => Problem(SystemFolder, then);

    public static string? Problem(string systemFolder, string then)
    {
        if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine) return null;
        var missing = Missing(systemFolder);
        if (missing.Count == 0) return null;
        return "The game needs " + (missing.Count == 1 ? "a Microsoft runtime" : "Microsoft runtimes") + " this PC does not have. Install " +
            string.Join(" and ", missing.Select(runtime => $"the {runtime.Name} from {runtime.Download}")) + ", then " + then;
    }
}
