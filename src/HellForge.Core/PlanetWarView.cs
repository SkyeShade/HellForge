using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// What the launcher's Planets page shows of a planet: the war the next session plays (ProfileStore.LoadWar: the HellForge
// war's last snapshot, the live war's cache, the all-planets war made from the frozen war, else the frozen war). The
// planet modifiers are on it only with Custom planets (Active), as the backend will serve it.
// Enemy: whom its missions fight (its owner, or the attacker of Super Earth's planet: an event's race, else a campaign's,
// as the Battle for Super Earth's; 0 none).
public sealed record PlanetView(int Index, int Owner, int Enemy, bool Playable, IReadOnlyList<string> Subfactions, IReadOnlyList<string> Surges, IReadOnlyList<string> Left);

public static class PlanetWarView
{
    // The planets as the next session's war serves them, where that war came from, and whether the plan is on it.
    public static (IReadOnlyDictionary<int, PlanetView> Planets, string Source, bool Active) Read(ISessionPaths paths, PlanetPlan plan)
    {
        var war = ProfileStore.LoadWar(paths);
        var (info, frozen) = OpenWar.Frozen();
        var (status, source) = war switch
        {
            ProfileStore.WarHellForge => (Status(WarServer.CachePath(paths.HostRoot, ProfileStore.LoadWarServer(paths)), "snapshot"), "the HellForge war's last snapshot"),
            ProfileStore.WarLive => (Status(Path.Combine(paths.HostRoot, "live-war.json"), null), "the live war, as last fetched"),
            ProfileStore.WarOpen or ProfileStore.WarCustom => (Node(OpenWar.Status(info, frozen, seed: ProfileStore.LoadSubfactionSeed(paths),
                dealSubfactions: ProfileStore.LoadDealSubfactions(paths), dealSeaf: ProfileStore.LoadDealSeaf(paths))),
                war == ProfileStore.WarCustom ? "Custom planets (every planet open, with your changes)" : "All planets open"),
            _ => (null, ""),
        };
        if (status == null) (status, source) = (Node(frozen), "the frozen war");
        var active = war == ProfileStore.WarCustom;
        return (View(active ? plan.Status(status, info) : status), source, active);
    }

    // A war cache's Status (the HellForge war's under "snapshot"), or null.
    static JsonNode? Status(string file, string? under)
    {
        try
        {
            if (!File.Exists(file)) return null;
            var root = JsonNode.Parse(File.ReadAllText(file));
            if (under != null) root = root?[under];
            return root?["status"] is JsonObject status && status["planetStatus"] is JsonArray ? status.DeepClone() : null;
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException) { return null; }
    }

    static JsonNode Node(JsonElement element) => JsonNode.Parse(element.GetRawText())!;

    static Dictionary<int, PlanetView> View(JsonNode served)
    {
        var catalog = PlanetModifiers.Catalog;
        var subfactions = catalog.Subfactions.ToDictionary(s => s.Effect, s => s.Name);
        var surges = catalog.Enemies.SelectMany(g => g.Surge.Select(id => (id, g.Name))).ToDictionary(p => p.id, p => p.Name);
        var left = catalog.Enemies.SelectMany(g => g.None.Select(id => (id, g.Name))).ToDictionary(p => p.id, p => p.Name);
        var playable = Rows(served, "campaigns").Concat(Rows(served, "planetEvents")).Select(c => Int(c["planetIndex"])).OfType<int>().ToHashSet();
        var attacker = new Dictionary<int, int>();
        foreach (var e in Rows(served, "planetEvents").Concat(Rows(served, "campaigns")))
            if (Int(e["planetIndex"]) is { } i && Int(e["race"]) is { } race and > PlanetModifiers.SuperEarth) attacker.TryAdd(i, race);
        var effects = (served["planetActiveEffects"] as JsonArray ?? []).OfType<JsonObject>()
            .Select(e => (Index: Int(e["index"]), Id: Int(e["galacticEffectId"]))).Where(e => e.Index != null && e.Id != null)
            .GroupBy(e => e.Index!.Value).ToDictionary(g => g.Key, g => g.Select(e => e.Id!.Value).ToList());
        var view = new Dictionary<int, PlanetView>();
        foreach (var row in (served["planetStatus"] as JsonArray ?? []).OfType<JsonObject>())
        {
            if (Int(row["index"]) is not { } index) continue;
            var on = effects.GetValueOrDefault(index) ?? [];
            var owner = Int(row["owner"]) ?? PlanetModifiers.SuperEarth;
            view[index] = new PlanetView(index, owner, owner != PlanetModifiers.SuperEarth ? owner : attacker.GetValueOrDefault(index), playable.Contains(index),
                [.. on.Where(subfactions.ContainsKey).Select(id => subfactions[id]).Distinct()],
                [.. on.Where(surges.ContainsKey).Select(id => surges[id]).Distinct()],
                [.. on.Where(left.ContainsKey).Select(id => left[id]).Distinct()]);
        }
        return view;
    }

    static IEnumerable<JsonObject> Rows(JsonNode served, string list) => (served[list] as JsonArray ?? []).OfType<JsonObject>();

    static int? Int(JsonNode? n) => n is JsonValue v && v.GetValueKind() == JsonValueKind.Number && v.TryGetValue<int>(out var i) ? i : null;
}
