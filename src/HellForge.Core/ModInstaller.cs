using System.IO.Compression;
using System.Security.Cryptography;

namespace HellForge.Core;

// Adding and removing mods (the launcher's Mods page: ADD MODS, REMOVE). A mod comes as a zip (copied into mods/ as it
// is: ModArchives unpacks it) or as a folder, chosen by its hellforge-mod.json (the folder is copied); a cosmetic pack
// (CosmeticPacks) also by its manifest.json. A .rar or .7z of either is unpacked (OtherArchives) and copied as a folder,
// also one dropped into mods/ itself (TakeDropped). A zipped cosmetic pack is too, also one in mods/ already: a zip stays
// beside its unpacked copy (two copies of the pack on the drive), and only a gameplay mod needs its zip (what friends
// compare: ModArchives.Shipped). A mod whose id is installed already replaces that one, so adding a newer zip updates a
// mod. Removing deletes the mod's zip or folder from mods/: only there, never through a link. The mods HellForge ships
// come back by themselves (BundledMods), so the page offers switching them off instead.
public static class ModInstaller
{
    // One mod to add: where it comes from, what it is, and the installed copy it replaces (its zip or folder), if any.
    // InstalledFolder: the folder the installed copy loads from (its zip's unpacked copy, for a zip), if any.
    public sealed record Candidate(string Source, bool IsZip, string Id, string Name, string Version, string? Replaces)
    {
        public string? InstalledFolder { get; init; }
    }

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
        var isOther = OtherArchives.Is(path);
        var file = Path.GetFileName(path);
        var source = isZip || isOther ? path : Path.GetDirectoryName(path)!;
        if (!isZip && !isOther && !file.Equals(ModCatalog.ManifestName, StringComparison.OrdinalIgnoreCase) &&
            !file.Equals(CosmeticPacks.ManifestName, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"choose a mod's .zip (or .rar, .7z), its {ModCatalog.ManifestName}, or a cosmetic pack's {CosmeticPacks.ManifestName}.");
        if (LocalInstall.Within(source, install.Mods))
            throw new InvalidDataException("it is in this installation's mods folder already.");
        if (isZip && ZipHoldsPack(path)) (isZip, isOther) = (false, true); // a zipped pack goes in as a folder
        if (isOther) source = Unpack(install, path); // a folder from here on, copied like one
        var mod = isZip ? ReadZip(install, path) : ParseAny(source);
        if (chosen.Any(m => m.Id == mod.Id)) throw new InvalidDataException($"{mod.Id} is chosen twice.");
        var current = installed.FirstOrDefault(m => m.Id == mod.Id);
        var replaces = current == null ? null : current.Archive != null ? Path.Combine(install.Mods, current.Archive) : current.Folder;
        return new(source, isZip, mod.Id, mod.Name, mod.Version, replaces) { InstalledFolder = current?.Folder };
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
        path = LocalInstall.NoLinks(path, install.Mods);
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

    // A .rar or .7z (OtherArchives), or a zipped pack (ModArchives), unpacked into the installation's temp as a folder to
    // copy: its files, under the archive's name when they sit at its root.
    static string Unpack(InstallLayout install, string archive)
    {
        var target = Path.Combine(install.Temp, "mod-unpacked-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(install.Temp);
        if (archive.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) ModArchives.Unpack(archive, target);
        else OtherArchives.Unpack(archive, target);
        var root = ModArchives.ManifestRoot(target);
        if (root != target) return root;
        var named = Path.Combine(install.Temp, "mod-unpacked-" + Guid.NewGuid().ToString("N")[..8], Path.GetFileNameWithoutExtension(archive));
        Directory.CreateDirectory(Path.GetDirectoryName(named)!);
        Directory.Move(target, named);
        return named;
    }

    // .rar and .7z files dropped into mods/ itself (the loaders read only zips and folders there), and zipped cosmetic packs
    // there (dropped, or added as zips by an earlier HellForge): each is added as Add mods adds one, as a folder, and the
    // archive goes; a pack keeps its settings (ModSettings.RenameId). One whose mod is installed already (an update waits for
    // the player's say on Add mods), or that cannot be added, stays where it is, and its line says why; one that holds
    // exactly the installed copy's files is removed, and its line says so. Returns those lines; the archives that failed are
    // not unpacked again until they change.
    public static List<string> TakeDropped(InstallLayout install)
    {
        var notes = new List<string>();
        if (!Directory.Exists(install.Mods)) return notes;
        foreach (var archive in Directory.GetFiles(install.Mods).Where(f => OtherArchives.Is(f) || IsPackZip(f)).Order(StringComparer.OrdinalIgnoreCase))
        {
            string? known;
            lock (refusedDrops) refusedDrops.TryGetValue(DropKey(archive), out known);
            if (known == null && archive.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && MoveLoadedPack(install, archive) is { } moved)
            {
                if (moved.Length > 0) notes.Add(moved);
                continue;
            }
            if ((known ?? TakeOne(install, archive)) is { } note) notes.Add(note);
        }
        return notes;
    }

    // what TakeDropped could not add, by path, size and time: not unpacked again until the file changes
    static readonly Dictionary<string, string> refusedDrops = new(StringComparer.OrdinalIgnoreCase);

    // zips of mods/ that are not packs (gameplay mods, which stay zips), by path, size and time: not read again until they change
    static readonly HashSet<string> notPackZips = new(StringComparer.OrdinalIgnoreCase);

    static bool IsPackZip(string file)
    {
        if (!file.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return false;
        var key = DropKey(file);
        lock (notPackZips) if (notPackZips.Contains(key)) return false;
        if (ZipHoldsPack(file)) return true;
        lock (notPackZips) notPackZips.Add(key);
        return false;
    }

    // Whether the zip holds a cosmetic pack where ModArchives loads a mod from (ManifestRoot: its root, or its one top-level
    // folder), read from its entries without unpacking it: a pack's patches or manifest.json there, and no HellForge
    // manifest. False for a zip that cannot be read: it stays a zip, and loading it says why.
    internal static bool ZipHoldsPack(string file)
    {
        try
        {
            using var zip = ZipFile.OpenRead(file);
            var files = zip.Entries.Select(e => (Name: e.FullName.Replace('\\', '/'), Entry: e)).Where(e => !e.Name.EndsWith('/')).ToList();
            bool? PackAt(string prefix)
            {
                var here = files.Where(f => f.Name.StartsWith(prefix, StringComparison.Ordinal) && !f.Name[prefix.Length..].Contains('/'))
                    .ToDictionary(f => f.Name[prefix.Length..], f => f.Entry, StringComparer.Ordinal);
                if (here.ContainsKey(ModCatalog.ManifestName) || here.ContainsKey(ModCatalog.LegacyManifestName)) return false; // a mod
                if (here.Keys.Any(n => ModCatalog.PatchFile().IsMatch(n))) return true;
                if (here.TryGetValue(CosmeticPacks.ManifestName, out var manifest))
                {
                    using var reader = new StreamReader(manifest.Open());
                    if (CosmeticPacks.IsPackManifest(reader.ReadToEnd())) return true;
                }
                return null; // neither here
            }
            if (PackAt("") is { } atRoot) return atRoot;
            var tops = zip.Entries.Select(e => e.FullName.Replace('\\', '/')).Where(n => n.Length > 0)
                .Select(n => n.Contains('/') ? n[..(n.IndexOf('/') + 1)] : n).Distinct(StringComparer.Ordinal).ToList();
            return tops.Count == 1 && tops[0].EndsWith('/') && PackAt(tops[0]) == true;
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return false; }
    }

    static string DropKey(string archive)
    {
        var info = new FileInfo(archive);
        return $"{info.FullName}|{info.Length}|{info.LastWriteTimeUtc.Ticks}";
    }

    // One dropped archive added; else why not.
    static string? TakeOne(InstallLayout install, string archive)
    {
        var file = Path.GetFileName(archive);
        // moved out first: Inspect takes nothing from mods/ itself, and two programs never add the same one
        var moved = Path.Combine(install.Temp, "mod-dropped-" + Guid.NewGuid().ToString("N")[..8], file);
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(moved)!);
            File.Move(archive, moved);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return $"{file}: not added yet ({error.Message}); it is added when the mods folder can be changed (not while a session runs).";
        }
        string? refused, note = null;
        try
        {
            var (mods, problems) = Inspect(install, [moved]);
            refused = problems.Count > 0 ? problems[0]
                : mods[0].Replaces != null ? $"{file}: {mods[0].Id} is installed already. To replace it with this one, use Add mods... and choose the {Path.GetExtension(file)}."
                : null;
            if (refused == null) Add(install, mods[0]);
            else if (problems.Count == 0 && !mods[0].IsZip && mods[0].InstalledFolder is { } installed && HoldsSameFiles(mods[0].Source, installed))
            {
                // a second copy of what is installed: nothing to decide, and it would only take the space again
                note = $"{file}: {mods[0].Id} is installed already with the same files, so this copy was removed.";
                refused = null;
            }
            if (refused != null || note != null) DeleteUnpacked(install, mods.FirstOrDefault()?.Source ?? moved);
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            refused = $"{file}: {error.Message}";
        }
        if (refused != null) refused = PutBack(moved, archive, refused);
        // added, removed, or put back: its temp folder goes (one the archive could not leave is named in the line)
        if (refused == null || !File.Exists(moved)) TryDelete(Path.GetDirectoryName(moved)!);
        return refused ?? note;
    }

    // A zipped pack loaded from mods/ before has its unpacked copy in mods/.unpacked: that folder is moved into mods/ (no
    // unpacking or copying again: a drive's worth of packs on the Mods page's first look) and the zip goes. Null when it has
    // no unpacked copy (TakeOne adds it then); "" when it moved, or could not move now (not while a session runs, on
    // Windows: the game holds the copy's files) and stays a zip until the next look; else the line for the Mods page.
    static string? MoveLoadedPack(InstallLayout install, string archive)
    {
        var file = Path.GetFileName(archive);
        string folder, root;
        ModManifest pack;
        try
        {
            folder = ModArchives.UnpackedFolderOf(Path.Combine(install.Mods, ModArchives.UnpackedFolder), archive);
            if (!Directory.Exists(folder)) return null;
            root = ModArchives.ManifestRoot(folder);
            if (ModCatalog.ManifestIn(root) != null || !CosmeticPacks.IsPack(root)) return null;
            pack = CosmeticPacks.Parse(root, CosmeticPacks.Choice.Default);
            // a folder with the same pack installed as well: TakeOne decides (the same files: the zip goes; else it stays)
            if (Directory.GetDirectories(install.Mods).Any(d => !Path.GetFileName(d).StartsWith('.') && CosmeticPacks.IsPack(d) &&
                CosmeticPacks.Parse(d, CosmeticPacks.Choice.Default).Id == pack.Id)) return null;
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { return null; }
        // its folder's name as Add mods would give it: the zip's, or its one top-level folder's
        var name = root == folder ? Path.GetFileNameWithoutExtension(archive) : Path.GetFileName(root);
        var target = Path.Combine(install.Mods, name);
        for (var n = 2; File.Exists(target) || Directory.Exists(target); n++) target = Path.Combine(install.Mods, $"{name}-{n}");
        // moved out first, as TakeOne does: two programs never take the same one
        var away = Path.Combine(install.Temp, "mod-dropped-" + Guid.NewGuid().ToString("N")[..8], file);
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(away)!);
            File.Move(archive, away);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            TryDelete(Path.GetDirectoryName(away)!);
            return "";
        }
        try { Directory.Move(root, target); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            try { File.Move(away, archive); }
            catch (Exception back) when (back is IOException or UnauthorizedAccessException)
            {
                return $"{file}: it could not be unpacked into the mods folder ({error.Message}), and it was moved to {away}.";
            }
            TryDelete(Path.GetDirectoryName(away)!);
            return "";
        }
        TryDelete(Path.GetDirectoryName(away)!);
        // the folder named after the zip, not its unpacked copy: a pack without a name of its own gets that id
        try
        {
            var now = CosmeticPacks.Parse(target, CosmeticPacks.Choice.Default).Id;
            if (now != pack.Id) ModSettings.RenameId(install.Mods, pack.Id, now);
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { }
        return "";
    }

    // Every file of the unpacked archive is in the installed folder with the same bytes (the installed copy may have more:
    // what the mod wrote beside itself). False for an archive without files.
    static bool HoldsSameFiles(string unpacked, string installed)
    {
        var files = Directory.GetFiles(unpacked, "*", SearchOption.AllDirectories);
        if (files.Length == 0) return false;
        foreach (var file in files)
        {
            var other = Path.Combine(installed, Path.GetRelativePath(unpacked, file));
            if (!File.Exists(other) || new FileInfo(other).Length != new FileInfo(file).Length || !SameBytes(file, other)) return false;
        }
        return true;
    }

    static bool SameBytes(string a, string b)
    {
        using var first = ModCatalog.OpenShared(a);
        using var second = ModCatalog.OpenShared(b);
        return SHA256.HashData(first).AsSpan().SequenceEqual(SHA256.HashData(second));
    }

    // A refused archive back where the player put it, remembered so it is not unpacked again until it changes.
    static string PutBack(string moved, string archive, string refused)
    {
        try { File.Move(moved, archive); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return refused + $" It was moved to {moved}."; }
        lock (refusedDrops) refusedDrops[DropKey(archive)] = refused;
        return refused;
    }

    static void TryDelete(string folder)
    {
        try { if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
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
