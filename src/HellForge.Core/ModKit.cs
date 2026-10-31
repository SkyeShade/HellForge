using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// Where the mod SDK is: an installation's app/sdk (headers, cmake/, templates/, docs/) with its tools in <installation>/tools,
// or the repository's native/mod-sdk with templates/ and tools/ beside it. Both keep the tools three levels above
// sdk/cmake, so the same CMake files work in either.
public sealed record ModSdk(string Folder, string Tools, string Template, string Docs)
{
    // Windows sessions compile with the pinned zig.exe; a Linux host compiles natively with zig-linux (tools.json).
    public string Zig => Path.Combine(Tools, "zig", "zig.exe");
    public string NativeZig => Path.Combine(Tools, "zig-linux", "zig");

    // The SDK of an installation, else of the repository a folder is in (searching upwards from it).
    public static ModSdk? Find(string start)
    {
        for (var folder = new DirectoryInfo(Path.GetFullPath(start)); folder != null; folder = folder.Parent)
        {
            var installed = Path.Combine(folder.FullName, "app", "sdk");
            if (File.Exists(Path.Combine(installed, "hellforge_mod.h")))
                return new(installed, Path.Combine(folder.FullName, "tools"), Path.Combine(installed, "templates", "c-mod"), Path.Combine(installed, "docs"));
            var repository = Path.Combine(folder.FullName, "native", "mod-sdk");
            if (File.Exists(Path.Combine(repository, "hellforge_mod.h")) && Directory.Exists(Path.Combine(folder.FullName, "templates", "c-mod")))
                return new(repository, Path.Combine(folder.FullName, "tools"), Path.Combine(folder.FullName, "templates", "c-mod"),
                    Path.Combine(folder.FullName, "docs"));
        }
        return null;
    }
}

public sealed record ModTool(string Name, string Version, string Url, string Sha256, string Exe, bool Ide,
    string Platform = "windows-x64", long Bytes = 0);

public static partial class ModKit
{
    // owner.mod-name: the owner and the mod's name, each lowercase letters, digits and '-'.
    [GeneratedRegex("^[a-z0-9][a-z0-9-]{0,30}\\.[a-z0-9][a-z0-9-]{0,30}$")]
    private static partial Regex NewModId();

    // Creates a mod from the SDK's template in mods/<mod-name> (an installation: built there, it is installed) or in
    // `parent`. Returns the mod's folder.
    public static string Create(ModSdk sdk, string parent, string id, string name, string? installForIde = null)
    {
        if (!NewModId().IsMatch(id)) throw new InvalidDataException("The mod id is owner.mod-name: lowercase letters, digits and '-' (\"yourname.my-mod\").");
        name = name.Trim();
        if (name.Length is 0 or > 64 || name.Any(c => char.IsControl(c) || c is '"' or '\\' or '{' or '}'))
            throw new InvalidDataException("The mod's name is 1-64 characters without quotes, backslashes or braces.");
        var folderName = id[(id.IndexOf('.') + 1)..];
        var dll = folderName.Replace('-', '_');
        if (char.IsAsciiDigit(dll[0])) dll = "mod_" + dll;
        var folder = Path.GetFullPath(Path.Combine(parent, folderName));
        if (Directory.Exists(folder) || File.Exists(folder)) throw new IOException($"{folder} exists already.");
        if (!Directory.Exists(sdk.Template)) throw new IOException($"The mod template is missing: {sdk.Template}.");
        string Relative(string target) => Path.GetRelativePath(folder, target).Replace('\\', '/');
        var values = new Dictionary<string, string>
        {
            ["{{id}}"] = id, ["{{name}}"] = name, ["{{dll}}"] = dll,
            ["{{sdk}}"] = Relative(sdk.Folder), ["{{tools}}"] = Relative(sdk.Tools),
            // An IDE build copies the mod into this installation; one made inside an installation is built where it is.
            ["{{install}}"] = (installForIde ?? "").Replace('\\', '/'),
        };
        string Fill(string text) => values.Aggregate(text, (t, v) => t.Replace(v.Key, v.Value));
        var staging = folder + ".creating";
        if (Directory.Exists(staging)) Directory.Delete(staging, true);
        foreach (var file in Directory.EnumerateFiles(sdk.Template, "*", SearchOption.AllDirectories))
        {
            var target = Path.Combine(staging, Fill(Path.GetRelativePath(sdk.Template, file)));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.WriteAllText(target, Fill(File.ReadAllText(file)).ReplaceLineEndings("\n"));
        }
        Directory.Move(staging, folder);
        return folder;
    }

    // The compiler flags every HellForge mod is built with (scripts/Build-Mod.ps1); part of a source build's cache key.
    public static IReadOnlyList<string> Flags(bool cpp) => [cpp ? "c++" : "cc", cpp ? "-std=c++17" : "-std=c11", "-target", "x86_64-windows-gnu",
        "-shared", "-O2", "-Wall", "-Werror", "-Wno-unused-parameter", "-s"];

    // A mod's C and C++ sources (src/**/*.c, *.cpp), in a fixed order.
    public static List<string> Sources(string modFolder)
    {
        var sourceFolder = Path.Combine(modFolder, "src");
        return Directory.Exists(sourceFolder)
            ? Directory.EnumerateFiles(sourceFolder, "*", SearchOption.AllDirectories).Where(f => Path.GetExtension(f) is ".c" or ".cpp").Order(StringComparer.Ordinal).ToList()
            : [];
    }

    // Builds a code mod's sources (src/*.c, *.cpp) into the DLL its manifest names, beside the manifest (or at `into`), with
    // the SDK's pinned Zig and the flags every HellForge mod is built with. Returns the DLL; throws with the compiler's
    // output when it does not build.
    public static string Build(string modFolder, ModSdk? sdk = null, Action<string>? output = null, string? into = null)
    {
        modFolder = Path.GetFullPath(modFolder);
        sdk ??= ModSdk.Find(modFolder) ?? throw new IOException("The mod SDK was not found above " + modFolder + ".");
        var linuxHost = OperatingSystem.IsLinux();
        var zig = Compiler(sdk, linuxHost, NativeBuildOnly);
        if (!File.Exists(zig)) throw new IOException("The mod tools are missing: install them with the Mods page's Install mod tools (or HellForge.Cli mod-tools).");
        var dllName = CodeDll(modFolder);
        var sources = Sources(modFolder);
        if (sources.Count == 0) throw new InvalidDataException("No C or C++ sources in the mod's src folder.");
        var cpp = sources.Any(f => f.EndsWith(".cpp", StringComparison.Ordinal));
        var dll = into ?? Path.Combine(modFolder, dllName);
        var start = new ProcessStartInfo(zig) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        // Zig's own cache beside the tools, not in %LOCALAPPDATA%\zig or ~/.cache/zig: what HellForge writes stays in its
        // folders (the native host's is separate, so a shared installation keeps the two caches apart).
        start.Environment["ZIG_GLOBAL_CACHE_DIR"] = Path.Combine(sdk.Tools, linuxHost ? "zig-cache-linux" : "zig-cache");
        foreach (var argument in Flags(cpp).Concat(["-I", sdk.Folder])) start.ArgumentList.Add(argument);
        foreach (var source in sources) start.ArgumentList.Add(source);
        start.ArgumentList.Add("-o"); start.ArgumentList.Add(dll);
        using var process = Process.Start(start) ?? throw new IOException("Zig did not start.");
        var errors = process.StandardError.ReadToEndAsync();
        var text = process.StandardOutput.ReadToEnd() + errors.Result;
        process.WaitForExit();
        if (text.Length > 0) output?.Invoke(text.TrimEnd());
        if (process.ExitCode != 0) throw new InvalidDataException("The mod did not compile:\n" + text.TrimEnd());
        foreach (var leftover in Directory.EnumerateFiles(Path.GetDirectoryName(dll)!).Where(f => Path.GetExtension(f) is ".lib" or ".pdb")) File.Delete(leftover);
        return dll;
    }

    // The compiler a build starts: the pinned native Linux Zig on a Linux host, else the pinned zig.exe. A Wine-hosted
    // session on Linux (the wrapper sets HELLFORGE_NATIVE_MOD_BUILDS) is refused instead of trying zig.exe: its mods were
    // built natively before the session, and one without a build stays off.
    public static string Compiler(ModSdk sdk, bool linuxHost, bool nativeOnly)
    {
        if (linuxHost) return NativeLinuxZig(sdk) ?? throw new IOException("The native Linux Zig is missing: install it with the Mods page's Install mod tools (or HellForge.Cli mod-tools), set ZIG, or put zig on PATH.");
        if (nativeOnly) throw new IOException("Source mods are built with the native Linux Zig before the session; this build did not run, so the mod stays off rather than compiling tools/zig/zig.exe under Wine. Use the Mods page's Install mod tools, then start the session again.");
        return sdk.Zig;
    }

    [GeneratedRegex("^[A-Za-z0-9_.-]{1,64}\\.dll$")]
    private static partial Regex DllName();

    // The DLL a mod's manifest names (before it is built, which ModCatalog.Parse requires).
    public static string CodeDll(string modFolder)
    {
        var path = Path.Combine(modFolder, ModCatalog.ManifestName);
        if (!File.Exists(path)) throw new IOException($"{modFolder} has no {ModCatalog.ManifestName}.");
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (document.RootElement.TryGetProperty("code", out var code) && code.ValueKind == JsonValueKind.Object &&
                code.TryGetProperty("dll", out var dll) && dll.ValueKind == JsonValueKind.String && DllName().IsMatch(dll.GetString()!))
                return dll.GetString()!;
        }
        catch (JsonException error) { throw new InvalidDataException($"{ModCatalog.ManifestName}: {error.Message}"); }
        throw new InvalidDataException("The mod's manifest names no DLL: \"code\": {\"dll\": \"<name>.dll\", \"sdk\": 6}.");
    }

    public static IReadOnlyList<ModTool> Tools(ModSdk sdk)
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(sdk.Folder, "tools.json")));
        return document.RootElement.GetProperty("tools").EnumerateArray().Select(t => new ModTool(t.GetProperty("name").GetString()!,
            t.GetProperty("version").GetString()!, t.GetProperty("url").GetString()!, t.GetProperty("sha256").GetString()!,
            t.GetProperty("exe").GetString()!, t.GetProperty("ide").GetBoolean(),
            t.TryGetProperty("platform", out var platform) ? platform.GetString()! : "windows-x64",
            t.TryGetProperty("bytes", out var bytes) ? bytes.GetInt64() : 0)).ToList();
    }

    public static string ToolPath(ModSdk sdk, ModTool tool) => Path.Combine(sdk.Tools, tool.Name, tool.Exe);

    // What a pinned tool needs before its archive may be downloaded: HTTPS, a real 64-digit SHA-256 (not a zero-filled
    // placeholder), and a file name. An entry without a size can still be installed (CMake and Ninja have none).
    public static bool HasPinnedArchive(ModTool tool) =>
        Uri.TryCreate(tool.Url, UriKind.Absolute, out var url) && url.Scheme == Uri.UriSchemeHttps &&
        !string.IsNullOrEmpty(Path.GetFileName(url.AbsolutePath)) && tool.Sha256.Length == 64 && tool.Sha256.All(c => char.IsAsciiHexDigit(c)) &&
        tool.Sha256.Any(c => c != '0');

    // A complete pin names the archive and its exact size. New native tools use this so an unfilled placeholder falls back
    // instead of downloading something the release did not pin.
    public static bool HasCompletePin(ModTool tool) => tool.Bytes > 0 && HasPinnedArchive(tool);

    // The host whose tools an installation needs: Windows sessions and IDEs keep zig.exe, CMake and Ninja; a Linux host
    // installs zig-linux, which builds its source mods natively before a session, and hpatchz-linux for build patches.
    public static string HostPlatform => OperatingSystem.IsLinux() ? "linux-x64" : "windows-x64";

    public static IReadOnlyList<ModTool> ToolsForThisPlatform(ModSdk sdk, bool ide = false) =>
        Tools(sdk).Where(t => t.Platform == HostPlatform && (ide || !t.Ide)).ToList();

    public static bool ToolsReady(ModSdk sdk, bool ide = false)
    {
        var tools = ToolsForThisPlatform(sdk, ide);
        return tools.Count > 0 && tools.All(t => File.Exists(ToolPath(sdk, t)));
    }

    // The compiler a cache key identifies: the Zig version and target, deliberately not the host, URL or archive hash.
    // Both pins are Zig 0.17.0 targeting x86_64-windows-gnu, so a native build and a Windows session share the key.
    public static string CompilerIdentity(ModSdk sdk)
    {
        var tools = Tools(sdk);
        var version = tools.FirstOrDefault(t => t.Name == "zig")?.Version
            ?? tools.FirstOrDefault(t => t.Name == "zig-linux")?.Version ?? "no zig";
        return "zig " + version + " / x86_64-windows-gnu";
    }

    // A Wine-hosted session on Linux cannot run the native compiler, so the wrapper builds first and sets this for the
    // session it starts: one without a build is reported, never compiled with zig.exe under Wine.
    public const string NativeBuildsVariable = "HELLFORGE_NATIVE_MOD_BUILDS";
    public static bool NativeBuildOnly => Environment.GetEnvironmentVariable(NativeBuildsVariable) == "1";

    // The native Linux Zig hf-build-mod.sh and a Linux-host build use: the pinned tools/zig-linux/zig first, then ZIG,
    // ~/.local/share/zig-native/zig, then zig on PATH; each candidate must be executable.
    public static string? NativeLinuxZig(ModSdk sdk, string? zig, string? home, string? path, Func<string, bool> isExecutable)
    {
        if (isExecutable(sdk.NativeZig)) return sdk.NativeZig;
        return FindNativeLinuxZig(zig, home, path, isExecutable);
    }

    public static string? NativeLinuxZig(ModSdk sdk) => NativeLinuxZig(sdk, Environment.GetEnvironmentVariable("ZIG"),
        Environment.GetEnvironmentVariable("HOME"), Environment.GetEnvironmentVariable("PATH"), CanRun);

    static bool CanRun(string path)
    {
        if (!File.Exists(path)) return false;
        if (!OperatingSystem.IsLinux()) return true;
        try
        {
            const UnixFileMode execute = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
            return (File.GetUnixFileMode(path) & execute) != 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    // The native Linux Zig from ZIG (else ~/.local/share/zig-native/zig), then zig on PATH; each candidate must be
    // executable. The pinned tools/zig-linux/zig is checked first by NativeLinuxZig.
    public static string? FindNativeLinuxZig(string? zig, string? home, string? path, Func<string, bool> isExecutable)
    {
        var candidate = !string.IsNullOrEmpty(zig) ? zig
            : !string.IsNullOrEmpty(home) ? Path.Combine(home, ".local", "share", "zig-native", "zig")
            : null;
        if (candidate != null && isExecutable(candidate)) return candidate;
        foreach (var folder in (path ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            candidate = Path.Combine(folder, "zig");
            if (isExecutable(candidate)) return candidate;
        }
        return null;
    }

    // Downloads the pinned tools into the SDK's tools folder (Zig; CMake and Ninja too with ide; hpatchz-linux on Linux),
    // each checked against its SHA-256 before it is unpacked. Tools already there at their pinned archive are kept.
    public static async Task InstallToolsAsync(ModSdk sdk, bool ide, Action<string> progress, CancellationToken cancel = default)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        foreach (var tool in ToolsForThisPlatform(sdk, ide))
            await InstallToolAsync(http, sdk, tool, progress, cancel);
    }

    // One pinned tool, by the same path and checks as InstallToolsAsync; the build-patch setup uses this for hpatchz-linux
    // without fetching the mod compiler.
    public static async Task InstallToolAsync(ModSdk sdk, ModTool tool, Action<string> progress, CancellationToken cancel = default)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        await InstallToolAsync(http, sdk, tool, progress, cancel);
    }

    internal static async Task InstallToolAsync(HttpClient http, ModSdk sdk, ModTool tool, Action<string> progress, CancellationToken cancel)
    {
        if (!HasPinnedArchive(tool)) throw new InvalidDataException($"{tool.Name}'s release is not pinned in tools.json yet.");
        var downloads = Directory.CreateDirectory(Path.Combine(sdk.Tools, "downloads")).FullName;
        var destination = Path.Combine(sdk.Tools, tool.Name);
        var marker = Path.Combine(destination, ".hellforge-archive-sha256");
        var executable = ToolPath(sdk, tool);
        if (File.Exists(marker) && File.ReadAllText(marker).Trim() == tool.Sha256 && File.Exists(executable))
        { EnsureExecutable(executable); progress($"{tool.Name} {tool.Version}: installed"); return; }
        var archive = Path.Combine(downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
        await DownloadAsync(http, tool, archive, progress, cancel);
        progress($"{tool.Name} {tool.Version}: unpacking");
        await UnpackAsync(archive, destination, progress, cancel);
        if (!File.Exists(executable)) throw new InvalidDataException($"{tool.Name}'s archive has no {tool.Exe}.");
        EnsureExecutable(executable);
        File.WriteAllText(marker, tool.Sha256);
        progress($"{tool.Name} {tool.Version}: installed");
    }

    // tar keeps the executable bit, but an installation copied from Windows or unpacked with another program may not.
    static void EnsureExecutable(string path)
    {
        if (!OperatingSystem.IsLinux()) return;
        const UnixFileMode execute = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
        File.SetUnixFileMode(path, File.GetUnixFileMode(path) | execute);
    }

    // The tool's archive in downloads: fetched unless it is there with its pinned size and checksum, and kept only with
    // both. An entry without a size (CMake and Ninja) is checked by checksum alone.
    static async Task DownloadAsync(HttpClient http, ModTool tool, string archive, Action<string> progress, CancellationToken cancel)
    {
        if (File.Exists(archive) && (tool.Bytes <= 0 || new FileInfo(archive).Length == tool.Bytes) && Hash(archive) == tool.Sha256) return;
        progress($"{tool.Name} {tool.Version}: downloading {tool.Url}");
        var partial = archive + ".partial";
        await using (var source = await http.GetStreamAsync(tool.Url, cancel))
        await using (var file = File.Create(partial)) await source.CopyToAsync(file, cancel);
        if (tool.Bytes > 0 && new FileInfo(partial).Length != tool.Bytes)
        { File.Delete(partial); throw new InvalidDataException($"{tool.Url} does not match its pinned size."); }
        if (Hash(partial) != tool.Sha256) { File.Delete(partial); throw new InvalidDataException($"{tool.Url} does not match its pinned checksum."); }
        File.Move(partial, archive, true);
    }

    // The archive unpacked into destination (its one top-level folder, when it has one), in place of what was there.
    static async Task UnpackAsync(string archive, string destination, Action<string> progress, CancellationToken cancel)
    {
        var staging = destination + "-staging";
        foreach (var folder in new[] { staging, destination }) if (Directory.Exists(folder)) await RetryWhileHeld(() => Directory.Delete(folder, true), progress, cancel);
        var lower = archive.ToLowerInvariant();
        if (lower.EndsWith(".tar.xz") || lower.EndsWith(".tar.gz") || lower.EndsWith(".tgz")) ExtractTar(archive, staging);
        else ZipFile.ExtractToDirectory(archive, staging);
        var entries = Directory.GetFileSystemEntries(staging);
        if (entries.Length == 1 && Directory.Exists(entries[0]))
        {
            await RetryWhileHeld(() => Directory.Move(entries[0], destination), progress, cancel);
            await RetryWhileHeld(() => Directory.Delete(staging), progress, cancel);
        }
        else await RetryWhileHeld(() => Directory.Move(staging, destination), progress, cancel);
    }

    // Zig's Linux archive is .tar.xz and HDiffPatch's is .tar.gz; the system tar, with the matching decompressor, unpacks
    // both. No managed fallback: a missing tar is reported clearly instead of leaving a half-installed tool.
    static void ExtractTar(string archive, string staging)
    {
        Directory.CreateDirectory(staging);
        var start = new ProcessStartInfo("tar") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("-xf"); start.ArgumentList.Add(archive);
        start.ArgumentList.Add("-C"); start.ArgumentList.Add(staging);
        Process process;
        try { process = Process.Start(start) ?? throw new IOException("tar did not start."); }
        catch (System.ComponentModel.Win32Exception) { throw new InvalidDataException("tar is needed to unpack " + Path.GetFileName(archive) + " and was not found; install tar with xz and gzip support, then try again."); }
        using (process)
        {
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            if (process.ExitCode != 0) throw new InvalidDataException("Could not unpack " + Path.GetFileName(archive) + ":\n" + (output.Result + error.Result).Trim());
        }
    }

    // Moving or deleting files that were just unpacked: antivirus (Windows Defender) scans a new executable, Zig's is
    // 190 MB, and holds it open meanwhile, which refuses the move ("access denied"). Waits for it, up to two minutes.
    public static async Task RetryWhileHeld(Action action, Action<string> progress, CancellationToken cancel = default)
    {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        for (var attempt = 1; ; attempt++)
        {
            try { action(); return; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException && clock.Elapsed < TimeSpan.FromMinutes(2))
            {
                if (attempt == 1) progress("waiting for the antivirus to finish scanning the unpacked files...");
                await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(250 * attempt, 2000)), cancel);
            }
        }
    }

    static string Hash(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file));
    }
}
