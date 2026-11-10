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
    // Live-like: a starter profile until Sync Inventory, which then brings in everything it can of the live account, and
    // the premium warbonds the account owns are open without a Forge Credits purchase (#282).
    public const string LiveLikeTemplate = "live-like";
    // The profile a fresh install starts with, and the one the launcher offers first: live-like, no paid content until a sync.
    public const string FirstProfileTemplate = LiveLikeTemplate;
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
    // the starter template is created; new profiles made in the launcher still pick their template.
    public static List<LocalProfile> List(ISessionPaths paths)
    {
        RetireLegacyDefault(paths);
        LowercaseNames(paths);
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

    // Older launchers made profiles with capital letters ("Avaleine", #260). Scan leaves them out, and on Windows they still
    // block the lowercase name, so each is renamed to its lowercase name (through a temporary name, as a case-only move
    // is refused on Windows) and the selection follows it. One whose lowercase name is taken stays as it is.
    static void LowercaseNames(ISessionPaths paths)
    {
        try
        {
            if (!Directory.Exists(paths.ProfilesRoot)) return;
            foreach (var folder in Directory.GetDirectories(paths.ProfilesRoot))
            {
                var name = Path.GetFileName(folder);
                var lower = name.ToLowerInvariant();
                if (lower == name || !IsValidName(lower) || !File.Exists(Path.Combine(folder, "launcher.json"))) continue;
                if (Directory.GetDirectories(paths.ProfilesRoot).Any(other => Path.GetFileName(other) == lower)) continue;
                if (Rename(folder, Path.Combine(paths.ProfilesRoot, lower)) && LoadSelected(paths) == name) SaveSelected(paths, lower);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static bool Rename(string folder, string target)
    {
        var temporary = target + ".renaming";
        try
        {
            Directory.Move(folder, temporary);
            Directory.Move(temporary, target);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            if (Directory.Exists(temporary) && !Directory.Exists(folder)) Directory.Move(temporary, folder);
            return false;
        }
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
        if (Directory.Exists(folder))
            return Directory.GetDirectories(paths.ProfilesRoot).Select(Path.GetFileName).FirstOrDefault(other =>
                    string.Equals(other, name, StringComparison.OrdinalIgnoreCase)) is { } hidden && hidden != name
                ? $"The folder {Path.Combine(paths.ProfilesRoot, hidden)} already uses this name. Rename or remove it first."
                : $"A profile named \"{name}\" already exists.";
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

    // The all-planets war's seed (backend --war-seed), for its subfactions and SEAF allies: 0 unless one was typed or rolled.
    public static uint LoadSubfactionSeed(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings && uint.TryParse(SessionFiles.Text(settings, "subfactionSeed"), NumberStyles.None,
            CultureInfo.InvariantCulture, out var seed) ? seed : 0;
    public static void SaveSubfactionSeed(ISessionPaths paths, uint seed) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths), seed));

    // The engine's own log (--log-to-file), copied into the session's logs. On unless switched off (multiplayer testing).
    public static bool LoadEngineLog(ISessionPaths paths) => LoadFlag(paths, "engineLog");
    // Off unless chosen: a full memory dump of the game when it crashes (large), for bug reports.
    public static bool LoadCrashDump(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings && SessionFiles.Text(settings, "crashDump") == "true";

    // Whether the all-planets war deals its subfactions and its SEAF allies from the seed (backend --war-subfactions,
    // --war-seaf): on unless turned off, which keeps the frozen war's.
    public static bool LoadDealSubfactions(ISessionPaths paths) => LoadFlag(paths, "dealSubfactions");
    public static bool LoadDealSeaf(ISessionPaths paths) => LoadFlag(paths, "dealSeaf");
    public static void SaveDealSubfactions(ISessionPaths paths, bool deal) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths),
            LoadLaunchOptions(paths), DealSubfactions: deal));
    public static void SaveDealSeaf(ISessionPaths paths, bool deal) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths),
            LoadLaunchOptions(paths), DealSeaf: deal));

    // On unless turned off (since the 0.3.0 beta): the game at low integrity, with Steam through the session's Steam
    // broker (HellForge.Session play --sandbox, docs/steam-bridge.md); a session is sandboxed by default only with an engine
    // built with the Steam bridge (SessionRunner.HasSteamBridge). Kept as "sandboxed": the betas before wrote "sandbox":
    // false for everyone who never chose, so that key is not read and every installation starts sandboxed once.
    public static bool LoadSandbox(ISessionPaths paths) => LoadFlag(paths, "sandboxed");
    public static void SaveSandbox(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths),
            LoadLaunchOptions(paths), Sandbox: enabled));

    // Co-op over Steam relays on Linux: UDP to Valve's relays only (HELLFORGE_STEAM_UDP=1, #62), so a session can play with
    // others; TCP and every other address stay refused. On unless turned off (since the 0.4.0 beta). Kept as "coopRelays":
    // the launchers before wrote "steamUdp": false for everyone who never chose, so that key is not read and every
    // installation starts with co-op once. Off, a session has no route out at all, and its lobby browser offers to turn
    // it on (CoopRelaysSwitch).
    public static bool LoadSteamUdp(ISessionPaths paths) => LoadFlag(paths, "coopRelays");
    public static void SaveSteamUdp(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths),
            LoadLaunchOptions(paths), SteamUdp: enabled));

    // The game's launch options, as typed (GameLaunchOptions); empty unless set.
    public static string LoadLaunchOptions(ISessionPaths paths) =>
        SessionFiles.ReadJson(paths.SettingsFile) is { } settings ? SessionFiles.Text(settings, "launchOptions") : "";

    static bool LoadFlag(ISessionPaths paths, string name) =>
        SessionFiles.ReadJson(paths.SettingsFile) is not { } settings || SessionFiles.Text(settings, name) != "false";

    public static void SaveSelected(ISessionPaths paths, string name) =>
        Save(paths, new(name, LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths)));

    public static void SaveLocalPlayFab(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), enabled, LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths)));

    public static void SaveSdkShims(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), enabled, LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths)));

    public static void SaveWar(ISessionPaths paths, string war) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), war, LoadEngineLog(paths), LoadCrashDump(paths), LoadLaunchOptions(paths)));

    public static void SaveCrashDump(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), enabled, LoadLaunchOptions(paths)));

    public static void SaveEngineLog(ISessionPaths paths, bool enabled) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), enabled, LoadCrashDump(paths), LoadLaunchOptions(paths)));

    public static void SaveLaunchOptions(ISessionPaths paths, string text) =>
        Save(paths, new(LoadSelected(paths), LoadLocalPlayFab(paths), LoadSdkShims(paths), LoadWar(paths), LoadEngineLog(paths), LoadCrashDump(paths), text));

    // What launcher-settings.json holds; a null Seed, Sandbox, DealSubfactions, DealSeaf or SteamUdp keeps the saved one.
    sealed record SavedSettings(string Profile, bool LocalPlayFab, bool SdkShims, string War, bool EngineLog, bool CrashDump, string LaunchOptions,
        uint? Seed = null, bool? Sandbox = null, bool? DealSubfactions = null, bool? DealSeaf = null, bool? SteamUdp = null);

    static void Save(ISessionPaths paths, SavedSettings saved)
    {
        try
        {
            var subfactionSeed = (saved.Seed ?? LoadSubfactionSeed(paths)).ToString(CultureInfo.InvariantCulture);
            var sandboxed = saved.Sandbox ?? LoadSandbox(paths);
            var subfactions = saved.DealSubfactions ?? LoadDealSubfactions(paths);
            var seaf = saved.DealSeaf ?? LoadDealSeaf(paths);
            var steamUdp = saved.SteamUdp ?? LoadSteamUdp(paths);
            Directory.CreateDirectory(paths.HostRoot);
            File.WriteAllText(paths.SettingsFile, JsonSerializer.Serialize(new
            {
                selectedProfile = saved.Profile, localPlayFab = saved.LocalPlayFab, sdkShims = saved.SdkShims, war = saved.War, liveWar = saved.War == WarLive,
                engineLog = saved.EngineLog, crashDump = saved.CrashDump, launchOptions = saved.LaunchOptions, subfactionSeed,
                dealSubfactions = subfactions, dealSeaf = seaf, sandboxed, coopRelays = steamUdp
            }, WriteOptions) + "\n");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }  // Only a convenience.
    }
}
