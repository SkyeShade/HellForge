using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core;

public enum SteamRuntimeMode { Host, Container }

// The result of reading the Proton's toolmanifest.vdf and the runtime app's own manifest. The command lines keep
// %verb% unsubstituted, so readiness and tests can see exactly what Steam's manifests said. VersionsReadable is true
// when VERSIONS.txt was present and readable; a readable but unparseable table records "unknown" and still counts,
// while a missing or unreadable file is what readiness treats as a failure later.
public sealed record SteamRuntime(
    SteamRuntimeMode Mode,
    string Reason,
    string? AppId,
    string? Folder,
    string? EntryPoint,
    IReadOnlyList<string> RuntimeCommandLine,
    IReadOnlyList<string> ProtonCommandLine,
    string? Name,
    string? Version,
    string? VersionsSha256,
    bool VersionsReadable)
{
    // The readiness identity: the runtime folder plus the exact VERSIONS.txt bytes Steam shipped. A runtime update
    // changes the hash even when the parsed lines do not change.
    public string Identity => Folder is null ? "" : Folder + "|" + (VersionsSha256 ?? "no-hash");
}

// Steam's two-manifest compat-tool chain, the same resolver as linux/hf-steam.sh. A Proton whose toolmanifest has no
// require_tool_appid runs on the host, exactly as it does today. Every failure that selects host mode carries a short
// reason; a present runtime that fails later is never silently retried on the host.
public static class SteamRuntimeLocator
{
    const string ManifestName = "toolmanifest.vdf";
    const string VersionsName = "VERSIONS.txt";

    public static SteamRuntime Resolve(string protonScript, string steamRoot)
    {
        if (!OperatingSystem.IsLinux()) return Host("the Steam Linux Runtime chain is Linux-only", []);
        var proton = LocalInstall.ResolveLinks(LocalInstall.FullPath(protonScript));
        var steam = LocalInstall.ResolveLinks(LocalInstall.FullPath(steamRoot));
        var protonFolder = Path.GetDirectoryName(proton)!;
        var manifestPath = Path.Combine(protonFolder, ManifestName);
        if (Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME") == "off")
            return Host("HELLFORGE_STEAM_RUNTIME=off", ManifestCommandLine(manifestPath, protonFolder, proton));
        if (!File.Exists(manifestPath))
            return Host($"no toolmanifest.vdf beside {Path.GetFileName(protonScript)}", []);
        var manifest = ReadToolManifest(manifestPath);
        if (manifest == null) return Host("the Proton toolmanifest could not be read", []);
        if (Value(manifest, "version") != "2")
            return Host($"unsupported Proton toolmanifest version '{VersionText(manifest)}'", []);
        var protonCommandLine = ManifestCommandLine(manifestPath, protonFolder, proton);
        var appId = Value(manifest, "require_tool_appid");
        if (appId.Length == 0) return Host("the Proton toolmanifest has no require_tool_appid", protonCommandLine);
        if (!appId.All(char.IsAsciiDigit))
            return Host($"the Proton toolmanifest has a non-numeric require_tool_appid '{appId}'", protonCommandLine);
        var folder = FindRuntimeFolder(steam, appId, out var problem);
        if (folder == null) return Host(problem, protonCommandLine);
        return ResolveRuntime(folder, appId, protonCommandLine);
    }

    // The entry point's own part of a container-mode argv (<entry point> --verb=<verb> --), for a caller that puts a
    // program of its own (hf-sandbox) between it and Proton. Empty in host mode.
    public static IReadOnlyList<string> EntryPointArguments(SteamRuntime runtime, string verb)
    {
        var result = new List<string>();
        if (runtime.Mode == SteamRuntimeMode.Container)
            AppendCommandLine(result, runtime.RuntimeCommandLine, runtime.EntryPoint ?? "", verb);
        return result;
    }

    // Proton's own part (<proton> <verb>), from its manifest when usable, else the direct form.
    public static IReadOnlyList<string> ProtonArguments(SteamRuntime runtime, string protonScript, string verb)
    {
        var proton = LocalInstall.ResolveLinks(LocalInstall.FullPath(protonScript));
        var result = new List<string>();
        if (runtime.ProtonCommandLine.Count > 0) AppendCommandLine(result, runtime.ProtonCommandLine, proton, verb);
        else result.AddRange([proton, verb]);
        return result;
    }

    // argv for one Proton invocation. Container mode is <entry point> --verb=<verb> -- <proton> <verb> <args>; host
    // mode uses a usable Proton manifest commandline, else the direct proton <verb> form the wrapper uses today.
    public static IReadOnlyList<string> Arguments(SteamRuntime runtime, string protonScript, string verb,
        IEnumerable<string> arguments)
    {
        var result = new List<string>(EntryPointArguments(runtime, verb));
        result.AddRange(ProtonArguments(runtime, protonScript, verb));
        result.AddRange(arguments);
        return result;
    }

    static SteamRuntime Host(string reason, IReadOnlyList<string> protonCommandLine) =>
        new(SteamRuntimeMode.Host, reason, null, null, null, [], protonCommandLine, null, null, null, false);

    static SteamRuntime ResolveRuntime(string folder, string appId, IReadOnlyList<string> protonCommandLine)
    {
        var manifestPath = Path.Combine(folder, ManifestName);
        if (!File.Exists(manifestPath))
            return Host($"runtime app {appId} has no toolmanifest.vdf", protonCommandLine);
        var manifest = ReadToolManifest(manifestPath);
        if (manifest == null)
            return Host($"runtime app {appId} has an unreadable toolmanifest.vdf", protonCommandLine);
        if (Value(manifest, "version") != "2")
            return Host($"unsupported runtime toolmanifest version '{VersionText(manifest)}' for app {appId}",
                protonCommandLine);
        var nested = Value(manifest, "require_tool_appid");
        if (nested.Length > 0)
            return Host($"runtime app {appId} requires another tool ({nested}); Steam has one runtime layer",
                protonCommandLine);
        var commandLine = SplitCommandLine(Value(manifest, "commandline"));
        if (commandLine.Count == 0 || !HasVerb(commandLine))
            return Host($"runtime app {appId} has no commandline containing %verb%", protonCommandLine);
        if (commandLine[0].Length < 2 || commandLine[0][0] != '/')
            return Host($"the runtime commandline does not start with an absolute entry point: {commandLine[0]}",
                protonCommandLine);
        if (HasTraversal(commandLine[0]))
            return Host($"the runtime entry point path has a traversal: {commandLine[0]}", protonCommandLine);
        var folderReal = LocalInstall.ResolveLinks(folder);
        var entry = Path.Combine(folderReal, commandLine[0][1..]);
        if (!File.Exists(entry) || !IsExecutable(entry))
            return Host($"the runtime entry point is missing or not executable: {entry}", protonCommandLine);
        var entryReal = LocalInstall.ResolveLinks(entry);
        if (!Within(entryReal, folderReal))
            return Host($"the runtime entry point resolves outside its app folder: {entryReal}", protonCommandLine);
        if (protonCommandLine.Count == 0)
            return Host("the Proton toolmanifest has no usable commandline", []);
        var versions = ReadVersions(folderReal);
        return new SteamRuntime(SteamRuntimeMode.Container, "", appId, folderReal, entryReal, commandLine,
            protonCommandLine, versions.Name, versions.Version, versions.Sha256, versions.Readable);
    }

    static ValveData? ReadManifest(string path)
    {
        try { return ValveData.Parse(File.ReadAllText(path)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return null;
        }
    }

    // Steam's toolmanifest.vdf keeps version, commandline and require_tool_appid inside a nested "manifest" block.
    // A file that puts them at the root is also accepted, so a small custom manifest still works.
    static ValveData? ReadToolManifest(string path)
    {
        var document = ReadManifest(path);
        if (document == null) return null;
        if (!document.Children.TryGetValue("manifest", out var manifest)) return document;
        // Accept a mixed shape too: the real files are nested, but a small root-level key must not be lost.
        foreach (var pair in document.Values) manifest.Values.TryAdd(pair.Key, pair.Value);
        return manifest;
    }

    static string Value(ValveData data, string key) =>
        data.Values.TryGetValue(key, out var value) ? value : "";

    static string VersionText(ValveData data) => Value(data, "version") is { Length: > 0 } text ? text : "missing";

    // A Proton toolmanifest commandline is usable only when its first token names the selected proton script. The
    // manifest can add tokens before the verb (a custom tool such as GE-Proton does); they are kept.
    static IReadOnlyList<string> ManifestCommandLine(string manifestPath, string folder, string proton)
    {
        if (!File.Exists(manifestPath)) return [];
        var manifest = ReadToolManifest(manifestPath);
        if (manifest == null) return [];
        if (Value(manifest, "version") != "2") return [];
        var parts = SplitCommandLine(Value(manifest, "commandline"));
        if (parts.Count == 0 || !HasVerb(parts) || !SafeAbsolute(parts[0])) return [];
        var candidate = Path.Combine(folder, parts[0][1..]);
        return File.Exists(candidate) && SameFile(candidate, proton) ? parts : [];
    }

    static bool SafeAbsolute(string token) => token.Length >= 2 && token[0] == '/' && !HasTraversal(token);

    static bool HasTraversal(string token)
    {
        if (token.Length < 2 || token[0] != '/') return false;
        var relative = token[1..];
        if (relative.Contains("//", StringComparison.Ordinal)) return true;
        foreach (var part in relative.Split('/'))
            if (part is "" or "." or "..") return true;
        return false;
    }

    static bool SameFile(string left, string right) =>
        LocalInstall.ResolveLinks(LocalInstall.FullPath(left)) == LocalInstall.ResolveLinks(LocalInstall.FullPath(right));

    static bool Within(string path, string folder)
    {
        var prefix = folder.EndsWith(Path.DirectorySeparatorChar) ? folder : folder + Path.DirectorySeparatorChar;
        return path == folder || path.StartsWith(prefix, StringComparison.Ordinal);
    }

    static bool IsExecutable(string path)
    {
        if (!OperatingSystem.IsLinux()) return false;
        try
        {
            var mode = File.GetUnixFileMode(path);
            return (mode & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) != 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    // The first valid runtime install in Steam's deterministic library order wins. installdir and StateFlags are checked
    // the way LocalInstall.Inspect checks Steam's game manifest.
    static string? FindRuntimeFolder(string steamRoot, string appId, out string problem)
    {
        problem = $"runtime app {appId} is not installed in any Steam library";
        IReadOnlyList<string> libraries;
        try { libraries = LocalInstall.FindLibraries(steamRoot); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            problem = $"the Steam libraries could not be read: {error.Message}";
            return null;
        }
        foreach (var library in libraries)
        {
            var manifestPath = Path.Combine(library, "steamapps", $"appmanifest_{appId}.acf");
            if (!File.Exists(manifestPath)) continue;
            var document = ReadManifest(manifestPath);
            if (document == null || !document.Children.TryGetValue("AppState", out var app))
            {
                problem = $"runtime app {appId} has an unreadable app manifest: {manifestPath}";
                continue;
            }
            if (Value(app, "appid") != appId) continue;
            var name = Value(app, "installdir");
            if (!ValidInstallDir(name)) continue;
            if (!RuntimeInstalled(app)) continue;
            var folder = Path.Combine(library, "steamapps", "common", name);
            if (Directory.Exists(folder)) return LocalInstall.ResolveLinks(folder);
        }
        return null;
    }

    static bool ValidInstallDir(string name) =>
        name is not ("" or "." or "..") && name.IndexOfAny(['/', '\\', ':']) < 0 &&
        !name.Any(char.IsControl) && name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;

    // EAppState: installed in full (4); missing (32), corrupt (128), running (64) or any busy bit means not usable.
    static bool RuntimeInstalled(ValveData app)
    {
        var text = Value(app, "StateFlags");
        if (text.Length == 0) return true;
        if (!uint.TryParse(text, out var flags)) return false;
        const uint installed = 4, running = 64, missing = 32, corrupt = 128;
        const uint busy = 256 | 1024 | 2048 | 4096 | 65536 | 131072 | 262144 | 524288 | 1048576 | 2097152 | 4194304 |
            8388608;
        return (flags & installed) != 0 && (flags & (missing | corrupt | running | busy)) == 0;
    }

    // VERSIONS.txt is a tab-separated table with a #Name header. The runtime row (the row whose Runtime column is set,
    // usually steamrt4) names the runtime; the depot row supplies the overall version. Anything else parses to the
    // folder name and "unknown" but still counts as readable.
    static (string Name, string Version, string? Sha256, bool Readable) ReadVersions(string folder)
    {
        var fallback = Path.GetFileName(folder);
        var path = Path.Combine(folder, VersionsName);
        if (!File.Exists(path)) return (fallback, "unknown", null, false);
        try
        {
            var bytes = File.ReadAllBytes(path);
            var (name, version) = ParseVersionsText(Encoding.UTF8.GetString(bytes), fallback);
            return (name, version, Convert.ToHexString(SHA256.HashData(bytes)), true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return (fallback, "unknown", null, false);
        }
    }

    internal static (string Name, string Version) ParseVersionsText(string text, string fallbackName)
    {
        var name = fallbackName;
        var version = "unknown";
        string? depot = null;
        string? runtimeName = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0 || line.TrimStart().StartsWith('#')) continue;
            var columns = line.Split('\t');
            if (columns.Length < 2) continue;
            var first = columns[0].Trim();
            if (depot == null && first.Equals("depot", StringComparison.OrdinalIgnoreCase))
                depot = columns[1].Trim();
            if (runtimeName == null && first.Length > 0 && columns.Length >= 3 && columns[2].Trim().Length > 0)
                runtimeName = first;
        }
        if (!string.IsNullOrEmpty(runtimeName)) name = runtimeName!;
        if (!string.IsNullOrEmpty(depot)) version = depot!;
        return (name, version);
    }

    // Whitespace separates tokens and double quotes group them. ValveData has already unescaped \" in the value.
    internal static IReadOnlyList<string> SplitCommandLine(string text)
    {
        var tokens = new List<string>();
        var token = new StringBuilder();
        var quoted = false;
        var any = false;
        foreach (var character in text)
        {
            if (character == '"') { quoted = !quoted; any = true; continue; }
            if (char.IsWhiteSpace(character) && !quoted)
            {
                if (any || token.Length > 0) { tokens.Add(token.ToString()); token.Clear(); any = false; }
                continue;
            }
            token.Append(character);
            any = true;
        }
        if (quoted) return [];
        if (any || token.Length > 0) tokens.Add(token.ToString());
        return tokens;
    }

    // %verb% may be a whole word (Proton's "/proton %verb%") or inside one (the runtime's "--verb=%verb%").
    static bool HasVerb(IReadOnlyList<string> words) => words.Any(word => word.Contains("%verb%", StringComparison.Ordinal));

    static void AppendCommandLine(List<string> target, IReadOnlyList<string> words, string first, string verb)
    {
        for (var index = 0; index < words.Count; index++)
        {
            var token = index == 0 ? first : words[index];
            target.Add(token.Replace("%verb%", verb, StringComparison.Ordinal));
        }
    }
}
