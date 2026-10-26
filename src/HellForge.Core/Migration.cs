namespace HellForge.Core;

public sealed record MigrationResult(IReadOnlyList<string> Copied, IReadOnlyList<string> Kept);

// Copies data from the repository-era layout into an installation: HellForge profiles and launcher settings from
// artifacts/host-startup, and the game's user data from %APPDATA%/Arrowhead/HD2BeyondTheStars (per-profile folders,
// the older shared settings and saves, which belonged to the default profile, and the shader cache). Nothing is moved
// or deleted, and a destination that already exists is kept as it is.
public static class Migration
{
    const string DefaultProfile = "default";

    public static MigrationResult Run(InstallLayout install, string hostRoot, string appDataRoot)
    {
        var copied = new List<string>();
        var kept = new List<string>();
        void Tree(string from, string to)
        {
            if (!Directory.Exists(from)) return;
            if (Directory.Exists(to)) { kept.Add(to); return; }
            CopyTree(from, to);
            copied.Add(to);
        }

        Tree(Path.Combine(hostRoot, "local-profile"), Path.Combine(install.Profiles, DefaultProfile));
        foreach (var profile in Folders(Path.Combine(hostRoot, "profiles")))
            Tree(Path.Combine(hostRoot, "profiles", profile), Path.Combine(install.Profiles, profile));
        var settings = Path.Combine(hostRoot, "launcher-settings.json");
        var installSettings = Path.Combine(install.Root, "launcher-settings.json");
        if (File.Exists(settings))
        {
            if (File.Exists(installSettings)) kept.Add(installSettings);
            else { File.Copy(settings, installSettings); copied.Add(installSettings); }
        }

        CopySharedData(install, appDataRoot, copied, kept);
        foreach (var profile in Folders(Path.Combine(appDataRoot, "profiles")))
            Tree(Path.Combine(appDataRoot, "profiles", profile), install.GameProfile(profile));
        Tree(Path.Combine(appDataRoot, "shader_cache"), Path.Combine(install.UserData, "shader_cache"));
        return new(copied, kept);
    }

    // Before game user data was kept per profile, every session used the shared settings and saves: the default
    // profile's now.
    static void CopySharedData(InstallLayout install, string appDataRoot, List<string> copied, List<string> kept)
    {
        var defaultData = install.GameProfile(DefaultProfile);
        var sharedSettings = Path.Combine(appDataRoot, "user_settings.config");
        var sharedSaves = Path.Combine(appDataRoot, "saves");
        if (!(File.Exists(sharedSettings) || Directory.Exists(sharedSaves))) return;
        if (Directory.Exists(defaultData)) kept.Add(defaultData);
        else
        {
            Directory.CreateDirectory(Path.Combine(defaultData, "saves"));
            if (File.Exists(sharedSettings)) File.Copy(sharedSettings, Path.Combine(defaultData, "user_settings.config"));
            if (Directory.Exists(sharedSaves)) CopyTree(sharedSaves, Path.Combine(defaultData, "saves"));
            copied.Add(defaultData);
        }
    }

    static IEnumerable<string> Folders(string root) => Directory.Exists(root)
        ? Directory.GetDirectories(root).Select(Path.GetFileName).OfType<string>()
            .Where(name => GameSettings.ProfileName().IsMatch(name)).Order(StringComparer.Ordinal)
        : [];

    // Profile lock files only mark a profile as in use by a running backend.
    static void CopyTree(string from, string to)
    {
        LocalInstall.NoLinks(from);
        Directory.CreateDirectory(to);
        foreach (var file in Directory.EnumerateFiles(from))
            if (!Path.GetFileName(file).Equals("profile.lock", StringComparison.OrdinalIgnoreCase))
                File.Copy(file, Path.Combine(to, Path.GetFileName(file)));
        foreach (var folder in Directory.EnumerateDirectories(from))
            CopyTree(folder, Path.Combine(to, Path.GetFileName(folder)));
    }
}
