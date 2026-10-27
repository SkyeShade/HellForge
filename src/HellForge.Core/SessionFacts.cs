using System.Globalization;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Win32;

namespace HellForge.Core;

public sealed record GpuInfo(string Name, string Driver);

// The choices a session ran with, as the session runner was given them.
public sealed record SessionSettings(bool Sandboxed, string War, bool LocalPlayFab, bool Multiplayer, bool EngineLog, bool CrashDump,
    IReadOnlyList<string> GameOptions);

// What a bug report needs to know first about a session: HellForge's, the engine's, the mod SDK's and the game's versions,
// the system it ran on (Windows' version, Wine's when under Wine or Proton, the display adapters and their drivers) and the
// session's settings. The session runner writes them as session.log's opening lines and into result.json ("report");
// a crash report (CrashReport) reads them back. None of it names the PC or the player: the lines go through Privacy.
public sealed record SessionFacts(string HellForge, string Engine, int ModSdk, string GameBuild, string System, string? Wine,
    IReadOnlyList<GpuInfo> Gpus, SessionSettings Settings)
{
    public static SessionFacts Collect(string engine, SessionSettings settings, string? wine) =>
        new(AppVersion.Text, engine, ModCatalog.SdkVersion, SupportedBuild.Current.BuildId, SystemFacts.Windows(), wine, SystemFacts.Gpus(), settings);

    static string OnOff(bool value) => value ? "on" : "off";

    public IEnumerable<string> Lines()
    {
        yield return Privacy.Scrub($"HellForge {HellForge}, engine {Engine}, mod SDK {ModSdk}, game build {GameBuild}");
        yield return Privacy.Scrub("System: " + System + (Wine != null ? ", under Wine " + Wine : ""));
        foreach (var gpu in Gpus) yield return Privacy.Scrub($"GPU: {gpu.Name}, driver {gpu.Driver}");
        if (Gpus.Count == 0) yield return "GPU: not found";
        var s = Settings;
        yield return Privacy.Scrub($"Settings: sandboxed {(s.Sandboxed ? "yes" : "no")}, war {s.War}, Local PlayFab {OnOff(s.LocalPlayFab)}, " +
            $"multiplayer {OnOff(s.Multiplayer)}, engine log {OnOff(s.EngineLog)}, crash dump {OnOff(s.CrashDump)}, launch options " +
            (s.GameOptions.Count == 0 ? "none" : GameLaunchOptions.CommandLine(s.GameOptions)));
    }

    public JsonObject ToJson() => new()
    {
        ["hellforge"] = HellForge, ["engine"] = Engine, ["modSdk"] = ModSdk, ["gameBuild"] = GameBuild, ["system"] = System, ["wine"] = Wine,
        ["gpus"] = new JsonArray([.. Gpus.Select(g => (JsonNode?)new JsonObject { ["name"] = g.Name, ["driver"] = g.Driver })]),
        ["sandboxed"] = Settings.Sandboxed, ["war"] = Settings.War, ["localPlayFab"] = Settings.LocalPlayFab, ["multiplayer"] = Settings.Multiplayer,
        ["engineLog"] = Settings.EngineLog, ["crashDump"] = Settings.CrashDump,
        ["gameOptions"] = new JsonArray([.. Settings.GameOptions.Select(o => (JsonNode?)JsonValue.Create(o))])
    };

    // The facts result.json keeps ("report"), or null for a session from before they were kept.
    public static SessionFacts? From(JsonElement report)
    {
        if (report.ValueKind != JsonValueKind.Object) return null;
        string Text(string name) => SessionFiles.Text(report, name);
        bool Flag(string name) => report.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
        IEnumerable<JsonElement> Items(string name) => report.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray() : [];
        var gpus = Items("gpus").Select(g => new GpuInfo(SessionFiles.Text(g, "name"), SessionFiles.Text(g, "driver"))).ToList();
        var options = Items("gameOptions").Where(o => o.ValueKind == JsonValueKind.String).Select(o => o.GetString()!).ToList();
        var settings = new SessionSettings(Flag("sandboxed"), Text("war"), Flag("localPlayFab"), Flag("multiplayer"), Flag("engineLog"), Flag("crashDump"), options);
        var wine = Text("wine");
        return new(Text("hellforge"), Text("engine"), int.TryParse(Text("modSdk"), CultureInfo.InvariantCulture, out var sdk) ? sdk : 0,
            Text("gameBuild"), Text("system"), wine.Length > 0 && wine != "null" ? wine : null, gpus, settings);
    }

    // The session's mods, one line each: id, version and content hash (the first 16 characters, as a refused co-op join
    // names them; ModSet) when the mod is one the modlist digest counts. loaded: result.json's mods.loaded ("id version");
    // modset: the session's modset.json.
    public static List<string> ModLines(IEnumerable<string> loaded, JsonElement? modset)
    {
        var hashes = new Dictionary<string, string>(StringComparer.Ordinal);
        if (modset is { ValueKind: JsonValueKind.Object } set && set.TryGetProperty("mods", out var mods) && mods.ValueKind == JsonValueKind.Array)
            foreach (var mod in mods.EnumerateArray())
                if (SessionFiles.Text(mod, "hash") is { Length: >= 16 } hash) hashes[SessionFiles.Text(mod, "id")] = hash[..16];
        return [.. loaded.Select(entry =>
        {
            var id = entry.Split(' ', 2)[0];
            return Privacy.Scrub("Mod: " + entry + (hashes.TryGetValue(id, out var hash) ? ", content hash " + hash : ""));
        })];
    }
}

// The system a session runs on, read once per session (never from the game's process): Windows' version and the display
// adapters with their driver versions, from the registry, which any user may read.
public static class SystemFacts
{
    const string DisplayClass = @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";

    public static string Windows()
    {
        if (!OperatingSystem.IsWindows()) return RuntimeInformation.OSDescription;
        try { return WindowsVersion(); }
        catch (Exception error) when (error is System.Security.SecurityException or IOException or UnauthorizedAccessException)
        { return RuntimeInformation.OSDescription; }
    }

    [SupportedOSPlatform("windows")]
    static string WindowsVersion()
    {
        using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
        var version = Environment.OSVersion.Version;
        var build = int.TryParse(key?.GetValue("CurrentBuildNumber") as string, CultureInfo.InvariantCulture, out var b) ? b : version.Build;
        var patch = key?.GetValue("UBR") is int ubr ? ubr : 0;
        var release = key?.GetValue("DisplayVersion") as string ?? key?.GetValue("ReleaseId") as string;
        return Describe(version.Major, version.Minor, build, patch, release);
    }

    // "Windows 11 24H2 (10.0.26100.4652)": Windows 11 still says 10.0 in its version, and is told by its build.
    public static string Describe(int major, int minor, int build, int patch, string? release)
    {
        var name = major == 10 && build >= 22000 ? "Windows 11" : major == 10 ? "Windows 10" : "Windows";
        return $"{name}{(string.IsNullOrWhiteSpace(release) ? "" : " " + release.Trim())} ({major}.{minor}.{build}.{patch})";
    }

    public static IReadOnlyList<GpuInfo> Gpus()
    {
        if (!OperatingSystem.IsWindows()) return [];
        try { return DisplayAdapters(); }
        catch (Exception error) when (error is System.Security.SecurityException or IOException or UnauthorizedAccessException) { return []; }
    }

    [SupportedOSPlatform("windows")]
    static List<GpuInfo> DisplayAdapters()
    {
        using var adapters = Registry.LocalMachine.OpenSubKey(DisplayClass);
        if (adapters == null) return [];
        var found = new List<GpuInfo>();
        foreach (var name in adapters.GetSubKeyNames().Where(n => n.Length == 4 && n.All(char.IsAsciiDigit)).Order(StringComparer.Ordinal))
        {
            try
            {
                using var adapter = adapters.OpenSubKey(name);
                if (adapter?.GetValue("DriverDesc") is string { Length: > 0 } description)
                    found.Add(new GpuInfo(description.Trim(), Driver(description, adapter.GetValue("DriverVersion") as string)));
            }
            catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException) { } /* a key only the system may read */
        }
        return [.. found.Distinct().Take(4)];
    }

    // The driver's Windows version, and NVIDIA's own number for it: 32.0.15.8129 is driver 581.29 (its last five digits).
    public static string Driver(string adapter, string? version)
    {
        if (string.IsNullOrWhiteSpace(version)) return "unknown";
        var digits = string.Concat(version.Split('.').Skip(2));
        return adapter.Contains("NVIDIA", StringComparison.OrdinalIgnoreCase) && digits.Length >= 5 && digits.All(char.IsAsciiDigit)
            ? $"{version} (NVIDIA {digits[^5..^2]}.{digits[^2..]})" : version;
    }
}
