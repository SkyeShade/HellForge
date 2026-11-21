using System.Text.Json.Nodes;

namespace HellForge.Core;

// The network block's watch for a session that started offline (NetworkBlockProbe.Offline, ticket 0352): when the probe
// runs again, and what its result means. PlaySession.CheckOfflineBlock runs it from the observation loop each second.
public sealed class OfflineBlockWatch(long? outsideInterface)
{
    public const long IntervalMs = 10000;

    public enum Verdict { StillOffline, ProvenOnline, Failed }

    long next;
    long? last = outsideInterface;

    // Whether the probe is due at nowMs (the observation clock): every IntervalMs after the last run (the first look is
    // due at once), and at once when the interface Windows sends the outside through changes (NetworkBlockProbe.BestInterface:
    // a route appearing, or another adapter taking over, as when Wi-Fi joins a network).
    public bool Due(long nowMs, long? outsideInterface)
    {
        var changed = outsideInterface != last;
        last = outsideInterface;
        return changed || nowMs >= next;
    }

    // The probe finished at nowMs: the next one is due IntervalMs later.
    public void Ran(long nowMs) => next = nowMs + IntervalMs;

    // One probe run (RunNetworkProbe's problem, and the report it left): a problem fails; else the outside still offline
    // keeps watching, and the outside refused (Problem's only other null) proves the block online.
    public static Verdict Judge(string? problem, JsonNode? report) =>
        problem != null ? Verdict.Failed : NetworkBlockProbe.Offline(report) ? Verdict.StillOffline : Verdict.ProvenOnline;
}
