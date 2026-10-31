using System.Collections.Concurrent;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// HellForge 0.3 (protocol hellforge-mp-8) and this one meet: a 0.3 joiner, played here by raw frames, is admitted by its
// mods one by one and offered the set it reads; a 0.3 host is joined with its protocol and, once this session plays its
// mods, its digest. Every mod is synthetic.
static partial class ModSyncTests
{
    const ulong OlderId = 0x110000100000031;

    public static async Task Legacy(string root, Action<bool, string> check)
    {
        root = Path.Combine(root, "mod-sync-legacy");
        var hostMods = Path.Combine(root, "host", "mods");
        Write(Path.Combine(hostMods, "ammo"), ModCatalog.ManifestName, Ammo);
        Write(Path.Combine(hostMods, "stats"), ModCatalog.ManifestName, """{"format":1,"id":"test.stats","name":"Stats","version":"1"}""");
        Write(Path.Combine(hostMods, "stats"), StatsData.FileName, Stats);
        var hostSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(hostMods).Mods));
        var (ammo, stats) = (hostSet.Mods.Single(m => m.Id == "test.ammo"), hostSet.Mods.Single(m => m.Id == "test.stats"));
        check(hostSet.LegacyDigest != null && stats.Tier == CoopTier.PerPlayer, "The host's set has no legacy digest, or its stats mod is shared.");
        var network = new InMemoryNetwork();
        var host = new Node(network, HostId, Path.Combine(root, "host"), hostSet, (_, _) => false);

        // A 0.3 joiner: its digest counts its own bundled mods' files and its stats mod, so only its mods one by one say it
        // plays the host's; one that lacks the host's mod is refused and offered the set 0.3 reads.
        var older = network.Join(OlderId);
        var frames = new BlockingCollection<Frame>();
        older.Received += (_, bytes) => frames.Add(Frame.Decode(bytes));
        host.Lobby.FromLocal(MultiplayerTests.Create("l-create", HostId));
        var connection = host.Next("lobby.result", check).Text(4);
        Frame Join(string op, string digest, IReadOnlyList<ModFingerprint> mods, out List<Frame> seen)
        {
            older.Send(HostId, MultiplayerTests.Join(op, OlderId, connection).Add(digest).Add(LobbyService.LegacyProtocol).Add(ModlistDiff.Encode(mods)).Encode(), reliable: true);
            seen = [];
            while (frames.TryTake(out var frame, TimeSpan.FromSeconds(5)))
            {
                if (frame.Type == "lobby.result" && frame.Text(0) == op) return frame;
                seen.Add(frame);
            }
            check(false, "The host did not answer the " + op + " join.");
            return new Frame("none").Add("").Add("").Add("");
        }
        ModFingerprint Print(ModSetEntry mod, string? hash = null) => new(mod.Id, mod.Name, mod.Version, hash ?? mod.Hash[..16]);
        var oldDigest = new string('0', 32);
        check(Join("l1", oldDigest, [Print(ammo), Print(stats, new string('5', 16))], out _).Text(2) != "0",
            "A 0.3 joiner with another stats mod (shared, to 0.3) was admitted.");
        check(Join("l2", oldDigest, [Print(ammo), Print(stats)], out _).Text(2) == "0", "A 0.3 joiner playing the host's mods was refused.");
        var refused = Join("l3", oldDigest, [Print(stats)], out var seen);
        var offer = seen.FirstOrDefault(f => f.Type == "mods.offer");
        var offered = offer != null ? ModSet.Parse(offer.Text(1), local: false) : null;
        check(refused.Text(2) == LobbyService.LobbyNotJoinable.ToString() && offered != null && offered.Digest == hostSet.LegacyDigest &&
            offered.Mods.Single().Id == "test.ammo", "A 0.3 joiner lacking the host's mod was not offered the set it reads: " + string.Join(" | ", seen.Select(f => f.Type)));
        check(Join("l4", hostSet.LegacyDigest!, [], out _).Text(2) == "0", "A 0.3 joiner back with the offered set was refused.");

        // A 0.3 host: joined with its protocol, with this session's digest until the plan has all of its mods, then with
        // the host's own.
        var guestMods = Path.Combine(root, "guest", "mods");
        Write(Path.Combine(guestMods, "ammo"), ModCatalog.ManifestName, Ammo);
        var guestSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(guestMods).Mods));
        var guest = new Node(network, GuestId, Path.Combine(root, "guest"), guestSet, (_, _) => false);
        check(guest.Sync.JoinAs(OlderId) == null, "A host not known as 0.3's was joined as one.");
        guest.Sync.LegacyHost(OlderId, null);
        check(guest.Sync.JoinAs(OlderId) == (guestSet.Digest, LobbyService.LegacyProtocol), "A 0.3 host of unknown mods is not joined with 0.3's protocol.");
        var theirs = new ModSet("0123456789abcdef0123456789abcdef", [ammo with { Folder = null, Files = [] }]);
        guest.Sync.LegacyHost(OlderId, theirs);
        check(guest.Sync.JoinAs(OlderId) == (theirs.Digest, LobbyService.LegacyProtocol), "A 0.3 host whose mods this session plays is not joined with its digest.");
        var more = new ModSet(theirs.Digest, [.. theirs.Mods, stats with { Folder = null, Files = [], Tier = CoopTier.Shared }]);
        guest.Sync.LegacyHost(OlderId, more);
        check(guest.Sync.JoinAs(OlderId) == (guestSet.Digest, LobbyService.LegacyProtocol), "A 0.3 host with a mod this session lacks was claimed.");
        guest.Sync.LegacyHost(OlderId, theirs);
        var lobby = Guid.NewGuid().ToString("D");
        guest.Lobby.FromLocal(MultiplayerTests.Join("g1", GuestId, LobbyService.ConnectionPrefix + lobby + ";" + LobbyService.EntityId(OlderId)));
        Frame? asked = null;
        while (frames.TryTake(out var frame, TimeSpan.FromSeconds(5))) if (frame.Type == "lobby.join") { asked = frame; break; }
        check(asked != null && asked.Text(asked.Count - 2) == theirs.Digest && asked.Text(asked.Count - 1) == LobbyService.LegacyProtocol,
            "The join to a 0.3 host does not carry its digest and protocol.");
        // After the sync, the rejoin to a 0.3 host checks this session's mods one by one (its digest is not the host's): a
        // 0.3 host's stats mod is per-player here and still counts; a mod missing, or one more shared, is not the host's.
        var file = Path.Combine(root, "legacy-session.json");
        var session = new ModSyncSession(LobbyService.EntityId(OlderId), "Old Host", LobbyService.ConnectionPrefix + lobby + ";" + LobbyService.EntityId(OlderId),
            theirs.Digest, [new(ammo.Id, ammo.Hash, 0), new(stats.Id, stats.Hash, 1)]) { Legacy = true };
        session.Write(file);
        var (target, why) = AutoRejoin.Read(file, GuestId, guestSet.Digest, DateTime.UtcNow, hostSet);
        check(target is { Legacy: true } && target.Digest == theirs.Digest, "The rejoin to a 0.3 host was not made: " + why);
        check(AutoRejoin.Read(file, GuestId, guestSet.Digest, DateTime.UtcNow, guestSet).Target == null, "A session without the host's stats mod rejoins a 0.3 host.");
        (session with { Mods = [new(ammo.Id, ammo.Hash, 0)] }).Write(file);
        check(AutoRejoin.Read(file, GuestId, guestSet.Digest, DateTime.UtcNow, guestSet).Target != null &&
            AutoRejoin.Read(file, GuestId, guestSet.Digest, DateTime.UtcNow, new ModSet("x", [.. guestSet.Mods, stats with { Tier = CoopTier.Shared }])).Target == null,
            "A session with one more shared mod rejoins a 0.3 host.");
        guest.Sync.Rejoining(OlderId, theirs.Digest);
        check(guest.Sync.JoinAs(OlderId) == (theirs.Digest, LobbyService.LegacyProtocol), "The session after a sync does not join the 0.3 host with its digest.");
        var fresh = new Node(network, 0x110000100000032, Path.Combine(root, "fresh"), guestSet, (_, _) => false);
        fresh.Sync.Rejoining(OlderId, theirs.Digest);
        check(fresh.Sync.JoinAs(OlderId) == (theirs.Digest, LobbyService.LegacyProtocol) && fresh.Sync.IsLegacy(OlderId),
            "A session after a sync, knowing nothing else of the host, does not join it as 0.3's.");
        await fresh.Sync.DisposeAsync();
        guest.Sync.NotLegacy(OlderId);
        check(guest.Sync.JoinAs(OlderId) == null, "A host no longer 0.3's is still joined as one.");
        await host.Sync.DisposeAsync();
        await guest.Sync.DisposeAsync();
    }
}
