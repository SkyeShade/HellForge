using System.IO.Compression;

namespace HellForge.Core;

// Where the mod is in what a player downloaded: a zip as mod sites ship it ("ModName v1.2.zip" holding "ModName v1.2/
// ModName/hellforge-mod.json" and a README), a zip holding only another zip, or a folder in mods/ around the mod's folder.
// One rule for zips, their unpacked copies and the mods folder's folders (docs/internals.md, "The mods folder"):
// 1. A level with a HellForge manifest, or a cosmetic pack, is a mod: nothing under it is looked at.
// 2. Else its zips and the mods in its folders are found, when it holds nothing else but readme files (Loose). A level
//    with any other file is something else, such as a mod that lost its manifest, and its folders are not looked at.
// 3. MaxDepth levels down at most, and dot folders never: a large unrelated archive is not walked whole.
// A zip must hold one mod (ModArchives refuses more), and MaxInnerZips zips inside it are opened (ModArchives.OpenInner);
// a folder in mods/ may hold several mods, and a zip in it is not loaded.
public static class ModSources
{
    public const int MaxDepth = 4;
    public const int MaxInnerZips = 2;

    // What the rule found: a mod's folder, or a zip that may hold one, by its path in the source ('/'; "" for its root).
    public sealed record Found(string Path, bool Zip);

    // A place in a source: an unpacked folder, or a folder of a zip's entries (ZipLevel).
    internal interface ILevel
    {
        string Path { get; }
        IReadOnlyList<string> Files { get; }
        IReadOnlyList<string> Folders { get; }
        bool IsMod { get; }
        ILevel Child(string name);
    }

    public static List<Found> Search(string folder) => Search(new FolderLevel(folder, ""), 0);

    internal static List<Found> Search(ILevel level, int depth)
    {
        if (level.IsMod) return [new(level.Path, false)];
        var files = level.Files;
        if (depth >= MaxDepth || files.Any(f => !Loose(f) && !IsZip(f))) return [];
        var found = files.Where(IsZip).Order(StringComparer.OrdinalIgnoreCase).Select(f => new Found(Join(level.Path, f), true)).ToList();
        foreach (var folder in level.Folders.Where(f => !Hidden(f)).Order(StringComparer.OrdinalIgnoreCase))
            found.AddRange(Search(level.Child(folder), depth + 1));
        return found;
    }

    // The folders a source wraps the mod in: its root, then each level's one folder (Hidden ones aside), MaxDepth deep.
    // Where a known kind of not-a-mod is looked for (ModArchives.NotAModReason).
    public static IEnumerable<string> Wrappers(string folder)
    {
        for (var depth = 0; depth <= MaxDepth; depth++)
        {
            yield return folder;
            var folders = Directory.GetDirectories(folder).Where(f => !Hidden(Path.GetFileName(f))).ToList();
            if (folders.Count != 1) yield break;
            folder = folders[0];
        }
    }

    // The mod folders of a mods folder: each folder there that is a mod, wraps one or holds several (Search), and a line
    // for each folder that holds only a zip (the zips the mods folder loads are its own: ModArchives).
    public static (List<string> Folders, List<string> Problems) InModsFolder(string modsFolder)
    {
        var folders = new List<string>();
        var problems = new List<string>();
        if (!Directory.Exists(modsFolder)) return (folders, problems);
        foreach (var top in Directory.GetDirectories(modsFolder).Where(d => !Hidden(Path.GetFileName(d))).Order(StringComparer.OrdinalIgnoreCase))
        {
            List<Found> found;
            try { found = Search(top); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { continue; }
            folders.AddRange(found.Where(f => !f.Zip).Select(f => PathIn(top, f.Path)));
            if (found is [{ Zip: true } zip])
                problems.Add($"{Path.GetFileName(top)}: it holds {zip.Path} and no mod. A zip loads from the mods folder itself: move it there.");
        }
        return (folders, problems);
    }

    // A path Search found, in the folder it searched.
    public static string PathIn(string folder, string path) =>
        path.Length == 0 ? folder : Path.Combine(folder, path.Replace('/', Path.DirectorySeparatorChar));

    // Why a source with several mods is refused: one zip is one mod, which friends and mod sync compare by its files.
    public static string SeveralReason(IReadOnlyList<Found> found) =>
        $"it holds {found.Count} mods ({string.Join(", ", found.Select(f => f.Path))}). A zip holds one mod, so friends compare the same " +
        "files: unpack it and add each mod's folder or zip on its own.";

    public static string NoModReason =>
        $"no {ModCatalog.ManifestName} (or a cosmetic pack's {CosmeticPacks.ManifestName}). HellForge looks at the archive's root, then in " +
        $"its folders up to {MaxDepth} levels down, where nothing but readme files is beside them, and in up to {MaxInnerZips} zips inside it.";

    // Files a download keeps beside a mod: readmes, licences, pictures, links. Anything else makes a level a dead end.
    internal static bool Loose(string name)
    {
        if (name.StartsWith('.') || name.Equals("Thumbs.db", StringComparison.OrdinalIgnoreCase) || name.Equals("desktop.ini", StringComparison.OrdinalIgnoreCase)) return true;
        var extension = Path.GetExtension(name).ToLowerInvariant();
        if (extension.Length == 0)
            return Path.GetFileName(name).ToLowerInvariant() is "readme" or "license" or "licence" or "copying" or "changelog" or "authors" or "credits" or "notice";
        return extension is ".txt" or ".md" or ".markdown" or ".pdf" or ".rtf" or ".nfo" or ".htm" or ".html" or ".url" or
            ".png" or ".jpg" or ".jpeg" or ".gif" or ".webp" or ".bmp" or ".ico";
    }

    internal static bool IsZip(string name) => name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase);

    // A folder no mod is in: a dot folder (.unpacked, .synced, an editor's), or the one macOS adds to zips.
    internal static bool Hidden(string name) => name.StartsWith('.') || name.Equals("__MACOSX", StringComparison.OrdinalIgnoreCase);

    static string Join(string path, string name) => path.Length == 0 ? name : path + "/" + name;

    sealed class FolderLevel(string folder, string path) : ILevel
    {
        public string Path => path;
        public IReadOnlyList<string> Files => [.. Directory.GetFiles(folder).Select(f => System.IO.Path.GetFileName(f))];
        public IReadOnlyList<string> Folders => [.. Directory.GetDirectories(folder).Select(f => System.IO.Path.GetFileName(f))];
        public bool IsMod => ModCatalog.ManifestIn(folder) != null || CosmeticPacks.IsPack(folder);
        public ILevel Child(string name) => new FolderLevel(System.IO.Path.Combine(folder, name), Join(path, name));
    }

    // The mod a zip holds by the rule, read without unpacking it: read gets its level in the zip its files are in (one of
    // MaxInnerZips zips inside it, each copied to a temporary file while it is read). Default when the zip holds no mod or
    // several.
    internal static T? ReadZip<T>(string file, Func<ZipLevel, T> read)
    {
        var open = new List<IDisposable>();
        try
        {
            var zip = ZipFile.OpenRead(file);
            open.Add(zip);
            for (var inner = 0; ; inner++)
            {
                var tree = new ZipTree(zip);
                if (Search(tree.At(""), 0) is not [var one]) return default;
                if (!one.Zip) return read(tree.At(one.Path));
                if (inner == MaxInnerZips || tree.Entry(one.Path) is not { } entry) return default;
                var copy = new FileStream(Path.Combine(Path.GetTempPath(), "hellforge-inner-" + Guid.NewGuid().ToString("N")[..8] + ".zip"),
                    FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 1 << 16, FileOptions.DeleteOnClose);
                open.Add(copy);
                using (var stream = entry.Open()) stream.CopyTo(copy);
                copy.Position = 0;
                zip = new ZipArchive(copy, ZipArchiveMode.Read, leaveOpen: true);
                open.Add(zip);
            }
        }
        finally
        {
            for (var i = open.Count - 1; i >= 0; i--) open[i].Dispose();
        }
    }

    // A zip's entries as folders of files ('\' read as '/'; "." parts left out, as unpacking does).
    internal sealed class ZipTree
    {
        readonly Dictionary<string, List<(string Name, ZipArchiveEntry Entry)>> files = new(StringComparer.Ordinal);
        readonly Dictionary<string, HashSet<string>> folders = new(StringComparer.Ordinal) { [""] = [] };
        public readonly List<(string Name, ZipArchiveEntry Entry)> All = [];

        public ZipTree(ZipArchive zip)
        {
            foreach (var entry in zip.Entries)
            {
                var name = entry.FullName.Replace('\\', '/');
                var parts = name.Split('/', StringSplitOptions.RemoveEmptyEntries).Where(p => p != ".").ToList();
                if (parts.Count == 0) continue;
                var folder = name.EndsWith('/');
                if (!folder) All.Add((name, entry));
                var at = "";
                for (var i = 0; i < (folder ? parts.Count : parts.Count - 1); i++)
                {
                    folders[at].Add(parts[i]);
                    at = Join(at, parts[i]);
                    folders.TryAdd(at, []);
                }
                if (!folder)
                {
                    if (!files.TryGetValue(at, out var list)) files[at] = list = [];
                    list.Add((parts[^1], entry));
                }
            }
        }

        public ZipLevel At(string path) => new(this, path);
        public ZipArchiveEntry? Entry(string path)
        {
            var slash = path.LastIndexOf('/');
            var (folder, name) = slash < 0 ? ("", path) : (path[..slash], path[(slash + 1)..]);
            return FilesIn(folder).FirstOrDefault(f => f.Name == name).Entry;
        }
        public IReadOnlyList<(string Name, ZipArchiveEntry Entry)> FilesIn(string path) => files.TryGetValue(path, out var list) ? list : [];
        public IReadOnlyList<string> FoldersIn(string path) => folders.TryGetValue(path, out var set) ? [.. set] : [];
    }

    // A folder in a zip. Names are compared as the system's file lookups compare them once unpacked (ModCatalog.ManifestIn,
    // CosmeticPacks.IsPack).
    internal sealed class ZipLevel : ILevel
    {
        readonly ZipTree tree;
        readonly Dictionary<string, ZipArchiveEntry> here;

        internal ZipLevel(ZipTree tree, string path)
        {
            this.tree = tree;
            Path = path;
            var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            here = tree.FilesIn(path).GroupBy(f => f.Name, comparer).ToDictionary(g => g.Key, g => g.First().Entry, comparer);
        }

        public string Path { get; }
        public IReadOnlyList<string> Files => [.. here.Keys];
        public IReadOnlyList<string> Folders => tree.FoldersIn(Path);
        public bool IsHellForge => here.ContainsKey(ModCatalog.ManifestName) || here.ContainsKey(ModCatalog.LegacyManifestName);
        public bool IsPack => !IsHellForge && (here.Keys.Any(n => ModCatalog.PatchFile().IsMatch(n)) || PackManifest());
        public bool IsMod => IsHellForge || IsPack;
        public ILevel Child(string name) => new ZipLevel(tree, Join(Path, name));

        bool PackManifest()
        {
            if (!here.TryGetValue(CosmeticPacks.ManifestName, out var manifest)) return false;
            using var reader = new StreamReader(manifest.Open());
            return CosmeticPacks.IsPackManifest(reader.ReadToEnd());
        }

        // The files under this folder, by their path in it ('/'), as the zip names them.
        public IEnumerable<(string Path, ZipArchiveEntry Entry)> Shipped()
        {
            var prefix = Path.Length == 0 ? "" : Path + "/";
            return tree.All.Where(f => f.Name.StartsWith(prefix, StringComparison.Ordinal)).Select(f => (f.Name[prefix.Length..], f.Entry));
        }
    }
}
