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

    static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public void Save(string folder, byte[] literals)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, FileName), JsonSerializer.Serialize(this, Json));
        File.WriteAllBytes(Path.Combine(folder, LiteralsName), literals);
    }

    public static (Recipe Recipe, byte[] Literals) Load(string folder)
    {
        var recipe = JsonSerializer.Deserialize<Recipe>(File.ReadAllText(Path.Combine(folder, FileName)), Json)
            ?? throw new InvalidDataException("The recipe is empty.");
        if (recipe.Format != 1) throw new InvalidDataException($"Recipe format {recipe.Format} is not one this HellForge reads.");
        var literals = Path.Combine(folder, LiteralsName);
        return (recipe, File.Exists(literals) ? File.ReadAllBytes(literals) : []);
    }
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
        var sources = new byte[recipe.Sources.Count][];
        for (var i = 0; i < recipe.Sources.Count; i++)
        {
            var source = recipe.Sources[i];
            sources[i] = Resolve(source, games)
                ?? throw new InvalidDataException($"No given game folder has {Describe(source.Places[0])} (needed from build {string.Join(" or ", source.Places.Select(p => p.Build).Distinct())}).");
        }
        log?.Invoke($"Found all {sources.Length} game resource parts the recipe uses in the given game folders.");
        var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var file in recipe.Files)
        {
            var output = new byte[file.Size];
            long at = 0;
            foreach (var op in file.Ops)
            {
                var length = op[^1];
                switch (op[0])
                {
                    case Recipe.Copy: sources[op[1]].AsSpan((int)op[2], (int)length).CopyTo(output.AsSpan((int)at)); break;
                    case Recipe.Literal: literals.AsSpan((int)op[1], (int)length).CopyTo(output.AsSpan((int)at)); break;
                    case Recipe.Fill: output.AsSpan((int)at, (int)length).Fill((byte)op[1]); break;
                    default: throw new InvalidDataException($"Unknown recipe operation {op[0]}.");
                }
                at += length;
            }
            if (at != file.Size) throw new InvalidDataException($"{file.Path}: the recipe makes {at} bytes, not {file.Size}.");
            var sha = Convert.ToHexString(SHA256.HashData(output)).ToLowerInvariant();
            if (sha != file.Sha256) throw new InvalidDataException($"{file.Path}: rebuilt file does not match the original ({sha} != {file.Sha256}).");
            files[file.Path] = output;
        }
        return files;
    }

    static byte[]? Resolve(RecipeSource source, IReadOnlyList<IGameResources> games)
    {
        foreach (var place in source.Places)
            foreach (var game in games.OrderBy(g => g.Build == place.Build ? 0 : 1))
            {
                var bytes = game.Read(place.Key, place.Kind);
                if (bytes is not null && bytes.Length == source.Size && Convert.ToHexString(SHA256.HashData(bytes)).Equals(source.Sha256, StringComparison.OrdinalIgnoreCase))
                    return bytes;
            }
        return null;
    }

    public static string Describe(RecipePlace place) =>
        $"{ResourceTypes.Describe(place.Key.Type)} 0x{place.Key.Name:x16} ({place.Part})";
}
