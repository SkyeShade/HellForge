using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// What a HellForge version's session runner understands, for a launcher newer than it (KeptVersions.Activate puts that
// version's programs in app/). Each older line is read from its release: 0.3.0-beta and 0.4.0-beta take the same play
// command as this one, without the HellForge war (--hellforge-war, --war-server) and the war script (--war-script);
// their backend has its game catalogue built in (no Sync needed first) and knows only its own profile templates.
// Family "current": a build of this launcher's version line, which takes everything.
public sealed partial record SessionRuntime(string Family, AppIdentity App, int? Sdk, bool HellForgeWar, bool WarScript, bool NeedsCatalogue,
    IReadOnlySet<string>? Templates)
{
    public const string Current = "current";
    public const string Supported = "0.3.0-beta, 0.4.0-beta";

    public bool IsCurrent => Family == Current;
    public string Text => App.Text;

    [GeneratedRegex(@"#define\s+HF_MOD_SDK_VERSION\s+(\d+)")]
    private static partial Regex SdkDefine();

    // The runtime of an app folder (its app-build.json, mod SDK header and templates), or null when this launcher cannot
    // start that version's sessions.
    public static SessionRuntime? Of(string app)
    {
        var identity = KeptVersions.Identify(app);
        var family = FamilyOf(identity);
        if (family == null) return null;
        var old = family != Current;
        return new(family, identity, ReadSdk(app), HellForgeWar: !old, WarScript: !old, NeedsCatalogue: !old, old ? ReadTemplates(app) : null);
    }

    // The last engine of each older line (app/engine.json "version"): development builds between releases carry the
    // release's version number with a newer engine, and take this launcher's command line.
    static readonly Version Engine03 = new(0, 1, 41), Engine04 = new(0, 1, 47);

    // Which line a version belongs to: 0.3 or 0.4 (by its version and engine), null for anything older, else this
    // launcher's own line ("current"), which a version it cannot read belongs to too.
    public static string? FamilyOf(AppIdentity app)
    {
        var engine = Version.TryParse(app.Engine, out var parsed) ? parsed : null;
        var number = LauncherUpdate.Number.Parse(app.Version);
        if (number == null) return engine != null && engine <= Engine03 ? "0.3" : Current;
        if (number.Major == 0 && number.Minor < 3) return null;
        if (number.Major == 0 && number.Minor == 3) return "0.3";
        if (number.Major == 0 && number.Minor == 4 && (engine == null || engine <= Engine04)) return "0.4";
        return Current;
    }

    public static SessionRuntime? For(AppIdentity app) => FamilyOf(app) is { } family
        ? new(family, app, null, family == Current, family == Current, family == Current, null) : null;

    // The runtime the session runner of these paths belongs to (<app>/session/HellForge.Session.exe).
    public static SessionRuntime Of(ISessionPaths paths) =>
        Path.GetDirectoryName(Path.GetDirectoryName(paths.Session)) is { } app && File.Exists(Path.Combine(app, "app-build.json")) && Of(app) is { } runtime
            ? runtime : new(Current, new("unknown", "", null, null), null, true, true, true, null);

    static int? ReadSdk(string app)
    {
        try
        {
            var header = Path.Combine(app, "sdk", "hellforge_mod.h");
            return File.Exists(header) && SdkDefine().Match(File.ReadAllText(header)) is { Success: true } match ? int.Parse(match.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    static HashSet<string>? ReadTemplates(string app)
    {
        if (SessionFiles.ReadJson(Path.Combine(app, "profile-templates.json")) is not { ValueKind: JsonValueKind.Object } root ||
            !root.TryGetProperty("templates", out var all) || all.ValueKind != JsonValueKind.Object) return null;
        return all.EnumerateObject().Select(t => t.Name).ToHashSet(StringComparer.Ordinal);
    }

    // What this version plays instead of a war it does not have, with the line the launcher shows; null when it has it.
    public string? WarFallback(string war) => war == ProfileStore.WarHellForge && !HellForgeWar ? ProfileStore.WarFrozen : null;

    // The note for a feature of this launcher an older version has no counterpart for.
    public string Lacks(string feature) => $"{feature} works with the installed version only: you play with HellForge {Text} now. Pick the installed one (first in the version list, bottom left) to use it.";

    // The mods folder's problems as this version's own loader sees them (its HellForge.Cli mods, in app/ while it is
    // played with): a mod made for a newer mod SDK, or a manifest it cannot read. Empty when it could not be asked.
    public static IReadOnlyList<string> LoaderProblems(InstallLayout install, TimeSpan timeout)
    {
        var cli = Path.Combine(install.App, "cli", "HellForge.Cli.exe");
        if (!File.Exists(cli)) return [];
        var start = new ProcessStartInfo(cli) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = install.Root };
        foreach (var argument in new[] { "mods", "--install", install.Root }) start.ArgumentList.Add(argument);
        try
        {
            using var process = Process.Start(start);
            if (process == null) return [];
            var output = process.StandardOutput.ReadToEndAsync();
            _ = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(timeout)) { process.Kill(); return []; }
            using var document = JsonDocument.Parse(output.Result);
            return document.RootElement.TryGetProperty("problems", out var problems) && problems.ValueKind == JsonValueKind.Array
                ? problems.EnumerateArray().Select(p => p.GetString()).OfType<string>().ToList() : [];
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or IOException or JsonException) { return []; }
    }

    // A version without app-build.json's "version" (0.3.0-beta): the launcher's own assembly says it.
    internal static string? VersionFromLauncher(string app)
    {
        foreach (var name in new[] { "HellForge.Launcher.dll", "HellForge.Core.dll", "HellForge.Launcher.exe" })
        {
            var file = Path.Combine(app, "launcher", name);
            if (!File.Exists(file)) continue;
            var text = FileVersionInfo.GetVersionInfo(file).ProductVersion;
            if (text is { Length: > 0 }) return text.Split('+')[0];
        }
        return null;
    }
}
