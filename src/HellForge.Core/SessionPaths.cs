namespace HellForge.Core;

// The files a session is started from and reports to: the launcher's (LauncherPaths, which also covers a build in the
// repository) and an installation's own (InstallSessionPaths, what HellForge.Cli and other launchers use).
public interface ISessionPaths
{
    // The folder the session runner starts in.
    string Root { get; }
    // The installation, or null for a build in the repository (no sessions there).
    string? InstallRoot { get; }
    // Where the launcher's settings and the live war's cache live.
    string HostRoot { get; }
    // The engine's session runner, HellForge.Session.exe.
    string Session { get; }
    string Backend { get; }
    string TemplatesFile { get; }
    string ProfilesRoot { get; }
    // launcher-settings.json: the launcher's choices, which other launchers start from too (SessionOptions).
    string SettingsFile { get; }
    // logs/play-session.json: the session runner names its newest run there.
    string SessionFile { get; }
    // logs/sessions: one folder per run.
    string RunsFolder { get; }
}

// An installation's session files, as its launcher uses them.
public sealed class InstallSessionPaths(InstallLayout install) : ISessionPaths
{
    public InstallLayout Install { get; } = install;
    public string Root => Install.Root;
    public string? InstallRoot => Install.Root;
    public string HostRoot => Install.Root;
    public string Session => Path.Combine(Install.App, "session", "HellForge.Session.exe");
    public string Backend => Path.Combine(Install.App, "backend", "HellForge.LocalBackend.exe");
    public string TemplatesFile => Path.Combine(Install.App, "profile-templates.json");
    public string ProgressionFile => GameCatalog.ProgressionData(Install.Root);
    public string ScriptsFolder => Path.Combine(Install.App, "scripts");
    public string ProfilesRoot => Install.Profiles;
    public string SettingsFile => Path.Combine(Install.Root, "launcher-settings.json");
    public string SessionFile => Path.Combine(Install.Logs, "play-session.json");
    public string RunsFolder => Install.Sessions;
    public string LiveWarCache => Path.Combine(Install.Root, "live-war.json");
}
