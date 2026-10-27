using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// How a profile takes an account snapshot (Profile > Account sync): which categories are imported and how each settles a
// difference with what the profile earned in HellForge. Kept in the profile folder as account-import.json. Premium
// ownership (warbond tokens, Superstore items) is not a category: the backend always takes it from the sealed snapshot.
// Super Credits are never imported.
public enum ImportRule
{
    // numbers: the larger of the two; ship modules and weapon levels: the further of the two, per package
    Higher,
    // item lists: everything either side holds, each at the larger amount
    Merge,
    // the vanilla account's values replace the profile's
    Vanilla,
    // warbond unlocks: every item of every warbond the account owns, besides what either side holds
    All,
    // nothing of the category is imported (for a category without a checkbox)
    None
}

// Optional: the category has a checkbox. One without (warbond unlocks) is always shown, and None leaves it out.
public sealed record ImportCategory(string Id, string Label, bool IncludedByDefault, IReadOnlyList<ImportRule> Rules, bool Optional = true);

public sealed class AccountImportSettings
{
    public const string FileName = "account-import.json";

    public static IReadOnlyList<ImportCategory> Categories { get; } =
    [
        new("level", "Level and XP", true, [ImportRule.Higher, ImportRule.Vanilla]),
        // What the warbonds you own unlock: as on the vanilla account, every item, or nothing. Owning them is not a choice.
        new("warbondItems", "Warbond unlocks", true, [ImportRule.Merge, ImportRule.All, ImportRule.None], Optional: false),
        new("armory", "Other armory items", true, [ImportRule.Merge, ImportRule.Vanilla]),
        new("packages", "Ship modules and weapon levels", true, [ImportRule.Higher, ImportRule.Vanilla]),
        new("currencies", "Medals, requisition and samples", true, [ImportRule.Higher, ImportRule.Vanilla]),
        new("ship", "Ship name and location", false, [ImportRule.Vanilla]),
        new("onboarding", "Tutorial progress", false, [ImportRule.Vanilla])
    ];

    readonly Dictionary<string, (bool Include, ImportRule Rule)> choices = Categories.ToDictionary(
        category => category.Id, category => (category.IncludedByDefault, category.Rules[0]), StringComparer.Ordinal);

    // Later syncs apply with these choices without asking.
    public bool AutoApply { get; set; }

    public bool Includes(string category) => choices[category].Include && choices[category].Rule != ImportRule.None;

    public ImportRule RuleFor(string category) => choices[category].Rule;

    // A rule the category does not offer is refused, so a hand-edited file cannot ask for one. A category without a
    // checkbox is always included; its None rule is how it is left out.
    public void Set(string category, bool include, ImportRule rule)
    {
        var known = Categories.FirstOrDefault(c => c.Id == category) ?? throw new ArgumentException("Unknown import category " + category + ".");
        if (!known.Rules.Contains(rule)) throw new ArgumentException(known.Label + " cannot use " + rule + ".");
        choices[category] = (include || !known.Optional, rule);
    }

    public static string PathIn(string profileFolder) => Path.Combine(profileFolder, FileName);

    // A missing file gives the defaults; a category the file leaves out, or names a rule it does not offer, keeps its default.
    // A category without a checkbox that an older file left out loads as None.
    public static AccountImportSettings Load(string path)
    {
        var settings = new AccountImportSettings();
        if (!File.Exists(path)) return settings;
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        var root = document.RootElement;
        settings.AutoApply = root.TryGetProperty("autoApply", out var auto) && auto.ValueKind == JsonValueKind.True;
        if (!root.TryGetProperty("categories", out var categories) || categories.ValueKind != JsonValueKind.Object) return settings;
        foreach (var category in Categories)
            if (categories.TryGetProperty(category.Id, out var entry) && Choice(category, entry) is { } choice)
                settings.choices[category.Id] = choice;
        return settings;
    }

    static (bool Include, ImportRule Rule)? Choice(ImportCategory category, JsonElement entry)
    {
        if (entry.ValueKind != JsonValueKind.Object || !entry.TryGetProperty("include", out var include) ||
            include.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) return null;
        if (!category.Optional && !include.GetBoolean() && category.Rules.Contains(ImportRule.None)) return (true, ImportRule.None);
        if (!entry.TryGetProperty("rule", out var rule) || rule.ValueKind != JsonValueKind.String ||
            !Enum.TryParse<ImportRule>(rule.GetString(), out var parsed) || !category.Rules.Contains(parsed)) return null;
        return (include.GetBoolean() || !category.Optional, parsed);
    }

    public void Save(string path)
    {
        var categories = new JsonObject();
        foreach (var category in Categories)
            categories[category.Id] = new JsonObject { ["include"] = Includes(category.Id), ["rule"] = RuleFor(category.Id).ToString() };
        var root = new JsonObject { ["autoApply"] = AutoApply, ["categories"] = categories };
        File.WriteAllText(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }
}
