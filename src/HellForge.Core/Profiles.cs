using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// A template from data/profile-templates.json (app/profile-templates.json in an installation). The backend applies it when
// it creates profile.json.
public sealed record ProfileTemplate(string Name, string Description, int ItemCount)
{
    public bool IsDev => Name.Contains("unlock-all", StringComparison.Ordinal);
    public string Summary => $"{Description} ({ItemCount} item{(ItemCount == 1 ? "" : "s")})";
}

// A profile is a directory with launcher.json naming its template; profile.json appears once a session has used it.
public sealed record LocalProfile(string Name, string Folder, string Template)
{
    public string ProfileFile => Path.Combine(Folder, "profile.json");
    public bool IsDev => Template.Contains("unlock-all", StringComparison.Ordinal);
    public string Detail => Template;
}

public static partial class ProfileStore
{
    public const string DefaultName = "default";
    public const string FallbackTemplate = "starter";
    // The profile a fresh install starts with: the development template, everything unlocked.
    public const string FirstProfileTemplate = "unlock-all";
    static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true, NewLine = "\n" };

    [GeneratedRegex("^[a-z0-9-]{1,32}$")]
    private static partial Regex ProfileName();

    // Template names are passed on the PowerShell command line, so only plain names are accepted.
    [GeneratedRegex("^[A-Za-z0-9_-]{1,64}$")]
    private static partial Regex TemplateName();

    public static bool IsValidName(string name) => ProfileName().IsMatch(name);

    public static List<ProfileTemplate> LoadTemplates(ISessionPaths paths)
    {
        var templates = new List<ProfileTemplate>();
        if (SessionFiles.ReadJson(paths.TemplatesFile) is not { ValueKind: JsonValueKind.Object } root ||
            !root.TryGetProperty("templates", out var all) || all.ValueKind != JsonValueKind.Object) return templates;
        foreach (var template in all.EnumerateObject())
        {
            if (!TemplateName().IsMatch(template.Name)) continue;
            var items = template.Value.ValueKind == JsonValueKind.Object && template.Value.TryGetProperty("items", out var list) &&
                list.ValueKind == JsonValueKind.Array ? list.GetArrayLength() : 0;
            templates.Add(new ProfileTemplate(template.Name, SessionFiles.Text(template.Value, "description"), items));
        }
        return templates;
    }

    // Every folder under profiles/ made by the launcher, by name. With none (a fresh install), a "default" profile from
    // the unlock-all development template is created (the starter template if the install has no unlock-all); new
    // profiles made in the launcher still pick their template.
    public static List<LocalProfile> List(ISessionPaths paths)
    {
        RetireLegacyDefault(paths);
        var profiles = Scan(paths);
        if (profiles.Count > 0) return profiles;
        var first = LoadTemplates(paths).Any(t => t.Name == FirstProfileTemplate) ? FirstProfileTemplate : FallbackTemplate;
        Create(paths, DefaultName, first);
        profiles = Scan(paths);
        return profiles.Count > 0 ? profiles : [new(DefaultName, Path.Combine(paths.ProfilesRoot, DefaultName), first)];
    }

    static List<LocalProfile> Scan(ISessionPaths paths)
    {
        try
        {
            if (!Directory.Exists(paths.ProfilesRoot)) return [];
            return Directory.GetDirectories(paths.ProfilesRoot)
                .Select(Path.GetFileName)
                .OfType<string>()
                .Where(name => IsValidName(name) && File.Exists(Path.Combine(paths.ProfilesRoot, name, "launcher.json")))
                .Order(StringComparer.Ordinal)
                .Select(name => Load(paths, name))
                .ToList();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    // Earlier versions kept a "default" profile without a template; its fixture defaults skip the new-player start.
    // It is renamed out of the list (".legacy" is not a valid profile name) and kept, in case it holds progress.
    static void RetireLegacyDefault(ISessionPaths paths)
    {
        var folder = Path.Combine(paths.ProfilesRoot, DefaultName);
        if (!Directory.Exists(folder) || File.Exists(Path.Combine(folder, "launcher.json"))) return;
        var target = folder + ".legacy";
        if (Directory.Exists(target)) target += "-" + DateTime.UtcNow.ToString("yyyyMMddHHmmss", CultureInfo.InvariantCulture);
        try { Directory.Move(folder, target); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static LocalProfile Load(ISessionPaths paths, string name)
    {
        var folder = Path.Combine(paths.ProfilesRoot, name);
        var template = SessionFiles.ReadJson(Path.Combine(folder, "launcher.json")) is { } meta ? SessionFiles.Text(meta, "template") : "";
        return new LocalProfile(name, folder, TemplateName().IsMatch(template) ? template : FallbackTemplate);
    }

    // Returns an error message, or null once profiles/<name>/launcher.json is written.
    public static string? Create(ISessionPaths paths, string name, string template)
    {
        if (!IsValidName(name)) return "Use 1-32 lowercase letters, digits or hyphens.";
        if (!TemplateName().IsMatch(template)) return "Pick a template.";
        var folder = Path.Combine(paths.ProfilesRoot, name);
        if (Directory.Exists(folder)) return $"A profile named \"{name}\" already exists.";
        try
        {
            Directory.CreateDirectory(folder);
            var meta = new { template, createdUtc = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture) };
            File.WriteAllText(Path.Combine(folder, "launcher.json"), JsonSerializer.Serialize(meta, WriteOptions) + "\n");
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return "Could not create the profile: " + error.Message; }
    }

    public static string LoadSelected(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings && SessionFiles.Text(settings, "selectedProfile") is { Length: > 0 } name
            ? name : DefaultName;

    // Multiplayer: route the engine's PlayFab calls to the local backend. On unless switched off.
    public static bool LoadLocalPlayFab(ISessionPaths paths) => LoadFlag(paths, "localPlayFab");

    // Multiplayer: swap in the HellForge stand-ins for the PlayFab Party/Lobby DLLs, which reach Steam friends through the
    // backend. On unless switched off.
    public static bool LoadSdkShims(ISessionPaths paths) => LoadFlag(paths, "sdkShims");

    // The galactic war a session serves: the live war read-only through the community mirror (backend --live-war), the
    // frozen war HellForge ships, or the all-planets war made from it (backend --war open). Live unless chosen otherwise;
    // a settings file from before the choice keeps its live-war switch ("liveWar").
    public const string WarLive = "live", WarFrozen = "frozen", WarOpen = "open";
    public static string LoadWar(ISessionPaths paths)
    {
        var settings = SessionFiles.ReadJson(paths.SettingsFile);
        var war = settings is { } s ? SessionFiles.Text(s, "war") : "";
        if (war is WarLive or WarFrozen or WarOpen) return war;
        return LoadFlag(paths, "liveWar") ? WarLive : WarFrozen;
    }

    // Off unless chosen: the Democracy Space Station's effects picked in LoadDssEffects, on every planet of whichever war
    // is served (backend --dss-everywhere, DssEverywhere).
    public static bool LoadDssEverywhere(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings && SessionFiles.Text(settings, "dssEverywhere") == "true";

    // The DSS effects picked for every planet (DssEffects keys, in its order): every one unless chosen otherwise. Keys this
    // version does not know are left out.
    public static IReadOnlyList<string> LoadDssEffects(ISessionPaths paths)
    {
        if (SessionFiles.ReadJson(paths.SettingsFile) is not { } settings || !settings.TryGetProperty("dssEffects", out var list) ||
            list.ValueKind != JsonValueKind.Array)
            return DssEffects.Keys;
        var keys = list.EnumerateArray().Where(k => k.ValueKind == JsonValueKind.String).Select(k => k.GetString()!).ToList();
        return DssEffects.Keys.Where(keys.Contains).ToList();
    }

    // What a session puts on every planet: the picked effects when the setting is on, else none.
    public static IReadOnlyList<string> DssSessionEffects(bool everywhere, IReadOnlyList<string> effects) => everywhere ? effects : [];

    // The engine's own log (--log-to-file), copied into the session's logs. On unless switched off (multiplayer testing).
    public static bool LoadEngineLog(ISessionPaths paths) => LoadFlag(paths, "engineLog");
    // Off unless chosen: a full memory dump of the game when it crashes (large), for bug reports.
    public static bool LoadCrashDump(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings && SessionFiles.Text(settings, "crashDump") == "true";

    // The game's launch options, as typed (GameLaunchOptions); empty unless set.
    public static string LoadLaunchOptions(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings ? SessionFiles.Text(settings, "launchOptions") : "";

    static bool LoadFlag(ISessionPaths paths, string name) =>
        SessionFiles.ReadJson(paths.SettingsFile) is not { } settings || SessionFiles.Text(settings, name) != "false";

    public static void SaveSelected(ISessionPaths paths, string name) =>
        Save(paths, name, LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveLocalPlayFab(ISessionPaths paths, bool enabled) =>
        Save(paths, LoadSelected(paths), enabled, LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveSdkShims(ISessionPaths paths, bool enabled) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), enabled, LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveWar(ISessionPaths paths, string war) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), war, LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveDssEverywhere(ISessionPaths paths, bool enabled) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), enabled, LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveDssEffects(ISessionPaths paths, IReadOnlyList<string> effects) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), effects, LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveCrashDump(ISessionPaths paths, bool enabled) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), enabled, LoadLaunchOptions(paths));

    public static void SaveEngineLog(ISessionPaths paths, bool enabled) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), enabled, LoadCrashDump(paths), LoadLaunchOptions(paths));

    public static void SaveLaunchOptions(ISessionPaths paths, string text) =>
        Save(paths, LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadDssEverywhere(paths), LoadDssEffects(paths), LoadEngineLog(paths), LoadCrashDump(paths), text);

    static void Save(ISessionPaths paths, string name, bool localPlayFab, bool sdkShims, string war, bool dssEverywhere, IReadOnlyList<string> dssEffects, bool engineLog, bool crashDump, string launchOptions)
    {
        try
        {
            Directory.CreateDirectory(paths.HostRoot);
            File.WriteAllText(paths.SettingsFile, JsonSerializer.Serialize(new { selectedProfile = name, localPlayFab, sdkShims, war, liveWar = war == WarLive, dssEverywhere, dssEffects, engineLog, crashDump, launchOptions }, WriteOptions) + "\n");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }  // Only a convenience.
    }
}
