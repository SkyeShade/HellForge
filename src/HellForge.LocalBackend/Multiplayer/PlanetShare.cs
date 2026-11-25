using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// The host's planet modifiers for everyone in its lobby (docs/planet-modifiers.md, PlanetOverrides): every machine builds
// its missions' enemy tags and loads their packages from its own backend's war, so a joiner plays with the host's.
//   host:   "war.planets" [version, gzip of its planet-modifiers.json ("" none)] to every machine in a lobby hosted here
//           when it joins, and again, numbered, whenever the file changes (checked each second).
//   joiner: taken only from the host of the lobby this machine joined (any host, as war.clock), newer versions only; the
//           backend serves the host's until the host is gone (left, closed or silent), then this machine's own again.
// A host from before planet modifiers sends none: its joiners keep their own. The game sees a change on its next poll of
// the war's Status (about a minute).
public sealed class PlanetShare : IAsyncDisposable
{
    // What a host's file may unpack to.
    const int MaxBytes = PlanetModifiers.MaxFileBytes;
    readonly IPeerTransport transport;
    readonly LobbyService lobby;
    readonly PlanetOverrides planets;
    readonly Func<ulong, string> nameOf;
    readonly Action<string> log;
    readonly object gate = new();
    readonly Timer timer;
    // host: the version each machine was sent
    readonly Dictionary<ulong, long> sentTo = [];
    // joiner: the host this machine follows and its last version
    ulong hostPeer;
    long hostVersion = -1;

    public PlanetShare(IPeerTransport transport, LobbyService lobby, PlanetOverrides planets, Func<ulong, string> nameOf, Action<string> log)
    {
        (this.transport, this.lobby, this.planets, this.nameOf, this.log) = (transport, lobby, planets, nameOf, log);
        timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    void Tick()
    {
        try
        {
            SendToJoiners();
            bool gone;
            lock (gate)
            {
                gone = hostPeer != 0 && !lobby.HostsJoinedLobby(hostPeer);
                if (gone) (hostPeer, hostVersion) = (0, -1);
            }
            if (gone) planets.ClearHost();
        }
        catch (Exception error) { log("Planet modifiers: sharing failed: " + error.Message); }
    }

    // This machine's file to every machine in its lobbies that has not had this version.
    void SendToJoiners()
    {
        var joiners = lobby.HostedFriends();
        var (json, version) = planets.Own;
        List<ulong> send;
        lock (gate)
        {
            foreach (var gone in sentTo.Keys.Where(k => !joiners.Contains(k)).ToList()) sentTo.Remove(gone);
            send = [.. joiners.Where(j => !sentTo.TryGetValue(j, out var had) || had != version)];
            foreach (var j in send) sentTo[j] = version;
        }
        if (send.Count == 0) return;
        var packed = StatsSync.Pack(json);
        foreach (var joiner in send)
        {
            transport.Send(joiner, new Frame("war.planets").Add(version).Add(packed).Encode(), reliable: true);
            log($"Planet modifiers: sent version {version} ({packed.Length} bytes) to {LobbyService.EntityId(joiner)}.");
        }
    }

    public void FromPeer(ulong peer, Frame frame)
    {
        if (frame.Type != "war.planets" || frame.Count < 2 || !lobby.HostsJoinedLobby(peer)) return;
        if (!long.TryParse(frame.Text(0), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var version)) return;
        string json;
        try { json = StatsSync.Unpack(frame.Bytes(1), MaxBytes); }
        catch (Exception error) when (error is InvalidDataException or IOException) { log("Planet modifiers: the host's could not be read: " + error.Message); return; }
        lock (gate)
        {
            if (peer == hostPeer && version <= hostVersion) return;
            (hostPeer, hostVersion) = (peer, version);
        }
        planets.SetHost(nameOf(peer), json);
    }

    public async ValueTask DisposeAsync() => await timer.DisposeAsync();
}
