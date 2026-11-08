using System.Net;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The network block's probe report, as every session reads it (app/scripts/OfflineControlNetworkProbe.cs, which
// NetworkProbe.exe is built from, or the session's own copy in the Linux namespace): {"loopbackHttpPassed",
// "externalIpv4TcpError", "externalIpv6TcpError"}, the outside route and connectivity ("externalIpv4RouteError",
// "internetConnectivity") from a probe that has them and, for a block narrowed to the backend's port, "loopbackTcpError"
// and "otherLoopbackTcpError". The block holds when the backend answers over loopback, a connect to a documentation
// address (192.0.2.1, which goes nowhere) is refused by the block itself (WSAEACCES) and, narrowed, so is another
// loopback port.
public static class NetworkBlockProbe
{
    public const int Refused = 10013;

    // No internet (ticket 0352): Windows fails the outside connect in the stack, with one of these (WSAENETDOWN,
    // WSAENETUNREACH, WSAEHOSTUNREACH), so the block's refusal (always WSAEACCES) cannot show. The PC counts as offline
    // when the probe saw no route out ("externalIpv4RouteError", GetBestInterface, 0 with a route), or a route while
    // Windows sees no internet on either protocol ("internetConnectivity", INetworkListManager::GetConnectivity's
    // NLM_CONNECTIVITY flags, -1 unread): a Wi-Fi network or adapter with no way on, as on the ticket's PC (10065 with a
    // route, flags without INTERNET). A route with the internet up never counts: a router's unreachable answer means the
    // connect got out. Offline, a narrowed block holds when the other loopback port is refused: its BLOCK filter names only
    // the program, so the filter refusing that port refuses the outside too (native/network-block/hellforge_wfp.c,
    // AddPair). The session proves the outside again while the game runs (PlaySession.CheckOfflineBlock).
    static readonly int[] NoRoute = [10050, 10051, 10065];

    // NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET (netlistmgr.h)
    const int Internet = 0x40 | 0x400;

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

    // The report is from an offline PC (NoRoute above): the outside connect failed in the stack, with no route out or no
    // internet. A probe built before the route check, or before the connectivity check on a PC with a route, writes
    // neither, so this is false for its report.
    public static bool Offline(JsonNode? report) => report is JsonObject probe && NoNetwork(probe);

    static bool NoNetwork(JsonObject probe) =>
        Error(probe, "externalIpv4TcpError") is { } outside && NoRoute.Contains(outside) &&
        (Error(probe, "externalIpv4RouteError") is { } route && route != 0 ||
         Error(probe, "internetConnectivity") is { } connectivity && connectivity >= 0 && (connectivity & Internet) == 0);

    // Whether Windows has a route to the probe's outside address now (the probe's own check); null off Windows.
    public static bool? HasRoute()
    {
        if (!OperatingSystem.IsWindows()) return null;
        return GetBestInterface(BitConverter.ToUInt32(IPAddress.Parse("192.0.2.1").GetAddressBytes(), 0), out _) == 0;
    }

    // IPAddr is in_addr's form (network byte order): the address bytes as they are, read as one value.
    [DllImport("iphlpapi.dll")]
    static extern int GetBestInterface(uint destination, out uint index);

    static bool? Passed(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<bool>(out var passed) ? passed : null;

    static int? Error(JsonObject probe, string key) =>
        probe[key] is JsonValue value && value.TryGetValue<int>(out var error) ? error : null;
}
