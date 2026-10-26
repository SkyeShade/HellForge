using System.Text.Json.Nodes;

namespace HellForge.Core;

// The network block's probe report, as every session reads it (app/scripts/OfflineControlNetworkProbe.cs, which
// NetworkProbe.exe is built from, or the session's own copy in the Linux namespace): {"loopbackHttpPassed",
// "externalIpv4TcpError", "externalIpv6TcpError"} and, for a block narrowed to the backend's port, "loopbackTcpError"
// and "otherLoopbackTcpError". The block holds when the backend answers over loopback, a connect to a documentation
// address (192.0.2.1, which goes nowhere) is refused by the block itself (WSAEACCES) and, narrowed, so is another
// loopback port.
public static class NetworkBlockProbe
{
    public const int Refused = 10013;

    // Null when the report shows the block holding; else what is wrong, in players' words.
    public static string? Problem(JsonNode? report, bool narrowed)
    {
        if (report is not JsonObject probe) return "the network block's check wrote no report";
        var outside = Error(probe, "externalIpv4TcpError");
        if (Passed(probe, "loopbackHttpPassed") != true)
            return outside == Refused ? "the network block stops the game reaching HellForge's own backend"
                : "the game cannot reach HellForge's own backend on this PC";
        // -1: the connect was still waiting after 2.5 s, so it was let out (to an address that answers nothing)
        if (outside == -1) return "the network block is not stopping the game's connections to the internet";
        if (outside != Refused) return $"the network block is not stopping the game's connections to the internet (error {outside?.ToString() ?? "none"})";
        if (narrowed && Error(probe, "otherLoopbackTcpError") != Refused) return "the network block lets the game reach other programs on this PC";
        return null;
    }

    static bool? Passed(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<bool>(out var passed) ? passed : null;

    static int? Error(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<int>(out var error) ? error : null;
}
