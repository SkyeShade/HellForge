namespace HellForge.LocalBackend.Multiplayer;

// Pickups in co-op (PickupRelay): the game sends Mission/Activity to the picking player's backend only, which pays every
// participant in its reply; the game forwards each share to that player's game, and this tells their backends, so the
// share is kept in their profiles too.
//   "pickup.paid" [mission id, pile (objectId), activityId32, participant count, account ids...]
//   picker: a joiner sends it to the host of the lobby it joined, a host to every participant in its lobby.
//   host:   from a member of its lobby: credited here when it is a participant, and passed on to the other participants in
//           its lobby (joiners reach each other through their host, as with AssetShare).
//   joiner: from the host of the lobby it joined: credited here when it is a participant.
// The receiver works out the amount from the pile itself (MissionService.Rewards), and pays a pile once (ProfileStore.
// ApplyPickup), so a friend's frame can name a pile but not what it is worth.
public sealed class PickupShare
{
    public const string FrameType = "pickup.paid";
    const int MaxParticipants = 16, MaxMissionChars = 128;
    readonly IPeerTransport transport;
    readonly LobbyService lobby;
    readonly PickupRelay relay;
    readonly Action<string> log;

    public PickupShare(IPeerTransport transport, LobbyService lobby, PickupRelay relay, Action<string> log)
    {
        (this.transport, this.lobby, this.relay, this.log) = (transport, lobby, relay, log);
        relay.Paid += Paid;
    }

    // This machine's game picked up a pile and its backend paid it.
    void Paid(PickupRelay.Pickup pickup)
    {
        var local = transport.LocalId;
        var hosts = lobby.JoinedHosts();
        var targets = hosts.Count > 0 ? hosts.ToList() : [.. pickup.Participants.Where(p => p != local && lobby.InHostedLobby(p))];
        if (targets.Count == 0) return;
        var frame = Encode(pickup).Encode();
        foreach (var target in targets) transport.Send(target, frame, reliable: true);
        log($"Pickup: told {targets.Count} friend(s)' backend(s) of pile {pickup.Pile} (activity {pickup.Activity:X8}).");
    }

    public void FromPeer(ulong peer, Frame frame)
    {
        if (frame.Type != FrameType) return;
        PickupRelay.Pickup pickup;
        try { pickup = Decode(frame); }
        catch (InvalidDataException error) { log("Pickup: dropped a malformed frame: " + error.Message); return; }
        bool fromHost = lobby.HostsJoinedLobby(peer), fromMember = lobby.InHostedLobby(peer);
        if (!fromHost && !fromMember || !pickup.Participants.Contains(peer))
        {
            log($"Pickup: ignored pile {pickup.Pile} from a machine not in a lobby with this one.");
            return;
        }
        var local = transport.LocalId;
        if (pickup.Participants.Contains(local))
            log(relay.Credit?.Invoke(pickup) == true ? $"Pickup: credited pile {pickup.Pile} (activity {pickup.Activity:X8}) a friend picked up."
                : $"Pickup: pile {pickup.Pile} already paid here.");
        if (!fromMember) return;
        var bytes = frame.Encode();
        foreach (var other in pickup.Participants.Where(p => p != peer && p != local && lobby.InHostedLobby(p)))
            transport.Send(other, bytes, reliable: true);
    }

    static Frame Encode(PickupRelay.Pickup pickup)
    {
        var frame = new Frame(FrameType).Add(pickup.Mission).Add(pickup.Pile).Add(pickup.Activity).Add(pickup.Participants.Count);
        foreach (var participant in pickup.Participants) frame.Add(participant.ToString(System.Globalization.CultureInfo.InvariantCulture));
        return frame;
    }

    static PickupRelay.Pickup Decode(Frame frame)
    {
        var mission = frame.Text(0);
        if (mission.Length > MaxMissionChars) throw new InvalidDataException("mission id too long");
        if (!long.TryParse(frame.Text(1), System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out var pile))
            throw new InvalidDataException("bad pile");
        var activity = frame.UInt(2);
        var count = frame.Int(3);
        if (count is < 0 or > MaxParticipants) throw new InvalidDataException("bad participant count");
        var participants = new List<ulong>(count);
        for (var i = 0; i < count; i++)
            participants.Add(ulong.TryParse(frame.Text(4 + i), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var id)
                ? id : throw new InvalidDataException("bad participant"));
        return new PickupRelay.Pickup(mission, pile, activity, participants);
    }
}
