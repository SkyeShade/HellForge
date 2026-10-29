using System.Diagnostics;

namespace HellForge.Core;

// The paths a container-mode invocation must make visible besides the prefix: the game folder Steam would name, extra
// mounts and Steam's shader cache. Host mode ignores them.
public sealed record LinuxProtonPaths(string? InstallPath = null, IReadOnlyList<string>? Mounts = null,
    string? ShaderPath = null);

// One Proton invocation as the wrapper would build it. The arguments already contain the Steam Linux Runtime entry point
// in container mode, and Environment contains only the variables this invocation needs: host mode keeps today's
// STEAM_COMPAT_* set, container mode adds the tool and path variables pressure-vessel uses.
public sealed record LinuxProtonInvocation(
    SteamRuntime Runtime,
    string ProtonScript,
    string SteamRoot,
    string Prefix,
    IReadOnlyList<string> Arguments,
    IReadOnlyDictionary<string, string> Environment)
{
    public static LinuxProtonInvocation Create(string protonScript, string steamRoot, string prefix, string verb,
        IEnumerable<string> arguments, LinuxProtonPaths? paths = null) =>
        From(SteamRuntimeLocator.Resolve(protonScript, steamRoot), (protonScript, steamRoot, prefix), verb, arguments,
            paths);

    public static LinuxProtonInvocation From(SteamRuntime runtime,
        (string ProtonScript, string SteamRoot, string Prefix) tool, string verb, IEnumerable<string> arguments,
        LinuxProtonPaths? paths = null)
    {
        var (protonScript, steamRoot, prefix) = tool;
        var proton = LocalInstall.ResolveLinks(LocalInstall.FullPath(protonScript));
        var steam = LocalInstall.ResolveLinks(LocalInstall.FullPath(steamRoot));
        var compatibilityData = LocalInstall.ResolveLinks(LocalInstall.FullPath(prefix));
        var environment = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = steam,
            ["STEAM_COMPAT_DATA_PATH"] = compatibilityData,
            // Not SteamAppId: measured, SteamAppId together with SteamGameId makes Steam count this helper as a
            // HELLDIVERS 2 session (post-session shader processing, "Playing" status). The game itself gets both.
            ["STEAM_COMPAT_APP_ID"] = LocalInstall.AppId,
            ["SteamGameId"] = LocalInstall.AppId,
            ["WINEPREFIX"] = Path.Combine(compatibilityData, "pfx")
        };
        if (runtime.Mode == SteamRuntimeMode.Container)
            AddContainerEnvironment(environment, runtime, (steam, proton, compatibilityData), paths ?? new());
        return new(runtime, proton, steam, compatibilityData,
            SteamRuntimeLocator.Arguments(runtime, proton, verb, arguments), environment);
    }

    public ProcessStartInfo StartInfo(string workingDirectory, bool redirectOutput = false)
    {
        if (Arguments.Count == 0) throw new InvalidOperationException("A Proton invocation has no command.");
        var start = new ProcessStartInfo(Arguments[0])
        {
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = redirectOutput,
            RedirectStandardError = redirectOutput
        };
        foreach (var argument in Arguments.Skip(1)) start.ArgumentList.Add(argument);
        foreach (var pair in Environment) start.Environment[pair.Key] = pair.Value;
        // An inherited one (a launcher started from a Steam shortcut) would pair with SteamGameId the same way.
        start.Environment.Remove("SteamAppId");
        return start;
    }

    static void AddContainerEnvironment(Dictionary<string, string> environment, SteamRuntime runtime,
        (string Steam, string Proton, string CompatibilityData) tool, LinuxProtonPaths paths)
    {
        var (steam, proton, compatibilityData) = tool;
        var (installPath, mounts, shaderPath) = (paths.InstallPath, paths.Mounts, paths.ShaderPath);
        var protonFolder = Path.GetDirectoryName(proton)!;
        var toolPaths = new List<string>();
        if (!string.IsNullOrEmpty(runtime.Folder)) toolPaths.Add(runtime.Folder);
        toolPaths.Add(protonFolder);
        environment["STEAM_COMPAT_TOOL_PATHS"] = string.Join(Path.PathSeparator.ToString(), toolPaths);
        var libraries = new List<string>();
        try { libraries.AddRange(LocalInstall.FindLibraries(steam)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        if (libraries.Count == 0) libraries.Add(steam);
        environment["STEAM_COMPAT_LIBRARY_PATHS"] = string.Join(Path.PathSeparator.ToString(), libraries);
        if (!string.IsNullOrWhiteSpace(installPath)) environment["STEAM_COMPAT_INSTALL_PATH"] = installPath;
        if (!string.IsNullOrWhiteSpace(shaderPath)) environment["STEAM_COMPAT_SHADER_PATH"] = shaderPath;
        var visible = new List<string>();
        if (!string.IsNullOrWhiteSpace(installPath)) visible.Add(installPath);
        visible.Add(compatibilityData);
        if (mounts != null) visible.AddRange(mounts.Where(path => !string.IsNullOrWhiteSpace(path)));
        if (!string.IsNullOrWhiteSpace(shaderPath)) visible.Add(shaderPath);
        var distinct = visible.Select(LocalInstall.FullPath).Distinct(StringComparer.Ordinal).ToArray();
        if (distinct.Length > 0) environment["STEAM_COMPAT_MOUNTS"] = string.Join(Path.PathSeparator.ToString(), distinct);
    }
}
