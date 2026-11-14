using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.LocalBackend;

// The game's only idle kick sends a player who is alone on the ship back to the title screen after 15 minutes without
// input (game.dll 0xADAF10). Online override 2283105102, a bool, turns it on; the live game serves "true". Each client
// reads its own (peerSync false), so every HellForge player's backend serves "false". The entry stays enabled: a
// disabled entry is not stored, and the client then looks the key up in a second table.
public static class IdleKick
{
    public const uint Override = 2283105102;

    // The /Configuration/GameClient body with the idle kick off; any other body is returned unchanged.
    public static JsonElement TurnOff(JsonElement client)
    {
        if (JsonNode.Parse(client.GetRawText()) is not JsonObject node ||
            node["onlineOverrideConfiguration"] is not JsonArray overrides) return client;
        foreach (var entry in overrides.OfType<JsonObject>())
            if (entry["mixId"] is JsonValue mix && mix.TryGetValue(out uint id) && id == Override &&
                entry["dataType"] is JsonValue type && type.TryGetValue(out int kind) && kind == 7)
                entry["data"] = "false";
        return JsonSerializer.SerializeToElement(node, BackendJson.Default.JsonNode);
    }
}
