using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The single offline player profile behind the per-player startup endpoints. New profiles start
// from the parser-tested startup fixtures. Hand edits are validated on load; an invalid file is
// reported, never replaced. Only writes whose request builders are decoded are accepted.
public sealed class ProfileStore : IDisposable
{
    public const int SchemaVersion = 1;
    public const string FileName = "profile.json";

    // Profile section -> endpoint path served from it.
    static readonly (string Section, string Path)[] Sections =
    [
        ("progression", "/Progression"),
        ("inventory", "/Progression/inventory"),
        ("customization", "/Progression/Items/Customize/v2")
    ];
    // Valid onboarding values from apply routine 0x1038bf0; the client normalizes others to 128.
    static readonly int[] OnboardingStates = [0, 1, 2, 3, 4, 128];
    // Ship name word indices; the success handler 0xb6b420 resets values at or above these to 0.
    static readonly int[] ShipNameLimits = [90, 60];
    static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    readonly object gate = new();
    readonly JsonObject profile;
    readonly FileStream? lease;

    public string? FilePath { get; }
    public bool Created { get; }

    // When set, inventory responses follow its premium rules; Super Credits are never served either way.
    public OwnershipFilter? Ownership { get; set; }

    ProfileStore(JsonObject profile, string? filePath, FileStream? lease, bool created)
    {
        this.profile = profile;
        FilePath = filePath;
        this.lease = lease;
        Created = created;
    }

    // Inventory templates for new profiles (profile-templates.json). Without one, a new profile keeps
    // the fixture inventory (empty). "unlock-all" is a development template.
    public static IReadOnlyList<string> TemplateNames { get; } = LoadTemplateNames();

    static IReadOnlyList<string> LoadTemplateNames()
    {
        using var document = LocalBackend.LoadResource("profile-templates.json");
        return document.RootElement.GetProperty("templates").EnumerateObject().Select(t => t.Name).ToArray();
    }

    static JsonArray TemplateItems(string template)
    {
        using var document = LocalBackend.LoadResource("profile-templates.json");
        if (!document.RootElement.GetProperty("templates").TryGetProperty(template, out var found))
            throw new ArgumentException($"Unknown profile template '{template}'. Known: {string.Join(", ", TemplateNames)}.");
        var items = new JsonArray();
        foreach (var item in found.GetProperty("items").EnumerateArray().Where(i => !PaidContentSet.IsSuperCredits(i.GetProperty("mixId").GetUInt32())))
            items.Add((JsonNode?)new JsonObject { ["mixId"] = item.GetProperty("mixId").GetUInt32(), ["amount"] = item.GetProperty("amount").GetInt32(),
                ["isDiscovered"] = item.GetProperty("isDiscovered").GetBoolean() });
        return items;
    }

    // A null directory keeps an unsaved profile with fixture defaults. The template applies only when
    // the profile is created; an existing profile keeps its own inventory.
    public static ProfileStore Open(string? directory, string? template = null)
    {
        if (template != null) _ = TemplateItems(template);
        if (directory == null) return new ProfileStore(CreateDefault(template), null, null, true);
        directory = Path.GetFullPath(directory);
        Directory.CreateDirectory(directory);
        FileStream lease;
        try { lease = new FileStream(Path.Combine(directory, "profile.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) { throw new IOException("Profile is in use by another local backend: " + directory); }
        try
        {
            var path = Path.Combine(directory, FileName);
            if (File.Exists(path))
            {
                var existing = Load(path);
                if (NameShipIfIntroSkipped(existing) | GrantOnboardingRewards(existing) | GrantStandardStratagems(existing) | GrantTemplateBoosters(existing)) Save(path, existing);
                return new ProfileStore(existing, path, lease, false);
            }
            var profile = CreateDefault(template);
            Save(path, profile);
            return new ProfileStore(profile, path, lease, true);
        }
        catch
        {
            lease.Dispose();
            throw;
        }
    }

    // Validates a saved profile without taking the server lease.
    public static JsonObject Load(string path)
    {
        JsonNode? node;
        try { node = JsonNode.Parse(File.ReadAllText(path)); }
        catch (JsonException error) { throw new InvalidDataException($"Profile is not valid JSON: {error.Message}"); }
        if (node is not JsonObject profile) throw new InvalidDataException("Profile root must be an object.");
        Validate(profile);
        return profile;
    }

    public bool TryGetResponse(string path, out JsonElement body)
    {
        foreach (var (section, sectionPath) in Sections)
        {
            if (sectionPath != path) continue;
            lock (gate)
            {
                body = JsonSerializer.SerializeToElement(profile[section], BackendJson.Default.JsonNode);
                if (section == "inventory")
                    body = Ownership?.FilterInventoryResponse(body) ?? OwnershipFilter.WithoutSuperCredits(body);
            }
            return true;
        }
        body = default;
        return false;
    }

    // PUT /Progression/ShipName (builder 0x12bd3d0): {"shipName":[first, second]}. The client reads the
    // same array back from GET /Progression. Returns false for a body the client would not send.
    public bool TrySetShipName(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty("shipName", out var name)) return false;
        JsonArray value;
        try { value = (JsonArray)JsonNode.Parse(name.GetRawText())!; ValidateShipName(value); }
        catch (Exception error) when (error is InvalidDataException or InvalidCastException) { return false; }
        return Update(progression => progression["shipName"] = value);
    }

    // POST /Account/ReportPosition (builders 0x12bcf40, 0x12db270): where the Super Destroyer is, {"warId", "planetIndex",
    // "territoryIndex"?, "regionIndex"?}, sent as it moves. GET /Progression returns it as "location" (parser 0x1063320,
    // from 0x10624d0: warId and planetIndex required, the others optional), where the client places the ship at startup.
    // The builder leaves territoryIndex out when it is negative but always writes regionIndex, -1 away from a region:
    // a negative optional index is "none" and is not kept.
    public bool TrySetLocation(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object) return false;
        JsonObject value;
        try
        {
            var reported = JsonNode.Parse(body.GetRawText())!.AsObject();
            value = new JsonObject();
            foreach (var key in new[] { "warId", "planetIndex", "territoryIndex", "regionIndex" })
            {
                if (!reported.ContainsKey(key)) { if (key is "warId" or "planetIndex") return false; continue; }
                if (key is "territoryIndex" or "regionIndex" && reported[key] is JsonValue index &&
                    index.GetValueKind() == JsonValueKind.Number && index.ToJsonString() == "-1") continue;
                value[key] = Integer(reported, key, "location");
            }
            ValidateLocation(value);
        }
        catch (InvalidDataException) { return false; }
        return Update(progression => progression["location"] = value);
    }

    // PUT /Progression/OnboardingState (builders 0x12be3a0, 0x1355f00): state and flags as GET /Progression reports them.
    public bool TrySetOnboarding(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object) return false;
        JsonObject value;
        try
        {
            value = JsonNode.Parse(body.GetRawText())!.AsObject();
            if (!OnboardingStates.Contains((int)Integer(value, "onboardingState", "onboarding"))) return false;
            Integer(value, "onboardingFlags", "onboarding");
        }
        catch (InvalidDataException) { return false; }
        return Update(progression =>
        {
            progression["onboardingState"] = value["onboardingState"]!.DeepClone();
            progression["onboardingFlags"] = value["onboardingFlags"]!.DeepClone();
        });
    }

    // PUT /Progression/Items/Customize/v2 (builder 0x12bac70, after ItemCustomizationManager updates or equips a
    // template): {"mixId": weapon, "activeIndex": template (-1: defaults), "modes": [4], and, when a template changed,
    // "customizations": [{"index": 0-2, "slots": [8 attachment ids]}]}. GET returns the same state per weapon as
    // {"items": [{"mixId32", "modes", "activeIndex", "customizations": [{"index", "slots"}]}]} (parsers 0x1066420,
    // 0x10665a0, 0x10669b0). Returns the updated customization section, or null for a body the client would not send.
    public JsonElement? TrySetCustomization(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty("mixId", out var mix) || !mix.TryGetUInt32(out var weapon) ||
            weapon == 0 || !body.TryGetProperty("activeIndex", out var active) || !active.TryGetInt32(out var activeIndex) ||
            activeIndex is < -1 or > 2) return null;
        var modes = new JsonArray();
        if (body.TryGetProperty("modes", out var modeList))
        {
            if (modeList.ValueKind != JsonValueKind.Array || modeList.GetArrayLength() > 8) return null;
            if (!TryIds(modeList, out modes)) return null;
        }
        if (Templates(body) is not { } templates) return null;
        lock (gate) return SetCustomization(weapon, activeIndex, modes, templates);
    }

    // A list of ids (0 to uint.MaxValue each: modes, attachments), or false when any is not one.
    static bool TryIds(JsonElement list, out JsonArray ids)
    {
        ids = new JsonArray();
        foreach (var item in list.EnumerateArray())
            if (item.TryGetInt64(out var value) && value is >= 0 and <= uint.MaxValue) ids.Add((JsonNode?)value); else return false;
        return true;
    }

    // A customization body's templates (none without "customizations"), or null when they are not up to 3 of index 0-2
    // with up to 10 slots each.
    static List<(int Index, JsonArray Slots)>? Templates(JsonElement body)
    {
        var templates = new List<(int Index, JsonArray Slots)>();
        if (!body.TryGetProperty("customizations", out var customizations)) return templates;
        if (customizations.ValueKind != JsonValueKind.Array || customizations.GetArrayLength() > 3) return null;
        foreach (var template in customizations.EnumerateArray())
        {
            if (template.ValueKind != JsonValueKind.Object || !template.TryGetProperty("index", out var index) ||
                !index.TryGetInt32(out var at) || at is < 0 or > 2 || !template.TryGetProperty("slots", out var slots) ||
                slots.ValueKind != JsonValueKind.Array || slots.GetArrayLength() > 10) return null;
            if (!TryIds(slots, out var values)) return null;
            templates.Add((at, values));
        }
        return templates;
    }

    // Called with gate held.
    JsonElement SetCustomization(uint weapon, int activeIndex, JsonArray modes, List<(int Index, JsonArray Slots)> templates)
    {
        var updated = profile.DeepClone().AsObject();
        var section = (JsonObject)updated["customization"]!;
        var items = section["items"] as JsonArray ?? [];
        section["items"] = items;
        var item = items.OfType<JsonObject>().FirstOrDefault(i => Whole(i["mixId32"]) == weapon);
        if (item == null)
        {
            item = new JsonObject { ["mixId32"] = weapon, ["modes"] = new JsonArray(), ["activeIndex"] = -1, ["customizations"] = new JsonArray() };
            items.Add((JsonNode?)item);
        }
        item["modes"] = modes;
        item["activeIndex"] = activeIndex;
        var stored = item["customizations"] as JsonArray ?? [];
        item["customizations"] = stored;
        foreach (var (index, slots) in templates)
        {
            if (stored.OfType<JsonObject>().FirstOrDefault(t => Whole(t["index"]) == index) is { } existing)
                existing["slots"] = slots;
            else stored.Add((JsonNode?)new JsonObject { ["index"] = index, ["slots"] = slots });
        }
        Validate(updated);
        if (FilePath != null) Save(FilePath, updated);
        profile["customization"] = updated["customization"]!.DeepClone();
        return JsonSerializer.SerializeToElement(profile["customization"], BackendJson.Default.JsonNode);
    }

    // A JSON integer, however its node was made (a JsonValue made from a uint does not convert to long).
    static long? Whole(JsonNode? node) =>
        node is JsonValue value && value.GetValueKind() == JsonValueKind.Number &&
        long.TryParse(value.ToJsonString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number) ? number : null;

    bool Update(Action<JsonObject> edit)
    {
        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            edit((JsonObject)updated["progression"]!);
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            profile["progression"] = updated["progression"]!.DeepClone();
        }
        return true;
    }

    // An account import (AccountImport, the launcher's Profile > Account sync): the saved profile is backed up first.
    public void Import(AccountSnapshot snapshot, HellForge.Core.AccountImportSettings settings, PaidContentSet paid)
    {
        lock (gate)
        {
            var updated = AccountImport.Merge(profile, snapshot, settings, paid);
            Validate(updated);
            Commit(updated, "import");
        }
    }

    // Puts a backup (HellForge.Core ProfileBackups) back; the profile it replaces is backed up first.
    public void Restore(string backupPath)
    {
        var restored = Load(backupPath);
        lock (gate) Commit(restored, "restore");
    }

    // A copy of the profile as it is now.
    public JsonObject Current()
    {
        lock (gate) return (JsonObject)profile.DeepClone();
    }

    void Commit(JsonObject updated, string reason)
    {
        if (FilePath != null)
        {
            HellForge.Core.ProfileBackups.Save(Path.GetDirectoryName(FilePath)!, reason, DateTime.UtcNow);
            Save(FilePath, updated);
        }
        foreach (var (section, _) in Sections) profile[section] = updated[section]!.DeepClone();
    }

    // Mission end: adds experience and item amounts (samples, requisition). Amounts are clamped to the catalogue
    // maxCap when one exists; unknown items are added as discovered inventory entries. Super Credits are never granted,
    // whatever the mission report claims was found.
    public void ApplyMissionRewards(long experience, IReadOnlyDictionary<uint, long> items)
    {
        var caps = new Dictionary<uint, long>();
        foreach (var item in LocalBackend.LoadStartupFixtures()["/Progression/items"].EnumerateArray())
            if (item.TryGetProperty("maxCap", out var cap)) caps[item.GetProperty("mixId").GetUInt32()] = cap.GetInt64();
        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            var progression = (JsonObject)updated["progression"]!;
            progression["experience"] = Math.Min(uint.MaxValue, Integer(progression, "experience", "progression") + Math.Max(0, experience));
            var inventory = (JsonObject)updated["inventory"]!;
            if (inventory["items"] is not JsonArray list) inventory["items"] = list = new JsonArray();
            foreach (var (mix, amount) in items.Where(item => !PaidContentSet.IsSuperCredits(item.Key)))
            {
                var entry = list.OfType<JsonObject>().FirstOrDefault(e => e["mixId"] is JsonNode id && Number(id) == mix);
                if (entry == null) list.Add((JsonNode?)(entry = new JsonObject { ["mixId"] = mix, ["amount"] = 0, ["isDiscovered"] = true }));
                var total = Number(entry["amount"]!) + amount;
                entry["amount"] = Math.Min(caps.TryGetValue(mix, out var cap) ? cap : uint.MaxValue, total);
            }
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            profile["progression"] = updated["progression"]!.DeepClone();
            profile["inventory"] = updated["inventory"]!.DeepClone();
        }
    }

    // JSON numbers in the profile may be stored as any integer type; read them uniformly.
    static long Number(JsonNode node) => long.Parse(node.ToJsonString(), CultureInfo.InvariantCulture);

    // Replaces the inventory with a template's items and merges its progression fields (package progress,
    // warbond purchases, onboarding flags), for example to turn a profile into a dev profile.
    public void ApplyTemplate(string template)
    {
        var items = TemplateItems(template);
        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            updated["inventory"] = new JsonObject { ["items"] = items };
            using (var document = LocalBackend.LoadResource("profile-templates.json"))
                if (document.RootElement.GetProperty("templates").GetProperty(template).TryGetProperty("progression", out var extra))
                    foreach (var field in extra.EnumerateObject())
                        ((JsonObject)updated["progression"]!)[field.Name] = JsonNode.Parse(field.Value.GetRawText());
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            profile["inventory"] = updated["inventory"]!.DeepClone();
            profile["progression"] = updated["progression"]!.DeepClone();
        }
    }

    // Adds the template's items the inventory lacks and leaves everything else (earned amounts, progression)
    // as it is. Returns the number of items added.
    public int AddMissingTemplateItems(string template)
    {
        var items = TemplateItems(template);
        lock (gate)
        {
            var updated = profile.DeepClone().AsObject();
            var inventory = (JsonArray)updated["inventory"]!["items"]!;
            var held = inventory.Select(item => Number(item!["mixId"]!)).ToHashSet();
            var added = 0;
            foreach (var item in items)
                if (!held.Contains(Number(item!["mixId"]!)))
                {
                    inventory.Add(item.DeepClone());
                    added++;
                }
            if (added == 0) return 0;
            Validate(updated);
            if (FilePath != null) Save(FilePath, updated);
            profile["inventory"] = updated["inventory"]!.DeepClone();
            return added;
        }
    }

    public void Dispose() => lease?.Dispose();

    static JsonObject CreateDefault(string? template)
    {
        var fixtures = LocalBackend.LoadStartupFixtures();
        var profile = new JsonObject { ["schemaVersion"] = SchemaVersion };
        foreach (var (section, path) in Sections) profile[section] = JsonNode.Parse(fixtures[path].GetRawText());
        if (template != null)
        {
            profile["inventory"] = new JsonObject { ["items"] = TemplateItems(template) };
            // Template progression fields (package progress, onboarding flags) override the fixture defaults.
            using var document = LocalBackend.LoadResource("profile-templates.json");
            if (document.RootElement.GetProperty("templates").GetProperty(template).TryGetProperty("progression", out var extra))
                foreach (var field in extra.EnumerateObject())
                    ((JsonObject)profile["progression"]!)[field.Name] = JsonNode.Parse(field.Value.GetRawText());
        }
        NameShipIfIntroSkipped(profile);
        GrantOnboardingRewards(profile);
        GrantStandardStratagems(profile);
        Validate(profile);
        return profile;
    }

    // Boosters added to the unlock-all template after a profile was made from it (Integrated Extinguishers,
    // FireExtinguish 3545987157, a 0.1.8 report: the account the catalogue was rebuilt from lacked it). A profile that
    // holds every permanent booster (progression category 5, not consumable) is an unlock-all one, and gets the
    // template's boosters it lacks; one still earning its boosters is left alone. Nothing is removed.
    const int BoosterCategory = 5;
    static bool GrantTemplateBoosters(JsonObject profile)
    {
        List<(long Mix, bool Consumable)> boosters;
        using (var catalog = LocalBackend.LoadResource("item-catalog.json"))
            boosters = [.. catalog.RootElement.GetProperty("items").EnumerateArray()
                .Where(i => i.TryGetProperty("progressionCategory", out var c) && c.GetInt32() == BoosterCategory)
                .Select(i => (i.GetProperty("mixId").GetInt64(), i.TryGetProperty("isConsumable", out var k) && k.GetBoolean()))];
        var inventory = (JsonArray)profile["inventory"]!["items"]!;
        var held = inventory.Select(item => Number(item!["mixId"]!)).ToHashSet();
        if (!boosters.Any(b => !b.Consumable) || boosters.Where(b => !b.Consumable).Any(b => !held.Contains(b.Mix))) return false;
        var template = TemplateItems("unlock-all").Select(item => Number(item!["mixId"]!)).ToHashSet();
        var added = 0;
        foreach (var (mix, _) in boosters)
            if (!held.Contains(mix) && template.Contains(mix))
            {
                inventory.Add((JsonNode?)new JsonObject { ["mixId"] = mix, ["amount"] = 1, ["isDiscovered"] = true });
                added++;
            }
        return added > 0;
    }

    // Stratagems every account owns from the start: the requisition terminal lists them with no price, so a profile
    // without them cannot get them at all (GitHub issue 9: a fresh profile could not "buy" either). Catalogue items
    // (progression category 11) whose stratagem ids are 0xd2062cbc (Orbital Precision Strike, type 118) and 0x1b4f8fa2
    // (MG-43 Machine Gun, type 92). Missing ones are added to every profile, new or saved before this.
    static readonly uint[] StandardStratagems = [1497469599, 3787370254];

    static bool GrantStandardStratagems(JsonObject profile)
    {
        // a profile with no template (development only: the launcher always picks one) keeps the parser-tested fixture
        // inventory ({}) exactly
        if (profile["inventory"]?["items"] is not JsonArray inventory) return false;
        var held = inventory.Select(item => Number(item!["mixId"]!)).ToHashSet();
        var added = 0;
        foreach (var mixId in StandardStratagems)
            if (held.Add(mixId))
            {
                inventory.Add((JsonNode?)new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
                added++;
            }
        return added > 0;
    }

    // The onboarding chain (progression package type 3, 0x1ebe30d9) gives a feature-unlock item at each level
    // (progression category 16): without them the war table shows planets with no missions and no difficulty bar, as
    // starter profiles did until 2026-10-15. A profile at level N of the chain holds the chain's required items and the
    // item rewards of levels 1..N; missing ones are added, nothing is removed.
    static bool GrantOnboardingRewards(JsonObject profile)
    {
        var progression = (JsonObject)profile["progression"]!;
        if (progression["packageProgress"] is not JsonArray progress || progress.Count == 0) return false;
        using var data = LocalBackend.LoadResource("progression-data.json");
        var chains = data.RootElement.GetProperty("progressionPackages").EnumerateArray()
            .Where(p => p.GetProperty("type").GetInt32() == 3).ToDictionary(p => p.GetProperty("id32").GetUInt32());
        var inventory = (JsonArray)profile["inventory"]!["items"]!;
        var held = inventory.Select(item => Number(item!["mixId"]!)).ToHashSet();
        var added = 0;
        void Grant(uint mixId)
        {
            if (!held.Add(mixId)) return;
            inventory.Add((JsonNode?)new JsonObject { ["mixId"] = mixId, ["amount"] = 1, ["isDiscovered"] = true });
            added++;
        }
        foreach (var entry in progress.OfType<JsonObject>())
        {
            if (entry["id32"] is not JsonValue id || !id.TryGetValue<long>(out var id32) || !chains.TryGetValue((uint)id32, out var chain)) continue;
            var level = entry["level"] is JsonValue l && l.TryGetValue<int>(out var reached) ? reached : 0;
            foreach (var mixId in ChainItems(chain, level)) Grant(mixId);
        }
        return added > 0;
    }

    // The items a chain gives a profile at its level: the chain's required items, then the item rewards of levels 1..level.
    static IEnumerable<uint> ChainItems(JsonElement chain, int level)
    {
        foreach (var required in chain.GetProperty("requiredItems").EnumerateArray()) yield return required.GetUInt32();
        foreach (var step in chain.GetProperty("levels").EnumerateArray())
            if (step.GetProperty("level").GetInt32() <= level)
                foreach (var reward in step.GetProperty("rewards").EnumerateArray())
                    if (reward.GetProperty("type").GetInt32() == 1) yield return reward.GetProperty("id32").GetUInt32();
    }

    // Onboarding flags (from a template, or a profile saved before this) mark the new-player intro as done, so the
    // client skips its ship-naming step and then waits in the cryo pod for a name that never comes. Such a profile
    // gets a random name; a profile without flags still plays the intro and names its ship there.
    static bool NameShipIfIntroSkipped(JsonObject profile)
    {
        var progression = (JsonObject)profile["progression"]!;
        if (progression.ContainsKey("shipName") || Integer(progression, "onboardingFlags", "progression") == 0) return false;
        progression["shipName"] = new JsonArray(JsonValue.Create(Random.Shared.Next(ShipNameLimits[0])),
            JsonValue.Create(Random.Shared.Next(ShipNameLimits[1])));
        return true;
    }

    static void Save(string path, JsonObject profile)
    {
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, profile.ToJsonString(Indented));
        if (File.Exists(path)) File.Replace(temporary, path, path + ".bak");
        else File.Move(temporary, path);
    }

    static void Validate(JsonObject profile)
    {
        if (Integer(profile, "schemaVersion", "profile") != SchemaVersion)
            throw new InvalidDataException($"Unsupported profile schemaVersion; expected {SchemaVersion}.");
        foreach (var key in profile.Select(property => property.Key))
            if (key != "schemaVersion" && Sections.All(section => section.Section != key))
                throw new InvalidDataException($"Unknown profile section '{key}'.");
        foreach (var (section, _) in Sections)
            if (profile[section] is not JsonObject)
                throw new InvalidDataException($"Profile section '{section}' must be an object.");

        // Fields required by the /Progression parser; optional extras pass through to the client.
        var progression = (JsonObject)profile["progression"]!;
        Integer(progression, "experience", "progression");
        if (!OnboardingStates.Contains((int)Integer(progression, "onboardingState", "progression")))
            throw new InvalidDataException("progression.onboardingState must be 0-4 or 128.");
        Integer(progression, "onboardingFlags", "progression");
        if (progression.TryGetPropertyValue("shipName", out var shipName))
            ValidateShipName(shipName as JsonArray ?? throw new InvalidDataException("progression.shipName must be an array."));
        if (progression.TryGetPropertyValue("location", out var location))
            ValidateLocation(location as JsonObject ?? throw new InvalidDataException("progression.location must be an object."));

        foreach (var item in Items((JsonObject)profile["inventory"]!, "inventory"))
        {
            Integer(item, "mixId", "inventory item");
            Integer(item, "amount", "inventory item");
            if (item["isDiscovered"]?.GetValueKind() is not (JsonValueKind.True or JsonValueKind.False))
                throw new InvalidDataException("inventory item.isDiscovered must be true or false.");
        }
        _ = Items((JsonObject)profile["customization"]!, "customization").ToArray();
    }

    static void ValidateLocation(JsonObject location)
    {
        if (Integer(location, "warId", "progression.location") < 0 || Integer(location, "planetIndex", "progression.location") < 0)
            throw new InvalidDataException("progression.location needs a warId and a planetIndex of 0 or more.");
        foreach (var key in new[] { "territoryIndex", "regionIndex" })
            if (location.ContainsKey(key)) Integer(location, key, "progression.location");
        foreach (var key in location.Select(property => property.Key))
            if (key is not ("warId" or "planetIndex" or "territoryIndex" or "regionIndex"))
                throw new InvalidDataException($"Unknown progression.location field '{key}'.");
    }

    static void ValidateShipName(JsonArray name)
    {
        if (name.Count != ShipNameLimits.Length) throw new InvalidDataException("progression.shipName must have two word indices.");
        for (var i = 0; i < name.Count; i++)
            if (name[i] is not JsonValue word || word.GetValueKind() != JsonValueKind.Number ||
                !int.TryParse(word.ToJsonString(), NumberStyles.None, CultureInfo.InvariantCulture, out var index) || index >= ShipNameLimits[i])
                throw new InvalidDataException($"progression.shipName[{i}] must be an integer from 0 to {ShipNameLimits[i] - 1}.");
    }

    // Item lists are optional, but the native parsers reject non-object elements.
    static IEnumerable<JsonObject> Items(JsonObject section, string name)
    {
        if (!section.TryGetPropertyValue("items", out var items)) yield break;
        if (items is not JsonArray array) throw new InvalidDataException($"{name}.items must be an array.");
        foreach (var item in array)
            yield return item as JsonObject ?? throw new InvalidDataException($"{name}.items entries must be objects.");
    }

    // Non-negative 32-bit integers, matching the client's unsigned fields.
    static long Integer(JsonObject owner, string property, string name)
    {
        if (owner[property] is JsonValue value && value.GetValueKind() == JsonValueKind.Number &&
            long.TryParse(value.ToJsonString(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number) &&
            number is >= 0 and <= uint.MaxValue) return number;
        throw new InvalidDataException($"{name}.{property} must be an integer from 0 to {uint.MaxValue}.");
    }
}
