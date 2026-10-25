using System.Text;
using System.Text.RegularExpressions;

namespace HellForge.Core;

public sealed record PreparedSession(string BuildId, IReadOnlyList<string> Problems, string ProfileData, bool ProfileSeeded,
    IReadOnlyList<string> GameFiles);

// The build's data/settings.ini names %APPDATA%/Arrowhead/Helldivers2 for everything the game writes: settings, saves,
// shader cache, logs and crash dumps. A session points them into the installation's userdata folder instead, with the
// user settings and saves per profile. The engine resolves %APPDATA% itself (known-folder API), so only rewriting the
// paths keeps the game out of AppData.
public static partial class GameSettings
{
    const string VanillaRoot = "%APPDATA%/Arrowhead/Helldivers2";
    const string SettingsPath = "data/settings.ini";
    public const string OriginalSuffix = ".hellforge-original";
    static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    [GeneratedRegex("^[a-z0-9-]{1,32}$")]
    public static partial Regex ProfileName();

    public static string Rewrite(string original, string userData, string profile)
    {
        if (!ProfileName().IsMatch(profile)) throw new ArgumentException("Profile names use 1-32 lowercase letters, digits or hyphens.");
        var root = LocalInstall.FullPath(userData).Replace('\\', '/');
        if (root.Contains('"') || root.Contains('%')) throw new InvalidDataException("The installation path cannot contain quotes or percent signs.");
        if (!original.Contains(VanillaRoot)) throw new InvalidDataException("settings.ini lacks the expected user data paths.");
        var text = original.Replace(VanillaRoot, root);
        foreach (var (key, format) in new[] { ("user_settings", "{0}/user_settings.config"), ("save_dir", "{0}/saves") })
        {
            var shared = $"{key} = \"{string.Format(format, root)}\"";
            if (!text.Contains(shared)) throw new InvalidDataException($"settings.ini lacks its {key} line.");
            text = text.Replace(shared, $"{key} = \"{string.Format(format, root + "/profiles/" + profile)}\"");
        }
        return text;
    }

    // Verifies the installation's game for a session and points its settings at the profile's user data. A new
    // profile starts from the normal game's settings (graphics, controls, answered first-run prompts), read from
    // vanillaUserData; nothing is written there. Returns the problems instead of preparing when the game is not the
    // supported build. userData is the root the settings point at: the installation's, or a sandboxed session's copy.
    public static PreparedSession PrepareSession(InstallLayout install, string profile, string? vanillaUserData, string? userData = null)
    {
        var root = userData ?? install.UserData;
        if (!ProfileName().IsMatch(profile)) throw new ArgumentException("Profile names use 1-32 lowercase letters, digits or hyphens.");
        var build = SupportedBuild.Current;
        var settings = build.Files.Single(file => file.Path == SettingsPath);
        var settingsFile = Path.Combine(install.Game, SettingsPath);
        if (!File.Exists(install.OriginalSettings))
        {
            if (!File.Exists(settingsFile) || !SupportedBuild.Sha1(settingsFile).Equals(settings.Sha1, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("data/settings.ini was changed and its original is missing; repair the game copy.");
            File.Copy(settingsFile, install.OriginalSettings);
        }
        var problems = new List<string>();
        if (!SupportedBuild.Sha1(install.OriginalSettings).Equals(settings.Sha1, StringComparison.OrdinalIgnoreCase))
            problems.Add("changed: " + SettingsPath + " (kept original)");
        Directory.CreateDirectory(install.Temp);
        var others = build with { Files = build.Files.Where(file => file.Path != SettingsPath).ToList() };
        problems.AddRange(others.Verify(install.Game, install.VerifyCache));
        var profileData = Path.Combine(root, "profiles", profile);
        var files = build.Files.Select(file => file.Path).ToList();
        if (problems.Count > 0) return new(build.BuildId, problems, profileData, false, files);

        var seeded = false;
        if (!Directory.Exists(profileData))
        {
            Directory.CreateDirectory(Path.Combine(profileData, "saves"));
            if (vanillaUserData != null && Directory.Exists(vanillaUserData))
            {
                var userSettings = Path.Combine(vanillaUserData, "user_settings.config");
                if (File.Exists(userSettings)) File.Copy(userSettings, Path.Combine(profileData, "user_settings.config"));
                var saves = Path.Combine(vanillaUserData, "saves");
                if (Directory.Exists(saves))
                    foreach (var file in Directory.EnumerateFiles(saves, "*_user_settings.config"))
                        File.Copy(file, Path.Combine(profileData, "saves", Path.GetFileName(file)));
            }
            seeded = true;
        }
        var original = Utf8.GetString(File.ReadAllBytes(install.OriginalSettings));
        File.WriteAllBytes(settingsFile, Utf8.GetBytes(Rewrite(original, root, profile)));
        return new(build.BuildId, problems, profileData, seeded, files);
    }

    // The normal game's user data (%APPDATA%/Arrowhead/Helldivers2), where HELLDIVERS 2 keeps its options.
    public static string VanillaUserData() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "Helldivers2");

    // The key bindings the signed-in Steam account keeps for HELLDIVERS 2 in its Steam Cloud folder
    // (<Steam>/userdata/<account>/553850/remote/input_settings.config), or null when there are none.
    public static string? SteamControls()
    {
        if (!OperatingSystem.IsWindows() || SteamLocator.SteamRoot() is not { } steam ||
            Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null) is not int account || account == 0)
            return null;
        var file = Path.Combine(steam, "userdata", account.ToString(System.Globalization.CultureInfo.InvariantCulture), "553850", "remote", ControlsFile);
        return File.Exists(file) ? file : null;
    }

    public sealed record SettingsImport(IReadOnlyList<string> Copied, IReadOnlyList<string> Missing);

    const string ControlsFile = "input_settings.config";
    public const string BackupSuffix = ".before-import";
    const string ImportList = "settings-import.txt"; /* the files an import made where the profile had none */

    // Copies the normal game's settings into a profile again (the Profile page's "Import from HELLDIVERS 2"; a new profile
    // gets them once, PrepareSession): the game's options (user_settings.config), each Steam account's
    // (saves/<account>_user_settings.config) and, with controls, the key bindings from the signed-in Steam account's
    // cloud (cloud/input_settings.config, what the profile's game reads). The profile's own copies are kept beside them as
    // *.before-import (an earlier import's are replaced), so UndoImport takes them back. The normal game's files are
    // only read; not while a session runs (the game writes these when it closes).
    public static SettingsImport ImportFromGame(InstallLayout install, string profile, string vanillaUserData, string? steamControls, bool controls)
    {
        if (!ProfileName().IsMatch(profile)) throw new ArgumentException("Profile names use 1-32 lowercase letters, digits or hyphens.");
        var profileData = install.GameProfile(profile);
        var pairs = new List<(string From, string To, string Name)>
        {
            (Path.Combine(vanillaUserData, "user_settings.config"), Path.Combine(profileData, "user_settings.config"), "game options"),
        };
        var saves = Path.Combine(vanillaUserData, "saves");
        if (Directory.Exists(saves))
            foreach (var file in Directory.EnumerateFiles(saves, "*_user_settings.config").Order(StringComparer.OrdinalIgnoreCase))
                pairs.Add((file, Path.Combine(profileData, "saves", Path.GetFileName(file)), "account options (" + Path.GetFileName(file).Split('_')[0] + ")"));
        if (controls) pairs.Add((steamControls ?? "", Path.Combine(profileData, "cloud", ControlsFile), "key bindings"));
        var copied = new List<string>();
        var missing = new List<string>();
        var created = new List<string>();
        foreach (var (from, to, name) in pairs)
        {
            if (from.Length == 0 || !File.Exists(from)) { missing.Add(name); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            if (File.Exists(to)) File.Copy(to, to + BackupSuffix, overwrite: true);
            else
            {
                if (File.Exists(to + BackupSuffix)) File.Delete(to + BackupSuffix);
                created.Add(Path.GetRelativePath(profileData, to));
            }
            File.Copy(from, to + ".partial", overwrite: true);
            File.Move(to + ".partial", to, overwrite: true);
            copied.Add(name);
        }
        if (copied.Count > 0) File.WriteAllLines(Path.Combine(profileData, ImportList), created);
        return new(copied, missing);
    }

    // Whether the profile has an import UndoImport can take back.
    public static bool CanUndoImport(InstallLayout install, string profile) =>
        ProfileName().IsMatch(profile) && File.Exists(Path.Combine(install.GameProfile(profile), ImportList));

    // Takes the last import back: the profile's own copies return, the files the import made where it had none go.
    // Returns how many files were restored or removed.
    public static int UndoImport(InstallLayout install, string profile)
    {
        if (!CanUndoImport(install, profile)) return 0;
        var profileData = install.GameProfile(profile);
        var list = Path.Combine(profileData, ImportList);
        var count = 0;
        foreach (var backup in Directory.EnumerateFiles(profileData, "*" + BackupSuffix, SearchOption.AllDirectories).ToList())
        {
            File.Move(backup, backup[..^BackupSuffix.Length], overwrite: true);
            count++;
        }
        foreach (var made in File.ReadAllLines(list).Where(l => l.Length > 0))
        {
            var path = Path.GetFullPath(Path.Combine(profileData, made));
            if (!path.StartsWith(Path.GetFullPath(profileData) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(path)) continue;
            File.Delete(path);
            count++;
        }
        File.Delete(list);
        return count;
    }
}
