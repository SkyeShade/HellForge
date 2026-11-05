using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// A mod with the game's content taken out: each of its files as a list of operations that rebuild it byte for byte on
// the player's PC, from the player's own game files and the bytes the mod's author made (the literals file).
//   [0, source, offset, length]  copy from one of the game's resource parts (Sources[source])
//   [1, offset, length]          copy from the literals file
//   [2, value, length]           a run of one byte value
// Files with nothing from the game in them are not in the recipe: they ship as they are.
public sealed class Recipe
{
    public const string FileName = "hellforge-recipe.json", LiteralsName = "hellforge-recipe.literals";
    public const int Copy = 0, Literal = 1, Fill = 2;

    public int Format { get; set; } = 1;
    public List<RecipeSource> Sources { get; set; } = [];
    public List<RecipeFile> Files { get; set; } = [];

    public void Save(string folder, byte[] literals)
    {
        Save(folder);
        File.WriteAllBytes(Path.Combine(folder, LiteralsName), literals);
    }

    // The recipe alone (the literals file written by whoever wrote the literals).
    public void Save(string folder)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, FileName), JsonSerializer.Serialize(this, AssetJson.Default.Recipe));
    }

    public static (Recipe Recipe, byte[] Literals) Load(string folder)
    {
        var literals = Path.Combine(folder, LiteralsName);
        return (LoadRecipe(folder), File.Exists(literals) ? File.ReadAllBytes(literals) : []);
    }

    // The recipe alone; its literals are read from the literals file as the rebuild needs them (OpenLiterals).
    public static Recipe LoadRecipe(string folder)
    {
        Recipe recipe;
        try { recipe = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(folder, FileName)), AssetJson.Default.Recipe) ?? throw new InvalidDataException("The recipe is empty."); }
        catch (JsonException error) { throw new InvalidDataException($"{FileName} is not a recipe HellForge reads ({error.Message})."); }
        if (recipe.Format != 1) throw new InvalidDataException($"Recipe format {recipe.Format} is not one this HellForge reads.");
        return recipe;
    }

    public static Stream OpenLiterals(string folder) =>
        File.Exists(Path.Combine(folder, LiteralsName)) ? File.OpenRead(Path.Combine(folder, LiteralsName)) : new MemoryStream([], false);
}

// A part of one of the game's resources, by content: any place that holds bytes with this SHA-256 will do.
public sealed class RecipeSource
{
    public string Sha256 { get; set; } = "";
    public int Size { get; set; }
    public List<RecipePlace> Places { get; set; } = [];
}

public sealed class RecipePlace
{
    public string Build { get; set; } = "";
    public string Type { get; set; } = "";
    public string Name { get; set; } = "";
    public string Part { get; set; } = "";
    // The archive that holds it in that build (its 16-hex-digit name), for fetching an old build's files.
    public string? Archive { get; set; }

    [JsonIgnore] public ResourceKey Key => new(Convert.ToUInt64(Type, 16), Convert.ToUInt64(Name, 16));
    [JsonIgnore] public PartKind Kind => Enum.Parse<PartKind>(Part, ignoreCase: true);
}

public sealed class RecipeFile
{
    public string Path { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
    public List<long[]> Ops { get; set; } = [];
}

// Rebuilds a recipe's files from game folders the player has. Each source is looked up in every given build (the one it
// was found in first) and used only when its SHA-256 matches; each rebuilt file is checked against the original's SHA-256.
public static class RecipeRebuilder
{
    public static Dictionary<string, byte[]> Rebuild(Recipe recipe, byte[] literals, IReadOnlyList<IGameResources> games, Action<string>? log = null)
    {
        var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        Rebuild(recipe, literals, games, (file, bytes) => files[file.Path] = bytes, log);
        return files;
    }

    // Rebuilds the files one at a time, each checked against the original's SHA-256, and hands each to the caller (to
    // write or compare) before the next is built, so only one file is held. Game parts are read as the files need them
    // and kept within cacheBytes. A part no given game folder has stops the rebuild there.
    public static void Rebuild(Recipe recipe, byte[] literals, IReadOnlyList<IGameResources> games, Action<RecipeFile, byte[]> each,
        Action<string>? log = null, long cacheBytes = 256L << 20) =>
        Rebuild(recipe, new MemoryStream(literals, false), games, each, log, cacheBytes);

    // The same, with the literals read from a stream (a file) as the files need them.
    public static void Rebuild(Recipe recipe, Stream literals, IReadOnlyList<IGameResources> games, Action<RecipeFile, byte[]> each,
        Action<string>? log = null, long cacheBytes = 256L << 20)
    {
        MemoryStream? current = null;
        Write(recipe, literals, games, file =>
        {
            if (file.Size > Array.MaxLength) throw new InvalidDataException($"{file.Path}: {file.Size:N0} bytes is too big to rebuild in memory.");
            return current = new MemoryStream();
        }, file => each(file, current!.ToArray()), log, cacheBytes);
    }

    // Rebuilds the files one at a time into the streams open gives (Stream.Null to only check them), each checked
    // against the original's SHA-256 as it is written, so no file is held whole. Game parts are read as the operations
    // need them and kept within cacheBytes. A part no given game folder has, an operation out of range (a recipe is
    // untrusted input) or a file that does not match stops the rebuild there, with that file incomplete: write to a
    // staging folder. done is told of each file once it matches.
    public static void Write(Recipe recipe, Stream literals, IReadOnlyList<IGameResources> games, Func<RecipeFile, Stream> open,
        Action<RecipeFile>? done = null, Action<string>? log = null, long cacheBytes = 256L << 20)
    {
        var cache = new Dictionary<int, LinkedListNode<(int Source, byte[] Bytes)>>();
        var recent = new LinkedList<(int Source, byte[] Bytes)>();
        long cached = 0;
        byte[] Source(int i)
        {
            if (cache.TryGetValue(i, out var node))
            {
                recent.Remove(node);
                recent.AddFirst(node);
                return node.Value.Bytes;
            }
            var source = recipe.Sources[i];
            var bytes = Resolve(source, games)
                ?? throw new MissingSourceException(source.Places.Count == 0 ? "A recipe source names no place in the game."
                    : $"No given game folder has {Describe(source.Places[0])} (needed from build {string.Join(" or ", source.Places.Select(p => p.Build).Distinct())}).");
            cache[i] = recent.AddFirst((i, bytes));
            cached += bytes.Length;
            while (cached > cacheBytes && recent.Count > 1)
            {
                var old = recent.Last!.Value;
                recent.RemoveLast();
                cache.Remove(old.Source);
                cached -= old.Bytes.Length;
            }
            return bytes;
        }
        var buffer = new byte[1 << 20];
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var file in recipe.Files)
        {
            long at = 0;
            using (var output = open(file))
            {
                void Put(ReadOnlySpan<byte> bytes)
                {
                    output.Write(bytes);
                    hash.AppendData(bytes);
                    at += bytes.Length;
                }
                InvalidDataException Bad(string what) => new($"{file.Path}: {what}");
                foreach (var op in file.Ops)
                {
                    if (op.Length < 3) throw Bad("an operation is incomplete.");
                    var length = op[^1];
                    if (length < 0 || at + length > file.Size) throw Bad($"the recipe makes more than its {file.Size:N0} bytes.");
                    switch (op[0])
                    {
                        case Recipe.Copy:
                            if (op.Length != 4 || op[1] < 0 || op[1] >= recipe.Sources.Count) throw Bad("a copy names no source.");
                            var src = Source((int)op[1]);
                            if (op[2] < 0 || op[2] + length > src.Length) throw Bad("a copy runs past its source.");
                            Put(src.AsSpan((int)op[2], (int)length));
                            break;
                        case Recipe.Literal:
                            if (op.Length != 3 || op[1] < 0 || op[1] + length > literals.Length) throw Bad("a literal runs past the literals file.");
                            literals.Position = op[1];
                            for (var left = length; left > 0;)
                            {
                                var n = (int)Math.Min(left, buffer.Length);
                                literals.ReadExactly(buffer, 0, n);
                                Put(buffer.AsSpan(0, n));
                                left -= n;
                            }
                            break;
                        case Recipe.Fill:
                            if (op.Length != 3) throw Bad("a fill is malformed.");
                            buffer.AsSpan(0, (int)Math.Min(length, buffer.Length)).Fill((byte)op[1]);
                            for (var left = length; left > 0;)
                            {
                                var n = (int)Math.Min(left, buffer.Length);
                                Put(buffer.AsSpan(0, n));
                                left -= n;
                            }
                            break;
                        default: throw Bad($"unknown recipe operation {op[0]}.");
                    }
                }
                if (at != file.Size) throw Bad($"the recipe makes {at:N0} bytes, not {file.Size:N0}.");
            }
            var sha = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
            if (sha != file.Sha256) throw new InvalidDataException($"{file.Path}: rebuilt file does not match the original ({sha} != {file.Sha256}).");
            done?.Invoke(file);
        }
        log?.Invoke($"Rebuilt {recipe.Files.Count} files from {recipe.Sources.Count} game resource parts in the given game folders.");
    }

    // A part of the game the recipe needs that none of the given builds has (with those bytes): an IOException, like a
    // missing file, since the recipe is fine and this PC lacks the game files.
    public sealed class MissingSourceException(string message) : IOException(message);

    // Whether one of the given builds has the source's bytes.
    public static bool Has(RecipeSource source, IReadOnlyList<IGameResources> games) => Resolve(source, games) != null;

    static byte[]? Resolve(RecipeSource source, IReadOnlyList<IGameResources> games)
    {
        foreach (var place in source.Places)
            foreach (var game in games.OrderBy(g => g.Build == place.Build ? 0 : 1))
            {
                foreach (var bytes in game.ReadIn(place.Key, place.Kind, place.Archive))
                    if (bytes.Length == source.Size && Convert.ToHexString(SHA256.HashData(bytes)).Equals(source.Sha256, StringComparison.OrdinalIgnoreCase))
                        return bytes;
            }
        return null;
    }

    // What a player who has only the given builds still needs, per build: each build the recipe can use for the parts
    // the player lacks (the first place's build of each such part), with the archive files that hold them and the
    // download_depot commands that fetch that build. A loose build's archives are files of their own; a packed build's
    // are inside bundles.*.nxa, so the whole build's data is needed.
    public static List<(string Build, SortedSet<string> Files, List<string> Commands)> FetchPlan(Recipe recipe, IReadOnlyCollection<string> have, BuildCatalog catalog)
    {
        var plan = new Dictionary<string, SortedSet<string>>(StringComparer.Ordinal);
        foreach (var source in recipe.Sources)
        {
            if (source.Places.Any(p => have.Contains(p.Build))) continue;
            var place = source.Places[0];
            var layout = catalog.Find(place.Build)?.Layout ?? "loose";
            if (!plan.TryGetValue(place.Build, out var files)) plan[place.Build] = files = new(StringComparer.Ordinal);
            if (layout == "nxa") files.Add("data/bundles.*.nxa");
            else if (place.Archive is not null)
                files.Add("data/" + place.Archive + place.Kind switch { PartKind.Gpu => ".gpu_resources", PartKind.Stream => ".stream", _ => "" });
        }
        return plan.Select(p => (p.Key, p.Value, catalog.Find(p.Key) is { } b ? catalog.Commands(b).ToList() : [])).ToList();
    }

    public static string Describe(RecipePlace place) =>
        $"{ResourceTypes.Describe(place.Key.Type)} 0x{place.Key.Name:x16} ({place.Part})";
}
