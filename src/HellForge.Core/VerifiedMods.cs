using System.Text.Json.Nodes;

namespace HellForge.Core;

// HellForge's verified list: the mods HellForge itself stands behind (its own, reviewed by a maintainer), which mod sync
// downloads from any host without asking. A version of a mod is listed by its content hash (ModSets.ContentHash;
// HellForge.Cli mod-hash prints it, docs/modding/tools.md "The verified list"), so any other bytes under the same id and
// version are not verified.
//   {"format":1,"mods":[{"id":"...","name":"...","version":"...","hash":"<64 hex>","reviewed":"<date>, <by whom>"}]}
// It is embedded in Core (data/verified-mods.json) and changes with a release. Any other mod is the player's own trust
// (ModSettings' Trusted), which lives with the player's mods, so no update touches it.
public static class VerifiedMods
{
    public sealed record Entry(string Id, string Name, string Version, string Hash, string Reviewed);

    static readonly Lazy<IReadOnlyList<Entry>> embedded = new(() =>
    {
        using var stream = typeof(VerifiedMods).Assembly.GetManifestResourceStream("HellForge.Core.verified-mods.json")
            ?? throw new InvalidDataException("The verified mod list is not embedded.");
        using var reader = new StreamReader(stream);
        return Parse(reader.ReadToEnd());
    });

    public static IReadOnlyList<Entry> All => embedded.Value;

    public static bool Contains(string id, string hash) => All.Any(e => e.Id == id && e.Hash == hash);

    public static IReadOnlyList<Entry> Parse(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The verified mod list is not an object.");
        if ((int?)root["format"] != 1 || root["mods"] is not JsonArray mods) throw new InvalidDataException("The verified mod list has no format 1 or no mods.");
        var entries = new List<Entry>();
        foreach (var node in mods)
        {
            var entry = new Entry((string?)node?["id"] ?? "", (string?)node?["name"] ?? "", (string?)node?["version"] ?? "", (string?)node?["hash"] ?? "",
                (string?)node?["reviewed"] ?? "");
            if (!ModCatalog.IsModId(entry.Id) || !ModSets.Hex(entry.Hash, 64) || entry.Version.Length == 0 || entry.Reviewed.Length == 0)
                throw new InvalidDataException($"A verified mod entry is malformed: {entry.Id}.");
            entries.Add(entry);
        }
        return entries;
    }
}
