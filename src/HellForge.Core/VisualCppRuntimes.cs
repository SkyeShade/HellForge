using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;

namespace HellForge.Core;

// The Microsoft Visual C++ runtimes the supported build loads from Windows' system folder. helldivers2.exe imports
// lua51.dll, which needs the 2012 runtime's MSVCR110.dll; PartyWin, PlayFab, libcurl, XeSS, EOS, WinPixEventRuntime and
// the upscaler need the v14 (2015-2022) runtime, VCRUNTIME140_1.dll among it. The game ships neither, and the live
// build's Steam install no longer installs the 2012 one, so a fresh PC has the game stop at start (0xC0000135, a DLL not
// found). Setup and the launcher's PLAY download Microsoft's installer (pinned URL and SHA-256) and run it silently,
// with Windows' permission prompt; only when that fails does the message name the download, with the reason. The session
// runner checks again before it starts the game. Under Wine (Linux, Proton) Wine's own builtin runtimes serve.
//
// Why not ship msvcr110.dll beside the game instead: lua51.dll is the game's own file, and Microsoft's VS2012 terms let
// only a licensed Visual Studio 2012 user redistribute it app-locally with their program; HellForge's own native code
// (Zig) and .NET programs import neither runtime. Microsoft's redistributable installer is the supported route.
public static class VisualCppRuntimes
{
    // Size and SHA-256 of the exact installer at Url (the v14 URL carries its own hash in the path).
    public sealed record Runtime(string Name, string[] Files, string Download, string Sha256, long Size);

    public static readonly Runtime[] Required =
    [
        new("Microsoft Visual C++ 2012 Redistributable (x64)", ["msvcr110.dll"],
            "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe",
            "681BE3E5BA9FD3DA02C09D7E565ADFA078640ED66A0D58583EFAD2C1E3CC4064", 7_186_992),
        new("Microsoft Visual C++ 2015-2022 (v14) Redistributable (x64)", ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"],
            "https://download.visualstudio.microsoft.com/download/pr/ebdab8e5-1d7b-4d9f-a11b-cbb1720c3b12/843068991DAAA1F73AD9F6239BCE4D0F6A07A51F18C37EA2A867E9BECA71295C/VC_redist.x64.exe",
            "843068991DAAA1F73AD9F6239BCE4D0F6A07A51F18C37EA2A867E9BECA71295C", 18_731_856),
    ];

    // The 64-bit system folder, also from a 32-bit process (whose System32 Windows redirects to SysWOW64).
    public static string SystemFolder => !Environment.Is64BitProcess && Environment.Is64BitOperatingSystem
        ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Sysnative")
        : Environment.SystemDirectory;

    // A runtime counts as missing when any of its DLLs is not in the system folder as a 64-bit DLL: a registry entry
    // alone (a runtime whose files were removed) does not count, nor a 32-bit or damaged copy put there by hand.
    public static List<Runtime> Missing(string systemFolder, IEnumerable<Runtime>? required = null) =>
        (required ?? Required).Where(runtime => runtime.Files.Any(file => !IsX64Loadable(Path.Combine(systemFolder, file)))).ToList();

    // What this PC lacks; none outside Windows and under Wine, which provides the runtimes itself.
    public static List<Runtime> Needed() => !OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine ? [] : Missing(SystemFolder);

    // A PE image an x64 process can load: x64 (machine 0x8664), or on Windows on ARM the ARM64X/ARM64EC system DLLs
    // (0xAA64, 0xA641) that serve x64 processes too.
    public static bool IsX64Loadable(string path)
    {
        try
        {
            using var file = File.OpenRead(path);
            using var reader = new BinaryReader(file);
            if (file.Length < 0x40 || reader.ReadUInt16() != 0x5A4D) return false;
            file.Position = 0x3C;
            var header = reader.ReadUInt32();
            if (header > file.Length - 6) return false;
            file.Position = header;
            return reader.ReadUInt32() == 0x00004550 && reader.ReadUInt16() is 0x8664 or 0xAA64 or 0xA641;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    // What to tell the player, or null when every runtime is there.
    public static string? Problem(string then) => Problem(SystemFolder, then);

    public static string? Problem(string systemFolder, string then)
    {
        if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine) return null;
        var missing = Missing(systemFolder);
        return missing.Count == 0 ? null : Message(missing.Select(runtime => (runtime, (string?)null)).ToList(), then);
    }

    static string Message(List<(Runtime Runtime, string? Reason)> failed, string then) =>
        "The game needs " + (failed.Count == 1 ? "a Microsoft runtime" : "Microsoft runtimes") + " this PC does not have. " +
        string.Concat(failed.Where(f => f.Reason != null).Select(f => $"HellForge could not install the {f.Runtime.Name} itself: {f.Reason}. ")) +
        "Install " + string.Join(" and ", failed.Select(f => $"the {f.Runtime.Name} from {f.Runtime.Download}")) + ", then " + then;

    // Runs an installer with these arguments and returns its exit code; asks Windows for permission (UAC) first.
    public delegate Task<int> InstallerRunner(string installer, string arguments, CancellationToken cancel);

    // Installs every runtime this PC lacks; null once all are there, else the message naming the download and why.
    public static async Task<string?> EnsureAsync(string downloads, string then, Action<string> progress, CancellationToken cancel = default)
    {
        if (Needed().Count == 0) return null;
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        return await EnsureAsync(new(SystemFolder, downloads, http, RunElevated, progress), then, cancel);
    }

    // Where and how runtimes are installed: the system folder checked, the folder installers are downloaded to, and the
    // runner (Windows' permission prompt; a stand-in in tests), for the runtimes in Required unless given others.
    public sealed record Installation(string SystemFolder, string Downloads, HttpClient Http, InstallerRunner Run, Action<string> Progress,
        IReadOnlyList<Runtime>? Required = null);

    public static async Task<string?> EnsureAsync(Installation at, string then, CancellationToken cancel = default)
    {
        var failed = new List<(Runtime, string?)>();
        foreach (var runtime in Missing(at.SystemFolder, at.Required))
        {
            var reason = await InstallAsync(runtime, at, cancel);
            if (reason != null) failed.Add((runtime, reason));
        }
        // a runtime whose install went through but whose files are still not there (or a second one it removed)
        foreach (var runtime in Missing(at.SystemFolder, at.Required).Where(r => !failed.Any(f => f.Item1 == r)))
            failed.Add((runtime, "its installer finished, but " + string.Join(", ", runtime.Files) + " is still not in " + at.SystemFolder));
        return failed.Count == 0 ? null : Message(failed, then);
    }

    // null once the runtime's files are there, else why not.
    static async Task<string?> InstallAsync(Runtime runtime, Installation at, CancellationToken cancel)
    {
        var installer = Path.Combine(at.Downloads, runtime.Sha256[..8] + "-" + Path.GetFileName(new Uri(runtime.Download).AbsolutePath));
        try
        {
            at.Progress($"Downloading the {runtime.Name} the game needs ({runtime.Size / 1e6:0} MB)...");
            Directory.CreateDirectory(at.Downloads);
            await DownloadAsync(at.Http, runtime, installer, cancel);
        }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException or IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return error is InvalidDataException ? error.Message : "the download from Microsoft failed (" + error.Message + ")";
        }
        // Held open, unwritable, from the hash check until the installer has run, so what Windows runs as administrator
        // is the file that was checked.
        FileStream held;
        try { held = new FileStream(installer, FileMode.Open, FileAccess.Read, FileShare.Read); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return "the downloaded installer could not be opened (" + error.Message + ")"; }
        await using var _ = held;
        if (!Hash(held).Equals(runtime.Sha256, StringComparison.OrdinalIgnoreCase)) return "the downloaded installer did not match its pinned checksum";
        var log = Path.Combine(at.Downloads, Path.GetFileNameWithoutExtension(installer) + ".log");
        // A runtime Windows lists as installed while its DLLs are gone: /install does nothing then, /repair puts them back.
        foreach (var mode in new[] { "install", "repair" })
        {
            at.Progress($"Installing the {runtime.Name} the game needs. Windows asks for permission...");
            int code;
            try { code = await at.Run(installer, $"/{mode} /quiet /norestart /log \"{log}\"", cancel); }
            catch (Win32Exception error) when (error.NativeErrorCode == 1223) { return "the Windows permission prompt was declined"; }
            catch (Win32Exception error) { return "Windows could not start its installer (" + error.Message + ")"; }
            if (ExitCodeProblem(code) is { } problem) return problem + $" (installer log: {log})";
            if (Missing(at.SystemFolder, [runtime]).Count == 0) return null;
        }
        return "its installer finished, but " + string.Join(", ", runtime.Files) + " is still not in " + at.SystemFolder + $" (installer log: {log})";
    }

    // What an installer's exit code means for the player; null for success (3010 and 1641: Windows wants a restart,
    // the DLLs are in place; 1638: this or a newer version is installed already).
    public static string? ExitCodeProblem(int code) => code switch
    {
        0 or 3010 or 1641 or 1638 => null,
        1602 => "its installer was cancelled",
        1618 => "another installation (Windows Update or another installer) was running; wait for it to finish",
        1223 => "the Windows permission prompt was declined",
        _ => $"its installer stopped with code {code}",
    };

    // The installer at path: fetched unless it is there with its pinned size and checksum.
    static async Task DownloadAsync(HttpClient http, Runtime runtime, string path, CancellationToken cancel)
    {
        if (File.Exists(path) && new FileInfo(path).Length == runtime.Size)
        {
            await using var existing = File.OpenRead(path);
            if (Hash(existing).Equals(runtime.Sha256, StringComparison.OrdinalIgnoreCase)) return;
        }
        var partial = path + ".partial";
        await using (var source = await http.GetStreamAsync(runtime.Download, cancel))
        await using (var file = File.Create(partial)) await source.CopyToAsync(file, cancel);
        await using (var check = File.OpenRead(partial))
            if (check.Length != runtime.Size || !Hash(check).Equals(runtime.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                check.Dispose();
                File.Delete(partial);
                throw new InvalidDataException("the downloaded installer did not match its pinned checksum");
            }
        File.Move(partial, path, true);
    }

    static string Hash(Stream stream)
    {
        stream.Position = 0;
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    static async Task<int> RunElevated(string installer, string arguments, CancellationToken cancel)
    {
        using var process = Process.Start(new ProcessStartInfo(installer, arguments) { UseShellExecute = true, Verb = "runas" })
            ?? throw new Win32Exception("the installer did not start");
        await process.WaitForExitAsync(cancel);
        return process.ExitCode;
    }
}
