using HellForge.Core;

namespace HellForge.Launcher;

// Every file the launcher reads or starts. An installation (hellforge-install.json in a folder above the launcher) keeps
// everything inside its own folder. In the repository (a build of the launcher, for working on it) the engine is the one
// scripts/Build-Engine.ps1 last staged in artifacts/engine-package/staging, and the data comes from data/.
sealed class LauncherPaths : ISessionPaths
{
    public required string Root { get; init; }
    public required string? InstallRoot { get; init; }
    public required string HostRoot { get; init; }
    // The engine's session runner (HellForge.Session.exe play ...): it owns every step of a session.
    public required string Session { get; init; }
    // The engine's readable scripts: the network block (WFP filters, or the older Windows Firewall rules) and the Steam ownership check.
    public required string ScriptsFolder { get; init; }
    public required string Backend { get; init; }
    public required string TemplatesFile { get; init; }
    // The progression packages the backend serves (weapon mastery tracks for the profile page's weapon levels).
    public required string ProgressionFile { get; init; }
    public required string ProfilesRoot { get; init; }
    public required string SettingsFile { get; init; }
    public required string SessionFile { get; init; }
    public required string RunsFolder { get; init; }
    // Mods load in installations only (<installation>/mods, docs/mods.md).
    public string? ModsFolder => InstallRoot != null ? Path.Combine(InstallRoot, "mods") : null;

    public static LauncherPaths ForRepository(string root)
    {
        var host = Path.Combine(root, "artifacts", "host-startup");
        var engine = Path.Combine(root, "artifacts", "engine-package", "staging");
        return new LauncherPaths
        {
            Root = root, InstallRoot = null, HostRoot = host,
            Session = Path.Combine(engine, "session", "HellForge.Session.exe"), ScriptsFolder = Path.Combine(engine, "scripts"),
            Backend = Path.Combine(engine, "backend", "HellForge.LocalBackend.exe"),
            TemplatesFile = Path.Combine(root, "data", "profile-templates.json"),
            ProgressionFile = Path.Combine(root, "data", "progression-data.json"), ProfilesRoot = Path.Combine(host, "profiles"),
            SettingsFile = Path.Combine(host, "launcher-settings.json"), SessionFile = Path.Combine(host, "play-session.json"),
            RunsFolder = Path.Combine(host, "local-boots")
        };
    }

    public static LauncherPaths ForInstall(InstallLayout install)
    {
        var files = new InstallSessionPaths(install);
        return new LauncherPaths
        {
            Root = files.Root, InstallRoot = files.InstallRoot, HostRoot = files.HostRoot, Session = files.Session,
            ScriptsFolder = files.ScriptsFolder, Backend = files.Backend, TemplatesFile = files.TemplatesFile,
            ProgressionFile = files.ProgressionFile, ProfilesRoot = files.ProfilesRoot, SettingsFile = files.SettingsFile,
            SessionFile = files.SessionFile, RunsFolder = files.RunsFolder
        };
    }

    // The installation containing the launcher, else the repository it was built in (the folder with the solution).
    public static LauncherPaths? Find()
    {
        if (InstallLayout.Find(AppContext.BaseDirectory) is { } install) return ForInstall(install);
        var root = AppContext.BaseDirectory;
        while (root != null && !File.Exists(Path.Combine(root, "HellForge.slnx")))
            root = Path.GetDirectoryName(root.TrimEnd(Path.DirectorySeparatorChar));
        return root != null ? ForRepository(root) : null;
    }
}
