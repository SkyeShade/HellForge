using System.Text.Json;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// Cosmetic packs in the community's format: a folder (or zip) with a manifest.json and folders of game patches
// (<archive>.patch_<n> with its .gpu_resources and .stream), or just patches at its root. The manifest's options say which
// folders a choice includes; an option with sub-options includes the chosen one's folders too:
//     {"Version": 1, "Guid": "...", "Name": "...", "Description": "...", "IconPath": ".jpgs/icon.png",
//      "Options": [{"Name": "AC-11 Autocannon", "Image": "...", "Include": ["..."],
//                   "SubOptions": [{"Name": "AC-11/A", "Include": ["AC-11 Autocannon (Bipod)"]}, ...]}]}
// HellForge stages the chosen folders' patches like any mod's data (ModStaging). Packs are cosmetic: friends do not need
// the same ones to play together (ModCatalog.Digest), and they have a group of their own on the Mods page.
public static partial class CosmeticPacks
{
    public const string ManifestName = "manifest.json";

    public sealed record Option(string Name, string Description, string? Image, IReadOnlyList<string> Include, IReadOnlyList<Option> SubOptions);
    public sealed record Pack(string? Icon, IReadOnlyList<Option> Options, bool RootPatches);
    // What a player chose: options switched off (by name), and each option's sub-option (by name).
    public sealed record Choice(IReadOnlyCollection<string> Off, IReadOnlyDictionary<string, string> Sub)
    {
        public static readonly Choice Default = new(new HashSet<string>(), new Dictionary<string, string>());
    }

    [GeneratedRegex("[^a-z0-9-]+")]
    private static partial Regex NotSlug();

    // A pack: a manifest.json naming options or a Guid, or game patches at its root; never a HellForge mod.
    public static bool IsPack(string folder)
    {
        if (ModCatalog.ManifestIn(folder) != null) return false;
        if (Directory.EnumerateFiles(folder).Any(f => ModCatalog.PatchFile().IsMatch(Path.GetFileName(f)))) return true;
        var manifest = Path.Combine(folder, ManifestName);
        if (!File.Exists(manifest)) return false;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            return document.RootElement.ValueKind == JsonValueKind.Object &&
                (document.RootElement.TryGetProperty("Options", out _) || document.RootElement.TryGetProperty("Guid", out _));
        }
        catch (JsonException) { return false; }
    }

    // The pack as a mod: id pack.<guid> (or pack.<folder name>), cosmetic, with the folders the choice stages.
    public static ModManifest Parse(string folder, Choice choice)
    {
        var (name, description, guid, icon, options) = ReadManifest(folder, Path.GetFileName(Path.TrimEndingDirectorySeparator(folder)));
        var id = guid != null && Guid.TryParse(guid, out var g) ? "pack." + g.ToString("D") : "pack." + Slug(name);
        var rootPatches = Directory.EnumerateFiles(folder).Any(f => ModCatalog.PatchFile().IsMatch(Path.GetFileName(f)));
        if (options.Count == 0 && !rootPatches) throw new InvalidDataException("the pack has no options and no patches at its root.");
        var lua = HoldsLua(folder);
        var pack = new Pack(icon, options, rootPatches);
        var folders = Folders(folder, pack, choice);
        foreach (var f in folders.Where(f => !Directory.Exists(f)))
            throw new InvalidDataException($"an option includes {Path.GetRelativePath(folder, f)}, which the pack does not have.");
        return new ModManifest(id, name, "", description, false, folder, [], [], []) { Pack = pack, PackChoice = choice, PackFolders = folders, Icon = icon != null && File.Exists(icon) ? icon : null, Lua = lua };
    }

    // What the pack's manifest.json says, when it has one: its name (else the folder's), description, guid, icon and options.
    static (string Name, string Description, string? Guid, string? Icon, List<Option> Options) ReadManifest(string folder, string name)
    {
        var options = new List<Option>();
        var manifest = Path.Combine(folder, ManifestName);
        if (!File.Exists(manifest)) return (name, "", null, null, options);
        using var document = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{ManifestName} is not an object.");
        name = Text(root, "Name") is { Length: > 0 } n ? n : name;
        var description = Text(root, "Description") ?? "";
        var guid = Text(root, "Guid");
        var icon = Inside(folder, Text(root, "IconPath"));
        if (root.TryGetProperty("Options", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var option in list.EnumerateArray()) options.Add(ReadOption(folder, option));
        return (name, description, guid, icon, options);
    }

    // Any patch in it, whichever options are on: a Lua mod (ModManifest.Lua).
    static bool HoldsLua(string folder) =>
        Directory.EnumerateFiles(folder, "*.patch_*", SearchOption.AllDirectories)
            .Any(f => ModCatalog.PatchFile().Match(Path.GetFileName(f)) is { Success: true } m && m.Groups[2].Length == 0 && ModCatalog.HoldsLua(f));

    // The folders a choice stages, in the manifest's order: the root (if it has patches), then each option switched on with
    // its chosen sub-option (the first unless chosen).
    public static List<string> Folders(string folder, Pack pack, Choice choice)
    {
        var folders = new List<string>();
        if (pack.RootPatches) folders.Add(folder);
        foreach (var option in pack.Options)
        {
            if (choice.Off.Contains(option.Name)) continue;
            folders.AddRange(option.Include.Select(i => Path.Combine(folder, i)));
            if (option.SubOptions.Count > 0)
            {
                var sub = option.SubOptions.FirstOrDefault(s => choice.Sub.TryGetValue(option.Name, out var chosen) && s.Name == chosen) ?? option.SubOptions[0];
                folders.AddRange(sub.Include.Select(i => Path.Combine(folder, i)));
            }
        }
        return folders.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
    }

    // Each staged folder's patch files, as ModStaging stages a mod's data folder.
    public static IEnumerable<string> PatchFiles(string folder) =>
        Directory.Exists(folder) ? Directory.GetFiles(folder).Select(f => Path.GetFileName(f)).Where(f => ModCatalog.PatchFile().IsMatch(f)) : [];

    static Option ReadOption(string folder, JsonElement e)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new InvalidDataException("an option is not an object.");
        var name = Text(e, "Name") is { Length: > 0 } n ? n : throw new InvalidDataException("an option has no name.");
        // an empty name includes nothing: packs list [""] for a choice that keeps the game's own look ("VANILLA", "Default")
        var include = e.TryGetProperty("Include", out var list) && list.ValueKind == JsonValueKind.Array
            ? list.EnumerateArray().Select(i => i.ValueKind == JsonValueKind.String ? i.GetString()!
                : throw new InvalidDataException($"{name}'s Include lists something that is not a folder name.")).Where(i => !string.IsNullOrWhiteSpace(i))
                .Select(i => Inside(folder, i) ?? throw new InvalidDataException($"{name} includes a folder outside the pack."))
                .Select(p => Path.GetRelativePath(folder, p)).ToList()
            : [];
        var subs = new List<Option>();
        if (e.TryGetProperty("SubOptions", out var subList) && subList.ValueKind == JsonValueKind.Array)
            foreach (var sub in subList.EnumerateArray()) subs.Add(ReadOption(folder, sub));
        return new(name, Text(e, "Description") ?? "", Inside(folder, Text(e, "Image")), include, subs);
    }

    static string? Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    // A path the manifest names, inside the pack, or null.
    static string? Inside(string folder, string? relative)
    {
        if (string.IsNullOrWhiteSpace(relative)) return null;
        var path = Path.GetFullPath(Path.Combine(folder, relative.Replace('\\', '/')));
        return LocalInstall.Within(path, folder) ? path : null;
    }

    static string Slug(string name)
    {
        var slug = NotSlug().Replace(name.ToLowerInvariant(), "-").Trim('-');
        return (slug.Length == 0 ? "pack" : slug.Length > 58 ? slug[..58].TrimEnd('-') : slug);
    }
}
