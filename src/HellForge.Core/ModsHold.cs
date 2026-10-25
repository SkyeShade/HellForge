using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// A session's hold on the player's mods folder (lockdown item 4): every file is held read-only, so nothing the game runs
// can change a mod that loads as the player's own next time, except the files HellForge itself writes while a session
// runs (Writable). Check, after Release, lists what appeared during the session and switches off new mod folders until
// the player turns them on again. Outside the low-integrity sandbox this only slows code in the game down (it runs as
// the player and can close the hold's handles); the sandbox is what stops it.
public sealed class ModsHold : IDisposable
{
    readonly string root;
    readonly HashSet<string> snapshot = new(StringComparer.OrdinalIgnoreCase);
    readonly List<FileStream> locks = [];

    public int Held => locks.Count;

    ModsHold(string modsFolder) => root = Path.GetFullPath(modsFolder);

    // Holds every file but the writable ones; a file that cannot be held throws IOException naming it.
    public static ModsHold Take(string modsFolder)
    {
        var hold = new ModsHold(modsFolder);
        if (!Directory.Exists(hold.root)) return hold;
        try
        {
            foreach (var dir in Directory.EnumerateDirectories(hold.root, "*", SearchOption.AllDirectories))
                hold.snapshot.Add(Path.GetFullPath(dir));
            foreach (var file in Directory.EnumerateFiles(hold.root, "*", SearchOption.AllDirectories))
            {
                var full = Path.GetFullPath(file);
                hold.snapshot.Add(full);
                LocalInstall.NoLinks(full);
                if (Writable(hold.Relative(full))) continue;
                try { hold.locks.Add(new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read)); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    throw new IOException($"Mod file {hold.Relative(full)} could not be held read-only for the session ({error.Message}). Close what has it open and start again.", error);
                }
            }
        }
        catch
        {
            hold.Dispose();
            throw;
        }
        return hold;
    }

    // What HellForge writes into mods/ during a session: the mod settings (mod sync's "block"), the Local Stats file and
    // the editor lock (the in-game editor and the launcher's STATS page), the in-game editor's own files (its settings,
    // mod content values, presets, the host's files the backend puts beside it), the synced store's downloads, and the
    // temporary files each writes first. A Local Stats manifest written during the session is checked by Check.
    public static bool Writable(string relative)
    {
        var path = relative.Replace('\\', '/');
        var name = Path.GetFileName(path);
        var bare = Bare(name);
        var folder = Path.GetDirectoryName(path)?.Replace('\\', '/') ?? "";
        if (folder.Length == 0) return bare is ModSettings.FileName or ModSettings.LegacyFileName or ".hellforge-stats-editor.lock"; // ModSettings deletes the legacy file
        if (folder.Equals(ModCatalog.LocalStatsId, StringComparison.OrdinalIgnoreCase)) return bare == StatsData.FileName;
        if (folder.Equals(BundledMods.EditorFolder, StringComparison.OrdinalIgnoreCase))
            return bare is "editor-settings.json" or "mod-stats.json" or "host-stats.json" or "host-mod-stats.json" or "host-mounts.json";
        if (folder.Equals(BundledMods.EditorFolder + "/presets", StringComparison.OrdinalIgnoreCase))
            return bare.EndsWith(".json", StringComparison.OrdinalIgnoreCase);
        return path.StartsWith(SyncedStore.FolderName + "/", StringComparison.OrdinalIgnoreCase);
    }

    // A file's name without the suffix of the temporary file it was written as.
    static string Bare(string name)
    {
        foreach (var suffix in new[] { ".ingame.tmp", ".partial", ".tmp" })
            if (name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
            {
                var bare = name[..^suffix.Length];
                // StatsEditorData writes <file>.<32 hex digits>.tmp
                var dot = bare.LastIndexOf('.');
                if (suffix == ".tmp" && dot > 0 && bare.Length - dot - 1 == 32 && bare[(dot + 1)..].All(Uri.IsHexDigit)) bare = bare[..dot];
                return bare;
            }
        return name;
    }

    public void Release()
    {
        foreach (var held in locks) held.Dispose();
        locks.Clear();
    }

    public void Dispose() => Release();

    public sealed record Planted(List<string> Files, List<string> DisabledMods);

    // After Release: the files and folders that appeared during the session, but for HellForge's own (Writable, and a
    // Local Stats folder made by its editor), with every mod they appeared in switched off (a new folder, or a file added
    // to a mod's folder, such as a data patch it would load). Null when nothing appeared.
    public Planted? Check(Action<string> say)
    {
        if (!Directory.Exists(root)) return null;
        var files = new List<string>();
        var touched = new HashSet<string>(StringComparer.OrdinalIgnoreCase); // the top-level folders things appeared in
        foreach (var dir in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
        {
            var full = Path.GetFullPath(dir);
            var relative = Relative(full);
            if (snapshot.Contains(full) || relative.StartsWith(SyncedStore.FolderName + "/", StringComparison.OrdinalIgnoreCase) ||
                relative.Equals(SyncedStore.FolderName, StringComparison.OrdinalIgnoreCase)) continue;
            if (relative.Equals(ModCatalog.LocalStatsId, StringComparison.OrdinalIgnoreCase) && EditorMadeLocalStats(full)) continue;
            files.Add(relative + "/");
            touched.Add(TopFolder(relative));
        }
        foreach (var file in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            var full = Path.GetFullPath(file);
            var relative = Relative(full);
            if (snapshot.Contains(full) || Writable(relative)) continue;
            if (relative.Equals(ModCatalog.LocalStatsId + "/" + ModCatalog.ManifestName, StringComparison.OrdinalIgnoreCase) &&
                EditorMadeLocalStats(Path.GetDirectoryName(full)!)) continue;
            files.Add(relative);
            if (relative.Contains('/')) touched.Add(TopFolder(relative));
        }
        if (files.Count == 0) return null;
        var disabled = new List<string>();
        if (touched.Count > 0)
            try
            {
                var (mods, _) = ModCatalog.Load(root);
                foreach (var mod in mods.Where(m => m.Enabled && touched.Contains(Path.GetFullPath(m.Folder).TrimEnd('\\', '/'))))
                {
                    ModSettings.SetEnabled(root, mod.Id, false);
                    disabled.Add(mod.Id);
                }
            }
            catch (Exception error) when (error is IOException or JsonException or InvalidDataException or UnauthorizedAccessException)
            {
                say("The mods files appeared in could not be switched off: " + error.Message);
            }
        return new(files, disabled);
    }

    string TopFolder(string relative) => Path.GetFullPath(Path.Combine(root, relative.Split('/')[0]));

    // A Local Stats folder the editor made: its manifest is the editor's own (no code, no native edits) and it holds only
    // that manifest and the stats file.
    static bool EditorMadeLocalStats(string folder)
    {
        string[] allowed = [ModCatalog.ManifestName, StatsData.FileName];
        if (Directory.EnumerateDirectories(folder).Any() ||
            Directory.EnumerateFiles(folder).Any(f => !allowed.Contains(Path.GetFileName(f), StringComparer.OrdinalIgnoreCase) && !Writable(ModCatalog.LocalStatsId + "/" + Path.GetFileName(f))))
            return false;
        try
        {
            var manifest = JsonNode.Parse(File.ReadAllText(Path.Combine(folder, ModCatalog.ManifestName))) as JsonObject;
            string[] keys = ["format", "id", "name", "version", "description", "gameplay", "editor"];
            return manifest != null && manifest.All(p => keys.Contains(p.Key)) &&
                (string?)manifest["id"] == ModCatalog.LocalStatsId && (string?)manifest["editor"] == StatsEditorData.Marker;
        }
        catch (Exception error) when (error is IOException or JsonException or InvalidOperationException or UnauthorizedAccessException) { return false; }
    }

    string Relative(string full) => Path.GetRelativePath(root, full).Replace('\\', '/');
}
