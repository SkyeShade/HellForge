using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.LocalBackend;
using HellForge.LocalBackend.Multiplayer;

// Pickups in co-op, end to end: a host and two joiners over the in-memory network, each with its own profile, missions and
// PickupShare. A joiner's pickup pays the host and, through it, the other joiner; the host's pays both joiners; a pile is
// paid once everywhere; a machine outside the lobby, or a frame that does not name its sender, pays nobody. The receipt
// survives a restart.
static class PickupShareTests
{
    const ulong HostId = 0x110000100000031, FirstId = 0x110000100000032, SecondId = 0x110000100000033, StrangerId = 0x110000100000034;
    const uint Wallet = PaidContentSet.ForgeCreditsWallet;

    sealed class Machine : IDisposable
    {
        public readonly ProfileStore Store = ProfileStore.Open(null, "starter");
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly IPeerTransport Link;
        public readonly LobbyService Lobby;
        public readonly MissionService Missions;

        public Machine(InMemoryNetwork network, ulong id, ConcurrentQueue<string> log)
        {
            Link = network.Join(id);
            Lobby = new LobbyService(Link, Inbox.Add, _ => { }, "mods");
            var relay = new PickupRelay();
            Missions = new MissionService(Store, relay);
            var share = new PickupShare(Link, Lobby, relay, line => log.Enqueue($"{id:X}: {line}"));
            Link.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("pickup.", StringComparison.Ordinal)) share.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
        }

        public long Held(uint mixId) => Store.Current()["inventory"]!["items"]!.AsArray().OfType<JsonObject>()
            .Where(i => (uint)long.Parse(i["mixId"]!.ToJsonString(), System.Globalization.CultureInfo.InvariantCulture) == mixId)
            .Sum(i => long.Parse(i["amount"]!.ToJsonString(), System.Globalization.CultureInfo.InvariantCulture));

        // This machine's game picked up a pile: Mission/Activity with the lobby's players.
        public void PickUp(string mission, uint activity, long pile, params ulong[] participants)
        {
            using var body = JsonDocument.Parse("{\"missionId\":\"" + mission + "\",\"activityId32\":" + activity + ",\"objectId\":" + pile +
                ",\"participants\":[" + string.Join(",", participants) + "],\"gmiStr\":\"\"}");
            Missions.Handle("/Operation/Mission/Activity", body.RootElement);
        }

        public void Dispose() { Store.Dispose(); Link.Dispose(); }
    }

    static bool Eventually(Func<bool> test)
    {
        for (var deadline = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < deadline; Thread.Sleep(50)) if (test()) return true;
        return test();
    }

    public static void Run(string root, Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var log = new ConcurrentQueue<string>();
        using var host = new Machine(network, HostId, log);
        using var first = new Machine(network, FirstId, log);
        using var second = new Machine(network, SecondId, log);
        using var stranger = new Machine(network, StrangerId, log);

        host.Lobby.FromLocal(MultiplayerTests.Create("c1", HostId));
        host.Inbox.TryTake(out var created, TimeSpan.FromSeconds(5));
        var connection = created!.Text(4);
        first.Lobby.FromLocal(MultiplayerTests.Join("j1", FirstId, connection));
        second.Lobby.FromLocal(MultiplayerTests.Join("j2", SecondId, connection));
        check(Eventually(() => host.Lobby.InHostedLobby(FirstId) && host.Lobby.InHostedLobby(SecondId) &&
            first.Lobby.HostsJoinedLobby(HostId) && second.Lobby.HostsJoinedLobby(HostId)), "Pickups: both joiners are in the host's lobby.");

        // A joiner picks up Forge Credits: its own backend, the host's and (through the host) the other joiner's keep it.
        const long Pile = 424242;
        var amount = MissionService.Rewards.ForgeCredits(Pile);
        first.PickUp("m1", MissionService.ForgeCreditsActivity, Pile, HostId, FirstId, SecondId);
        check(Eventually(() => first.Held(Wallet) == amount && host.Held(Wallet) == amount && second.Held(Wallet) == amount),
            $"A joiner's pickup pays every backend once ({amount}): first {first.Held(Wallet)}, host {host.Held(Wallet)}, second {second.Held(Wallet)}.");

        // The host picks up medals: both joiners keep them. The pile again (a retried request) pays nobody twice.
        var medals = (first.Held(MissionService.MedalsMixId), second.Held(MissionService.MedalsMixId));
        var medalAmount = MissionService.Rewards.MedalPickup(77);
        host.PickUp("m1", MissionService.MedalsActivity, 77, HostId, FirstId, SecondId);
        check(Eventually(() => first.Held(MissionService.MedalsMixId) == medals.Item1 + medalAmount && second.Held(MissionService.MedalsMixId) == medals.Item2 + medalAmount),
            "The host's medal pickup pays both joiners.");
        first.PickUp("m1", MissionService.ForgeCreditsActivity, Pile, HostId, FirstId, SecondId);
        host.PickUp("m1", MissionService.MedalsActivity, 77, HostId, FirstId, SecondId);
        Thread.Sleep(500);
        check(first.Held(Wallet) == amount && host.Held(Wallet) == amount && second.Held(Wallet) == amount &&
            second.Held(MissionService.MedalsMixId) == medals.Item2 + medalAmount, "A pile picked up again paid a backend twice.");

        // A player left out of the participants is not paid.
        host.PickUp("m1", MissionService.ForgeCreditsActivity, 5, HostId, FirstId);
        check(Eventually(() => first.Held(Wallet) == amount + MissionService.Rewards.ForgeCredits(5)), "A listed joiner was not paid.");
        Thread.Sleep(300);
        check(second.Held(Wallet) == amount, "A joiner not among the participants was paid.");

        // Outside the lobby: a stranger's frame, and a member's frame that does not name it, pay nobody.
        var frame = new Frame(PickupShare.FrameType).Add("m2").Add(9).Add(MissionService.ForgeCreditsActivity).Add(2)
            .Add(StrangerId.ToString(System.Globalization.CultureInfo.InvariantCulture)).Add(HostId.ToString(System.Globalization.CultureInfo.InvariantCulture)).Encode();
        stranger.Link.Send(HostId, frame, reliable: true);
        var unnamed = new Frame(PickupShare.FrameType).Add("m2").Add(10).Add(MissionService.ForgeCreditsActivity).Add(1)
            .Add(HostId.ToString(System.Globalization.CultureInfo.InvariantCulture)).Encode();
        first.Link.Send(HostId, unnamed, reliable: true);
        check(Eventually(() => log.Count(l => l.Contains("not in a lobby with this one", StringComparison.Ordinal)) >= 2) && host.Held(Wallet) == amount + MissionService.Rewards.ForgeCredits(5),
            "A pickup from outside the lobby, or not naming its sender, paid the host.");
        first.Link.Send(HostId, new Frame(PickupShare.FrameType).Add("m2").Add("x").Encode(), reliable: true);
        check(Eventually(() => log.Any(l => l.Contains("malformed", StringComparison.Ordinal))), "A malformed pickup frame was not dropped.");

        // The receipt is saved with the profile: after a restart the same pile adds nothing.
        var profileDir = Path.Combine(root, "pickup-receipts-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            using (var store = ProfileStore.Open(profileDir, "starter"))
                check(store.ApplyPickup("m3", 1, Wallet, 10) && !store.ApplyPickup("m3", 1, Wallet, 10), "A pile's receipt did not hold.");
            using (var reopened = ProfileStore.Open(profileDir))
                check(!reopened.ApplyPickup("m3", 1, Wallet, 10) && reopened.ApplyPickup("m3", 2, Wallet, 10), "A pile's receipt did not survive a restart.");
        }
        finally { Directory.Delete(profileDir, recursive: true); }
    }
}
