using System.Globalization;
using System.Text.Json;

namespace HellForge.Core;

// Data mods for one session: their patch files are copied into game/data, renumbered so each bundle's patches stay
// contiguous after any the player installed there by hand, and listed in temp/staged-mods.json so the next session
// removes exactly them first. Game files from Steam are never touched. A mod's generated data (ModDataCache.Prepare) is
// staged after its data/ folder, the same way, and HellForge's own patches for the session (the locked warbond art,
// WarbondLocks) after every mod's.
public static class ModStaging
{
    const string RecordName = "staged-mods.json";

    // What the last Stage did: files copied, and files kept from the session before (the same source, unchanged).
    public static (int Copied, int Kept) LastCounts { get; private set; }

    // The previous session's staged files stay where they are what this one stages (same name, and the same size and
    // time as the file they come from: a copy keeps its source's time): a session start copied and deleted every patch
    // again, ~3 GB for a tester's packs, and kept the drive busy for minutes. Only new or changed patches are copied;
    // those no longer staged are removed. The journal is written before the first change and every file goes through a
    // .staging name, so Unstage can remove exactly this session's work even after a kill.
    // own: folders of HellForge's own patch files, staged after the mods'.
    public static IReadOnlyList<string> Stage(InstallLayout install, IReadOnlyList<ModManifest> mods, IReadOnlyList<string>? own = null)
    {
        var data = Path.Combine(install.Game, "data");
        var previous = Previous(install);
        var plan = Plan(mods, own ?? [], data, previous);
        var wanted = plan.Select(p => p.Target).ToHashSet(StringComparer.Ordinal);
        var left = new HashSet<string>(previous, StringComparer.Ordinal); /* staged here before and still in game/data */
        var staged = new List<string>();
        int copied = 0, kept = 0;
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
                var path = Path.Combine(data, name);
                if (File.Exists(path)) File.Delete(path);
                if (File.Exists(path + ".staging")) File.Delete(path + ".staging");
                left.Remove(name);
            }
            foreach (var (source, target) in plan)
            {
                var path = Path.Combine(data, target);
                var staging = path + ".staging";
                if (left.Contains(target) && Same(source, path))
                {
                    if (File.Exists(staging)) File.Delete(staging);
                    staged.Add(target);
                    left.Remove(target);
                    kept++;
                    continue;
                }
                left.Remove(target);
                File.Copy(source, staging, overwrite: true);
                if (new FileInfo(staging).Length != new FileInfo(source).Length)
                    throw new InvalidDataException("A staged mod file was not copied whole: " + target);
                File.Move(staging, path, overwrite: true);
                staged.Add(target);
                copied++;
            }
            completed = true;
        }
        finally
        {
            if (completed) WriteRecord(install, staged);
            LastCounts = (copied, kept);
        }
        return staged;
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

    // The record itself is replaced whole: a kill while writing it leaves the previous journal, not half a name list.
    static void WriteRecord(InstallLayout install, IEnumerable<string> names)
    {
        var record = Path.Combine(install.Temp, RecordName);
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
    }
}
