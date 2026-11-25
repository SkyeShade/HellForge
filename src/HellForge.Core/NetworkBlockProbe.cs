using System.Net;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The network block's probe report, as every session reads it (app/scripts/OfflineControlNetworkProbe.cs, which
// NetworkProbe.exe is built from, or the session's own copy in the Linux namespace): {"loopbackHttpPassed",
// "externalIpv4TcpError", "externalIpv6TcpError"}, the outside route and datagram ("externalIpv4RouteError",
// "externalIpv4UdpError") from a probe that has them and, for a block narrowed to the backend's port, "loopbackTcpError"
// and "otherLoopbackTcpError". The block holds when the backend answers over loopback, a connect to a documentation
// address (192.0.2.1, which goes nowhere) is refused by the block itself (WSAEACCES) and, narrowed, so is another
// loopback port.
public static class NetworkBlockProbe
{
    public const int Refused = 10013;

    // Offline (ticket 0352): Windows fails the outside TCP connect in the stack, with one of these (WSAENETDOWN,
    // WSAENETUNREACH, WSAEHOSTUNREACH), so the block's refusal (always WSAEACCES) cannot show there. The block still counts
    // as proved on the outside when the probe saw no route out ("externalIpv4RouteError", GetBestInterface, 0 with a
    // route: nothing can leave), or when its UDP datagram to the same address was refused by the block
    // ("externalIpv4UdpError" 10013). The datagram's first packet is judged at the same ALE connect layer by the same
    // program-only BLOCK filter (native/network-block/hellforge_wfp.c, AddPair, no protocol condition), and unlike the
    // connect it does not wait on the next hop: the ticket's PC had a route while offline, and its connect failed with
    // 10065. A route with a datagram that went out never counts: the connect's unreachable may be a router's answer.
    // Offline, a narrowed block also needs the other loopback port refused, as always. The session proves the block again
    // while the game runs (PlaySession.CheckOfflineBlock).
    static readonly int[] NoRoute = [10050, 10051, 10065];

    // Null when the report shows the block holding; else what is wrong, in players' words.
    public static string? Problem(JsonNode? report, bool narrowed)
    {
        if (report is not JsonObject probe) return "the network block's check wrote no report";
        var outside = Error(probe, "externalIpv4TcpError");
        if (Passed(probe, "loopbackHttpPassed") != true)
            return outside == Refused ? "the network block stops the game reaching HellForge's own backend"
                : "the game cannot reach HellForge's own backend on this PC";
        var offline = NoNetwork(probe);
        if (offline && !narrowed)
            return $"this PC has no internet connection (error {outside}), and the network block can be checked without one only once it is narrowed to HellForge's backend";
        // -1: the connect was still waiting after 2.5 s, so it was let out (to an address that answers nothing)
        if (outside == -1) return "the network block is not stopping the game's connections to the internet";
        if (outside != Refused && !offline) return $"the network block is not stopping the game's connections to the internet (error {outside?.ToString() ?? "none"})";
        if (narrowed && Error(probe, "otherLoopbackTcpError") != Refused) return "the network block lets the game reach other programs on this PC";
        return null;
    }

    // The report is from an offline PC (NoRoute above): the outside connect failed in the stack, with no route out or with
    // the outside datagram refused. A probe built before the route check writes neither, and one built before the datagram
    // writes no datagram, so this is false for its report on a PC with a route.
    public static bool Offline(JsonNode? report) => report is JsonObject probe && NoNetwork(probe);

    static bool NoNetwork(JsonObject probe) =>
        Error(probe, "externalIpv4TcpError") is { } outside && NoRoute.Contains(outside) &&
        (Error(probe, "externalIpv4RouteError") is { } route && route != 0 || Error(probe, "externalIpv4UdpError") == Refused);

    // The interface Windows would send the probe's outside address through now (the probe's own route check): its index,
    // -1 with no route; null off Windows. A change (a route appearing, or another adapter taking over) is when the session
    // proves the block again at once.
    public static long? BestInterface()
    {
        if (!OperatingSystem.IsWindows()) return null;
        return GetBestInterface(BitConverter.ToUInt32(IPAddress.Parse("192.0.2.1").GetAddressBytes(), 0), out var index) == 0 ? index : -1;
    }

    // IPAddr is in_addr's form (network byte order): the address bytes as they are, read as one value.
    [DllImport("iphlpapi.dll")]
    static extern int GetBestInterface(uint destination, out uint index);

    static bool? Passed(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<bool>(out var passed) ? passed : null;

    static int? Error(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<int>(out var error) ? error : null;
}
