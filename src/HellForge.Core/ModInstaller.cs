namespace HellForge.Core;

// Adding and removing mods (the launcher's Mods page: ADD MODS, REMOVE). A mod comes as a zip (copied into mods/ as it
// is: ModArchives unpacks it) or as a folder, chosen by its hellforge-mod.json (the folder is copied); a cosmetic pack
// (CosmeticPacks) also by its manifest.json. A .rar or .7z of either is unpacked and copied as a folder. A mod whose id is
// installed already replaces that one, so adding a newer zip updates a mod. Removing deletes the mod's zip or folder
// from mods/: only there, never through a link. The mods HellForge ships come back by themselves (BundledMods), so the
// page offers switching them off instead.
public static class ModInstaller
{
    // One mod to add: where it comes from, what it is, and the installed copy it replaces (its zip or folder), if any.
    public sealed record Candidate(string Source, bool IsZip, string Id, string Name, string Version, string? Replaces);

    // The archives Windows' tar unpacks for us (Unpack), as a folder to copy: .NET reads only zips.
    static readonly string[] TarArchives = [".rar", ".7z"];

    // Reads each chosen file (a .zip, .rar or .7z, or a hellforge-mod.json for its folder) without changing anything. A file that is
    // not a mod is a problem, not an exception, so the others still go in.
    public static (List<Candidate> Mods, List<string> Problems) Inspect(InstallLayout install, IEnumerable<string> chosen)
    {
        var (installed, _) = ModCatalog.Load(install.Mods);
        var mods = new List<Candidate>();
        var problems = new List<string>();
        foreach (var path in chosen.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try { mods.Add(InspectOne(install, path, installed, mods)); }
            catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                problems.Add($"{Path.GetFileName(path)}: {error.Message}");
            }
        }
        return (mods, problems);
    }

    // One chosen file as a mod to add, with the installed copy it replaces; chosen: the ones read before it.
    static Candidate InspectOne(InstallLayout install, string path, List<ModManifest> installed, List<Candidate> chosen)
    {
        var isZip = path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase);
        var isTar = TarArchives.Any(e => path.EndsWith(e, StringComparison.OrdinalIgnoreCase));
        var file = Path.GetFileName(path);
        var source = isZip || isTar ? path : Path.GetDirectoryName(path)!;
        if (!isZip && !isTar && !file.Equals(ModCatalog.ManifestName, StringComparison.OrdinalIgnoreCase) &&
            !file.Equals(CosmeticPacks.ManifestName, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"choose a mod's .zip (or .rar, .7z), its {ModCatalog.ManifestName}, or a cosmetic pack's {CosmeticPacks.ManifestName}.");
        if (LocalInstall.Within(source, install.Mods))
            throw new InvalidDataException("it is in this installation's mods folder already.");
        if (isTar) source = Unpack(install, path); // a folder from here on, copied like one
        var mod = isZip ? ReadZip(install, path) : ParseAny(source);
        if (chosen.Any(m => m.Id == mod.Id)) throw new InvalidDataException($"{mod.Id} is chosen twice.");
        var current = installed.FirstOrDefault(m => m.Id == mod.Id);
        var replaces = current == null ? null : current.Archive != null ? Path.Combine(install.Mods, current.Archive) : current.Folder;
        return new(source, isZip, mod.Id, mod.Name, mod.Version, replaces);
    }

    // Puts the mod into mods/ (after removing the copy it replaces); returns where it went.
    public static string Add(InstallLayout install, Candidate mod)
    {
        Directory.CreateDirectory(install.Mods);
        if (mod.Replaces != null) Delete(install, mod.Replaces);
        var target = FreeTarget(install, mod);
        // copied under a name nothing loads (a dot folder, a .partial file), then moved in whole: the Mods page and a
        // starting session never see half a mod
        var staging = Path.Combine(install.Mods, ".adding-" + Guid.NewGuid().ToString("N")[..8] + (mod.IsZip ? ".partial" : ""));
        try
        {
            if (mod.IsZip) File.Copy(mod.Source, staging);
            else CopyFolder(mod.Source, staging, whole: CosmeticPacks.IsPack(mod.Source));
            if (mod.IsZip) File.Move(staging, target);
            else Directory.Move(staging, target);
        }
        catch
        {
            Discard(staging);
            throw;
        }
        DeleteUnpacked(install, mod.Source);
        return target;
    }

    // The mod's name in mods/, numbered when a file or folder has it already.
    static string FreeTarget(InstallLayout install, Candidate mod)
    {
        var name = Path.GetFileName(mod.Source);
        var target = Path.Combine(install.Mods, name);
        for (var n = 2; File.Exists(target) || Directory.Exists(target); n++)
            target = Path.Combine(install.Mods, mod.IsZip ? $"{Path.GetFileNameWithoutExtension(name)}-{n}.zip" : $"{name}-{n}");
        return target;
    }

    // A half-copied mod, removed.
    static void Discard(string staging)
    {
        try { if (File.Exists(staging)) File.Delete(staging); else if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        catch (IOException) { }
    }

    // A .rar's or .7z's unpacked copy (Unpack) is not needed once the mod is in mods/.
    static void DeleteUnpacked(InstallLayout install, string source)
    {
        var extracted = Path.GetFullPath(source);
        if (LocalInstall.Within(extracted, install.Temp) && Path.GetFileName(Path.GetDirectoryName(extracted) ?? "").StartsWith("mod-unpacked-", StringComparison.Ordinal))
            try { Directory.Delete(Path.GetDirectoryName(extracted)!, recursive: true); } catch (IOException) { }
    }

    // Deletes the mod's files: its zip (its unpacked copy goes at the next load) or its folder.
    public static void Remove(InstallLayout install, ModManifest mod) =>
        Delete(install, mod.Archive != null ? Path.Combine(install.Mods, mod.Archive) : mod.Folder);

    static void Delete(InstallLayout install, string path)
    {
        path = Path.GetFullPath(path);
        if (!string.Equals(Path.GetDirectoryName(path), Path.GetFullPath(install.Mods).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) ||
            Path.GetFileName(path).StartsWith('.'))
            throw new InvalidDataException($"{path} is not a mod in this installation's mods folder; nothing was deleted.");
        LocalInstall.NoLinks(path);
        if (File.Exists(path)) File.Delete(path);
        else if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
    }

    static ModManifest ReadZip(InstallLayout install, string zip)
    {
        var staging = Path.Combine(install.Temp, "mod-inspect-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            ModArchives.Unpack(zip, staging);
            return ParseAny(ModArchives.ManifestRoot(staging));
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
    }

    static ModManifest ParseAny(string folder) =>
        ModCatalog.ManifestIn(folder) != null ? ModCatalog.Parse(folder, EntityProfile.Current)
        : CosmeticPacks.IsPack(folder) ? CosmeticPacks.Parse(folder, CosmeticPacks.Choice.Default)
        : throw new InvalidDataException($"there is no {ModCatalog.ManifestName}, and it is not a cosmetic pack.");

    // .NET reads no RAR or 7z: Windows' own tar (libarchive; Windows 11) unpacks it into the installation's temp, as a
    // folder to copy. tar refuses absolute and parent paths itself; links are refused here, so the copy reads nothing
    // outside the archive. Where tar cannot unpack it, the player unpacks it and adds the folder.
    static string Unpack(InstallLayout install, string archive)
    {
        var kind = Path.GetExtension(archive).ToLowerInvariant();
        var target = Path.Combine(install.Temp, "mod-unpacked-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(target);
        var tar = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
        var start = new System.Diagnostics.ProcessStartInfo(tar) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        foreach (var a in new[] { "-xf", archive, "-C", target }) start.ArgumentList.Add(a);
        try
        {
            using var process = System.Diagnostics.Process.Start(start) ?? throw new IOException("tar did not start.");
            var errors = process.StandardError.ReadToEndAsync();
            process.StandardOutput.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0) throw new InvalidDataException($"Windows could not unpack it ({errors.Result.Trim()}): unpack the {kind} yourself and add its folder.");
        }
        catch (System.ComponentModel.Win32Exception) { throw new InvalidDataException($"Windows has no tar to unpack it with: unpack the {kind} yourself and add its folder."); }
        if (new DirectoryInfo(target).EnumerateFileSystemInfos("*", SearchOption.AllDirectories).Any(e => (e.Attributes & FileAttributes.ReparsePoint) != 0))
            throw new InvalidDataException($"the {kind} contains a link; HellForge adds only plain files and folders.");
        var root = ModArchives.ManifestRoot(target);
        // the folder the mod is copied as: the archive's name when its files sit at its root
        if (root == target)
        {
            var named = Path.Combine(install.Temp, "mod-unpacked-" + Guid.NewGuid().ToString("N")[..8], Path.GetFileNameWithoutExtension(archive));
            Directory.CreateDirectory(Path.GetDirectoryName(named)!);
            Directory.Move(target, named);
            root = named;
        }
        return root;
    }

    // The folder as a mod: no workspace leftovers (.idea, build/...), as ModPackage packs it; a cosmetic pack whole (its
    // previews are often in a dot folder, .jpgs).
    static void CopyFolder(string from, string to, bool whole)
    {
        foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(from, file).Replace('\\', '/');
            if (!whole && ModCatalog.IsWorkspaceFile(relative)) continue;
            var destination = Path.Combine(to, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(file, destination);
        }
    }
}
