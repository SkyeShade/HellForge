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
    public string Zig => Path.Combine(Tools, "zig", "zig.exe");

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

public sealed record ModTool(string Name, string Version, string Url, string Sha256, string Exe, bool Ide);

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
        if (!File.Exists(sdk.Zig)) throw new IOException("The mod tools are missing: install them first (HellForge.Cli mod-tools, or the launcher's Mods page).");
        var dllName = CodeDll(modFolder);
        var sources = Sources(modFolder);
        if (sources.Count == 0) throw new InvalidDataException("No C or C++ sources in the mod's src folder.");
        var cpp = sources.Any(f => f.EndsWith(".cpp", StringComparison.Ordinal));
        var dll = into ?? Path.Combine(modFolder, dllName);
        var start = new ProcessStartInfo(sdk.Zig) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        // Zig's own cache beside the tools, not in %LOCALAPPDATA%\zig: what HellForge writes stays in its folders.
        start.Environment["ZIG_GLOBAL_CACHE_DIR"] = Path.Combine(sdk.Tools, "zig-cache");
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
            t.GetProperty("exe").GetString()!, t.GetProperty("ide").GetBoolean())).ToList();
    }

    public static bool ToolsReady(ModSdk sdk, bool ide = false) =>
        Tools(sdk).Where(t => ide || !t.Ide).All(t => File.Exists(Path.Combine(sdk.Tools, t.Name, t.Exe)));

    // Downloads the pinned tools into the SDK's tools folder (Zig; CMake and Ninja too with ide), each checked against
    // its SHA-256 before it is unpacked. Tools already there at their pinned archive are kept.
    public static async Task InstallToolsAsync(ModSdk sdk, bool ide, Action<string> progress, CancellationToken cancel = default)
    {
        var downloads = Directory.CreateDirectory(Path.Combine(sdk.Tools, "downloads")).FullName;
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        foreach (var tool in Tools(sdk).Where(t => ide || !t.Ide))
        {
            var destination = Path.Combine(sdk.Tools, tool.Name);
            var marker = Path.Combine(destination, ".hellforge-archive-sha256");
            if (File.Exists(marker) && File.ReadAllText(marker).Trim() == tool.Sha256 && File.Exists(Path.Combine(destination, tool.Exe)))
            { progress($"{tool.Name} {tool.Version}: installed"); continue; }
            var archive = Path.Combine(downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
            await DownloadAsync(http, tool, archive, progress, cancel);
            progress($"{tool.Name} {tool.Version}: unpacking");
            await UnpackAsync(archive, destination, progress, cancel);
            File.WriteAllText(marker, tool.Sha256);
            progress($"{tool.Name} {tool.Version}: installed");
        }
    }

    // The tool's archive in downloads: fetched unless it is there with its pinned checksum, and kept only with it.
    static async Task DownloadAsync(HttpClient http, ModTool tool, string archive, Action<string> progress, CancellationToken cancel)
    {
        if (File.Exists(archive) && Hash(archive) == tool.Sha256) return;
        progress($"{tool.Name} {tool.Version}: downloading {tool.Url}");
        var partial = archive + ".partial";
        await using (var source = await http.GetStreamAsync(tool.Url, cancel))
        await using (var file = File.Create(partial)) await source.CopyToAsync(file, cancel);
        if (Hash(partial) != tool.Sha256) { File.Delete(partial); throw new InvalidDataException($"{tool.Url} does not match its pinned checksum."); }
        File.Move(partial, archive, true);
    }

    // The archive unpacked into destination (its one top-level folder, when it has one), in place of what was there.
    static async Task UnpackAsync(string archive, string destination, Action<string> progress, CancellationToken cancel)
    {
        var staging = destination + "-staging";
        foreach (var folder in new[] { staging, destination }) if (Directory.Exists(folder)) await RetryWhileHeld(() => Directory.Delete(folder, true), progress, cancel);
        ZipFile.ExtractToDirectory(archive, staging);
        var entries = Directory.GetFileSystemEntries(staging);
        if (entries.Length == 1 && Directory.Exists(entries[0]))
        {
            await RetryWhileHeld(() => Directory.Move(entries[0], destination), progress, cancel);
            await RetryWhileHeld(() => Directory.Delete(staging), progress, cancel);
        }
        else await RetryWhileHeld(() => Directory.Move(staging, destination), progress, cancel);
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
