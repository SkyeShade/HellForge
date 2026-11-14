using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// The player's side of a mod that ships a recipe (hellforge-recipe.json and .literals, made by `assets strip`) instead of
// the game's own bytes: its game patch files are rebuilt into the mod's folder from the builds this PC has, each checked
// against the SHA-256 the recipe names, before the mod is staged. What was rebuilt is recorded beside the recipe
// (.hellforge-assets.json, a dot file, so neither friends' modlists nor packages count it); while the recipe and the
// rebuilt files are as that record says, nothing is rebuilt again.
//
// The rebuilt files are the game's content, so they are never shared: the modlist digest, mod sync and packages count the
// recipe and the literals instead, which pin every rebuilt byte through the SHA-256 they hold. A recipe may only make game
// patch files (<archive>.patch_<n>, .gpu_resources, .stream), inside the mod's folder: never code, a manifest or a
// script of its own.
public static class ModAssets
{
    public const string RecordName = ".hellforge-assets.json";
    const string PartialPrefix = ".hellforge-assets.partial-";

    // A mod folder's recipe: the files it makes, and whether they are in place as the record says (Ready). Pending says,
    // in plain words, why the mod cannot load yet; null when it can (or it has no recipe).
    public sealed record Info(bool HasRecipe, IReadOnlyList<string> Outputs, bool Ready)
    {
        public string? Pending => HasRecipe && !Ready ? "its game files are not rebuilt yet (HellForge rebuilds them from your game before it loads the mod)" : null;
        public static readonly Info None = new(false, [], true);
    }

    public static bool HasRecipe(string folder) => File.Exists(Path.Combine(folder, Recipe.FileName));

    // Cheap enough for every catalog load: the recipe's file list, and the record compared by sizes and times. A recipe
    // that cannot be read is an InvalidDataException, as a broken manifest is.
    public static Info Read(string folder)
    {
        var path = Path.Combine(folder, Recipe.FileName);
        var info = new FileInfo(path);
        if (!info.Exists) return Info.None;
        // a recipe can be megabytes of operations: its file list is kept while the file is unchanged
        var key = Path.GetFullPath(path);
        if (!outputsKnown.TryGetValue(key, out var known) || known.Length != info.Length || known.Written != info.LastWriteTimeUtc)
            outputsKnown[key] = known = (info.Length, info.LastWriteTimeUtc, CheckOutputs(Recipe.LoadRecipe(folder)));
        return new(true, known.Outputs, IsReady(folder, known.Outputs));
    }

    static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (long Length, DateTime Written, List<string> Outputs)> outputsKnown =
        new(StringComparer.OrdinalIgnoreCase);

    // The files a recipe makes, relative to the mod folder with '/': each a game patch file, at a safe path, once.
    public static List<string> CheckOutputs(Recipe recipe)
    {
        var paths = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in recipe.Files)
        {
            var path = file.Path.Replace('\\', '/');
            if (!SyncedStore.SafePath(path) || path.Split('/').Contains(".."))
                throw new InvalidDataException($"{Recipe.FileName}: {Visible(path)} is not a path inside the mod.");
            if (!ModCatalog.PatchFile().IsMatch(Path.GetFileName(path)))
                throw new InvalidDataException($"{Recipe.FileName}: {Visible(path)} is not a game patch file; a recipe only rebuilds those.");
            if (!seen.Add(path)) throw new InvalidDataException($"{Recipe.FileName}: it makes {Visible(path)} twice.");
            if (file.Size < 0 || file.Sha256.Length != 64 || !file.Sha256.All(Uri.IsHexDigit))
                throw new InvalidDataException($"{Recipe.FileName}: {Visible(path)} has no valid size and SHA-256.");
            paths.Add(path);
        }
        return paths;
    }

    static string Visible(string text) => string.Concat(text.Take(80).Select(c => char.IsControl(c) ? '?' : c));

    // The record holds the recipe's and the literals' sizes and times, and each rebuilt file's: any change (a new
    // version of the mod, a file deleted or edited) means a rebuild.
    static bool IsReady(string folder, IReadOnlyList<string> outputs)
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(Path.Combine(folder, RecordName))) is not JsonObject record) return false;
            if (!Same(record["recipe"], Path.Combine(folder, Recipe.FileName)) || !Same(record["literals"], Path.Combine(folder, Recipe.LiteralsName))) return false;
            if (record["files"] is not JsonObject files || files.Count != outputs.Count) return false;
            return outputs.All(path => Same(files[path], Path.Combine(folder, path)));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException) { return false; }
    }

    static bool Same(JsonNode? stamp, string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists) return stamp is null;
        return stamp is JsonObject s && (long?)s["size"] == info.Length && (long?)s["written"] == info.LastWriteTimeUtc.Ticks;
    }

    static JsonNode? Stamp(string path)
    {
        var info = new FileInfo(path);
        return info.Exists ? new JsonObject { ["size"] = info.Length, ["written"] = info.LastWriteTimeUtc.Ticks } : null;
    }

    // What rebuilding a mod needs that the given builds do not have: per build the recipe names, its parts and bytes.
    public sealed record Missing(string Build, int Parts, long Bytes);

    public sealed class MissingBuildsException(string message, IReadOnlyList<Missing> missing) : IOException(message)
    {
        public IReadOnlyList<Missing> Missing { get; } = missing;
    }

    // Rebuilds the recipe's files into the mod folder when they are not as the record says. games is opened only when a
    // rebuild is needed. Each file is rebuilt into a staging folder and checked against its SHA-256 before anything in the
    // mod changes; a file the recipe makes that the mod already has (and the record does not name) is refused, never
    // overwritten. Throws MissingBuildsException when the given builds lack parts the recipe needs (nothing is changed),
    // InvalidDataException when the recipe is broken or does not rebuild. True when it made the mod ready: rebuilt, or
    // found every file already the recipe's (only the record was missing).
    public static bool Ensure(string folder, Func<IReadOnlyList<IGameResources>> games, Action<string>? log = null)
    {
        var info = Read(folder);
        if (!info.HasRecipe || info.Ready) return false;
        var recipe = Recipe.LoadRecipe(folder);
        var previous = RecordedFiles(folder);
        // A file already there is replaced only when the record says it was rebuilt, or it already is what the recipe
        // makes (a copy of the mod that lost its record); anything else is the mod's own file and is never overwritten.
        var already = 0;
        foreach (var file in recipe.Files)
        {
            var path = file.Path.Replace('\\', '/');
            var target = Path.Combine(folder, path);
            if (!File.Exists(target)) continue;
            if (IsRecipeOutput(target, file)) already++;
            else if (!previous.Contains(path)) throw new InvalidDataException($"it ships {path} itself, and its recipe makes that file too.");
        }
        // every file already is the recipe's (mod sync wrote them: from this PC's game and the host's own): only the record
        if (already == recipe.Files.Count)
        {
            var recordStaging = Directory.CreateDirectory(Path.Combine(folder, PartialPrefix + Guid.NewGuid().ToString("N")[..8])).FullName;
            try { WriteRecord(folder, info, recordStaging); }
            finally { Directory.Delete(recordStaging, recursive: true); }
            return true;
        }
        var staging = Path.Combine(folder, PartialPrefix + Guid.NewGuid().ToString("N")[..8]);
        var given = games();
        try
        {
            log?.Invoke($"Rebuilding the game files of {Path.GetFileName(Path.TrimEndingDirectorySeparator(folder))} from your game...");
            using (var literals = Recipe.OpenLiterals(folder))
            {
                try
                {
                    RecipeRebuilder.Write(recipe, literals, given, file =>
                    {
                        var target = Path.Combine(staging, file.Path.Replace('/', Path.DirectorySeparatorChar));
                        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                        return File.Create(target);
                    });
                }
                catch (RecipeRebuilder.MissingSourceException)
                {
                    var missing = MissingFrom(recipe, given);
                    throw new MissingBuildsException(Describe(missing), missing);
                }
            }
            foreach (var path in info.Outputs)
            {
                var target = Path.Combine(folder, path.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Move(Path.Combine(staging, path.Replace('/', Path.DirectorySeparatorChar)), target, overwrite: true);
            }
            WriteRecord(folder, info, staging);
            return true;
        }
        finally
        {
            try { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    // The record of the outputs as they are now, written whole in staging, then moved into place.
    static void WriteRecord(string folder, Info info, string staging)
    {
        var files = new JsonObject();
        foreach (var path in info.Outputs) files[path] = Stamp(Path.Combine(folder, path));
        var record = new JsonObject
        {
            ["format"] = 1,
            ["recipe"] = Stamp(Path.Combine(folder, Recipe.FileName)),
            ["literals"] = Stamp(Path.Combine(folder, Recipe.LiteralsName)),
            ["files"] = files,
        };
        var partialRecord = Path.Combine(staging, RecordName);
        File.WriteAllText(partialRecord, record.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        File.Move(partialRecord, Path.Combine(folder, RecordName), overwrite: true);
    }

    static bool IsRecipeOutput(string path, RecipeFile file)
    {
        using var stream = File.OpenRead(path);
        return stream.Length == file.Size && Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(stream)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase);
    }

    // The files the record says were rebuilt (a rebuild may replace those), or none.
    static HashSet<string> RecordedFiles(string folder)
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(Path.Combine(folder, RecordName)))?["files"] is JsonObject files)
                return files.Select(f => f.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
        return new(StringComparer.OrdinalIgnoreCase);
    }

    // (An IOException, not an InvalidDataException: the mod is fine, this PC lacks game files it needs.)

    // Every part the given builds cannot supply, by the build the recipe names first for it.
    public static List<Missing> MissingFrom(Recipe recipe, IReadOnlyList<IGameResources> games)
    {
        var missing = new Dictionary<string, (int Parts, long Bytes)>(StringComparer.Ordinal);
        foreach (var source in recipe.Sources)
        {
            if (RecipeRebuilder.Has(source, games)) continue;
            var build = source.Places.Count > 0 ? source.Places[0].Build : "?";
            var (parts, bytes) = missing.GetValueOrDefault(build);
            missing[build] = (parts + 1, bytes + source.Size);
        }
        return missing.Select(m => new Missing(m.Key, m.Value.Parts, m.Value.Bytes)).OrderByDescending(m => m.Bytes).ToList();
    }

    public static string Describe(IReadOnlyList<Missing> missing) =>
        $"it needs {Size(missing.Sum(m => m.Bytes))} of game files from {(missing.Count == 1 ? "a build" : $"{missing.Count} builds")} this PC does not have " +
        $"({string.Join(", ", missing.Select(m => m.Build))})";

    static string Size(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F1} MB" : $"{Math.Max(1, bytes / 1000)} KB";

    // Why a mod's recipe could not be rebuilt: its folder, the reason, and the builds it needs that this PC lacks (null
    // when that was not the reason).
    public sealed record Problem(string Folder, string Reason, IReadOnlyList<Missing>? Missing = null);

    // Rebuilds every recipe mod in the mods folder (zipped ones in their unpacked folders) from the builds this PC has,
    // opening the game data once, only if some mod needs it. Returns why any could not be rebuilt, by folder; those mods
    // stay pending (ModManifest.AssetsPending) and do not load.
    public static List<Problem> PrepareAll(InstallLayout install, Action<string>? log = null, IEnumerable<string>? extraFolders = null)
    {
        var folders = new List<string>();
        if (Directory.Exists(install.Mods))
        {
            folders.AddRange(Directory.GetDirectories(install.Mods).Where(f => !Path.GetFileName(f).StartsWith('.')));
            foreach (var (_, root, problem) in ModArchives.Sync(install.Mods))
                if (problem == null && root != null) folders.Add(root);
        }
        if (extraFolders != null) folders.AddRange(extraFolders);
        return Prepare(folders, () => GameBuilds.Local(install), log);
    }

    public static List<Problem> Prepare(IEnumerable<string> folders, Func<IReadOnlyList<LocalBuild>> builds, Action<string>? log = null)
    {
        var problems = new List<Problem>();
        List<IGameResources>? games = null;
        try
        {
            foreach (var folder in folders.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                try { if (!HasRecipe(folder)) continue; }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { continue; }
                try
                {
                    Ensure(folder, () => games ??= builds().Select(GameBuilds.Open).ToList(), log);
                }
                catch (MissingBuildsException missing)
                {
                    problems.Add(new(Path.GetFullPath(folder), missing.Message, missing.Missing));
                }
                catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
                {
                    problems.Add(new(Path.GetFullPath(folder), error.Message));
                }
            }
        }
        finally
        {
            foreach (var game in games ?? []) (game as IDisposable)?.Dispose();
        }
        return problems;
    }

    // A recipe's own two files, as friends and packages count them (the rebuilt files are left out: ModCatalog.DigestFiles).
    public static bool IsRecipeFile(string relative) => relative is Recipe.FileName or Recipe.LiteralsName;

}
