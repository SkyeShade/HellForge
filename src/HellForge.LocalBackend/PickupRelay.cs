namespace HellForge.LocalBackend;

// Between the backend's pickups (MissionService) and multiplayer (Multiplayer.PickupShare): a pile this backend paid, which
// the other players' backends credit too, and the credit for one a friend's backend paid. The game forwards every
// player's share to their game itself (0xA0823945); without this their backends never hear of it and lose it on reload.
public sealed class PickupRelay
{
    // A pile paid: the mission, the pile (the request's objectId), its activity and the players it pays (account ids).
    public sealed record Pickup(string Mission, long Pile, uint Activity, IReadOnlyList<ulong> Participants);

    // This backend paid a pile its game picked up.
    public event Action<Pickup>? Paid;

    // Credits this profile with a pile a friend's backend paid, once per pile: whether it changed anything. Set by the
    // backend's MissionService; null without a profile.
    public Func<Pickup, bool>? Credit { get; set; }

    internal void OnPaid(Pickup pickup) => Paid?.Invoke(pickup);
}
