namespace HellForge.Core;

// Which Proton opens HELLDIVERS 2's own prefix. Steam's CompatToolMapping is the first source, but current Steam often
// has no entry there at all: when it names no tool, the prefix's own config_info says which Proton last opened it.
// Proton writes that file when it runs the game, so this follows the prefix rather than guessing a default.
public static partial class LinuxCapture
{
    const string ConfigInfoFile = "config_info";
    const string PrefixVersionFile = "version";
    const string CurrentPrefixVersionPrefix = "CURRENT_PREFIX_VERSION=";

    // The Proton that was found, and where it came from for the capture log.
    public sealed record ProtonResolution(string? Proton, string Source);

    public static string? ProtonForGame(string? steamRoot) => FindProtonForGame(steamRoot).Proton;

    public static ProtonResolution FindProtonForGame(string? steamRoot)
    {
        if (steamRoot == null) return new(null, "Steam was not found");
        string? toolName = null;
        try
        {
            var config = Path.Combine(steamRoot, "config", "config.vdf");
            if (File.Exists(config)) toolName = CompatToolName(File.ReadAllText(config));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var mappingProblem = "no CompatToolMapping entry";
        if (toolName != null)
        {
            var mapped = ResolveCompatTool(steamRoot, toolName);
            if (mapped != null) return new(mapped, "Steam's CompatToolMapping (" + toolName + ")");
            mappingProblem = "Steam's CompatToolMapping names " + toolName + ", but that Proton is not installed";
        }
        if (ProtonFromConfigInfo(steamRoot, out var version) is { } fromInfo)
            return new(fromInfo, "the prefix's config_info (CURRENT_PREFIX_VERSION " + version + ")");
        return new(null, mappingProblem + ", and the prefix's config_info did not name a matching Proton");
    }

    // config_info's first line is the prefix version; the paths below it point inside the Proton that wrote it, for
    // example .../steamapps/common/Proton - Experimental/files/share/fonts/. The Proton script beside that root must
    // declare the same CURRENT_PREFIX_VERSION as the prefix's version file. Nothing is taken from a missing folder.
    static string? ProtonFromConfigInfo(string? steamRoot, out string? version)
    {
        version = null;
        var prefix = FindPrefix(steamRoot, null);
        if (prefix == null) return null;
        version = PrefixVersion(prefix);
        if (string.IsNullOrWhiteSpace(version)) return null;
        string[] lines;
        try { lines = File.ReadAllLines(Path.Combine(prefix, ConfigInfoFile)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        var content = lines.Select(line => line.Trim()).Where(line => line.Length > 0).ToArray();
        if (content.Length < 2 || !string.Equals(content[0], version, StringComparison.Ordinal)) return null;
        foreach (var path in content.Skip(1))
            if (ProtonFromConfigInfoPath(path, version) is { } proton) return proton;
        return null;
    }

    static string? ProtonFromConfigInfoPath(string path, string version)
    {
        try
        {
            var normalized = path.Replace('\\', '/');
            var files = normalized.LastIndexOf("/files/", StringComparison.Ordinal);
            var dist = normalized.LastIndexOf("/dist/", StringComparison.Ordinal);
            var marker = Math.Max(files, dist);
            if (marker <= 0) return null;
            var proton = Path.Combine(Path.GetFullPath(normalized[..marker]), "proton");
            if (!File.Exists(proton)) return null;
            return string.Equals(CurrentPrefixVersion(proton), version, StringComparison.Ordinal) ? proton : null;
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    // The value of the proton script's CURRENT_PREFIX_VERSION line, without quotes. Proton's own install version file is
    // a build id and is not compared with the prefix's version.
    public static string? CurrentPrefixVersion(string protonScript)
    {
        try
        {
            foreach (var line in File.ReadLines(protonScript))
            {
                var text = line.Trim();
                if (text.StartsWith("export ", StringComparison.Ordinal)) text = text["export ".Length..].TrimStart();
                if (!text.StartsWith(CurrentPrefixVersionPrefix, StringComparison.Ordinal)) continue;
                var value = text[CurrentPrefixVersionPrefix.Length..].Trim();
                if (value.Length >= 2 && (value[0] == '"' && value[^1] == '"' || value[0] == '\'' && value[^1] == '\''))
                    value = value[1..^1];
                return value.Length == 0 ? null : value;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return null;
    }
}
