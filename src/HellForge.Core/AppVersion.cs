using System.Reflection;

namespace HellForge.Core;

// HellForge's version (Directory.Build.props), with the commit it was built from: "0.1.0-beta.1 (b89d1e7)". Shown in
// the corner of the launcher and of HellForge-Setup, so players know what they run and what they install.
public static class AppVersion
{
    public static string Text { get; } = Read();

    // The installed app's app-build.json has the publish version and commit separately. Format them the way this
    // assembly's informational version is shown, so the native readiness record matches the Wine runner's AppVersion.Text.
    public static string FromParts(string version, string commit)
    {
        version = version.Trim();
        commit = commit.Trim();
        if (commit.Length == 0) return version;
        return $"{version} ({commit[..Math.Min(7, commit.Length)]})";
    }

    static string Read()
    {
        var version = typeof(AppVersion).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unknown";
        var plus = version.IndexOf('+');
        return plus < 0 ? version : FromParts(version[..plus], version[(plus + 1)..]);
    }
}
