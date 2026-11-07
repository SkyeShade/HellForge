using System.Globalization;
using System.Text.Json;

namespace HellForge.Core;

// Data mods for one session: their patch files are placed into game/data, renumbered so each bundle's patches stay
// contiguous after any the player installed there by hand, and listed in temp/staged-mods.json so the next session
// removes exactly them first. Game files from Steam are never touched. A placed patch is a hard link to the mod's file
// where it may be (Linkable), so it takes no second copy of the mod's bytes; else a copy (FileLinks). A mod's generated
// data (ModDataCache.Prepare) is staged after its data/ folder, the same way, and HellForge's own patches for the session
// (the locked warbond art, WarbondLocks) after every mod's.
public static class ModStaging
{
    const string RecordName = "staged-mods.json";
    const string LiveRecordName = "live-packages.json";
    // The staged names this version placed (linked, or copied where a link may not be or failed). A name staged by an
    // earlier version (always a copy) is placed again once, as a link where it may be: kept as it was, a player's
    // existing staged copies would stay copies until their mods change.
    const string PlacedRecordName = "staged-placed.json";

    // A mod's patches as Stage stages them, each a patch's main file: its data/ folder's, then its generated data's.
    public static List<string> Patches(ModManifest mod)
    {
        var patches = mod.DataFiles.Where(f => ModCatalog.PatchFile().Match(f).Groups[2].Length == 0).Select(f => Path.Combine(mod.Folder, "data", f)).ToList();
        if (mod.GeneratedData != null)
            patches.AddRange(Directory.GetFiles(mod.GeneratedData).Where(f => ModCatalog.PatchFile().Match(Path.GetFileName(f)) is { Success: true } m && m.Groups[2].Length == 0)
                .Order(StringComparer.Ordinal));
        return patches;
    }

    // A live change's hot packages (GameData.LiveData) in game/data: recorded, so the next session's staging removes them.
    public static void RecordLive(InstallLayout install, IEnumerable<string> files)
    {
        Directory.CreateDirectory(install.Temp);
        var record = Path.Combine(install.Temp, LiveRecordName);
        var names = LivePackages(install);
        names.AddRange(files.Select(Path.GetFileName).OfType<string>());
        File.WriteAllText(record, JsonSerializer.Serialize(names.Distinct(StringComparer.Ordinal).ToList()));
    }

    static List<string> LivePackages(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, LiveRecordName);
        try { return File.Exists(record) ? JsonSerializer.Deserialize<List<string>>(File.ReadAllText(record)) ?? [] : []; }
        catch (JsonException) { return []; }
    }

    static bool IsLivePackage(string name)
    {
        var stem = name.EndsWith(".gpu_resources", StringComparison.Ordinal) ? name[..^14] : name.EndsWith(".stream", StringComparison.Ordinal) ? name[..^7] : name;
        return stem.Length == 16 && stem.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    }

    // The last sessions' hot packages removed (a name of 16 hex digits and its sidecars only); those still held stay listed.
    static void RemoveLive(InstallLayout install, string data)
    {
        var left = new List<string>();
        foreach (var name in LivePackages(install).Where(IsLivePackage))
        {
            var path = Path.Combine(data, name);
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { left.Add(name); }
        }
        var record = Path.Combine(install.Temp, LiveRecordName);
        if (left.Count > 0) File.WriteAllText(record, JsonSerializer.Serialize(left));
        else if (File.Exists(record)) File.Delete(record);
    }

    // What the last Stage did: files placed (linked or copied), and files kept from the session before (the same source,
    // unchanged); LastLinked: how many of the placed ones are links.
    public static (int Copied, int Kept) LastCounts { get; private set; }
    public static int LastLinked { get; private set; }

    // Whether a staged patch may be a hard link to its source. On Windows always: the game opens its data files with
    // FILE_SHARE_READ alone and keeps them open (nxstorage, helldivers2.exe 0x740E26), so while it runs no program can
    // write to a staged patch through either of its names; an editor's save fails as one into game/data does, and never
    // changes bytes under the game. Linux has no such sharing rule: a mod folder edited in place while the game runs would
    // change the game's file too. There a link is made only to HellForge's own stores, which nothing rewrites in place
    // (zips' unpacked copies, mod sync's downloads, temp: generated data and HellForge's own patches); a mod folder is
    // copied, which btrfs and XFS clone without using space.
    static bool Linkable(InstallLayout install, string source) =>
        OperatingSystem.IsWindows() ||
        LocalInstall.Within(source, Path.Combine(install.Mods, ModArchives.UnpackedFolder)) ||
        LocalInstall.Within(source, Path.Combine(install.Mods, SyncedStore.FolderName)) ||
        LocalInstall.Within(source, install.Temp);

    // The previous session's staged files stay where they are what this one stages (same name, and the same size and
    // time as the file they come from: a copy keeps its source's time, a link is its source): a session start copied and
    // deleted every patch again, ~3 GB for a tester's packs, and kept the drive busy for minutes. Only new or changed
    // patches are placed; those no longer staged are removed. The journal is written before the first change and every
    // file goes through a .staging name, so Unstage can remove exactly this session's work even after a kill.
    // own: folders of HellForge's own patch files, staged after the mods'.
    public static IReadOnlyList<string> Stage(InstallLayout install, IReadOnlyList<ModManifest> mods, IReadOnlyList<string>? own = null)
    {
        var data = Path.Combine(install.Game, "data");
        RemoveLive(install, data);
        var previous = Previous(install);
        var placed = Placed(install);
        var plan = Plan(mods, own ?? [], data, previous);
        var wanted = plan.Select(p => p.Target).ToHashSet(StringComparer.Ordinal);
        var left = new HashSet<string>(previous, StringComparer.Ordinal); /* staged here before and still in game/data */
        var staged = new List<string>();
        var counts = new Dictionary<Placement, int>();
        Directory.CreateDirectory(install.Temp);
        var journal = new HashSet<string>(previous, StringComparer.Ordinal);
        journal.UnionWith(wanted);
        WriteRecord(install, journal);
        var completed = false;
        try
        {
            Directory.CreateDirectory(data);
            foreach (var name in previous.Where(n => !wanted.Contains(n)))
            {
                DeleteStaged(Path.Combine(data, name));
                left.Remove(name);
            }
            foreach (var (source, target) in plan)
            {
                var how = Place(install, source, Path.Combine(data, target), left.Remove(target), placed.Contains(target));
                counts[how] = counts.GetValueOrDefault(how) + 1;
                staged.Add(target);
            }
            completed = true;
        }
        finally
        {
            if (completed)
            {
                WriteRecord(install, staged);
                WriteNames(Path.Combine(install.Temp, PlacedRecordName), staged);
            }
            LastLinked = counts.GetValueOrDefault(Placement.Linked);
            LastCounts = (LastLinked + counts.GetValueOrDefault(Placement.Copied), counts.GetValueOrDefault(Placement.Kept));
        }
        return staged;
    }

    enum Placement { Kept, Copied, Linked }

    static void DeleteStaged(string path)
    {
        if (File.Exists(path)) File.Delete(path);
        if (File.Exists(path + ".staging")) File.Delete(path + ".staging");
    }

    // One patch at path: kept when the last session staged it from this same file (and this version placed it, or it may
    // not be a link anyway), else linked or copied through its .staging name. wasStaged: the last session staged it here.
    static Placement Place(InstallLayout install, string source, string path, bool wasStaged, bool placedBefore)
    {
        var staging = path + ".staging";
        if (File.Exists(staging)) File.Delete(staging);
        var linkable = Linkable(install, source);
        if (wasStaged && Same(source, path) && (placedBefore || !linkable)) return Placement.Kept;
        var linked = FileLinks.LinkOrCopy(source, staging, linkable);
        if (new FileInfo(staging).Length != new FileInfo(source).Length)
            throw new InvalidDataException("A staged mod file was not copied whole: " + Path.GetFileName(path));
        File.Move(staging, path, overwrite: true);
        return linked ? Placement.Linked : Placement.Copied;
    }

    // Each mod's patch files (its data/, its pack's chosen folders, its generated data) with the names they are staged
    // under: each bundle's patches renumbered after the ones already in game/data, in load order.
    static List<(string Source, string Target)> Plan(IReadOnlyList<ModManifest> mods, IReadOnlyList<string> own, string data, HashSet<string> previous)
    {
        var next = new Dictionary<string, int>(StringComparer.Ordinal);
        int NextIndex(string bundle)
        {
            if (!next.TryGetValue(bundle, out var index))
            {
                index = 0;
                /* after the bundle's own patches (the game's or the player's); the last session's staged ones do not count */
                while (!previous.Contains($"{bundle}.patch_{index}") && File.Exists(Path.Combine(data, $"{bundle}.patch_{index}"))) index++;
            }
            next[bundle] = index + 1;
            return index;
        }
        var plan = new List<(string Source, string Target)>();
        void Add(string folder, IEnumerable<string> files)
        {
            foreach (var group in files.GroupBy(f => (Bundle: f[..16], Index: int.Parse(ModCatalog.PatchFile().Match(f).Groups[1].Value, CultureInfo.InvariantCulture)))
                .OrderBy(g => g.Key.Bundle, StringComparer.Ordinal).ThenBy(g => g.Key.Index))
            {
                var index = NextIndex(group.Key.Bundle);
                foreach (var file in group.Order(StringComparer.Ordinal))
                    plan.Add((Path.Combine(folder, file), $"{group.Key.Bundle}.patch_{index}{ModCatalog.PatchFile().Match(file).Groups[2].Value}"));
            }
        }
        foreach (var mod in mods)
        {
            if (mod.HasDataSpec && mod.GeneratedData == null) throw new InvalidDataException($"{mod.Id}: its data has not been generated.");
            foreach (var (folder, files) in Sources(mod)) Add(folder, files);
        }
        foreach (var folder in own) Add(folder, PatchFiles(folder));
        return plan;
    }

    // A mod's folders of patches and their files: its data/, its pack's chosen folders, its generated data.
    static List<(string Folder, IEnumerable<string> Files)> Sources(ModManifest mod)
    {
        var sources = new List<(string Folder, IEnumerable<string> Files)> { (Path.Combine(mod.Folder, "data"), mod.DataFiles) };
        foreach (var folder in mod.PackFolders) sources.Add((folder, CosmeticPacks.PatchFiles(folder)));
        if (mod.GeneratedData != null) sources.Add((mod.GeneratedData, PatchFiles(mod.GeneratedData)));
        return sources;
    }

    // The last session's staged files (patch file names only), for the data audit and Unstage.
    public static IReadOnlyCollection<string> Recorded(InstallLayout install) => Previous(install);

    static IEnumerable<string> PatchFiles(string folder) =>
        Directory.GetFiles(folder).Select(f => Path.GetFileName(f)).Where(f => ModCatalog.PatchFile().IsMatch(f));

    // The last session's staged files (patch file names only).
    static HashSet<string> Previous(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, RecordName);
        try
        {
            return File.Exists(record)
                ? (JsonSerializer.Deserialize<List<string>>(File.ReadAllText(record)) ?? []).Where(n => ModCatalog.PatchFile().IsMatch(n)).ToHashSet(StringComparer.Ordinal)
                : new HashSet<string>(StringComparer.Ordinal);
        }
        catch (JsonException) { return new HashSet<string>(StringComparer.Ordinal); }
    }

    // The names the last completed Stage placed or kept (PlacedRecordName); none before this version.
    static HashSet<string> Placed(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, PlacedRecordName);
        try
        {
            return File.Exists(record)
                ? (JsonSerializer.Deserialize<List<string>>(File.ReadAllText(record)) ?? []).ToHashSet(StringComparer.Ordinal)
                : new HashSet<string>(StringComparer.Ordinal);
        }
        catch (Exception error) when (error is JsonException or IOException) { return new HashSet<string>(StringComparer.Ordinal); }
    }

    // The record itself is replaced whole: a kill while writing it leaves the previous journal, not half a name list.
    static void WriteRecord(InstallLayout install, IEnumerable<string> names) => WriteNames(Path.Combine(install.Temp, RecordName), names);

    static void WriteNames(string record, IEnumerable<string> names)
    {
        var partial = record + ".partial";
        var entries = names.Where(name => ModCatalog.PatchFile().IsMatch(name)).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToList();
        File.WriteAllText(partial, JsonSerializer.Serialize(entries));
        File.Move(partial, record, overwrite: true);
    }

    // A staged file still is its source: the same size and last write time (a copy keeps its source's time). The staged
    // name is only ever reached through a rename, so a match cannot be a partially written file.
    static bool Same(string source, string staged)
    {
        var from = new FileInfo(source);
        var to = new FileInfo(staged);
        return to.Exists && from.Exists && to.Length == from.Length && to.LastWriteTimeUtc == from.LastWriteTimeUtc;
    }

    public static void Unstage(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, RecordName);
        if (!File.Exists(record)) return;
        var data = Path.Combine(install.Game, "data");
        foreach (var name in Previous(install))
            foreach (var path in new[] { Path.Combine(data, name), Path.Combine(data, name + ".staging") })
                if (File.Exists(path)) File.Delete(path);
        File.Delete(record);
        var partial = record + ".partial";
        if (File.Exists(partial)) File.Delete(partial);
        var placed = Path.Combine(install.Temp, PlacedRecordName);
        if (File.Exists(placed)) File.Delete(placed);
    }
}
