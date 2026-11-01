using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// A session's values from before any stats file applied (the game's, with code mods).
public sealed record StatsObservation(string Path, DateTime CapturedUtc, IReadOnlyDictionary<string, uint> Values)
{
    public uint? Bits(StatsField field) => Values.TryGetValue(field.AddressKey, out var bits) ? bits : null;
    public string? Value(StatsField field) => Bits(field) is { } bits ? StatsCatalog.Format(field, bits) : null;
}

// What a field is before the editor's own mod: another enabled mod's value, else the game's.
public sealed class StatsBaseline(StatsObservation? game, IReadOnlyDictionary<string, (string Mod, uint Bits)> mods)
{
    public static readonly StatsBaseline None = new(null, new Dictionary<string, (string, uint)>());
    public StatsObservation? Game => game;
    public (string Mod, uint Bits)? Mod(StatsField field, bool own) =>
        mods.TryGetValue(StatsCatalog.LinkKey(field, own), out var value) ? value : null;
    public uint? Base(StatsField field, bool own) => Mod(field, own)?.Bits ?? game?.Bits(field);

    // Later mods win, as in the loader. The editor's own mod is what is being edited, so it is left out.
    public static StatsBaseline From(StatsObservation? game, IEnumerable<ModManifest> mods)
    {
        var values = new Dictionary<string, (string, uint)>(StringComparer.Ordinal);
        foreach (var mod in mods.Where(m => m.Enabled && m.Id != StatsEditorData.ModId).OrderBy(m => m.Id, StringComparer.Ordinal))
            foreach (var edit in StatsData.Edits(mod)) values[StatsData.ConflictKey(edit)] = (mod.Name, edit.Value);
        return new(game, values);
    }
}

// The editor's stats document. Each field is stored once: a record several items share is set where it was last
// edited and shown everywhere it appears. Aliases in a loaded file are expanded to the schema's field names.
public sealed class StatsDraft
{
    public JsonObject Document { get; }
    public StatsDraft(JsonObject document) => Document = Normalize((JsonObject)document.DeepClone());

    public bool Own(string category, string target) => Document[category]?[target]?["own"] is JsonValue own && own.GetValueKind() == JsonValueKind.True;

    // An entry with its own records holds component fields only, so its shared-definition edits move to another
    // view of the same definition (preferably the projectile or damage type itself).
    public void SetOwn(string category, string target, bool own)
    {
        if (!own)
        {
            if (Document[category]?[target] is JsonObject entry) { entry.Remove("own"); Prune(category, target); }
            return;
        }
        var fields = StatsSchema.Current.Targets[category][target].Fields;
        var moves = new List<(string Key, (string Category, string Target, string Key) To)>();
        foreach (var (key, _) in Document[category]?[target] as JsonObject ?? [])
            if (fields.TryGetValue(key, out var field) && field.Kind != "component")
                moves.Add((key, Elsewhere(field, (category, target)) ??
                    throw new InvalidDataException($"{StatsCatalog.Label(key, field)} is a shared definition with no other place to keep it. Clear it first.")));
        foreach (var (key, to) in moves)
        {
            var value = Document[category]![target]![key]!.DeepClone();
            ((JsonObject)Document[category]![target]!).Remove(key);
            Entry(to.Category, to.Target)[to.Key] = value;
        }
        Entry(category, target)["own"] = true;
    }

    (string Category, string Target, string Key)? Elsewhere(StatsField field, (string Category, string Target) except) =>
        StatsCatalog.Linked(field).Where(l => (l.Category, l.Target) != except && !Own(l.Category, l.Target))
            .OrderBy(l => l.Category is "projectiles" or "damage" ? 0 : 1).Select(l => ((string, string, string)?)l).FirstOrDefault();

    // Where the row's value is stored.
    (string Category, string Target, string Key) Home(StatsRow row) =>
        row.Field.Kind == "component" || !Own(row.Part.Category, row.Part.Target) ? (row.Part.Category, row.Part.Target, row.Key) :
        Elsewhere(row.Field, (row.Part.Category, row.Part.Target)) ?? throw new InvalidDataException("Clear own records to edit this shared definition.");

    IEnumerable<(string Category, string Target, string Key)> Locations(StatsRow row) =>
        row.Field.Kind == "component" && Own(row.Part.Category, row.Part.Target)
            ? [(row.Part.Category, row.Part.Target, row.Key)]
            : StatsCatalog.Linked(row.Field).Where(l => l == (row.Part.Category, row.Part.Target, row.Key) ||
                !(row.Field.Kind == "component" && Own(l.Category, l.Target)));

    // The value set for this row, and where (another item when the record is shared).
    public (string Category, string Target, string Key, JsonNode Value)? Find(StatsRow row)
    {
        if (Document[row.Part.Category]?[row.Part.Target]?[row.Key] is { } own) return (row.Part.Category, row.Part.Target, row.Key, own);
        foreach (var (c, t, k) in Locations(row))
            if (Document[c]?[t]?[k] is { } value) return (c, t, k, value);
        return null;
    }

    // Sets the row's value, or clears it (null) wherever the shared record is set.
    public void Set(StatsRow row, JsonNode? value)
    {
        var home = value == null ? default : Home(row);
        foreach (var (c, t, k) in Locations(row).Append((row.Part.Category, row.Part.Target, row.Key)).Distinct())
            if (Document[c]?[t] is JsonObject entry && entry.Remove(k)) Prune(c, t);
        if (value != null) Entry(home.Category, home.Target)[home.Key] = value.DeepClone();
    }

    public IEnumerable<(string Category, string Target, string Key, JsonNode Value)> Entries() =>
        from section in Document where section.Key is not ("format" or "raw") && section.Value is JsonObject
        from target in (JsonObject)section.Value! where target.Value is JsonObject
        from field in (JsonObject)target.Value! where field.Key != "own" && field.Value != null
        select (section.Key, target.Key, field.Key, field.Value!);

    public int RawCount => Document["raw"] is JsonArray raw ? raw.Count : 0;

    // The document to save: no entries that only say "own".
    public JsonObject Clean()
    {
        var copy = (JsonObject)Document.DeepClone();
        foreach (var section in copy.Where(p => p.Key is not ("format" or "raw")).ToList())
            if (section.Value is JsonObject targets)
            {
                foreach (var target in targets.ToList())
                    if (target.Value is JsonObject entry && entry.All(p => p.Key == "own")) targets.Remove(target.Key);
                if (targets.Count == 0) copy.Remove(section.Key);
            }
        return copy;
    }

    JsonObject Entry(string category, string target)
    {
        if (Document[category] is not JsonObject section) Document[category] = section = new JsonObject();
        if (section[target] is not JsonObject entry) section[target] = entry = new JsonObject();
        return entry;
    }

    void Prune(string category, string target)
    {
        if (Document[category] is not JsonObject section) return;
        if (section[target] is JsonObject { Count: 0 }) section.Remove(target);
        if (section.Count == 0) Document.Remove(category);
    }

    static JsonObject Normalize(JsonObject document)
    {
        var schema = StatsSchema.Current;
        foreach (var section in document.Where(p => p.Key is not ("format" or "raw")).ToList())
        {
            if (section.Value is not JsonObject entries || !schema.Targets.TryGetValue(section.Key, out var targets)) continue;
            var normalized = new JsonObject();
            foreach (var (name, node) in entries.ToList())
            {
                var targetName = schema.TargetAliases.GetValueOrDefault(section.Key)?.GetValueOrDefault(name, name) ?? name;
                if (node is not JsonObject entry || !targets.TryGetValue(targetName, out var target)) { normalized[name] = node?.DeepClone(); continue; }
                normalized[targetName] = NormalizeEntry(entry, target, section.Key);
            }
            document[section.Key] = normalized;
        }
        return document;
    }

    // A target's stats by their fields' own keys: "spread" as its two fields, a damage type's "armorPenetration" as its
    // four, an alias as its field.
    static JsonObject NormalizeEntry(JsonObject entry, StatsTarget target, string category)
    {
        var result = new JsonObject();
        foreach (var (property, value) in entry)
        {
            if (property == "spread")
            { result["weapon.horizontal_spread"] = value?.DeepClone(); result["weapon.vertical_spread"] = value?.DeepClone(); }
            else if (property == "armorPenetration" && category == "damage" && value is JsonArray ap && ap.Count == 4)
                for (var i = 0; i < 4; i++) result[new[] { "ap_direct", "ap_slight", "ap_large", "ap_extreme" }[i]] = ap[i]?.DeepClone();
            else result[target.Aliases.GetValueOrDefault(property, property)] = value?.DeepClone();
        }
        return result;
    }
}

public static class StatsEditorData
{
    public const string ModId = "zz.local.stats";
    public const string Marker = "hellforge-stats-editor-v1";
    public static JsonObject Empty => new() { ["format"] = 1 };

    public static StatsObservation ReadSnapshot(string path)
    {
        if (new FileInfo(path).Length > 4 * 1024 * 1024) throw new InvalidDataException("Stats snapshot is too large.");
        using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
        var header = reader.ReadLine()?.Split('\t');
        if (header is not ["stats_snapshot", "2", "25480438", var hash] || hash != StatsSchema.Fingerprint)
            throw new InvalidDataException("Stats snapshot belongs to another build or schema. Start a new local session to refresh it.");
        var values = new Dictionary<string, uint>(StringComparer.Ordinal);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        while (reader.ReadLine() is { } line)
        {
            var f = line.Split('\t');
            if (f.Length != 8 || f[5] is not ("f32" or "i32" or "u32" or "u8") || f[6] is not ("ok" or "unavailable") ||
                !uint.TryParse(f[7], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var bits))
                throw new InvalidDataException("Malformed stats snapshot row.");
            var key = string.Join('\t', f.Take(6));
            if (!seen.Add(key)) throw new InvalidDataException("Duplicate stats snapshot row.");
            if (f[6] == "ok" && (f[5] != "f32" || float.IsFinite(BitConverter.UInt32BitsToSingle(bits)))) values.Add(key, bits);
        }
        return new(path, File.GetLastWriteTimeUtc(path), values);
    }

    public static StatsObservation? LatestSnapshot(string sessions)
    {
        if (!Directory.Exists(sessions)) return null;
        foreach (var path in Directory.EnumerateFiles(sessions, "stats-values.tsv", SearchOption.AllDirectories).OrderByDescending(File.GetLastWriteTimeUtc))
            try { return ReadSnapshot(path); }
            catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException) { }
        return null;
    }

    public static (JsonObject Document, string Revision) Load(string modsFolder)
    {
        var folder = Path.Combine(modsFolder, ModId);
        if (!Directory.Exists(folder)) return (Empty, "missing");
        CheckOwner(folder);
        var path = Path.Combine(folder, StatsData.FileName);
        var bytes = File.ReadAllBytes(path);
        var document = JsonNode.Parse(bytes) as JsonObject ?? throw new InvalidDataException("Editor stats file must be an object.");
        StatsData.Parse(document.ToJsonString(), EntityProfile.Current);
        return (document, Convert.ToHexString(SHA256.HashData(bytes)));
    }

    // Only this editor's mod is replaced, atomically; a changed file must be reloaded before saving.
    public static string Save(string modsFolder, JsonObject document, string expectedRevision)
    {
        var text = document.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n";
        var empty = document.Count == 1 && document["format"]?.ToJsonString() == "1";
        if (!empty) StatsData.Parse(text, EntityProfile.Current);
        Directory.CreateDirectory(modsFolder);
        using var editLock = new FileStream(Path.Combine(modsFolder, ".hellforge-stats-editor.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        var folder = Path.Combine(modsFolder, ModId);
        var (_, revision) = Load(modsFolder);
        if (revision != expectedRevision) throw new InvalidDataException("The editor mod changed on disk. Reload it before saving.");
        if (empty)
        {
            if (revision == "missing") return "missing";
            File.Delete(Path.Combine(folder, StatsData.FileName));
            File.Delete(Path.Combine(folder, ModCatalog.ManifestName));
            if (!Directory.EnumerateFileSystemEntries(folder).Any()) Directory.Delete(folder);
            return "missing";
        }
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, StatsData.FileName);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, text);
            File.Move(temporary, path, overwrite: true);
            if (revision == "missing")
            {
                var manifest = new JsonObject { ["format"] = 1, ["id"] = ModId, ["name"] = "Local Stats", ["version"] = "1.0.0",
                    ["description"] = "Stats saved by the HellForge arsenal editor.", ["gameplay"] = true, ["editor"] = Marker };
                File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), manifest.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
            }
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        return Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    }

    static void CheckOwner(string folder)
    {
        var path = Path.Combine(folder, ModCatalog.ManifestName);
        if (!File.Exists(path)) throw new InvalidDataException("The editor mod folder already exists without its manifest.");
        using var manifest = JsonDocument.Parse(File.ReadAllText(path));
        var root = manifest.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("id", out var id) || id.ValueKind != JsonValueKind.String || id.GetString() != ModId ||
            !root.TryGetProperty("editor", out var marker) || marker.ValueKind != JsonValueKind.String || marker.GetString() != Marker)
            throw new InvalidDataException("The editor mod folder belongs to another mod; choose another folder for that mod.");
    }
}
