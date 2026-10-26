using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

public enum LoginMode { LocalSession, RejectGmi }

// A synthetic backend contract probe, not a Steam ownership verifier or a game launcher.
public static class LocalBackend
{
    public const int MaximumBodyBytes = 1024 * 1024;

    // The error bodies the backend answers with ({"errorCode": "HELLFORGE_..."}).
    static JsonObject Error(string code) => new() { ["errorCode"] = code };

    // A response body: a JSON node or element, written compactly as the serializer writes them.
    static byte[] Utf8(object response)
    {
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            switch (response)
            {
                case JsonNode node: node.WriteTo(writer); break;
                case JsonElement element: element.WriteTo(writer); break;
                default: throw new InvalidOperationException("A response must be JSON: " + response.GetType().Name);
            }
        }
        return buffer.ToArray();
    }

    // The client's JSON decoder sizes its node buffer from the body length, so short or dense
    // bodies such as {"id":1} fail with "out of memory" (shown as error 0, status 200).
    // Trailing whitespace adds capacity without adding nodes; see scripts/ProbeSavedJsonDecoder.py.
    public static byte[] PadForClientDecoder(byte[] json)
    {
        var padded = new byte[json.Length + Math.Max(json.Length, 16)];
        json.CopyTo(padded, 0);
        padded.AsSpan(json.Length).Fill((byte)' ');
        return padded;
    }

    public static JsonDocument LoadResource(string name)
    {
        using var stream = typeof(LocalBackend).Assembly.GetManifestResourceStream("HellForge.LocalBackend." + name)
            ?? throw new InvalidOperationException("Missing embedded resource " + name);
        return JsonDocument.Parse(stream);
    }

    // A "bodySource" names an embedded data file and optional "#key" (default "items") whose value is the
    // body: the item catalogue and progression data rebuilt from the user's own snapshot by
    // scripts/BuildItemCatalog.py, and the game client's feature switches read from it (game-client-config.json).
    public static Dictionary<string, JsonElement> LoadStartupFixtures()
    {
        using var document = LoadResource("startup-fixtures.json");
        var fixtures = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (var endpoint in document.RootElement.GetProperty("endpoints").EnumerateArray())
        {
            var body = endpoint.GetProperty("body").Clone();
            if (endpoint.TryGetProperty("bodySource", out var source))
            {
                var parts = source.GetString()!.Split('#');
                using var data = LoadResource(parts[0]);
                body = data.RootElement.GetProperty(parts.Length > 1 ? parts[1] : "items").Clone();
            }
            fixtures.Add(endpoint.GetProperty("path").GetString()!, body);
        }
        return fixtures;
    }

    // The caller owns a supplied profile. Without one, fixtures use an unsaved default profile.
    // itemCatalog (development) replaces the embedded catalogue with a JSON file's "items" array;
    // fixtureOverride (development) is a JSON object mapping endpoint paths to replacement bodies.
    public static WebApplication Create(int port, string evidenceDirectory, LoginMode mode, bool startupFixtures = false,
        ProfileStore? profile = null, string? itemCatalog = null, string? fixtureOverride = null, bool captureMissionBodies = false,
        WarClock? warClock = null, AccountKeys? accountKeys = null, LiveWar? liveWar = null, FriendPresence? presence = null,
        RegisteredItems? registeredItems = null, bool openWar = false, IReadOnlyList<string>? dssEverywhere = null)
    {
        if (port is < 0 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        if (!Enum.IsDefined(mode)) throw new ArgumentOutOfRangeException(nameof(mode));
        if (startupFixtures && mode != LoginMode.LocalSession)
            throw new ArgumentException("Startup fixtures require local-session mode.");
        if (profile != null && !startupFixtures) throw new ArgumentException("A profile requires startup fixtures.");
        var fixtures = startupFixtures ? LoadStartupFixtures() : new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (itemCatalog != null)
        {
            if (!startupFixtures) throw new ArgumentException("An item catalogue requires startup fixtures.");
            using var data = JsonDocument.Parse(File.ReadAllText(itemCatalog));
            fixtures["/Progression/items"] = data.RootElement.GetProperty("items").Clone();
        }
        if (fixtureOverride != null)
        {
            if (!startupFixtures) throw new ArgumentException("A fixture override requires startup fixtures.");
            using var data = JsonDocument.Parse(File.ReadAllText(fixtureOverride));
            foreach (var entry in data.RootElement.EnumerateObject())
            {
                if (!fixtures.ContainsKey(entry.Name)) throw new ArgumentException("Override for an unknown fixture: " + entry.Name);
                fixtures[entry.Name] = entry.Value.Clone();
            }
        }
        // --war open: the all-planets war (OpenWar), made from the frozen war's bodies; it replaces the live war.
        if (openWar)
        {
            if (!startupFixtures) throw new ArgumentException("The all-planets war requires startup fixtures.");
            fixtures[$"/WarSeason/{LiveWar.WarId}/Status"] = OpenWar.Status(fixtures[$"/WarSeason/{LiveWar.WarId}/warinfo"], fixtures[$"/WarSeason/{LiveWar.WarId}/Status"]);
            liveWar = null;
        }
        // --dss-everywhere <keys>: those DSS effects on every planet of whichever war is served (DssEverywhere).
        if (dssEverywhere != null && !startupFixtures) throw new ArgumentException("DSS effects on every planet require startup fixtures.");
        // The game learns what those ids do from the effect definitions it reads at login (DssEverywhere.Definitions).
        if (dssEverywhere != null) fixtures[DssEverywhere.DefinitionsPath] = DssEverywhere.Definitions(fixtures[DssEverywhere.DefinitionsPath]);
        if (startupFixtures) profile ??= ProfileStore.Open(null);
        // Issued session IDs, used only to classify the Authorization header in the audit.
        var sessions = new ConcurrentDictionary<string, byte>(StringComparer.Ordinal);
        var missions = profile != null ? new MissionService(profile) : null;
        // Friends' lobbies and profiles for the social tab, filled by multiplayer when a caller supplies it.
        presence ??= new FriendPresence();
        var playFab = new PlayFabService(presence);
        string? captureBodies = captureMissionBodies ? Path.Combine(evidenceDirectory, "mission-bodies") : null;
        if (captureBodies != null) Directory.CreateDirectory(captureBodies);
        // Warbond pages (GET /SeasonPass/<id32>, request 0x6a23fbc2, parser 0x1068a80) from the snapshot export.
        var seasonPassDetails = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (startupFixtures)
        {
            using var data = LoadResource("progression-data.json");
            if (data.RootElement.TryGetProperty("seasonPassDetails", out var details))
                foreach (var detail in details.EnumerateObject()) seasonPassDetails[detail.Name] = detail.Value.Clone();
        }
        // The war clock continues from the imported war's time; the client re-polls it (parser 0x10817b0). A caller's
        // clock is shared with multiplayer, which aligns it with a host's.
        warClock ??= new WarClock();
        // A live war (LiveWar) has set the clock from its own Status; otherwise it continues from the embedded war.
        // Players' public keys, shared with multiplayer when a caller supplies them.
        accountKeys ??= new AccountKeys();
        if (startupFixtures && liveWar?.HasWar != true)
        {
            using var war = LoadResource("war-data.json");
            warClock.Set(war.RootElement.GetProperty("status").GetProperty("time").GetInt64());
        }
        bool TryStartupBody(string path, out JsonElement body, out string source)
        {
            source = "local_profile";
            if (profile != null && profile.TryGetResponse(path, out body)) return true;
            source = "live_war";
            if (liveWar != null && fixtures.ContainsKey(path) && liveWar.TryGet(path, out body)) return true;
            source = "synthetic_startup_fixture";
            return fixtures.TryGetValue(path, out body);
        }
        if (registeredItems != null) registeredItems.Source = path => TryStartupBody(path, out var body, out _) ? body : null;
        Directory.CreateDirectory(evidenceDirectory);
        var auditPath = Path.Combine(evidenceDirectory, "requests.jsonl");
        // A fresh evidence directory prevents accidental mixing of runs.
        using (new FileStream(auditPath, FileMode.CreateNew, FileAccess.Write)) { }
        var auditLock = new object();
        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions
        {
            ApplicationName = typeof(LocalBackend).Assembly.GetName().Name,
            EnvironmentName = "Production"
        });
        builder.WebHost.UseKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, port);
            options.Limits.MaxRequestBodySize = MaximumBodyBytes;
        });
        var app = builder.Build();
        app.Run(async context =>
        {
            var request = context.Request;
            string? gmiState = null;
            // Assumes the client sends its formatted "session %s" value as Authorization (unverified).
            var authorization = request.Headers.Authorization.ToString() switch
            {
                "" => "missing",
                var value when value.StartsWith("session ", StringComparison.Ordinal) =>
                    sessions.ContainsKey(value["session ".Length..]) ? "session_known" : "session_unknown",
                _ => "other_scheme"
            };
            var outcome = "unsupported_request";
            var status = StatusCodes.Status501NotImplemented;
            object response = new JsonObject { ["errorCode"] = "HELLFORGE_UNSUPPORTED_REQUEST", ["message"] = "Local contract not implemented." };
            try
            {
                if (request.Path == "/config/locator.json")
                {
                    if (!HttpMethods.IsGet(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "GET";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        status = StatusCodes.Status200OK;
                        outcome = "local_backend_selected";
                        // Derive from the bound socket, never the untrusted Host header.
                        var url = $"http://127.0.0.1:{context.Connection.LocalPort}";
                        response = new JsonObject { ["backends"] = new JsonArray((JsonNode)new JsonObject { ["url"] = url, ["validBranches"] = new JsonArray((JsonNode)"*") }) };
                    }
                }
                else if (request.Path == "/Account/Login")
                {
                    if (!HttpMethods.IsPost(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "POST";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        using var body = await JsonDocument.ParseAsync(request.Body,
                            cancellationToken: context.RequestAborted);
                        if (body.RootElement.ValueKind != JsonValueKind.Object)
                            throw new JsonException("Object required.");
                        gmiState = !body.RootElement.TryGetProperty("gmiStr", out var gmi) ? "missing"
                            : gmi.ValueKind == JsonValueKind.Null ? "null"
                            : gmi.ValueKind != JsonValueKind.String ? "other"
                            : gmi.GetString()!.Length == 0 ? "empty" : "present";
                        if (mode == LoginMode.RejectGmi)
                        {
                            status = StatusCodes.Status403Forbidden;
                            outcome = "gmi_rejection_control";
                            response = new JsonObject { ["errorCode"] = 10002039, ["message"] = "Gmi str verification failed." };
                        }
                        else
                        {
                            // The game's public key for this run; a host fetches it when this player joins (AccountKeys).
                            if (body.RootElement.TryGetProperty("publicKey", out var publicKey) && publicKey.ValueKind == JsonValueKind.String &&
                                AccountKeys.IsKey(publicKey.GetString()))
                            {
                                accountKeys.SetLocal(publicKey.GetString()!);
                                accountKeys.Log?.Invoke($"This player's key was received with the login ({publicKey.GetString()!.Length} characters).");
                            }
                            else accountKeys.Log?.Invoke("The login carried no usable player key; friends cannot be admitted to this player's games.");
                            status = StatusCodes.Status200OK;
                            outcome = "local_session_created";
                            // 42 ASCII chars plus the client's "session " prefix fit 64 bytes.
                            // This identifier has meaning only to this local experiment.
                            var sessionId = "hellforge-local-" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));
                            sessions.TryAdd(sessionId, 0);
                            response = new JsonObject { ["sessionId"] = sessionId };
                        }
                    }
                }
                else if (request.Path.StartsWithSegments("/lobby", out var lobbyPath))
                {
                    // The game's own lobby for friends' social tabs, and friends' lobbies for its own (FriendPresence).
                    var rest = lobbyPath.Value ?? "";
                    var segments = rest.Split('/', StringSplitOptions.RemoveEmptyEntries);
                    (string Method, string Shape)? expected = segments switch
                    {
                        [] => ("PUT", "lobby"),
                        ["accounts"] => ("POST", "accounts"),
                        ["clear"] => ("DELETE", "clear"),
                        [_, "member"] => ("POST", "member"),
                        _ => null
                    };
                    if (expected is not { } route) { }
                    else if (request.Method != route.Method)
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = route.Method;
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        status = StatusCodes.Status200OK;
                        // The game ignores the replies to its own lobby uploads (no response handler for their requests).
                        response = new JsonObject();
                        switch (route.Shape)
                        {
                            case "lobby":
                            {
                                using var body = await JsonDocument.ParseAsync(request.Body, new JsonDocumentOptions { AllowTrailingCommas = true }, context.RequestAborted);
                                presence.SetLocalLobby(body.RootElement);
                                outcome = "lobby_published";
                                break;
                            }
                            case "member":
                                presence.SetLocalLobbyId(segments[0]);
                                outcome = "lobby_member_alive";
                                break;
                            case "clear":
                                presence.ClearLocal();
                                outcome = "lobby_cleared";
                                break;
                            default:
                            {
                                using var body = await JsonDocument.ParseAsync(request.Body, new JsonDocumentOptions { AllowTrailingCommas = true }, context.RequestAborted);
                                List<string> ids = [];
                                if (body.RootElement.ValueKind == JsonValueKind.Object && body.RootElement.TryGetProperty("ids", out var list) && list.ValueKind == JsonValueKind.Array)
                                    foreach (var id in list.EnumerateArray())
                                        if (id.ValueKind == JsonValueKind.String) ids.Add(id.GetString()!);
                                        else if (id.ValueKind == JsonValueKind.Number) ids.Add(id.GetRawText());
                                response = presence.Lookup(ids);
                                outcome = "friend_lobbies";
                                break;
                            }
                        }
                    }
                }
                else if (request.Path == "/Account/Keys")
                {
                    if (!HttpMethods.IsGet(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "GET";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        // ?id=<platform account id>; an unknown player gets an empty list, which leaves them unadmitted.
                        var found = new JsonArray();
                        foreach (var text in request.Query["id"].SelectMany(v => (v ?? "").Split(',')))
                        {
                            if (!ulong.TryParse(text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id)) continue;
                            if (await accountKeys.WaitAsync(id, TimeSpan.FromSeconds(3), context.RequestAborted) is { } key)
                            {
                                found.Add((JsonNode)new JsonObject { ["platformAccountId"] = text, ["key"] = key });
                                accountKeys.Log?.Invoke($"Served the player key of account {id:X}.");
                            }
                            else accountKeys.Log?.Invoke($"No player key for account {id:X}; that player cannot be admitted.");
                        }
                        status = StatusCodes.Status200OK;
                        outcome = found.Count > 0 ? "account_keys_served" : "account_keys_unknown";
                        response = new JsonObject { ["accountKeys"] = found };
                    }
                }
                else if (request.Path.StartsWithSegments("/SeasonPass", out var seasonPassId) && seasonPassId.Value is { Length: > 1 } idPath &&
                    seasonPassDetails.TryGetValue(idPath[1..], out var seasonPassDetail))
                {
                    if (!HttpMethods.IsGet(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "GET";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        status = StatusCodes.Status200OK;
                        outcome = "season_pass_detail";
                        response = seasonPassDetail;
                    }
                }
                else if (PlayFabService.Handles(request.Path.Value ?? ""))
                {
                    if (!HttpMethods.IsPost(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "POST";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        // The login body carries the player's Steam session ticket: parsed in memory, never captured or logged.
                        // The engine's hand-written bodies have trailing commas (GetFriendsList, GetPlayerCombinedInfo).
                        using var body = await JsonDocument.ParseAsync(request.Body, new JsonDocumentOptions { AllowTrailingCommas = true },
                            context.RequestAborted);
                        if (request.Path.Value!.EndsWith("/Profile/GetProfiles", StringComparison.Ordinal) || request.Path.Value.EndsWith("/Profile/GetProfile", StringComparison.Ordinal))
                            await presence.WaitForFriendObjectsAsync(TimeSpan.FromSeconds(6), context.RequestAborted);
                        (status, var reply, outcome) = playFab.Handle(request.Path.Value!, body.RootElement);
                        response = reply;
                    }
                }
                else if (missions != null && MissionService.Handles(request.Path.Value ?? ""))
                {
                    if (!HttpMethods.IsPost(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "POST";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        using var body = await JsonDocument.ParseAsync(request.Body, cancellationToken: context.RequestAborted);
                        if (captureBodies != null)
                            // Development capture of mission reports for decoding; kept under the evidence folder only.
                            await File.WriteAllTextAsync(Path.Combine(captureBodies, DateTime.UtcNow.ToString("HHmmssfff") + "-" +
                                request.Path.Value!.Trim('/').Replace('/', '_') + ".json"), body.RootElement.GetRawText(), context.RequestAborted);
                        (response, outcome) = missions.Handle(request.Path.Value!, body.RootElement);
                        status = StatusCodes.Status200OK;
                    }
                }
                else if (startupFixtures && request.Path == "/WarSeason/801/timeSinceStart")
                {
                    if (!HttpMethods.IsGet(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "GET";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        status = StatusCodes.Status200OK;
                        outcome = "local_war_clock";
                        response = new JsonObject { ["secondsSinceStart"] = warClock.Seconds };
                    }
                }
                else if (profile != null && request.Path.Value == "/Progression/Items/Customize/v2" && HttpMethods.IsPut(request.Method))
                {
                    using var body = await JsonDocument.ParseAsync(request.Body, cancellationToken: context.RequestAborted);
                    // The whole customization state comes back, as GET serves it, in case the client reads the answer.
                    var saved = profile.TrySetCustomization(body.RootElement);
                    status = saved != null ? StatusCodes.Status200OK : StatusCodes.Status400BadRequest;
                    outcome = saved != null ? "local_profile_saved" : "profile_write_rejected";
                    response = saved != null ? (object)saved.Value : Error("HELLFORGE_INVALID_PROFILE_WRITE");
                }
                else if (request.Path == "/Account/ReportPosition")
                {
                    if (!HttpMethods.IsPost(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "POST";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        using var body = await JsonDocument.ParseAsync(request.Body,
                            cancellationToken: context.RequestAborted);
                        // The ship's position, kept for the next start (GET /Progression "location"). Without a saved
                        // profile it is accepted and forgotten.
                        var saved = profile?.TrySetLocation(body.RootElement) ?? true;
                        status = saved ? StatusCodes.Status200OK : StatusCodes.Status400BadRequest;
                        outcome = !saved ? "profile_write_rejected" : profile != null ? "local_profile_saved" : "position_not_kept";
                        response = saved ? new JsonObject() : Error("HELLFORGE_INVALID_PROFILE_WRITE");
                    }
                }
                else if (profile != null && request.Path.Value is "/Progression/ShipName" or "/Progression/OnboardingState")
                {
                    if (!HttpMethods.IsPut(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "PUT";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        using var body = await JsonDocument.ParseAsync(request.Body,
                            cancellationToken: context.RequestAborted);
                        var saved = request.Path.Value == "/Progression/ShipName"
                            ? profile.TrySetShipName(body.RootElement) : profile.TrySetOnboarding(body.RootElement);
                        // Neither success handler reads response fields; an empty object completes the request.
                        status = saved ? StatusCodes.Status200OK : StatusCodes.Status400BadRequest;
                        outcome = saved ? "local_profile_saved" : "profile_write_rejected";
                        response = saved ? new JsonObject() : Error("HELLFORGE_INVALID_PROFILE_WRITE");
                    }
                }
                else if (TryStartupBody(request.Path.Value ?? "", out var startupBody, out var source))
                {
                    if (!HttpMethods.IsGet(request.Method))
                    {
                        status = StatusCodes.Status405MethodNotAllowed;
                        context.Response.Headers.Allow = "GET";
                        outcome = "method_rejected";
                        response = Error("HELLFORGE_METHOD_NOT_ALLOWED");
                    }
                    else
                    {
                        status = StatusCodes.Status200OK;
                        outcome = source;
                        // The picked DSS effects join every planet of the war status (DssEverywhere).
                        if (dssEverywhere != null && string.Equals(request.Path.Value, $"/WarSeason/{LiveWar.WarId}/Status", StringComparison.OrdinalIgnoreCase))
                            startupBody = DssEverywhere.Status(startupBody, dssEverywhere);
                        // Weapons code mods registered this session join the catalogue, packages and inventory (RegisteredItems).
                        response = registeredItems?.Apply(request.Path.Value!, startupBody) ?? startupBody;
                    }
                }
            }
            catch (JsonException)
            {
                status = StatusCodes.Status400BadRequest;
                outcome = "invalid_json";
                response = Error("HELLFORGE_INVALID_JSON");
            }
            catch (BadHttpRequestException error) when (error.StatusCode == StatusCodes.Status413PayloadTooLarge)
            {
                status = StatusCodes.Status413PayloadTooLarge;
                outcome = "body_too_large";
                response = Error("HELLFORGE_BODY_TOO_LARGE");
            }

            // Never persist request bodies, credentials, headers, query strings or session IDs.
            // Paths are retained to identify the next missing local endpoint.
            var record = new JsonObject
            {
                ["timeUtc"] = DateTimeOffset.UtcNow, ["method"] = request.Method, ["path"] = request.Path.Value,
                ["status"] = status, ["outcome"] = outcome, ["gmiState"] = gmiState, ["authorization"] = authorization, ["mode"] = mode.ToString(),
                ["startupFixtures"] = startupFixtures
            }.ToJsonString();
            lock (auditLock) File.AppendAllText(auditPath, record + Environment.NewLine);
            context.Response.StatusCode = status;
            context.Response.Headers.CacheControl = "no-store";
            context.Response.ContentType = "application/json; charset=utf-8";
            var payload = PadForClientDecoder(Utf8(response));
            context.Response.ContentLength = payload.Length;
            await context.Response.Body.WriteAsync(payload, context.RequestAborted);
        });
        return app;
    }
}
