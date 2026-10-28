using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.SdkShimGen;

public sealed record SteamExport(string Name, uint Ordinal, bool Data);
public sealed record SteamExportManifest(int SchemaVersion, string SourceSha256, IReadOnlyList<SteamExport> Exports);

public static class SteamExportsManifest
{
    static readonly Regex NamePattern = new("^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.CultureInvariant);
    static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };

    public static SteamExportManifest Read(string path)
    {
        using var stream = File.OpenRead(path);
        var manifest = JsonSerializer.Deserialize<SteamExportManifest>(stream, JsonOptions)
            ?? throw new InvalidDataException("The Steam export manifest is empty.");
        if (manifest.SchemaVersion != 1 || manifest.SourceSha256 is null || manifest.SourceSha256.Length != 64 ||
            manifest.SourceSha256.Any(character => !Uri.IsHexDigit(character)) || manifest.Exports is null || manifest.Exports.Count == 0)
            throw new InvalidDataException("The Steam export manifest has invalid metadata.");
        var names = new HashSet<string>(StringComparer.Ordinal);
        var ordinals = new HashSet<uint>();
        foreach (var export in manifest.Exports)
            if (export is null || export.Name is null || !NamePattern.IsMatch(export.Name) ||
                !names.Add(export.Name) || !ordinals.Add(export.Ordinal))
                throw new InvalidDataException("The Steam export manifest contains an invalid or duplicate export.");
        return manifest;
    }

    public static SteamExportManifest FromPe(string path)
    {
        var data = PeTables.DataExports(path);
        var exports = PeTables.Exports(path).OrderBy(export => export.Ordinal)
            .Select(export => new SteamExport(export.Name, export.Ordinal, data.Contains(export.Name))).ToArray();
        if (exports.Length == 0) throw new InvalidDataException("The DLL has no named exports.");
        using var stream = File.OpenRead(path);
        return new(1, Convert.ToHexString(SHA256.HashData(stream)), exports);
    }

    public static void WriteFromPe(string source, string destination)
    {
        var manifest = FromPe(source);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destination))!);
        File.WriteAllText(destination, JsonSerializer.Serialize(manifest, JsonOptions) + Environment.NewLine);
    }

    public static void ValidateSourceHash(SteamExportManifest manifest, string expectedSha256)
    {
        if (!manifest.SourceSha256.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The Steam export manifest was not generated from the pinned supported-build DLL.");
    }

    public static string ForwardDefinition(SteamExportManifest manifest, string module, IReadOnlySet<string> ownFunctions)
    {
        var exports = manifest.Exports.ToDictionary(export => export.Name, StringComparer.Ordinal);
        var unknown = ownFunctions.Where(name => !exports.ContainsKey(name)).ToArray();
        if (unknown.Length > 0)
            throw new InvalidDataException("Hand-written functions the original does not export: " + string.Join(", ", unknown));
        var definition = new StringBuilder().AppendLine("LIBRARY steam_api64.dll").AppendLine("EXPORTS");
        foreach (var export in manifest.Exports.OrderBy(export => export.Ordinal))
            definition.AppendLine(ownFunctions.Contains(export.Name) ? $"    {export.Name} @{export.Ordinal}"
                : $"    {export.Name} = {module}.{export.Name} @{export.Ordinal}{(export.Data ? " DATA" : "")}");
        return definition.ToString();
    }
}
