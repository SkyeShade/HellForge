using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The in-game editor's saves from a sandboxed session (docs/steam-bridge.md): a game at low integrity cannot write to
// mods/, so the editor writes its saved changes as a list, its settings and its exported presets into the session's
// sandbox folder, and this applies them once the game is gone. Everything there was written by the game, so it is read
// as untrusted: sizes are bounded, each Local Stats change must pass StatsData.Parse with it in place (one that does not
// is left out), mod content values and settings are rebuilt by System.Text.Json, and presets keep only safe file names.
public static partial class SandboxStats
{
    public const string ChangesName = "changes.json", SettingsName = "editor-settings.json", PresetsName = "presets",
        ModValuesName = "mod-stats.json";
    const long MaxChangesBytes = 4 << 20, MaxSettingsBytes = 64 << 10, MaxPresetBytes = 4 << 20;
    const int MaxText = 256, MaxPresets = 100;

    public sealed record Change(string Category, string Target, string Key, double? Value);

    public sealed record Applied(int Stats, int ModValues, int Refused, bool Settings, int Presets);

    // Applies what the folder holds; null when the editor saved nothing. Throws InvalidDataException for a file that is not
    // one the editor writes, and IOException when the Local Stats file stays locked.
    public static Applied? Apply(string folder, string modsFolder, EntityProfile profile)
    {
        var changesFile = Path.Combine(folder, ChangesName);
        var settingsFile = Path.Combine(folder, SettingsName);
        var presets = Path.Combine(folder, PresetsName);
        if (!File.Exists(changesFile) && !File.Exists(settingsFile) && !Directory.Exists(presets)) return null;
        var (changes, refused) = File.Exists(changesFile) ? Read(Text(changesFile, MaxChangesBytes)) : ([], 0);
        var stats = changes.Where(c => !c.Category.StartsWith("mod:", StringComparison.Ordinal)).ToList();
        var modValues = changes.Where(c => c.Category.StartsWith("mod:", StringComparison.Ordinal)).ToList();
        var appliedStats = stats.Count == 0 ? 0 : ApplyStats(modsFolder, stats, profile);
        refused += stats.Count - appliedStats;
        var editor = Path.Combine(modsFolder, BundledMods.EditorFolder);
        if (modValues.Count > 0) ApplyModValues(Path.Combine(editor, ModValuesName), modValues);
        var settings = File.Exists(settingsFile) && CopySettings(settingsFile, Path.Combine(editor, SettingsName));
        var presetCount = Directory.Exists(presets) ? CopyPresets(presets, Path.Combine(editor, PresetsName)) : 0;
        return new(appliedStats, modValues.Count, refused, settings, presetCount);
    }

    // The editor's list: {"format":1,"changes":[{"category","target","key","value"} or {..., "remove":true}]}. A change
    // of another shape is counted as refused.
    public static (List<Change> Changes, int Refused) Read(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The session's editor changes are not an object.");
        if (root["format"] is not JsonValue format || !format.TryGetValue<int>(out var version) || version != 1 || root["changes"] is not JsonArray list)
            throw new InvalidDataException("The session's editor changes are not a file the editor writes.");
        if (list.Count > StatsData.MaxEdits) throw new InvalidDataException("The session's editor changes hold too many changes.");
        var changes = new List<Change>();
        var refused = 0;
        foreach (var node in list)
        {
            if (node is JsonObject item && Name(item["category"]) is { } category && Name(item["target"]) is { } target && Name(item["key"]) is { } key &&
                category != "format" && Value(item) is var (ok, value) && ok)
                changes.Add(new(category, target, key, value));
            else refused++;
        }
        return (changes, refused);

        static string? Name(JsonNode? node) =>
            node is JsonValue value && value.TryGetValue<string>(out var text) && text.Length is > 0 and <= MaxText && !text.Any(char.IsControl) ? text : null;
        static (bool, double?) Value(JsonObject item)
        {
            if (item["remove"] is JsonValue remove && remove.TryGetValue<bool>(out var gone) && gone) return (item["value"] == null, null);
            return item["value"] is JsonValue value && value.TryGetValue<double>(out var number) && double.IsFinite(number) ? (true, number) : (false, null);
        }
    }

    // Applies to the Local Stats file under the launcher's lock, as its STATS page saves; the launcher may have saved
    // during the session, so the changes go onto the file as it is now.
    static int ApplyStats(string modsFolder, List<Change> changes, EntityProfile profile)
    {
        for (var attempt = 1; ; attempt++)
        {
            var (document, revision) = StatsEditorData.Load(modsFolder);
            var all = (JsonObject)document.DeepClone();
            foreach (var change in changes) Put(all, change);
            var applied = changes.Count;
            if (!Valid(all, profile)) (all, applied) = ApplyEach(document, changes, profile);
            try
            {
                StatsEditorData.Save(modsFolder, all, revision);
                return applied;
            }
            catch (InvalidDataException) when (attempt < 3 && StatsEditorData.Load(modsFolder).Revision != revision) { } // saved meanwhile
            catch (IOException) when (attempt < 20) { Thread.Sleep(250); } // the launcher saving: its lock is held
        }
    }

    // The changes one by one onto the document, leaving out what the stats pass would refuse: the document, and how many
    // went in.
    static (JsonObject Document, int Applied) ApplyEach(JsonObject document, List<Change> changes, EntityProfile profile)
    {
        var applied = 0;
        foreach (var change in changes)
        {
            var trial = (JsonObject)document.DeepClone();
            Put(trial, change);
            if (!Valid(trial, profile)) continue;
            document = trial;
            applied++;
        }
        return (document, applied);
    }

    static bool Valid(JsonObject document, EntityProfile profile)
    {
        if (document.Count == 1 && document["format"] != null) return true;
        try { StatsData.Parse(document.ToJsonString(), profile); return true; }
        catch (Exception error) when (error is InvalidDataException or JsonException or FormatException) { return false; }
    }

    // As the editor's Apply: a removal takes its target, then its category, with it once nothing is left.
    static void Put(JsonObject document, Change change)
    {
        if (change.Value is not { } value)
        {
            if (document[change.Category] is not JsonObject category || category[change.Target] is not JsonObject target) return;
            target.Remove(change.Key);
            if (target.Count == 0) category.Remove(change.Target);
            if (category.Count == 0) document.Remove(change.Category);
            return;
        }
        if (document[change.Category] is not JsonObject section) document[change.Category] = section = [];
        if (section[change.Target] is not JsonObject entry) section[change.Target] = entry = [];
        entry[change.Key] = Math.Floor(value) == value && Math.Abs(value) < 1e15 ? JsonValue.Create((long)value) : JsonValue.Create(value);
    }

    // The editor's mod content values (mod-stats.json in its folder): read as the editor would (a broken file is a new
    // one), the changes put in, written whole.
    static void ApplyModValues(string path, List<Change> changes)
    {
        JsonObject document;
        try { document = (File.Exists(path) ? JsonNode.Parse(Text(path, MaxChangesBytes)) as JsonObject : null) ?? new JsonObject { ["format"] = 1 }; }
        catch (JsonException) { document = new JsonObject { ["format"] = 1 }; }
        foreach (var change in changes) Put(document, change);
        Write(path, document);
    }

    static bool CopySettings(string from, string to)
    {
        if (JsonNode.Parse(Text(from, MaxSettingsBytes)) is not JsonObject settings) throw new InvalidDataException("The session's editor settings are not an object.");
        Write(to, settings);
        return true;
    }

    // Exported presets: *.hd2rpreset.json with a plain name, each a JSON object the editor marks as its preset.
    static int CopyPresets(string from, string to)
    {
        var copied = 0;
        foreach (var file in Directory.EnumerateFiles(from).Take(MaxPresets))
        {
            var name = Path.GetFileName(file);
            if (!PresetName().IsMatch(name)) continue;
            try
            {
                if (JsonNode.Parse(Text(file, MaxPresetBytes)) is not JsonObject preset || (string?)preset["format"] != "hd2r-editor-preset") continue;
                Directory.CreateDirectory(to);
                Write(Path.Combine(to, name), preset);
                copied++;
            }
            catch (Exception error) when (error is JsonException or InvalidDataException or InvalidOperationException) { }
        }
        return copied;
    }

    [GeneratedRegex(@"^[A-Za-z0-9 _.()'-]{1,80}\.hd2rpreset\.json$")]
    private static partial Regex PresetName();

    static string Text(string path, long limit)
    {
        LocalInstall.NoLinks(path);
        if (new FileInfo(path).Length > limit) throw new InvalidDataException($"{Path.GetFileName(path)} from the session is too large.");
        return File.ReadAllText(path);
    }

    static void Write(string path, JsonObject document)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, document.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
