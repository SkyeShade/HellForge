using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public static partial class LocalBackend
{
    // A request's answer: its status, the outcome the audit records, the body, and (a login only) what its gmiStr was.
    readonly record struct Reply(int Status, string Outcome, object Response, string? GmiState = null);

    // The backend's requests: each answered by its local contract, then recorded in the audit (requests.jsonl). Made by
    // Create, from what it set up.
    sealed class Requests
    {
        public required LoginMode Mode { get; init; }
        public required bool StartupFixtures { get; init; }
        public required ProfileStore? Profile { get; init; }
        public required MissionService? Missions { get; init; }
        public required FriendPresence Presence { get; init; }
        public required PlayFabService PlayFab { get; init; }
        public required string? CaptureBodies { get; init; }
        public required Dictionary<string, JsonElement> SeasonPassDetails { get; init; }
        public required WarClock WarClock { get; init; }
        public required AccountKeys AccountKeys { get; init; }
        public required RegisteredItems? RegisteredItems { get; init; }
        public required StartupBodies Startup { get; init; }
        public required string AuditPath { get; init; }
        // Issued session IDs, used only to classify the Authorization header in the audit.
        readonly ConcurrentDictionary<string, byte> sessions = new(StringComparer.Ordinal);
        readonly object auditLock = new();

        public async Task HandleAsync(HttpContext context)
        {
            var request = context.Request;
            var authorization = Authorization(request);
            Reply reply;
            try { reply = await RouteAsync(context); }
            catch (JsonException) { reply = new(StatusCodes.Status400BadRequest, "invalid_json", Error("HELLFORGE_INVALID_JSON")); }
            catch (BadHttpRequestException error) when (error.StatusCode == StatusCodes.Status413PayloadTooLarge)
            {
                reply = new(StatusCodes.Status413PayloadTooLarge, "body_too_large", Error("HELLFORGE_BODY_TOO_LARGE"));
            }
            Audit(request, reply, authorization);
            context.Response.StatusCode = reply.Status;
            context.Response.Headers.CacheControl = "no-store";
            context.Response.ContentType = "application/json; charset=utf-8";
            var payload = PadForClientDecoder(Utf8(reply.Response));
            context.Response.ContentLength = payload.Length;
            await context.Response.Body.WriteAsync(payload, context.RequestAborted);
        }

        // Assumes the client sends its formatted "session %s" value as Authorization (unverified).
        string Authorization(HttpRequest request) => request.Headers.Authorization.ToString() switch
        {
            "" => "missing",
            var value when value.StartsWith("session ", StringComparison.Ordinal) =>
                sessions.ContainsKey(value["session ".Length..]) ? "session_known" : "session_unknown",
            _ => "other_scheme"
        };

        // Never persist request bodies, credentials, headers, query strings or session IDs.
        // Paths are retained to identify the next missing local endpoint.
        void Audit(HttpRequest request, Reply reply, string authorization)
        {
            var record = new JsonObject
            {
                ["timeUtc"] = DateTimeOffset.UtcNow, ["method"] = request.Method, ["path"] = request.Path.Value,
                ["status"] = reply.Status, ["outcome"] = reply.Outcome, ["gmiState"] = reply.GmiState, ["authorization"] = authorization, ["mode"] = Mode.ToString(),
                ["startupFixtures"] = StartupFixtures
            }.ToJsonString();
            lock (auditLock) File.AppendAllText(AuditPath, record + Environment.NewLine);
        }

        static Reply Unsupported() =>
            new(StatusCodes.Status501NotImplemented, "unsupported_request", new JsonObject { ["errorCode"] = "HELLFORGE_UNSUPPORTED_REQUEST", ["message"] = "Local contract not implemented." });

        static Reply MethodRejected(HttpContext context, string allow)
        {
            context.Response.Headers.Allow = allow;
            return new(StatusCodes.Status405MethodNotAllowed, "method_rejected", Error("HELLFORGE_METHOD_NOT_ALLOWED"));
        }

        // A path that takes one method: another is rejected (null when this one is taken).
        static Reply? OtherMethod(HttpContext context, Func<string, bool> taken, string allow) =>
            taken(context.Request.Method) ? null : MethodRejected(context, allow);

        // The contracts in the order they are tried; a request none of them takes is unsupported.
        async Task<Reply> RouteAsync(HttpContext context)
        {
            var request = context.Request;
            if (!FromLocalProgram(request)) return new(StatusCodes.Status403Forbidden, "web_page_rejected", Error("HELLFORGE_NOT_FROM_THIS_PC"));
            if (request.Path == "/config/locator.json") return Locator(context);
            if (request.Path == "/Account/Login") return await LoginAsync(context);
            if (request.Path.StartsWithSegments("/lobby", out var lobbyPath)) return await LobbyAsync(context, lobbyPath.Value ?? "");
            if (request.Path == "/Account/Keys") return await AccountKeysAsync(context);
            if (request.Path.StartsWithSegments("/SeasonPass", out var seasonPassId) && seasonPassId.Value is { Length: > 1 } idPath &&
                SeasonPassDetails.TryGetValue(idPath[1..], out var seasonPassDetail))
                return OtherMethod(context, HttpMethods.IsGet, "GET") ?? new Reply(StatusCodes.Status200OK, "season_pass_detail", seasonPassDetail);
            return await GameRouteAsync(context);
        }

        // The game's own services: PlayFab, missions, the war clock, profile writes, and the startup bodies.
        async Task<Reply> GameRouteAsync(HttpContext context)
        {
            var request = context.Request;
            if (PlayFabService.Handles(request.Path.Value ?? "")) return await PlayFabAsync(context);
            if (Missions != null && MissionService.Handles(request.Path.Value ?? "")) return await MissionAsync(context, Missions);
            if (StartupFixtures && request.Path == "/WarSeason/801/timeSinceStart")
                return OtherMethod(context, HttpMethods.IsGet, "GET") ??
                    new Reply(StatusCodes.Status200OK, "local_war_clock", new JsonObject { ["secondsSinceStart"] = WarClock.Seconds });
            if (Profile != null && request.Path.Value == "/Progression/Items/Customize/v2" && HttpMethods.IsPut(request.Method))
                return await CustomizeAsync(context, Profile);
            if (request.Path == "/Account/ReportPosition") return await ReportPositionAsync(context);
            if (Profile != null && request.Path.Value is "/Progression/ShipName" or "/Progression/OnboardingState")
                return await ShipNameOrOnboardingAsync(context, Profile);
            if (Startup.TryGet(request.Path.Value ?? "", out var startupBody, out var source))
                // Weapons code mods registered this session join the catalogue, packages and inventory (RegisteredItems).
                return OtherMethod(context, HttpMethods.IsGet, "GET") ??
                    new Reply(StatusCodes.Status200OK, source, RegisteredItems?.Apply(request.Path.Value!, startupBody) ?? startupBody);
            return Unsupported();
        }

        static Reply Locator(HttpContext context)
        {
            if (OtherMethod(context, HttpMethods.IsGet, "GET") is { } rejected) return rejected;
            // Derive from the bound socket, never the untrusted Host header.
            var url = $"http://127.0.0.1:{context.Connection.LocalPort}";
            return new(StatusCodes.Status200OK, "local_backend_selected",
                new JsonObject { ["backends"] = new JsonArray((JsonNode)new JsonObject { ["url"] = url, ["validBranches"] = new JsonArray((JsonNode)"*") }) });
        }

        async Task<Reply> LoginAsync(HttpContext context)
        {
            if (OtherMethod(context, HttpMethods.IsPost, "POST") is { } rejected) return rejected;
            using var body = await JsonDocument.ParseAsync(context.Request.Body,
                cancellationToken: context.RequestAborted);
            if (body.RootElement.ValueKind != JsonValueKind.Object)
                throw new JsonException("Object required.");
            var gmiState = GmiState(body.RootElement);
            if (Mode == LoginMode.RejectGmi)
                return new(StatusCodes.Status403Forbidden, "gmi_rejection_control", new JsonObject { ["errorCode"] = 10002039, ["message"] = "Gmi str verification failed." },
                    gmiState);
            TakePlayerKey(body.RootElement);
            // 42 ASCII chars plus the client's "session " prefix fit 64 bytes.
            // This identifier has meaning only to this local experiment.
            var sessionId = "hellforge-local-" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));
            sessions.TryAdd(sessionId, 0);
            return new(StatusCodes.Status200OK, "local_session_created", new JsonObject { ["sessionId"] = sessionId }, gmiState);
        }

        static string GmiState(JsonElement login) =>
            !login.TryGetProperty("gmiStr", out var gmi) ? "missing"
            : gmi.ValueKind == JsonValueKind.Null ? "null"
            : gmi.ValueKind != JsonValueKind.String ? "other"
            : gmi.GetString()!.Length == 0 ? "empty" : "present";

        // The game's public key for this run; a host fetches it when this player joins (AccountKeys).
        void TakePlayerKey(JsonElement login)
        {
            if (login.TryGetProperty("publicKey", out var publicKey) && publicKey.ValueKind == JsonValueKind.String &&
                AccountKeys.IsKey(publicKey.GetString()))
            {
                AccountKeys.SetLocal(publicKey.GetString()!);
                AccountKeys.Log?.Invoke($"This player's key was received with the login ({publicKey.GetString()!.Length} characters).");
            }
            else AccountKeys.Log?.Invoke("The login carried no usable player key; friends cannot be admitted to this player's games.");
        }

        // The game's own lobby for friends' social tabs and the lobby directory, and friends' and the directory's lobbies
        // for its own social tab and Galactic Map (FriendPresence, Multiplayer.DirectoryService).
        async Task<Reply> LobbyAsync(HttpContext context, string rest)
        {
            var request = context.Request;
            var segments = rest.Split('/', StringSplitOptions.RemoveEmptyEntries);
            (string[] Methods, string Shape)? expected = segments switch
            {
                [] => (["PUT"], "lobby"),
                ["accounts"] => (["POST"], "accounts"),
                ["clear"] => (["DELETE"], "clear"),
                [_, "member"] => (["POST"], "member"),
                [_, "live"] => (["GET", "PUT"], "live"),
                [_] => (["GET"], "get"),
                _ => null
            };
            if (expected is not { } route) return Unsupported();
            if (!route.Methods.Contains(request.Method)) return MethodRejected(context, string.Join(", ", route.Methods));
            // The game ignores the replies to its own lobby uploads (no response handler for their requests).
            switch (route.Shape)
            {
                case "lobby":
                {
                    using var body = await LobbyBodyAsync(context);
                    Presence.SetLocalLobby(body.RootElement);
                    return new(StatusCodes.Status200OK, "lobby_published", new JsonObject());
                }
                case "member":
                    Presence.SetLocalLobbyId(segments[0]);
                    return new(StatusCodes.Status200OK, "lobby_member_alive", new JsonObject());
                case "clear":
                    Presence.ClearLocal();
                    return new(StatusCodes.Status200OK, "lobby_cleared", new JsonObject());
                case "live" when request.Method == "PUT":
                {
                    using var body = await LobbyBodyAsync(context);
                    Presence.SetLocalLive(segments[0], body.RootElement);
                    return new(StatusCodes.Status200OK, "lobby_live_published", new JsonObject());
                }
                // Never seen called by the game so far (docs/multiplayer.md step 7); a lobby unknown here is a 404, logged
                // with its outcome.
                case "live" or "get":
                {
                    var found = route.Shape == "live" ? Presence.LiveRecord(segments[0]) : Presence.LobbyRecord(segments[0]);
                    if (found == null) return new(StatusCodes.Status404NotFound, "lobby_unknown", Error("HELLFORGE_LOBBY_NOT_FOUND"));
                    return new(StatusCodes.Status200OK, route.Shape == "live" ? "lobby_live" : "lobby_record", found);
                }
                default:
                {
                    using var body = await LobbyBodyAsync(context);
                    return new(StatusCodes.Status200OK, "friend_lobbies", Presence.Lookup(LobbyAccountIds(body.RootElement)));
                }
            }
        }

        static Task<JsonDocument> LobbyBodyAsync(HttpContext context) =>
            JsonDocument.ParseAsync(context.Request.Body, new JsonDocumentOptions { AllowTrailingCommas = true }, context.RequestAborted);

        // POST /lobby/accounts {"ids": [...]}: the account ids, as text or numbers.
        static List<string> LobbyAccountIds(JsonElement body)
        {
            List<string> ids = [];
            if (body.ValueKind == JsonValueKind.Object && body.TryGetProperty("ids", out var list) && list.ValueKind == JsonValueKind.Array)
                foreach (var id in list.EnumerateArray())
                    if (id.ValueKind == JsonValueKind.String) ids.Add(id.GetString()!);
                    else if (id.ValueKind == JsonValueKind.Number) ids.Add(id.GetRawText());
            return ids;
        }

        async Task<Reply> AccountKeysAsync(HttpContext context)
        {
            if (OtherMethod(context, HttpMethods.IsGet, "GET") is { } rejected) return rejected;
            // ?id=<platform account id>; an unknown player gets an empty list, which leaves them unadmitted.
            var found = new JsonArray();
            foreach (var text in context.Request.Query["id"].SelectMany(v => (v ?? "").Split(',')))
            {
                if (!ulong.TryParse(text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id)) continue;
                if (await AccountKeys.WaitAsync(id, TimeSpan.FromSeconds(3), context.RequestAborted) is { } key)
                {
                    found.Add((JsonNode)new JsonObject { ["platformAccountId"] = text, ["key"] = key });
                    AccountKeys.Log?.Invoke($"Served the player key of account {id:X}.");
                }
                else AccountKeys.Log?.Invoke($"No player key for account {id:X}; that player cannot be admitted.");
            }
            return new(StatusCodes.Status200OK, found.Count > 0 ? "account_keys_served" : "account_keys_unknown", new JsonObject { ["accountKeys"] = found });
        }

        async Task<Reply> PlayFabAsync(HttpContext context)
        {
            var request = context.Request;
            if (OtherMethod(context, HttpMethods.IsPost, "POST") is { } rejected) return rejected;
            // The login body carries the player's Steam session ticket: parsed in memory, never captured or logged.
            // The engine's hand-written bodies have trailing commas (GetFriendsList, GetPlayerCombinedInfo).
            using var body = await JsonDocument.ParseAsync(request.Body, new JsonDocumentOptions { AllowTrailingCommas = true },
                context.RequestAborted);
            if (request.Path.Value!.EndsWith("/Profile/GetProfiles", StringComparison.Ordinal) || request.Path.Value.EndsWith("/Profile/GetProfile", StringComparison.Ordinal))
                await Presence.WaitForFriendObjectsAsync(TimeSpan.FromSeconds(6), context.RequestAborted);
            var (status, reply, outcome) = PlayFab.Handle(request.Path.Value!, body.RootElement);
            return new(status, outcome, reply);
        }

        async Task<Reply> MissionAsync(HttpContext context, MissionService missions)
        {
            var request = context.Request;
            if (OtherMethod(context, HttpMethods.IsPost, "POST") is { } rejected) return rejected;
            using var body = await JsonDocument.ParseAsync(request.Body, cancellationToken: context.RequestAborted);
            if (CaptureBodies != null)
                // Development capture of mission reports for decoding; kept under the evidence folder only.
                await File.WriteAllTextAsync(Path.Combine(CaptureBodies, DateTime.UtcNow.ToString("HHmmssfff") + "-" +
                    request.Path.Value!.Trim('/').Replace('/', '_') + ".json"), body.RootElement.GetRawText(), context.RequestAborted);
            var (response, outcome) = missions.Handle(request.Path.Value!, body.RootElement);
            return new(StatusCodes.Status200OK, outcome, response);
        }

        static async Task<Reply> CustomizeAsync(HttpContext context, ProfileStore profile)
        {
            using var body = await JsonDocument.ParseAsync(context.Request.Body, cancellationToken: context.RequestAborted);
            // The whole customization state comes back, as GET serves it, in case the client reads the answer.
            var saved = profile.TrySetCustomization(body.RootElement);
            return saved != null ? new(StatusCodes.Status200OK, "local_profile_saved", saved.Value)
                : new(StatusCodes.Status400BadRequest, "profile_write_rejected", Error("HELLFORGE_INVALID_PROFILE_WRITE"));
        }

        async Task<Reply> ReportPositionAsync(HttpContext context)
        {
            if (OtherMethod(context, HttpMethods.IsPost, "POST") is { } rejected) return rejected;
            using var body = await JsonDocument.ParseAsync(context.Request.Body,
                cancellationToken: context.RequestAborted);
            // The ship's position, kept for the next start (GET /Progression "location"). Without a saved
            // profile it is accepted and forgotten.
            var saved = Profile?.TrySetLocation(body.RootElement) ?? true;
            return new(saved ? StatusCodes.Status200OK : StatusCodes.Status400BadRequest,
                !saved ? "profile_write_rejected" : Profile != null ? "local_profile_saved" : "position_not_kept",
                saved ? new JsonObject() : Error("HELLFORGE_INVALID_PROFILE_WRITE"));
        }

        static async Task<Reply> ShipNameOrOnboardingAsync(HttpContext context, ProfileStore profile)
        {
            var request = context.Request;
            if (OtherMethod(context, HttpMethods.IsPut, "PUT") is { } rejected) return rejected;
            using var body = await JsonDocument.ParseAsync(request.Body,
                cancellationToken: context.RequestAborted);
            var saved = request.Path.Value == "/Progression/ShipName"
                ? profile.TrySetShipName(body.RootElement) : profile.TrySetOnboarding(body.RootElement);
            // Neither success handler reads response fields; an empty object completes the request.
            return saved ? new(StatusCodes.Status200OK, "local_profile_saved", new JsonObject())
                : new(StatusCodes.Status400BadRequest, "profile_write_rejected", Error("HELLFORGE_INVALID_PROFILE_WRITE"));
        }
    }
}
