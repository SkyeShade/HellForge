using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The war's other services, which the game polls on the ship (HD2Mods research/galactic-war/assignments-rewards):
//   GET  /v2/Assignment/War/801   major orders, ~30 s      0xdad1fefc  array root, at most 16 (parser 0x1077770)
//   GET  /v2/Assignment/Player    personal orders          0xde1a3e56  the same shape
//   POST /v2/Assignment/War, /v2/Assignment/Player         the client's progress deltas; no reply is read
//   POST /v2/Assignment/Player/Complete  {id32}            sent once a personal order's tasks are done (0x1043d90); the
//                                                           reply's completionRewards[].completionReward.items the client
//                                                           adds itself, so they are saved here first
//   GET  /NewsFeed/801?maxEntries=16&fromTimeStamp=t       dispatches, ~30 s      array root (0x107d480), lowercase keys
//   GET  /Episode/801, /Episode/801/status                 {episodes: []}         (0x104be20, 0x104bab0)
//   GET  /Stats/war/801/summary                            {} (0x106d040: with galaxy_stats, all 14 counters)
//   POST /Mail/inbox                                       the mailbox, ~60 s (0x10567a0: all five header fields)
//   POST /Mail/claim/mail {mailIds}, /Mail/claim/mail/<id> a claim: the client adds the attachments' items itself
//                                                           (0x1373cf0), so they are saved here first
//   POST /Mission/Session, /Pes/GetConfiguration           mission telemetry (no reply read), feature messages
// Each got 501 before. Without a war script each answers with the empty body its parser accepts. With one (backend
// --war-script <file>, docs/war-script.md) its major orders, personal orders, dispatches and mail are served, and its
// galactic war effects and Status additions join whichever war is served. The file is read again whenever it changes,
// so an edit shows in game at the next poll. Mail is how a reward reaches every player, whether they played or not: each
// profile claims each mail once (progression "hellforgeClaimedMail"). Nothing paid and never Super Credits.
// Every reply here was run through the client's own parser in an emulator; none has been tried in game yet.
public sealed class WarServices(string? scriptPath, WarClock clock, ProfileStore? profile, Func<ulong> localAccount, Action<string> log)
{
    public const int WarId = LiveWar.WarId;
    const int MajorOrder = 4, PersonalOrder = 0, MaxEntries = 16, MaxTasks = 8, MaxRewards = 3;
    static readonly Lazy<PaidContentSet> DefaultPaid = new(PaidContentSet.Load);

    readonly object gate = new();
    DateTime scriptTime;
    JsonElement? script;

    public static bool Handles(string path) => Route(path) != null;

    // A path's service, or null.
    static string? Route(string path) => path switch
    {
        "/v2/Assignment/War/801" or "/v2/Assignment/Player" or "/v2/Assignment/War" => "assignments",
        "/v2/Assignment/Player/Complete" => "complete",
        "/NewsFeed/801" => "news",
        "/Episode/801" or "/Episode/801/status" => "episodes",
        "/Stats/war/801/summary" or "/Mission/Session" => "empty",
        "/Pes/GetConfiguration" => "pes",
        "/Mail/inbox" => "inbox",
        _ when path == "/Mail/claim/mail" || path.StartsWith("/Mail/claim/mail/", StringComparison.Ordinal) => "claim",
        _ => null
    };

    // The reply (status 200) and the audit's outcome.
    public (JsonNode Reply, string Outcome) Handle(string method, string path, IQueryCollection query, JsonElement body)
    {
        var current = Script();
        return Route(path) switch
        {
            "assignments" when method == "GET" => (Assignments(current, path.StartsWith("/v2/Assignment/War", StringComparison.Ordinal) ? "majorOrders" : "personalOrders",
                path.StartsWith("/v2/Assignment/War", StringComparison.Ordinal) ? MajorOrder : PersonalOrder), "war_assignments"),
            "assignments" => (new JsonObject(), "assignment_progress"),
            "complete" => (Complete(current, body), "assignment_complete"),
            "news" => (News(current, query), "war_news"),
            "episodes" => (new JsonObject { ["episodes"] = new JsonArray() }, "war_episodes"),
            "pes" => (new JsonObject { ["isValid"] = false }, "pes_configuration"),
            "inbox" => (Inbox(current, body), "mail_inbox"),
            "claim" => (Claim(current, path, body), "mail_claim"),
            _ => (new JsonObject(), "acknowledged")
        };
    }

    // The war script's additions to a war body the game was served: its effect definitions to /WarSeason/GalacticWarEffects
    // (an id already there is replaced), and its "status" arrays appended to /WarSeason/801/Status.
    public JsonElement Apply(string path, JsonElement body)
    {
        if (Script() is not { } current) return body;
        if (path == "/WarSeason/GalacticWarEffects" && current.TryGetProperty("effects", out var effects) && effects.ValueKind == JsonValueKind.Array)
            return WithEffects(body, effects);
        if (path == $"/WarSeason/{WarId}/Status" && current.TryGetProperty("status", out var status) && status.ValueKind == JsonValueKind.Object)
            return WithStatus(body, status);
        return body;
    }

    static JsonElement WithEffects(JsonElement body, JsonElement effects)
    {
        var added = effects.EnumerateArray().Where(e => e.TryGetProperty("id", out _)).ToList();
        var ids = added.Select(e => e.GetProperty("id").GetRawText()).ToHashSet();
        var merged = new JsonArray();
        foreach (var e in body.EnumerateArray().Where(e => !e.TryGetProperty("id", out var id) || !ids.Contains(id.GetRawText()))) merged.Add(JsonNode.Parse(e.GetRawText()));
        foreach (var e in added) merged.Add(JsonNode.Parse(e.GetRawText()));
        return JsonSerializer.SerializeToElement(merged, BackendJson.Default.JsonNode);
    }

    static JsonElement WithStatus(JsonElement body, JsonElement status)
    {
        var node = JsonNode.Parse(body.GetRawText())!.AsObject();
        foreach (var list in status.EnumerateObject().Where(l => l.Value.ValueKind == JsonValueKind.Array))
        {
            if (node[list.Name] is not JsonArray target) node[list.Name] = target = new JsonArray();
            foreach (var entry in list.Value.EnumerateArray()) target.Add(JsonNode.Parse(entry.GetRawText()));
        }
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }

    // The war script as last read, read again when the file changed; a file that does not parse keeps the last good one.
    JsonElement? Script()
    {
        if (scriptPath == null) return null;
        lock (gate)
        {
            try
            {
                var time = File.Exists(scriptPath) ? File.GetLastWriteTimeUtc(scriptPath) : default;
                if (time == scriptTime) return script;
                scriptTime = time;
                if (time == default) { script = null; return null; }
                using var document = JsonDocument.Parse(File.ReadAllText(scriptPath), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                if (document.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException("the file is not a JSON object");
                script = document.RootElement.Clone();
                log($"War script read ({Path.GetFileName(scriptPath)}).");
            }
            catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException)
            {
                log($"War script not read ({error.Message}); the last good one stays.");
            }
            return script;
        }
    }

    static IEnumerable<JsonElement> List(JsonElement? current, string name) =>
        current is { } s && s.TryGetProperty(name, out var list) && list.ValueKind == JsonValueKind.Array ? list.EnumerateArray() : [];

    // Major or personal orders that have not ended: {id32, progress, expiresIn, setting: {type, overrideTitle,
    // overrideBrief, taskDescription, tasks, rewards, flags}} (0x1042f20). Title up to 255 bytes, brief up to 1023; the game
    // writes each task's line itself from its type and values. An order that does not convert is left out, logged.
    JsonArray Assignments(JsonElement? current, string name, int type)
    {
        var orders = new JsonArray();
        foreach (var order in List(current, name))
        {
            if (orders.Count == MaxEntries) break;
            try
            {
                if (Assignment(order, type) is { } converted) orders.Add((JsonNode)converted);
            }
            catch (Exception error) when (error is InvalidOperationException or KeyNotFoundException or FormatException)
            {
                log($"War script: an entry of {name} was left out ({error.Message}).");
            }
        }
        return orders;
    }

    JsonObject? Assignment(JsonElement order, int type)
    {
        var expiresIn = order.TryGetProperty("endsAt", out var ends) ? ends.GetInt64() - clock.Seconds : order.GetProperty("expiresIn").GetInt64();
        if (expiresIn <= 0) return null;
        var tasks = new JsonArray();
        foreach (var task in order.GetProperty("tasks").EnumerateArray().Take(MaxTasks))
            tasks.Add((JsonNode)new JsonObject { ["type"] = task.GetProperty("type").GetInt32(), ["values"] = Integers(task, "values"), ["valueTypes"] = Integers(task, "valueTypes") });
        var progress = order.TryGetProperty("progress", out var p) ? Integers(order, "progress") : new JsonArray([.. tasks.Select(_ => (JsonNode?)0)]);
        return new JsonObject
        {
            ["id32"] = order.GetProperty("id").GetUInt32(), ["progress"] = progress, ["expiresIn"] = expiresIn,
            ["setting"] = new JsonObject
            {
                ["type"] = type, ["overrideTitle"] = Text(order, "title", 255), ["overrideBrief"] = Text(order, "brief", 1023), ["taskDescription"] = "",
                ["tasks"] = tasks,
                ["rewards"] = new JsonArray([.. Items(order, "rewards").Take(MaxRewards).Select(r => (JsonNode?)new JsonObject { ["type"] = 1, ["id32"] = r.MixId, ["amount"] = r.Amount })]),
                ["flags"] = order.TryGetProperty("flags", out var flags) ? flags.GetInt32() : 0
            }
        };
    }

    static JsonArray Integers(JsonElement owner, string name) =>
        new([.. owner.TryGetProperty(name, out var list) ? list.EnumerateArray().Select(v => (JsonNode?)v.GetInt64()) : []]);

    // Text cut to the client's buffer (bytes, UTF-8, whole characters).
    static string Text(JsonElement owner, string name, int bytes)
    {
        var text = owner.TryGetProperty(name, out var value) ? value.GetString() ?? "" : "";
        while (System.Text.Encoding.UTF8.GetByteCount(text) > bytes) text = text[..^1];
        return text;
    }

    // Reward items: {mixId, amount}; currencies (medals, requisition, samples) and anything not paid, never Super Credits.
    // The generic medal reward (which the catalogue prices in Super Credits) is a currency here.
    IEnumerable<(uint MixId, long Amount)> Items(JsonElement owner, string name)
    {
        var paid = profile?.Ownership?.Paid ?? DefaultPaid.Value;
        foreach (var item in owner.TryGetProperty(name, out var list) ? list.EnumerateArray() : [])
        {
            var mixId = item.GetProperty("mixId").GetUInt32();
            var amount = item.GetProperty("amount").GetInt64();
            if (amount > 0 && !PaidContentSet.IsSuperCredits(mixId) && (paid.Currencies.Contains(mixId) || !paid.IsPaid(mixId))) yield return (mixId, amount);
        }
    }

    // What a reward item adds to an inventory: the generic medal reward is the medals a player holds, as the client adds it
    // (game.dll 0x136efa8).
    static uint Held(uint mixId) => mixId == MissionService.MedalRewardMixId ? MissionService.MedalsMixId : mixId;

    // A personal order's reward, once per profile: saved, then echoed for the client to add.
    JsonObject Complete(JsonElement? current, JsonElement body)
    {
        if (profile == null || !body.TryGetProperty("id32", out var id) || !id.TryGetUInt32(out var id32)) return new JsonObject();
        var order = List(current, "personalOrders").FirstOrDefault(o => o.TryGetProperty("id", out var i) && i.TryGetUInt32(out var n) && n == id32);
        if (order.ValueKind != JsonValueKind.Object) return new JsonObject();
        var granted = Grant($"order/{id32}", Items(order, "rewards").ToList());
        if (granted == null) return new JsonObject();
        var reward = new JsonObject { ["id32"] = id32, ["completionReward"] = new JsonObject { ["items"] = granted } };
        if (localAccount() is var account and not 0) reward["platformAccountId"] = account.ToString(CultureInfo.InvariantCulture);
        return new JsonObject { ["completionRewards"] = new JsonArray((JsonNode)reward) };
    }

    // Dispatches published after fromTimeStamp (war seconds), newest 16: {id, published, type, tagIds, message}. The
    // message is free text; <i=1>..</i> and <i=2>..</i> are yellow, <i=3>..</i> white (a heading).
    JsonArray News(JsonElement? current, IQueryCollection query)
    {
        var from = long.TryParse(query["fromTimeStamp"], NumberStyles.None, CultureInfo.InvariantCulture, out var t) ? t : 0;
        var news = new JsonArray();
        foreach (var dispatch in List(current, "dispatches")
                     .Where(d => d.TryGetProperty("id", out _) && d.TryGetProperty("published", out var p) && p.GetInt64() > from && p.GetInt64() <= clock.Seconds)
                     .OrderByDescending(d => d.GetProperty("published").GetInt64()).Take(MaxEntries))
            news.Add((JsonNode)new JsonObject
            {
                ["id"] = dispatch.GetProperty("id").GetUInt32(), ["published"] = dispatch.GetProperty("published").GetInt64(), ["type"] = 0,
                ["tagIds"] = new JsonArray(), ["message"] = dispatch.TryGetProperty("message", out var m) ? m.GetString() ?? "" : ""
            });
        return news;
    }

    // The mailbox: the war script's mail sent by now, with what this profile has claimed marked claimed.
    JsonObject Inbox(JsonElement? current, JsonElement body)
    {
        var unclaimedOnly = body.TryGetProperty("unclaimedOnly", out var u) && u.ValueKind == JsonValueKind.True;
        var oldestFirst = body.TryGetProperty("orderDateAsc", out var o) && o.ValueKind == JsonValueKind.True;
        var offset = body.TryGetProperty("offset", out var off) && off.TryGetInt32(out var skip) ? Math.Max(0, skip) : 0;
        var limit = body.TryGetProperty("limit", out var lim) && lim.TryGetInt32(out var take) ? Math.Clamp(take, 0, 100) : 20;
        var claimed = Claimed();
        var mail = Mail(current).Where(m => !unclaimedOnly || !claimed.Contains(m.Id)).ToList();
        var ordered = oldestFirst ? mail.OrderBy(m => m.Sent) : mail.OrderByDescending(m => m.Sent);
        var entries = new JsonArray();
        foreach (var m in ordered.Skip(offset).Take(limit))
            entries.Add((JsonNode)new JsonObject
            {
                ["id"] = m.Id, ["sentTime"] = m.Sent, ["sourceType"] = 0, ["warId"] = WarId, ["message"] = m.Message,
                ["itemAttachments"] = new JsonArray((JsonNode)Attachment(m.Id, m.Items, claimed.Contains(m.Id)))
            });
        return new JsonObject
        {
            ["totalMailCount"] = mail.Count, ["currentLimit"] = limit, ["currentOffset"] = offset, ["unclaimedOnly"] = unclaimedOnly,
            ["orderDateAsc"] = oldestFirst, ["entries"] = entries
        };
    }

    static JsonObject Attachment(uint id, IEnumerable<(uint MixId, long Amount)> items, bool claimed)
    {
        var attachment = new JsonObject
        {
            ["id"] = id, ["type"] = 0,
            ["items"] = new JsonArray([.. items.Select(i => (JsonNode?)new JsonObject { ["mixId"] = i.MixId, ["amount"] = i.Amount })])
        };
        if (claimed) attachment["claimedAt"] = 1;
        return attachment;
    }

    sealed record Letter(uint Id, long Sent, string Message, List<(uint MixId, long Amount)> Items);

    // The war script's mail that has been sent by now (sentAt, war seconds; without one, from the start).
    List<Letter> Mail(JsonElement? current)
    {
        var mail = new List<Letter>();
        foreach (var m in List(current, "mail"))
        {
            if (!m.TryGetProperty("id", out var id) || !id.TryGetUInt32(out var mailId)) continue;
            var sent = m.TryGetProperty("sentAt", out var s) && s.TryGetInt64(out var at) ? at : 0;
            if (sent > clock.Seconds) continue;
            mail.Add(new Letter(mailId, sent, m.TryGetProperty("message", out var text) ? text.GetString() ?? "" : "", Items(m, "items").ToList()));
        }
        return mail;
    }

    // POST /Mail/claim/mail {mailIds: [...]} or /Mail/claim/mail/<id> {}: each unclaimed mail's items saved, then echoed
    // ({mail: [{mailId, claimedItemAttachments, claimedSeasonPassItemIds}]}, or the single mail element for one id).
    JsonNode Claim(JsonElement? current, string path, JsonElement body)
    {
        var single = path.Length > "/Mail/claim/mail/".Length;
        var ids = single ? (uint.TryParse(path["/Mail/claim/mail/".Length..], NumberStyles.None, CultureInfo.InvariantCulture, out var one) ? [one] : [])
            : body.TryGetProperty("mailIds", out var list) && list.ValueKind == JsonValueKind.Array
                ? list.EnumerateArray().Where(i => i.TryGetUInt32(out _)).Select(i => i.GetUInt32()).Take(0x100).ToList() : [];
        var letters = Mail(current).ToDictionary(m => m.Id);
        var claimed = new JsonArray();
        foreach (var id in ids.Distinct())
        {
            if (!letters.TryGetValue(id, out var letter) || Grant($"mail/{id}", letter.Items) is not { } granted) continue;
            var items = granted.Select(g => ((uint)g!["mixId"]!, (long)g["amount"]!)).ToList();
            claimed.Add((JsonNode)new JsonObject
            {
                ["mailId"] = id, ["claimedItemAttachments"] = new JsonArray((JsonNode)Attachment(id, items, claimed: true)), ["claimedSeasonPassItemIds"] = new JsonArray()
            });
        }
        if (single) return claimed.Count > 0 ? claimed[0]!.DeepClone() : new JsonObject { ["mailId"] = ids.FirstOrDefault() };
        return new JsonObject { ["mail"] = claimed };
    }

    // What this profile has claimed: mail ids, from progression "hellforgeClaimedMail" ("mail/<id>", "order/<id>").
    HashSet<uint> Claimed()
    {
        if (profile == null || !profile.TryGetResponse("/Progression", out var progression) ||
            !progression.TryGetProperty("hellforgeClaimedMail", out var list) || list.ValueKind != JsonValueKind.Array) return [];
        return [.. list.EnumerateArray().Select(k => k.GetString() ?? "").Where(k => k.StartsWith("mail/", StringComparison.Ordinal))
            .Select(k => uint.TryParse(k[5..], NumberStyles.None, CultureInfo.InvariantCulture, out var id) ? id : 0).Where(id => id != 0)];
    }

    // Saves a reward once under its key; the items as added (within maxCap), or null when the key was claimed already.
    JsonArray? Grant(string key, List<(uint MixId, long Amount)> items)
    {
        if (profile == null) return null;
        var granted = new JsonArray();
        var saved = profile.TryChange((progression, inventory) =>
        {
            if (progression["hellforgeClaimedMail"] is not JsonArray keys) progression["hellforgeClaimedMail"] = keys = new JsonArray();
            if (keys.Any(k => k?.GetValue<string>() == key)) return false;
            keys.Add((JsonNode)key);
            foreach (var (mixId, amount) in items)
                granted.Add((JsonNode)new JsonObject { ["mixId"] = Held(mixId), ["amount"] = ProfileStore.AddItem(inventory, Held(mixId), amount) });
            return true;
        });
        if (saved) log($"War script reward {key} claimed: {string.Join(", ", items.Select(i => $"{i.Amount} x {i.MixId}"))}.");
        return saved ? granted : null;
    }
}
