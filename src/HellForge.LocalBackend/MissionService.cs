using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// Local operation and mission lifecycle. Request and reply shapes were decoded from the client:
//   POST /Operation/Create          0xaa641804  body {operations:[{id, seed, warId, planetIndex, campaignId, type,
//                                               difficulty, race, ...missions}]}, reply {operations:[{id, rewards, backupRewards}]} (parser 0x105d9d0)
//   POST /Operation/Mission/Start   0xc50b20e7  reply {missionId: string} (parser 0x107d980)
//   POST /Operation/Mission/end     0x1e555ea6  body from 0x11e9680 (missionSuccess, difficultyType, objectives,
//                                               lootedItems, heavyLootedItems, ...), reply parsed by 0x105e3e0/0x105e6b0
//   POST /Operation/Mission/Activity 0x385dc715 builder 0x12c2940 {missionId, activityId32, objectId, participants:[account
//                                               ids], gmiStr}, sent once every player confirmed a pickup; reply {rewards:[
//                                               {platformAccountId: string, rewards:[{mixId, amount}] (15 at most)}]} (parsers
//                                               0x105e010, 0x105e190, 0x1044660). The client adds the local player's entry
//                                               (0x1370a20) and forwards each other one to that player's game (0xA0823945).
//   POST /Operation/Mission/SetMaxRewards, POST /Operation/Abandon: no reply fields are read.
//   POST /Progression/inventory/consume  0xeea479dc  body {consumedItems:[{mixId, amount}]} sent as a mission ends (the
//                                               dispatcher has no reply case for it): acknowledged, nothing taken.
// Rewards are local rules, not Arrowhead's: XP by difficulty, success and objectives; requisition; medals for a won
// mission; looted samples as the client reports them. Level 11 (a mod's, native/mod-loader/difficulty.c) pays half as much
// XP and medals again as level 10. Each mission id is paid once (ProfileStore.ApplyMissionRewards). Mastery: docs/internals.md.
public sealed class MissionService
{
    public const uint RequisitionMixId = 0xD7150EEC;
    // The medals a player holds and spends on warbond pages (every warbond's medalsConsumableMixId; catalogue maxCap 250).
    public const uint MedalsMixId = 1276729503;
    // The "medals" reward item the live server names in its rewards (major orders, medal packages); the client turns it
    // into MedalsMixId when it adds it (game.dll 0x136efa8). Shown on the operation's reward card.
    public const uint MedalRewardMixId = 0x3584C850;
    static readonly int[] DifficultyExperience = [100, 150, 200, 300, 400, 500, 650, 800, 1000, 1200, 1500, 2250];
    static readonly int[] DifficultyMedals = [0, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15];

    readonly ProfileStore profile;
    readonly ConcurrentDictionary<long, int> operationDifficulty = new();
    readonly PickupRelay? pickups;

    // This session's mod weapons, set by LocalBackend: their packages earn too.
    public RegisteredItems? Registered { get; init; }
    // The player's Steam account, which picks them out of a report that lists the whole squad; set by the backend's program.
    public ulong? LocalAccount { get; init; }

    public MissionService(ProfileStore profile, PickupRelay? pickups = null)
    {
        this.profile = profile;
        this.pickups = pickups;
        if (pickups != null) pickups.Credit = Credit;
    }

    public static bool Handles(string path) => path is "/Operation/Create" or "/Operation/Abandon" or "/Operation/Mission/Start"
        or "/Operation/Mission/end" or "/Operation/Mission/Activity" or "/Operation/Mission/SetMaxRewards" or "/Progression/inventory/consume";

    public (JsonNode Reply, string Outcome) Handle(string path, JsonElement body) => path switch
    {
        "/Operation/Create" => (Create(body), "operation_created"),
        "/Operation/Mission/Start" => (new JsonObject { ["missionId"] = Guid.NewGuid().ToString() }, "mission_started"),
        "/Operation/Mission/end" => (End(body), "mission_ended"),
        "/Operation/Mission/Activity" => (Activity(body), "mission_activity"),
        "/Progression/inventory/consume" => (new JsonObject(), "items_consumed"),
        _ => (new JsonObject(), "acknowledged")
    };

    // The pickups' activities (hash32 of "mission_activity_<type>", set at spawn by 0x593d30): a Super Credits pile (Forge
    // Credits), medals and requisition slips.
    public const uint ForgeCreditsActivity = 0x7F8FCE26, MedalsActivity = 0x0A2ECD4E, RequisitionActivity = 0x7549A744;
    // What a pile pays: the item the profile holds (medals as MedalsMixId), the one the reply names (medals as the reward
    // item MedalRewardMixId, which the client turns into its medals) and the amount; null for another activity.
    static (uint Held, uint Shown, long Amount)? PileOf(uint activity, long pile) => activity switch
    {
        ForgeCreditsActivity => (PaidContentSet.ForgeCreditsWallet, PaidContentSet.ForgeCreditsWallet, Rewards.ForgeCredits(pile)),
        MedalsActivity => (MedalsMixId, MedalRewardMixId, Rewards.MedalPickup(pile)),
        RequisitionActivity => (RequisitionMixId, RequisitionMixId, Rewards.RequisitionPickup(pile)),
        _ => null
    };

    // A pickup: every participant gets the same amount, as on the live game. This profile is credited here, once per pile
    // (ProfileStore.ApplyPickup), and the other participants' backends through multiplayer (PickupRelay); the reply names
    // what the client adds, and the client forwards the others' entries to their games.
    JsonObject Activity(JsonElement body)
    {
        var rewards = new JsonArray();
        if (body.ValueKind != JsonValueKind.Object || !TryInt(body, "activityId32", out var activity) || !TryInt(body, "objectId", out var pile))
            return new JsonObject { ["rewards"] = rewards };
        var mission = body.TryGetProperty("missionId", out var m) && m.ValueKind == JsonValueKind.String ? m.GetString()! : "";
        if (PileOf((uint)activity, pile) is not { } paid || !profile.ApplyPickup(mission, pile, paid.Held, paid.Amount))
            return new JsonObject { ["rewards"] = rewards };
        var accounts = Participants(body);
        foreach (var account in accounts)
            rewards.Add((JsonNode)new JsonObject
            {
                ["platformAccountId"] = account.ToString(System.Globalization.CultureInfo.InvariantCulture),
                ["rewards"] = new JsonArray((JsonNode)new JsonObject { ["mixId"] = paid.Shown, ["amount"] = paid.Amount })
            });
        pickups?.OnPaid(new PickupRelay.Pickup(mission, pile, (uint)activity, accounts));
        return new JsonObject { ["rewards"] = rewards };
    }

    // The request's participants: account ids as numbers or decimal strings, each once, 16 at most.
    static List<ulong> Participants(JsonElement body)
    {
        var accounts = new List<ulong>();
        if (!body.TryGetProperty("participants", out var participants) || participants.ValueKind != JsonValueKind.Array) return accounts;
        foreach (var participant in participants.EnumerateArray().Take(16))
            if (participant.ValueKind is JsonValueKind.Number or JsonValueKind.String &&
                ulong.TryParse(participant.ValueKind == JsonValueKind.String ? participant.GetString() : participant.GetRawText(),
                    System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var account) && !accounts.Contains(account))
                accounts.Add(account);
        return accounts;
    }

    // A pile a friend's backend paid: this profile gets the same amount, worked out here from the pile, once.
    bool Credit(PickupRelay.Pickup pickup) =>
        PileOf(pickup.Activity, pickup.Pile) is { } paid && profile.ApplyPickup(pickup.Mission, pickup.Pile, paid.Held, paid.Amount);

    JsonObject Create(JsonElement body)
    {
        var operations = new JsonArray();
        if (body.ValueKind == JsonValueKind.Object && body.TryGetProperty("operations", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var operation in list.EnumerateArray())
            {
                if (!TryInt(operation, "id", out var id)) continue;
                var level = TryInt(operation, "difficulty", out var difficulty) ? (int)difficulty : 1;
                operationDifficulty[id] = level;
                // The planet card's reward: the medals the operation's missions pay when all are won (parser 0x10558f0).
                var missions = operation.TryGetProperty("missions", out var listed) && listed.ValueKind == JsonValueKind.Array ? listed.GetArrayLength() : 0;
                var medals = Rewards.Medals(level) * Math.Max(1, missions);
                operations.Add((JsonNode)new JsonObject
                {
                    ["id"] = id,
                    ["rewards"] = new JsonArray((JsonNode)new JsonObject { ["type"] = 1, ["id32"] = MedalRewardMixId, ["amount"] = medals, ["isFullReward"] = true }),
                    ["backupRewards"] = new JsonArray()
                });
            }
        return new JsonObject { ["operations"] = operations };
    }

    JsonObject End(JsonElement body)
    {
        var success = FindBool(body, "missionSuccess") ?? FindBool(body, "succeeded") ?? false;
        var difficulty = (int)(FindInt(body, "difficultyType") ?? (FindInt(body, "operationId") is { } op &&
            operationDifficulty.TryGetValue(op, out var known) ? known : 1));
        var objectives = CountSucceededObjectives(body);
        var experience = Rewards.Experience(difficulty, success, objectives);
        var looted = SumItems(body, "lootedItems");
        var heavy = SumItems(body, "heavyLootedItems");
        var requisition = success ? Rewards.Requisition(difficulty) : 0;
        var medals = success ? Rewards.Medals(difficulty) : 0;
        var grants = new Dictionary<uint, long>();
        foreach (var source in new[] { looted, heavy })
            foreach (var (mix, amount) in source) grants[mix] = grants.GetValueOrDefault(mix) + amount;
        if (requisition > 0) grants[RequisitionMixId] = grants.GetValueOrDefault(RequisitionMixId) + requisition;
        if (medals > 0) grants[MedalsMixId] = grants.GetValueOrDefault(MedalsMixId) + medals;
        static JsonArray Items(Dictionary<uint, long> items) =>
            new(items.Select(item => (JsonNode?)new JsonObject { ["mixId"] = item.Key, ["amount"] = item.Value }).ToArray());
        var rewards = new JsonArray();
        if (requisition > 0) rewards.Add((JsonNode)new JsonObject { ["mixId"] = RequisitionMixId, ["amount"] = requisition });
        if (medals > 0) rewards.Add((JsonNode)new JsonObject { ["mixId"] = MedalsMixId, ["amount"] = medals });
        // A retried end (its reply was lost) is answered with nothing earned: the first one paid.
        var missionId = FindString(body, "missionId");
        var player = LocalParticipant(body);
        var tracks = ReportedTracks(player);
        var updates = new JsonArray();
        var paid = profile.ApplyMissionRewards(experience, grants, missionId, (progression, inventory) =>
        {
            foreach (var track in tracks)
            {
                var earned = WeaponMastery.Earn(track, progression, inventory, experience);
                var update = new JsonObject { ["id32"] = earned.Package, ["level"] = earned.Level, ["progress"] = earned.Progress };
                if (player is { } participant && participant.TryGetProperty("platformAccountId", out var account) && account.ValueKind == JsonValueKind.String)
                    update["platformAccountId"] = account.GetString();
                updates.Add((JsonNode)update);
            }
        });
        if (!paid)
        {
            experience = 0;
            looted.Clear();
            rewards.Clear();
        }
        return new JsonObject
        {
            ["operationId"] = FindInt(body, "operationId") ?? 0, ["missionIndex"] = FindInt(body, "missionIndex") ?? 0,
            ["missionId"] = missionId ?? "", ["experience"] = experience, ["multipliers"] = new JsonArray(),
            ["badges"] = new JsonArray(), ["campaignWon"] = false, ["impact"] = 0, ["othersImpactPlanet"] = 0,
            ["othersImpactCommunityTarget"] = 0, ["othersImpactPlanetEvent"] = 0, ["progressionPackageUpdates"] = updates,
            // Heavy loot is granted above, but its reply list wants {id32, rewards: [...]} per heavy item (parser 0x105e6b0;
            // {mixId, amount} there made the client refuse the whole reply), and which id32 is not known yet.
            ["lootedItemRewards"] = Items(looted), ["lootedHeavyItemRewards"] = new JsonArray(),
            ["rewards"] = rewards,
            ["repeatingPlayer"] = false
        };
    }

    // This player's entry of the report: the one with their account, else the first. A host's report lists every player, its
    // own first (captured); a friend's backend gets no report of its own, so a friend's weapon earns nothing yet.
    JsonElement? LocalParticipant(JsonElement body)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty("participants", out var participants) || participants.ValueKind != JsonValueKind.Array) return null;
        JsonElement? first = null;
        foreach (var participant in participants.EnumerateArray().Where(p => p.ValueKind == JsonValueKind.Object))
        {
            first ??= participant;
            if (LocalAccount is { } account && participant.TryGetProperty("platformAccountId", out var id) && id.ValueKind == JsonValueKind.String &&
                id.GetString() == account.ToString(System.Globalization.CultureInfo.InvariantCulture)) return participant;
        }
        return first;
    }

    // The packages reported for a player, each once, that can earn: a weapon's, or a mod weapon's the player may have. The
    // client's list holds 15, and it names one today.
    List<MasteryTrack> ReportedTracks(JsonElement? participant)
    {
        var tracks = new List<MasteryTrack>();
        if (participant is not { } player || !player.TryGetProperty("progressedPackages", out var packages) || packages.ValueKind != JsonValueKind.Array) return tracks;
        foreach (var package in packages.EnumerateArray())
        {
            if (!TryInt(package, "id32", out var id) || id is <= 0 or > uint.MaxValue || tracks.Count == MaxPackageUpdates || tracks.Any(t => t.Package == id)) continue;
            if (WeaponMastery.Vanilla((uint)id) is { } vanilla && WeaponMastery.IsWeapon(vanilla)) tracks.Add(vanilla);
            else if (Registered?.Track((uint)id) is { } copy && !Registered.IsLeftOut(copy.Item!)) tracks.Add(copy);
        }
        return tracks;
    }

    const int MaxPackageUpdates = 15;

    public static class Rewards
    {
        public static long Experience(int difficulty, bool success, int objectives)
        {
            var base_ = DifficultyExperience[Math.Clamp(difficulty, 0, DifficultyExperience.Length - 1)];
            return (success ? base_ : base_ / 2) + 50L * Math.Clamp(objectives, 0, 20);
        }

        // A Forge Credits pile: the live game's amounts are the server's (the client sends none), so HellForge's own: mostly 10,
        // sometimes 20, 50 or 100, the same for a pile wherever it is asked (its objectId).
        public static long ForgeCredits(long pile) => ((uint)(unchecked((ulong)pile * 0x9E3779B97F4A7C15UL) >> 32) % 100) switch
        {
            < 75 => 10,
            < 92 => 20,
            < 98 => 50,
            _ => 100
        };

        // A medal pickup: mostly 1, sometimes 2, now and then 5; requisition slips: 50, 100 or 200 the same way. Per pile, as
        // ForgeCredits.
        public static long MedalPickup(long pile) => Bucket(pile) switch { < 75 => 1, < 95 => 2, _ => 5 };
        public static long RequisitionPickup(long pile) => Bucket(pile) switch { < 75 => 50, < 95 => 100, _ => 200 };
        static uint Bucket(long pile) => (uint)(unchecked((ulong)pile * 0x9E3779B97F4A7C15UL) >> 32) % 100;

        // Level 11 is a mod's (native/mod-loader/difficulty.c, HfDifficulty).
        public static long Requisition(int difficulty) => 200L * Math.Clamp(difficulty, 1, 11);

        // A won mission's medals.
        public static long Medals(int difficulty) => DifficultyMedals[Math.Clamp(difficulty, 1, DifficultyMedals.Length - 1)];
    }

    static int CountSucceededObjectives(JsonElement body)
    {
        var count = 0;
        foreach (var objectives in FindAll(body, "objectives"))
            if (objectives.ValueKind == JsonValueKind.Array)
                foreach (var objective in objectives.EnumerateArray())
                    if (objective.ValueKind == JsonValueKind.Object && objective.TryGetProperty("succeeded", out var s) && s.ValueKind == JsonValueKind.True) count++;
        return count;
    }

    static Dictionary<uint, long> SumItems(JsonElement body, string key)
    {
        var items = new Dictionary<uint, long>();
        foreach (var list in FindAll(body, key))
            if (list.ValueKind == JsonValueKind.Array)
                foreach (var item in list.EnumerateArray())
                    // Super Credits found on a map (or claimed by a modified report) are a premium currency: never granted.
                    if (TryInt(item, "mixId", out var mix) && TryInt(item, "amount", out var amount) && mix > 0 && amount > 0 && mix <= uint.MaxValue &&
                        !PaidContentSet.IsSuperCredits((uint)mix))
                        items[(uint)mix] = items.GetValueOrDefault((uint)mix) + Math.Min(amount, 10_000);
        return items;
    }

    // The client's report nests per-player data; search the whole document for a key.
    static IEnumerable<JsonElement> FindAll(JsonElement node, string key)
    {
        if (node.ValueKind == JsonValueKind.Object)
            foreach (var property in node.EnumerateObject())
                foreach (var found in FindIn(property, key)) yield return found;
        else if (node.ValueKind == JsonValueKind.Array)
            foreach (var element in node.EnumerateArray())
                foreach (var nested in FindAll(element, key)) yield return nested;
    }

    // A property's value when it is the key, then the key's values inside it.
    static IEnumerable<JsonElement> FindIn(JsonProperty property, string key)
    {
        if (property.NameEquals(key)) yield return property.Value;
        foreach (var nested in FindAll(property.Value, key)) yield return nested;
    }

    static long? FindInt(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number)) return number;
        return null;
    }

    static bool? FindBool(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind is JsonValueKind.True or JsonValueKind.False) return value.ValueKind == JsonValueKind.True;
        return null;
    }

    static string? FindString(JsonElement body, string key)
    {
        foreach (var value in FindAll(body, key))
            if (value.ValueKind == JsonValueKind.String) return value.GetString();
        return null;
    }

    static bool TryInt(JsonElement node, string key, out long value)
    {
        value = 0;
        return node.ValueKind == JsonValueKind.Object && node.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value);
    }
}
