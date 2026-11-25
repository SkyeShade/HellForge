using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace HellForge.Core.GameData;

// The factions' emblems as the game draws them in its menus (vector Paths in its XAML DataTemplates: SELogo_01 and the
// Terminid, Automaton and Illuminate icons), for the launcher's Galactic war page. Read from the player's own game data
// and kept in the installation's temp folder, never shipped: they are the game's art. The cache names the data it came
// from (bundles.nxa's size and time), so a game update reads them again.
public sealed record WarEmblemPath(string Data, double Opacity, double Left, double Top);
public sealed record WarEmblem(int Race, double Width, double Height, IReadOnlyList<WarEmblemPath> Paths);

public static partial class WarEmblems
{
    public const string CacheName = "war-emblems.json";
    static readonly (int Race, string Key)[] Wanted =
        [(1, "DataTemplate.SELogo_01"), (2, "DataTemplate_Icon_Terminid"), (3, "DataTemplate_Icon_Automaton"), (4, "DataTemplate_Icon_Illuminate")];
    static readonly XNamespace Presentation = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";

    // The emblems by race, from the cache when it is this game data's, else read and cached; empty when the data has none.
    public static IReadOnlyDictionary<int, WarEmblem> Load(string gameData, string cacheFolder)
    {
        var stamp = Stamp(gameData);
        if (stamp == null) return new Dictionary<int, WarEmblem>();
        var cache = Path.Combine(cacheFolder, CacheName);
        if (FromCache(cache, stamp) is { } cached) return cached;
        var emblems = Read(gameData);
        try
        {
            Directory.CreateDirectory(cacheFolder);
            File.WriteAllText(cache, ToJson(emblems, stamp));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return emblems;
    }

    static string? Stamp(string gameData)
    {
        var index = new FileInfo(Path.Combine(gameData, "bundles.nxa"));
        return index.Exists ? $"{index.Length}/{index.LastWriteTimeUtc.Ticks}" : null;
    }

    // Every XAML resource's DataTemplates, until the four are found.
    static Dictionary<int, WarEmblem> Read(string gameData)
    {
        var found = new Dictionary<int, WarEmblem>();
        using var reader = new GameDataReader(gameData);
        foreach (var at in reader.All(ResourceTypes.Hash("xaml")).Values)
        {
            var main = reader.Read(at).Main;
            if (main.Length <= 16) continue;
            var xaml = Encoding.UTF8.GetString(main, 16, main.Length - 16);
            foreach (var (race, key) in Wanted)
                if (!found.ContainsKey(race) && Template(xaml, key) is { } body && Parse(race, body) is { } emblem) found[race] = emblem;
            if (found.Count == Wanted.Length) break;
        }
        return found;
    }

    // A DataTemplate's body by its key, or null.
    static string? Template(string xaml, string key)
    {
        var start = xaml.IndexOf($"<DataTemplate x:Key=\"{key}\"", StringComparison.Ordinal);
        if (start < 0) return null;
        var end = xaml.IndexOf("</DataTemplate>", start, StringComparison.Ordinal);
        var open = xaml.IndexOf('>', start) + 1;
        return end < 0 || open <= 0 || open > end ? null : xaml[open..end];
    }

    // A template's Paths (their Data, opacity and Canvas offsets) and the box they fill.
    static WarEmblem? Parse(int race, string body)
    {
        body = PrefixedAttribute().Replace(body, "");
        XElement root;
        try { root = XElement.Parse($"<r xmlns=\"{Presentation}\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">{body}</r>"); }
        catch (System.Xml.XmlException) { return null; }
        var paths = new List<WarEmblemPath>();
        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var path in root.Descendants(Presentation + "Path"))
        {
            var data = ((string?)path.Attribute("Data"))?.Trim();
            if (string.IsNullOrEmpty(data) || !PathData().IsMatch(data)) continue;
            var (left, top) = (Number(path.Attribute("Canvas.Left"), 0), Number(path.Attribute("Canvas.Top"), 0));
            paths.Add(new WarEmblemPath(data, Number(path.Attribute("Opacity"), 1), left, top));
            var numbers = Numbers().Matches(data.StartsWith('F') ? data[2..] : data).Select(m => double.Parse(m.Value, CultureInfo.InvariantCulture)).ToList();
            for (var i = 0; i + 1 < numbers.Count; i += 2)
            {
                (minX, maxX) = (Math.Min(minX, numbers[i] + left), Math.Max(maxX, numbers[i] + left));
                (minY, maxY) = (Math.Min(minY, numbers[i + 1] + top), Math.Max(maxY, numbers[i + 1] + top));
            }
        }
        if (paths.Count == 0) return null;
        var canvas = root.Descendants(Presentation + "Canvas").FirstOrDefault();
        var (width, height) = (Number(canvas?.Attribute("Width"), 0), Number(canvas?.Attribute("Height"), 0));
        if (width > 0 && height > 0) return new WarEmblem(race, width, height, paths);
        // no canvas size: the paths' own box, moved to the origin
        return new WarEmblem(race, maxX - minX, maxY - minY, [.. paths.Select(p => p with { Left = p.Left - minX, Top = p.Top - minY })]);
    }

    static double Number(XAttribute? attribute, double fallback) =>
        attribute != null && double.TryParse(attribute.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;

    static string ToJson(IReadOnlyDictionary<int, WarEmblem> emblems, string stamp)
    {
        var list = new JsonArray();
        foreach (var e in emblems.Values.OrderBy(e => e.Race))
            list.Add((JsonNode)new JsonObject
            {
                ["race"] = e.Race, ["width"] = e.Width, ["height"] = e.Height,
                ["paths"] = new JsonArray([.. e.Paths.Select(p => (JsonNode?)new JsonObject { ["data"] = p.Data, ["opacity"] = p.Opacity, ["left"] = p.Left, ["top"] = p.Top })]),
            });
        return new JsonObject { ["stamp"] = stamp, ["emblems"] = list }.ToJsonString();
    }

    static Dictionary<int, WarEmblem>? FromCache(string cache, string stamp)
    {
        try
        {
            if (!File.Exists(cache)) return null;
            using var document = JsonDocument.Parse(File.ReadAllText(cache));
            var root = document.RootElement;
            if (root.GetProperty("stamp").GetString() != stamp) return null;
            return root.GetProperty("emblems").EnumerateArray().Select(e => new WarEmblem(e.GetProperty("race").GetInt32(), e.GetProperty("width").GetDouble(),
                e.GetProperty("height").GetDouble(), [.. e.GetProperty("paths").EnumerateArray().Select(p => new WarEmblemPath(p.GetProperty("data").GetString()!,
                    p.GetProperty("opacity").GetDouble(), p.GetProperty("left").GetDouble(), p.GetProperty("top").GetDouble()))])).ToDictionary(e => e.Race);
        }
        catch (Exception error) when (error is JsonException or IOException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException) { return null; }
    }

    // Design-time and other prefixed attributes would need their namespaces declared, and nothing here uses them.
    [GeneratedRegex(@"\s[A-Za-z]+:[A-Za-z.]+=""[^""]*""")] private static partial Regex PrefixedAttribute();
    [GeneratedRegex("^(F[01])?[MmLlHhVvCcSsQqTtAaZz0-9eE.,+\\- \\t\\r\\n]+$")] private static partial Regex PathData();
    [GeneratedRegex(@"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")] private static partial Regex Numbers();
}
