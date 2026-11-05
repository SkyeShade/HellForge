using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.Cli;

// assets-check, assets-strip and package-mod's strip (docs/modding/assets.md): a mod's game patch files compared with the
// builds of the game on this PC, and the mod written without Arrowhead's bytes.
static class AssetCommands
{
    // --install, else the installation whose mods folder holds the mod, else the one this program is in; null when none
    // is (the Steam install alone is used then).
    public static InstallLayout? Installation(string? given, string modFolder)
    {
        if (given != null)
        {
            var layout = new InstallLayout(Path.GetFullPath(given));
            return layout.Exists ? layout : throw new InvalidDataException($"{layout.Root} is not a HellForge installation.");
        }
        var parent = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(modFolder));
        if (parent != null && Path.GetFileName(parent).Equals("mods", StringComparison.OrdinalIgnoreCase) && Path.GetDirectoryName(parent) is { } root &&
            new InstallLayout(root) is { Exists: true } beside) return beside;
        for (var dir = AppContext.BaseDirectory; dir != null; dir = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(dir)))
            if (new InstallLayout(dir) is { Exists: true } around) return around;
        return null;
    }

    // Checks (outFolder null) or strips the mod; the report goes to reportPath, else beside the stripped mod, else into
    // the asset kit's folder.
    public static (AssetKit.Result Result, string Report) Run(string modFolder, InstallLayout? install, string? outFolder, string? reportPath = null)
    {
        var builds = GameBuilds.Local(install);
        var index = AssetKit.Index(builds, AssetKit.CacheFolder(install), Console.Error.WriteLine);
        var games = builds.Select(GameBuilds.Open).ToList();
        try
        {
            var named = builds.ToDictionary(b => b.Label, b => b.What, StringComparer.Ordinal);
            var result = AssetKit.Strip(modFolder, index, games, outFolder, Console.Error.WriteLine, label => named.GetValueOrDefault(label));
            var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(modFolder));
            var report = reportPath ?? (outFolder != null ? Path.TrimEndingDirectorySeparator(outFolder) + ".asset-report.md"
                : Path.Combine(AssetKit.CacheFolder(install), $"{name}.asset-report.md"));
            Directory.CreateDirectory(Path.GetDirectoryName(report)!);
            File.WriteAllText(report, result.ReportText);
            Console.Error.WriteLine(result.Summary);
            Console.Error.WriteLine($"Report: {report}");
            return (result, report);
        }
        finally
        {
            foreach (var game in games) (game as IDisposable)?.Dispose();
        }
    }

    // The mod stripped into a temporary folder named as the mod's (a package is named after its folder), and what was
    // done; the folder is null when nothing in the mod is the game's (it is packaged as it is). A mod whose recipe would
    // not rebuild it exactly is not packaged. The report goes to reportPath (beside the zip).
    public static (string? Folder, string Summary) StripForPackage(string modFolder, InstallLayout? install, string reportPath)
    {
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(modFolder));
        var root = Path.Combine(Path.GetTempPath(), $"hellforge-package-{Guid.NewGuid():N}");
        var target = Path.Combine(root, name);
        try
        {
            var (result, _) = Run(modFolder, install, target, reportPath);
            switch (result.Outcome)
            {
                case AssetKit.Outcome.Failed:
                    throw new InvalidDataException($"The mod is not packaged: {result.Summary}");
                case AssetKit.Outcome.NoGameContent:
                    Directory.Delete(root, recursive: true);
                    return (null, result.Summary);
                default:
                    return (target, result.Summary);
            }
        }
        catch
        {
            if (Directory.Exists(root)) try { Directory.Delete(root, recursive: true); } catch (IOException) { }
            throw;
        }
    }
}
