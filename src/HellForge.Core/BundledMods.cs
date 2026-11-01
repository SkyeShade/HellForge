using HellForge.Core.GameData;

namespace HellForge.Core;

// Mods that ship with HellForge, built (the in-game editor, the Hell Forge difficulty, Vanilla Plus): <installation>/app/
// bundled-mods/<folder> (scripts/Publish-HellForge.ps1), copied into <installation>/mods/<folder> when the launcher starts,
// when its setup has copied the game, and before each session, when one is missing or differs, so a new HellForge (an
// update with the setup) brings its new version. Only the shipped files are replaced, and the ones an older version
// shipped and this one does not are removed (.hellforge-bundled.txt lists them): what a mod keeps in its folder (the
// in-game editor's settings, presets and mod-stats.json) stays. Switching one off on the Mods page keeps it off.
// Other copies of a bundled mod (the same mod id: one downloaded before it was bundled, in a folder of another name or
// a zip) are moved to <installation>/mods-retired, keeping what the player saved in them, so the bundled one is the only
// copy. A copy with sources of its own (src/: a modder working on the example) of the same version or newer is left as
// it is, and the bundled one is not installed beside it; an older one is retired like the others.
// The in-game editor's catalogue, icons and fonts are made here, the icons and fonts from the installation's own game
// copy: they are the player's game files and are never part of what HellForge ships.
public static class BundledMods
{
    public const string FolderName = "bundled-mods";
    public const string EditorFolder = "hellforge-editor";
    public const string RetiredFolder = "mods-retired";
    const string ListName = ".hellforge-bundled.txt"; /* the files the copy in mods got from the app, one per line */
    // What a bundled mod keeps for its player (relative paths; a folder ends in '/'): carried into the bundled copy from
    // one that is retired.
    static readonly Dictionary<string, string[]> Kept = new(StringComparer.OrdinalIgnoreCase)
    {
        [EditorFolder] = ["editor-settings.json", "mod-stats.json", "presets/"],
    };

    public static string Folder(InstallLayout install) => Path.Combine(install.App, FolderName);

    // A mod's id and version from its manifest (a folder's, or the one a zip holds at its root or one folder down), or null.
    sealed record Identity(string Id, Version Version, string Text);

    static Identity? IdentityOf(string json)
    {
        try
        {
            using var document = System.Text.Json.JsonDocument.Parse(json);
            var root = document.RootElement;
            if (root.ValueKind != System.Text.Json.JsonValueKind.Object || !root.TryGetProperty("id", out var id) || id.GetString() is not { Length: > 0 } text) return null;
            var version = root.TryGetProperty("version", out var v) && v.GetString() is { } raw ? raw : "0";
            return new(text, ParseVersion(version), version);
        }
        catch (Exception error) when (error is System.Text.Json.JsonException or InvalidOperationException) { return null; }
    }

    static Version ParseVersion(string text)
    {
        var parts = text.Split('-', '+')[0].Split('.').Select(p => int.TryParse(p, out var n) && n >= 0 ? n : 0).Concat([0, 0, 0]).Take(4).ToArray();
        return new Version(parts[0], parts[1], parts[2], parts[3]);
    }

    static Identity? FolderIdentity(string folder) =>
        ModCatalog.ManifestIn(folder) is { } manifest ? IdentityOf(File.ReadAllText(manifest)) : null;

    static Identity? ZipIdentity(string archive)
    {
        try
        {
            using var zip = System.IO.Compression.ZipFile.OpenRead(archive);
            var entry = zip.Entries.Where(e => e.Name == ModCatalog.ManifestName && e.FullName.Replace('\\', '/').Count(c => c == '/') <= 1)
                .OrderBy(e => e.FullName.Length).FirstOrDefault();
            if (entry == null) return null;
            using var reader = new StreamReader(entry.Open());
            return IdentityOf(reader.ReadToEnd());
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return null; }
    }

    // The problems (a bundled mod that could not be put in place); the session still runs without it.
    public static List<string> Sync(InstallLayout install, Action<string>? log = null)
    {
        var problems = new List<string>();
        var source = Folder(install);
        if (!Directory.Exists(source)) return problems;
        foreach (var shipped in Directory.GetDirectories(source).Order(StringComparer.OrdinalIgnoreCase))
        {
            var name = Path.GetFileName(shipped);
            var target = Path.Combine(install.Mods, name);
            try { SyncOne(install, shipped, name, target, log); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                problems.Add($"{name} (bundled with HellForge) could not be installed: {error.Message}");
            }
        }
        return problems;
    }

    // One bundled mod into mods/<name>, unless a copy with sources of its own keeps its place.
    static void SyncOne(InstallLayout install, string shipped, string name, string target, Action<string>? log)
    {
        var bundled = FolderIdentity(shipped);
        if (bundled != null && !Settle(install, name, target, bundled, log)) return;
        if (Directory.Exists(Path.Combine(target, "src")))
        {
            log?.Invoke($"Bundled mod {name}: mods/{name} has sources of its own, so HellForge's built copy is not installed over it.");
            return;
        }
        var files = Directory.GetFiles(shipped, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(shipped, f).Replace('\\', '/')).ToList();
        var changed = CopyShipped(shipped, target, files);
        // the files an earlier version shipped and this one does not
        var list = Path.Combine(target, ListName);
        var before = File.Exists(list) ? File.ReadAllLines(list).Where(l => l.Length > 0).ToList() : [];
        if (DeleteStale(target, before.Except(files, StringComparer.OrdinalIgnoreCase))) changed = true;
        if (!before.SequenceEqual(files, StringComparer.OrdinalIgnoreCase))
            File.WriteAllLines(list, files);
        if (changed) log?.Invoke($"Bundled mod {name}: installed from this HellForge version.");
        if (name.Equals(EditorFolder, StringComparison.OrdinalIgnoreCase)) EditorData(install, target, changed, log);
    }

    // The shipped files that are missing or differ in target, copied: true when any was.
    static bool CopyShipped(string shipped, string target, List<string> files)
    {
        var changed = false;
        foreach (var relative in files)
        {
            var file = Path.Combine(shipped, relative);
            var destination = Path.Combine(target, relative);
            if (File.Exists(destination) && Same(file, destination)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(file, destination, overwrite: true);
            changed = true;
        }
        return changed;
    }

    // Files listed as shipped before that are still inside target, deleted: true when any was.
    static bool DeleteStale(string target, IEnumerable<string> stale)
    {
        var changed = false;
        foreach (var relative in stale)
        {
            var path = Path.GetFullPath(Path.Combine(target, relative));
            if (!path.StartsWith(Path.GetFullPath(target) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(path)) continue;
            File.Delete(path);
            changed = true;
        }
        return changed;
    }

    // Every other copy of a bundled mod (its id) in mods: retired, unless one is a modder's (sources of its own) of the
    // same version or newer, which keeps its place: false, the bundled one is not installed. mods/<name> itself is
    // retired when it holds an older modder's copy or another mod.
    static bool Settle(InstallLayout install, string name, string target, Identity bundled, Action<string>? log)
    {
        var copies = Copies(install);
        var own = copies.FirstOrDefault(c => !c.Archive && Path.GetFullPath(c.Path).Equals(Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase));
        var keeper = copies.FirstOrDefault(c => c.Id.Id == bundled.Id && c.Source && c.Id.Version >= bundled.Version);
        if (keeper.Path != null)
        {
            log?.Invoke($"Bundled mod {name}: {Path.GetFileName(keeper.Path)} is a copy with sources of its own (version {keeper.Id.Text}), so HellForge's built copy is not installed beside it.");
            if (own.Path != null && own.Path != keeper.Path && own.Id.Id == bundled.Id) Retire(install, name, target, own.Path, false, own.Id, log); /* a built copy from before */
            return false;
        }
        foreach (var copy in copies.Where(c => c.Id.Id == bundled.Id && c.Path != own.Path))
            Retire(install, name, target, copy.Path, copy.Archive, copy.Id, log);
        // mods/<name>: an older modder's copy of this mod, or another mod in the folder this one ships to
        if (own.Path != null && (own.Id.Id != bundled.Id || own.Source)) Retire(install, name, target, own.Path, false, own.Id, log);
        return true;
    }

    // The mods in mods (folders, not hidden ones, and zips) with an identity.
    static List<(string Path, bool Archive, Identity Id, bool Source)> Copies(InstallLayout install)
    {
        var copies = new List<(string Path, bool Archive, Identity Id, bool Source)>();
        if (!Directory.Exists(install.Mods)) return copies;
        foreach (var folder in Directory.GetDirectories(install.Mods).Where(f => !Path.GetFileName(f).StartsWith('.')))
            if (FolderIdentity(folder) is { } id) copies.Add((folder, false, id, Directory.Exists(Path.Combine(folder, "src"))));
        foreach (var archive in Directory.GetFiles(install.Mods, "*.zip"))
            if (ZipIdentity(archive) is { } id) copies.Add((archive, true, id, false)); /* a zip's sources are its author's; HellForge's build replaces it */
        return copies;
    }

    static void Retire(InstallLayout install, string name, string target, string path, bool archive, Identity id, Action<string>? log)
    {
        var retired = Directory.CreateDirectory(Path.Combine(install.Root, RetiredFolder)).FullName;
        var label = (archive ? Path.GetFileNameWithoutExtension(path) : Path.GetFileName(path)) + "-" + id.Text;
        var destination = Path.Combine(retired, archive ? label + ".zip" : label);
        for (var n = 2; File.Exists(destination) || Directory.Exists(destination); n++)
            destination = Path.Combine(retired, archive ? $"{label}-{n}.zip" : $"{label}-{n}");
        // what the player saved in a copy of this mod comes along to the bundled one (a zip holds none); the bundled
        // copy's own saved files win
        var hold = archive ? null : HoldKept(install, name, path, id);
        if (archive) File.Move(path, destination);
        else Directory.Move(path, destination);
        if (hold != null && Directory.Exists(hold)) { CopyTree(hold, target, overwrite: false); Directory.Delete(hold, recursive: true); }
        log?.Invoke($"Bundled mod {name}: the other copy {Path.GetFileName(path)} (version {id.Text}) was moved to {RetiredFolder}/{Path.GetFileName(destination)}; HellForge's built copy replaces it.");
    }

    // A folder copy's kept files, held in a temporary folder until the bundled copy is in place: its path, or null when
    // the copy is another mod or the mod keeps nothing.
    static string? HoldKept(InstallLayout install, string name, string path, Identity id)
    {
        if (id.Id != FolderIdentity(Path.Combine(Folder(install), name))?.Id || !Kept.TryGetValue(name, out var kept)) return null;
        var hold = Path.Combine(install.Temp, "bundled-keep-" + Guid.NewGuid().ToString("N")[..8]);
        foreach (var keep in kept)
        {
            var from = Path.Combine(path, keep);
            if (keep.EndsWith('/') && Directory.Exists(from)) CopyTree(from, Path.Combine(hold, keep), overwrite: true);
            else if (File.Exists(from)) { Directory.CreateDirectory(hold); File.Copy(from, Path.Combine(hold, keep)); }
        }
        return hold;
    }

    static void CopyTree(string from, string to, bool overwrite)
    {
        foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
        {
            var into = Path.Combine(to, Path.GetRelativePath(from, file));
            if (!overwrite && File.Exists(into)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(into)!);
            File.Copy(file, into, overwrite);
        }
    }

    // The catalogue follows the app: written whenever its text is not this HellForge's (a catalogue change ships with an
    // unchanged editor too). Icons and fonts come from the game copy: made when missing, when the editor changed, and when
    // the sprites the app names or the icon file's format did (IconStampFile holds EditorIcons.Stamp).
    public const string IconStampFile = "game-icons.stamp";
    static void EditorData(InstallLayout install, string folder, bool changed, Action<string>? log)
    {
        var catalog = Path.Combine(folder, EditorCatalog.FileName);
        var text = EditorCatalog.Text();
        if (!File.Exists(catalog) || File.ReadAllText(catalog) != text)
        {
            File.WriteAllText(catalog + ".partial", text);
            File.Move(catalog + ".partial", catalog, overwrite: true);
        }
        // Only from a complete copy: the launcher syncs at start, which can be before or during setup's copy.
        var gameData = Path.Combine(install.Game, "data");
        var icons = Path.Combine(folder, EditorIcons.FileName);
        var stamp = Path.Combine(folder, IconStampFile);
        var current = File.Exists(icons) && !changed && File.Exists(stamp) && File.ReadAllText(stamp) == EditorIcons.Stamp;
        if (current || !SupportedBuild.Current.IsComplete(install.Game)) return;
        var (count, missing) = EditorIcons.Write(gameData, icons);
        var fonts = EditorFonts.Write(gameData, folder);
        File.WriteAllText(stamp, EditorIcons.Stamp);
        log?.Invoke($"In-game editor: {count} icons ({missing.Count} missing) and {fonts.Count} fonts from this installation's game copy.");
    }

    // The files HellForge ships for the bundled mod in modFolder (relative, '/'), or null when it is not one (no
    // app/bundled-mods/<folder> in its installation, or a modder's copy with src/). Friends compare these
    // (ModCatalog.Digest), not what each player's copy made or saved there: the editor's settings, presets, icons, fonts.
    public static HashSet<string>? ShippedFiles(string modFolder)
    {
        modFolder = Path.GetFullPath(modFolder);
        if (Directory.Exists(Path.Combine(modFolder, "src")) || Path.GetDirectoryName(Path.GetDirectoryName(modFolder)) is not { } root) return null;
        var shipped = Path.Combine(root, "app", FolderName, Path.GetFileName(modFolder));
        return Directory.Exists(shipped)
            ? Directory.GetFiles(shipped, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(shipped, f).Replace('\\', '/')).ToHashSet(StringComparer.Ordinal)
            : null;
    }

    static bool Same(string a, string b)
    {
        var x = new FileInfo(a);
        var y = new FileInfo(b);
        if (x.Length != y.Length) return false;
        using var first = x.OpenRead();
        using var second = y.OpenRead();
        return System.Security.Cryptography.SHA256.HashData(first).AsSpan().SequenceEqual(System.Security.Cryptography.SHA256.HashData(second));
    }
}
