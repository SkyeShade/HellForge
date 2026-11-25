using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The game's catalogues the backend serves (items, item and progression packages, levels, warbond list): HellForge ships
// none. Sync Inventory keeps the normal game's own reads of them (CaptureCompletion.Catalogues) in catalogue/captured,
// and Assemble writes catalogue/progression-data.json and item-catalog.json from them, which every reader takes.
// The live API's catalogues are newer than the supported build: only the ids in the pinned file (catalogue-pinned.json)
// are kept, in the API's order; newer entries stay captured for a backport. The parsers look keys up by name
// (game.dll 0x1041970), so the API's extra keys are harmless.
public static class GameCatalog
{
    public const string Folder = "catalogue";
    public const string CapturedFolder = "captured";
    // The supported build's ids, plus what a passive capture never sees: the warbond pages (/SeasonPass/<id32>, read when a
    // warbond is opened) and the requisition store (/StoreFront).
    public const string PinnedFile = "catalogue-pinned.json";
    public const string ProgressionDataFile = "progression-data.json";
    public const string ItemCatalogFile = "item-catalog.json";
    // When the captured catalogues were taken (UTC), written beside them.
    public const string CapturedStampFile = "captured.json";

    public static string Directory(string installRoot) => Path.Combine(installRoot, Folder);
    public static string CapturedDirectory(string installRoot) => Path.Combine(Directory(installRoot), CapturedFolder);
    public static string ProgressionData(string installRoot) => Path.Combine(Directory(installRoot), ProgressionDataFile);
    public static string ItemCatalog(string installRoot) => Path.Combine(Directory(installRoot), ItemCatalogFile);
    public static string Pinned(string installRoot) => Path.Combine(installRoot, "app", PinnedFile);

    // What a session needs before it can start: the assembled catalogues, or why they are missing.
    public const string SyncFirst = "This installation has no game catalogue yet. Run Sync Inventory once (Profile > Account sync): " +
        "it copies the item and warbond catalogues from the normal game, which HellForge does not ship.";

    // What the launcher says when a session starts without a sync.
    public const string WithoutSync = "No Sync Inventory yet: this session plays without warbonds, prices or unlock levels. " +
        "Sync Inventory (Profile > Account sync) copies them from the normal game.";

    // No sync has given this installation a catalogue yet: a session plays with the fallback (Fallback), and the launcher
    // says what a sync adds. It reads only.
    public static bool Missing(string installRoot) =>
        !Captured(installRoot) && (!Outputs(installRoot).All(File.Exists) || File.Exists(FallbackMarker(installRoot)));

    // Written beside the assembled files when they are the fallback, so the first sync replaces them.
    public const string FallbackFile = "fallback.json";
    static string FallbackMarker(string installRoot) => Path.Combine(Directory(installRoot), FallbackFile);

    static bool Captured(string installRoot) =>
        System.IO.Directory.Exists(CapturedDirectory(installRoot)) &&
        File.Exists(Path.Combine(CapturedDirectory(installRoot), CapturedStampFile));

    static string[] Outputs(string installRoot) => [ProgressionData(installRoot), ItemCatalog(installRoot)];

    // Brings the assembled files up to date with the captured catalogues and the pinned file, when either is newer.
    // Null when the assembled files are there and current; otherwise why a session cannot start.
    public static string? Ensure(string installRoot)
    {
        var captured = CapturedDirectory(installRoot);
        var pinned = Pinned(installRoot);
        var outputs = Outputs(installRoot);
        if (!File.Exists(pinned)) return outputs.All(File.Exists) && !Captured(installRoot) ? null
            : "HellForge's pinned catalogue (" + pinned + ") is missing: repair or reinstall HellForge.";
        if (!Captured(installRoot)) return outputs.All(File.Exists) ? null : WriteFallback(installRoot, pinned);
        var fallback = File.Exists(FallbackMarker(installRoot)); /* a sync replaces the fallback */
        var newest = new[] { pinned }.Concat(CaptureCompletion.Catalogues.Select(entry => Path.Combine(captured, entry.File)))
            .Where(File.Exists).Max(File.GetLastWriteTimeUtc);
        if (!fallback && outputs.All(file => File.Exists(file) && File.GetLastWriteTimeUtc(file) >= newest)) return null;
        try
        {
            Write(installRoot, Assemble(captured, pinned));
            File.Delete(FallbackMarker(installRoot));
            return null;
        }
        catch (Exception error) when (error is InvalidDataException or JsonException or IOException or UnauthorizedAccessException)
        {
            return outputs.All(File.Exists) ? null : "The game catalogue from the last Sync Inventory cannot be used (" + error.Message + "): sync again.";
        }
    }

    // No sync yet: the fallback catalogue, marked as such. Null when written, else why a session cannot start.
    static string? WriteFallback(string installRoot, string pinned)
    {
        try
        {
            Write(installRoot, Fallback(pinned));
            File.WriteAllText(FallbackMarker(installRoot), new JsonObject { ["writtenUtc"] = DateTime.UtcNow.ToString("O") }.ToJsonString());
            return null;
        }
        catch (Exception error) when (error is InvalidDataException or JsonException or IOException or UnauthorizedAccessException or KeyNotFoundException or InvalidOperationException)
        {
            return SyncFirst + " (Without one HellForge could not make its own: " + error.Message + ")";
        }
    }

    // The assembled files copied into a session's run folder, which the backend can always read and a sync during the
    // session never changes; null when they are missing.
    public static string? CopyForSession(string installRoot, string runFolder)
    {
        var folder = System.IO.Directory.CreateDirectory(Path.Combine(runFolder, Folder)).FullName;
        foreach (var source in new[] { ProgressionData(installRoot), ItemCatalog(installRoot) })
        {
            if (!File.Exists(source)) return null;
            File.Copy(source, Path.Combine(folder, Path.GetFileName(source)), overwrite: true);
        }
        return folder;
    }

    // A capture's catalogue folder, checked: every catalogue there and each a JSON list. Null when usable, else why not.
    public static string? CheckCaptured(string folder)
    {
        foreach (var (_, file) in CaptureCompletion.Catalogues)
        {
            var path = Path.Combine(folder, file);
            if (!File.Exists(path)) return file + " was not captured";
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllBytes(path));
                if (document.RootElement.ValueKind != JsonValueKind.Array || document.RootElement.GetArrayLength() == 0)
                    return file + " is not a list of entries";
            }
            catch (JsonException) { return file + " is not JSON"; }
        }
        return null;
    }

    // Moves a capture's checked catalogues into the installation (replacing the last ones) and assembles them.
    public static void Install(string installRoot, string captureFolder, DateTime capturedUtc)
    {
        if (CheckCaptured(captureFolder) is { } problem) throw new InvalidDataException(problem);
        var target = CapturedDirectory(installRoot);
        var staging = target + ".new";
        if (System.IO.Directory.Exists(staging)) System.IO.Directory.Delete(staging, recursive: true);
        System.IO.Directory.CreateDirectory(staging);
        foreach (var (_, file) in CaptureCompletion.Catalogues) File.Copy(Path.Combine(captureFolder, file), Path.Combine(staging, file));
        File.WriteAllText(Path.Combine(staging, CapturedStampFile),
            new JsonObject { ["capturedUtc"] = capturedUtc.ToUniversalTime().ToString("O") }.ToJsonString());
        if (System.IO.Directory.Exists(target)) System.IO.Directory.Delete(target, recursive: true);
        System.IO.Directory.Move(staging, target);
        Write(installRoot, Assemble(target, Pinned(installRoot)));
        File.Delete(FallbackMarker(installRoot));
    }

    // A catalogue from the pinned file alone, for an installation that has had no sync (some players cannot sync): the
    // build's ids, with nothing of the game's server data. Items cost nothing and need no level, packages grant nothing,
    // no warbond is listed (their pages ship, but the list that names them is the server's), and the level table is
    // HellForge's own curve. The pinned warbond pages and requisition store ship as they do with a sync.
    public static Assembled Fallback(string pinnedFile)
    {
        using var pinnedDocument = JsonDocument.Parse(File.ReadAllBytes(pinnedFile));
        var pinned = pinnedDocument.RootElement;
        var items = new JsonArray();
        foreach (var pair in pinned.GetProperty("items").EnumerateArray())
            items.Add((JsonNode)new JsonObject
            {
                ["itemId"] = pair[1].GetUInt32(), ["mixId"] = pair[0].GetUInt32(), ["isConsumable"] = false, ["requiredLevel"] = 0, ["progressionCategory"] = 0,
                ["tags"] = new JsonArray(), ["requiredItems"] = new JsonArray(), ["buyPrice"] = new JsonArray(), ["sellPrice"] = new JsonArray(),
            });
        if (items.Count == 0) throw new InvalidDataException("the pinned catalogue lists no items");
        var levels = new JsonArray();
        for (var level = 1; level <= FallbackLevels; level++)
            levels.Add((JsonNode)new JsonObject { ["level"] = level, ["requiredExperience"] = level == 1 ? 1 : 50 * level * (level - 1) });
        var itemPackages = new JsonArray([.. pinned.GetProperty("itemPackages").EnumerateArray()
            .Select(id => (JsonNode?)new JsonObject { ["itemMixId32"] = id.GetUInt32(), ["items"] = new JsonArray() })]);
        const string scope = "HellForge's own catalogue for an installation without a Sync Inventory: the supported build's ids only (GameCatalog.Fallback).";
        var data = new JsonObject
        {
            ["scope"] = scope,
            ["build"] = pinned.TryGetProperty("build", out var build) ? build.GetString() : null,
            ["levelSpec"] = levels,
            ["itemPackages"] = itemPackages,
            ["seasonPass"] = new JsonArray(),
            ["seasonPassDetails"] = JsonNode.Parse(pinned.GetProperty("seasonPassDetails").GetRawText()),
            ["progressionPackages"] = new JsonArray(),
            ["storeFront"] = JsonNode.Parse(pinned.GetProperty("storeFront").GetRawText())
        };
        return new Assembled(data, new JsonObject { ["scope"] = scope, ["build"] = data["build"]?.GetValue<string>(), ["items"] = items });
    }

    const int FallbackLevels = 150;

    public sealed record Assembled(JsonObject ProgressionData, JsonObject ItemCatalog);

    // The captured catalogues cut to the pinned ids, in the API's order, with the pinned warbond pages and store.
    public static Assembled Assemble(string capturedFolder, string pinnedFile)
    {
        using var pinnedDocument = JsonDocument.Parse(File.ReadAllBytes(pinnedFile));
        var pinned = pinnedDocument.RootElement;
        var items = Keys(pinned, "items", entry => ItemKey(entry));
        var itemPackages = Keys(pinned, "itemPackages", entry => entry.GetUInt32().ToString());
        var progressionPackages = Keys(pinned, "progressionPackages", entry => entry.GetUInt32().ToString());
        var seasonPass = Keys(pinned, "seasonPass", entry => entry.GetUInt32().ToString());

        JsonArray Read(string file, Func<JsonNode, string?> key, HashSet<string>? keep)
        {
            var node = JsonNode.Parse(File.ReadAllBytes(Path.Combine(capturedFolder, file))) as JsonArray
                ?? throw new InvalidDataException(file + " is not a list");
            var kept = new JsonArray();
            foreach (var entry in node.ToArray())
            {
                if (entry == null) continue;
                if (keep != null && (key(entry) is not { } id || !keep.Contains(id))) continue;
                node.Remove(entry);
                kept.Add(entry);
            }
            return kept;
        }

        var keptItems = Read("items.json", entry => entry is JsonObject item ? ItemKey(item) : null, items);
        var data = new JsonObject
        {
            ["scope"] = "The game's catalogues from this installation's last Sync Inventory, cut to the ids the supported build knows (GameCatalog).",
            ["build"] = pinned.TryGetProperty("build", out var build) ? build.GetString() : null,
            ["levelSpec"] = Read("levelspec.json", _ => null, null),
            ["itemPackages"] = Read("ItemPackages.json", entry => Id(entry, "itemMixId32"), itemPackages),
            ["seasonPass"] = Read("SeasonPass.json", entry => Id(entry, "id32"), seasonPass),
            ["seasonPassDetails"] = JsonNode.Parse(pinned.GetProperty("seasonPassDetails").GetRawText()),
            ["progressionPackages"] = Read("ProgressionPackages.json", entry => Id(entry, "id32"), progressionPackages),
            ["storeFront"] = JsonNode.Parse(pinned.GetProperty("storeFront").GetRawText())
        };
        // A catalogue with none of the pinned build's entries is not this game's: never served as if it were.
        if (keptItems.Count == 0 || ((JsonArray)data["seasonPass"]!).Count == 0)
            throw new InvalidDataException("the captured catalogues share no entries with the supported build");
        var catalog = new JsonObject
        {
            ["scope"] = data["scope"]!.GetValue<string>(),
            ["build"] = data["build"]?.GetValue<string>(),
            ["items"] = keptItems
        };
        return new Assembled(data, catalog);
    }

    static void Write(string installRoot, Assembled assembled)
    {
        System.IO.Directory.CreateDirectory(Directory(installRoot));
        WriteAtomically(ProgressionData(installRoot), assembled.ProgressionData.ToJsonString());
        WriteAtomically(ItemCatalog(installRoot), assembled.ItemCatalog.ToJsonString());
    }

    static void WriteAtomically(string path, string text)
    {
        File.WriteAllText(path + ".tmp", text);
        File.Move(path + ".tmp", path, overwrite: true);
    }

    static HashSet<string> Keys(JsonElement pinned, string name, Func<JsonElement, string> key) =>
        pinned.TryGetProperty(name, out var list) && list.ValueKind == JsonValueKind.Array
            ? [.. list.EnumerateArray().Select(key)]
            : throw new InvalidDataException("the pinned catalogue has no " + name + " list");

    // An item is its mixId and itemId together: one mixId can be listed under several items.
    static string ItemKey(JsonElement entry) => entry[0].GetUInt32() + ":" + entry[1].GetUInt32();

    static string? ItemKey(JsonObject item) =>
        Id(item, "mixId") is { } mix && Id(item, "itemId") is { } id ? mix + ":" + id : null;

    static string? Id(JsonNode entry, string name) =>
        entry is JsonObject value && value[name] is JsonValue id && id.TryGetValue<long>(out var number) ? number.ToString() : null;
}
