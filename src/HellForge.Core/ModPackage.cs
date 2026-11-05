using System.IO.Compression;

namespace HellForge.Core;

// A mod packed for sharing: <folder>-<version>.zip holding the mod's folder, which players drop into their mods folder as
// it is (ModArchives unpacks it) or unpacked. In it: everything the mod folder holds but workspace leftovers (dot
// folders, build/, *.user: what friends' modlists ignore too) and, for a source mod, the DLL its folder may hold, since
// every player's HellForge builds the sources itself (ModBuilds). Entries are sorted with a fixed time, so the same mod
// always makes the same zip. Never written into a mods folder: a second copy of a mod's id loads neither.
public static class ModPackage
{
    static readonly DateTimeOffset Stamp = new(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);

    // The files that go into the zip, relative to the mod folder with '/'.
    // A recipe's rebuilt game patch files (ModManifest.AssetOutputs) are the game's and never go in: the recipe does.
    public static List<string> Files(ModManifest mod) =>
        Directory.GetFiles(mod.Folder, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(mod.Folder, f).Replace('\\', '/'))
            .Where(f => !ModCatalog.IsWorkspaceFile(f))
            .Where(f => !mod.AssetOutputs.Contains(f, StringComparer.OrdinalIgnoreCase))
            .Where(f => !mod.HasSource || !(f == mod.CodeDll || f.EndsWith(".lib", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase)))
            .Order(StringComparer.Ordinal).ToList();

    public static string Create(ModManifest mod, string outFolder)
    {
        outFolder = Path.GetFullPath(outFolder);
        for (var dir = outFolder; dir != null; dir = Path.GetDirectoryName(dir))
            if (Path.GetFileName(dir).Equals("mods", StringComparison.OrdinalIgnoreCase) && Path.GetDirectoryName(dir) is { } root && new InstallLayout(root).Exists)
                throw new InvalidDataException("A mod's zip does not go into an installation's mods folder: HellForge would load it beside the mod, and two copies of one id load neither.");
        if (mod.HasCode && !mod.HasSource && !File.Exists(Path.Combine(mod.Folder, mod.CodeDll!)))
            throw new InvalidDataException($"The mod has no sources and its DLL ({mod.CodeDll}) is missing: there is nothing to run.");
        var folder = Path.GetFileName(Path.TrimEndingDirectorySeparator(mod.Folder));
        var name = string.Concat($"{folder}-{mod.Version}".Select(c => char.IsLetterOrDigit(c) || c is '.' or '-' or '_' ? c : '_')) + ".zip";
        var target = Path.Combine(Directory.CreateDirectory(outFolder).FullName, name);
        var partial = target + ".partial";
        using (var zip = ZipFile.Open(partial, ZipArchiveMode.Create))
            foreach (var file in Files(mod))
            {
                var entry = zip.CreateEntry(folder + "/" + file, CompressionLevel.Optimal);
                entry.LastWriteTime = Stamp;
                using var input = File.OpenRead(Path.Combine(mod.Folder, file));
                using var output = entry.Open();
                input.CopyTo(output);
            }
        File.Move(partial, target, overwrite: true);
        return target;
    }
}
