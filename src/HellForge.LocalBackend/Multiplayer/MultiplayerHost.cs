using System.Globalization;
using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// Wires the multiplayer services of one backend: the named pipe for the game's stand-in DLLs, the peer link to
// friends' backends, and the lobby and Party services between them. Friends sharing a lobby or network are pinged every
// 2 s; one silent for 15 s (game crashed, connection lost) is treated as gone. Logs to <evidence>/multiplayer.log.
//   link "none"                                  this machine only (hosting works, joins fail)
//   link "steam:<steam_api64.dll>"               Steam networking: friends, and strangers through the lobby directory
//   link "udp:<id>:<port>:<peer id>=<port>,..."  loopback UDP between backends on one PC (test harness; ids in hex)
public sealed class MultiplayerHost : IAsyncDisposable
{
    readonly PipeHost pipes;
    readonly IPeerTransport? transport;
    readonly StreamWriter logFile;
    readonly object logGate = new();
    readonly Dictionary<ulong, DateTime> lastHeard = [];
    Timer? heartbeat;
    static readonly TimeSpan PeerTimeout = TimeSpan.FromSeconds(15);
    // The war's time a host may set (war.clock): war 801 began in February 2024, so 20 years is far beyond any real one.
    public const long MaxWarSeconds = 20L * 365 * 24 * 3600;

    public LobbyService Lobby { get; }
    public PartyService Party { get; }
    // The host's stats for joiners, and this game's when joined (null without a peer link or the session's mods folder).
    public StatsShare? Stats { get; private set; }
    public AssetShare? Assets { get; private set; }
    // Mod sync: joiners download this session's mods, and this machine the mods of a host it could not join (null
    // without a peer link or the session's mods folder).
    public ModSync? Sync { get; private set; }
    // The Galactic Map's public lobbies (null without a peer link): this machine's public lobby listed, the game's searches
    // answered.
    public DirectoryService? Directory { get; private set; }
    // After a mod sync restart: this game is invited back into the host's lobby (null in any other session).
    public AutoRejoin? Rejoin { get; private set; }
    StrangerGate? gate;
    string? modsFolder;

    readonly WarClock? warClock;
    AccountKeyExchange? keyExchange;
    readonly FriendPresence? presence;
    long presenceSentVersion = -1;
    DateTime presenceSent = DateTime.MinValue;
    readonly object presenceGate = new();
    readonly HashSet<ulong> presenceSentTo = [];
    static readonly TimeSpan PresenceInterval = TimeSpan.FromSeconds(10);
    // This machine's co-op peers (lobby and Party), for a sandboxed game's Steam broker: the game's Steam P2P passes only to
    // and from them (docs/steam-bridge.md "Steam P2P"). Decimal SteamIDs, one a line, rewritten when they change: at once
    // when a lobby here changes or a join starts (a host sends to a joiner the moment it joins), and on each heartbeat.
    public const string PeersFileName = "p2p-peers.txt";
    readonly string peersFile;
    readonly object peersGate = new();
    string? peersWritten;
    long peersTicket, peersWrittenTicket; /* WritePeers' order: taken before its list is read; the last written, under peersGate */
    readonly Dictionary<ulong, DateTime> pinned = []; /* ListPeer's, under peersGate */
    static readonly TimeSpan PinnedFor = TimeSpan.FromSeconds(30);

    MultiplayerHost(MultiplayerSettings settings)
    {
        var (accountKeys, warClock) = (settings.AccountKeys, settings.WarClock);
        presence = settings.Presence;
        this.warClock = warClock;
        logFile = new StreamWriter(Path.Combine(settings.EvidenceDirectory, "multiplayer.log"), append: true) { AutoFlush = true };
        peersFile = Path.Combine(settings.EvidenceDirectory, PeersFileName);
        transport = CreateTransport(settings.Link, Log);
        pipes = new PipeHost(settings.PipeName, Log);
        Lobby = new LobbyService(transport, pipes.SendLobby, Log, settings.ModDigest, warClock, settings.ModList);
        // off the lobby service's lock; a peer admitted or joined is listed before its game hears of it (ListPeer)
        Lobby.HostedChanged += () => Task.Run(WritePeers);
        Lobby.PeerAdmitted += ListPeer;
        Lobby.LocalJoinStarted += (owner, _) => { ListPeer(owner); Task.Run(WritePeers); };
        // A refused join, in both games (the in-game editor shows it over the game): the joiner's says why, the host's who,
        // briefly, in a corner.
        Lobby.Refused += (peer, lines) => Stats?.Notice("error", "Couldn't join " + FriendName(peer), lines);
        Lobby.RefusedFriend += (peer, lines) => Stats?.Notice("brief", FriendName(peer) + " couldn't join", lines);
        Party = new PartyService(transport, pipes.SendParty, Log, Lobby.InHostedLobby) { Kick = Lobby.Kick };
        // A kick (the game's PFLobbyForceRemoveMember, or its Party kick): the kicked machine's game traffic stops here, and
        // its player is told in game.
        Lobby.Kicked += Party.Expel;
        Lobby.Kicked += peer => { lock (peersGate) pinned.Remove(peer); Task.Run(WritePeers); }; /* out of the P2P peers at once */
        Lobby.KickedByHost += peer => Stats?.Notice("error", "Removed from " + FriendName(peer) + "'s lobby",
            ["The host removed you from their lobby.", "You can't join that lobby again while it lasts."]);
        pipes.Lobby = Lobby;
        pipes.Party = Party;
        if (transport != null && accountKeys != null) accountKeys.Log = Log;
        if (transport != null && presence != null) presence.Log = Log;
        if (transport != null)
            transport.Received += (peer, bytes) =>
            {
                Frame frame;
                try { frame = Frame.Decode(bytes); }
                catch (InvalidDataException) { Log("Dropped a malformed peer frame."); return; }
                if (gate != null && !gate.Admit(peer, frame)) return;
                lock (lastHeard) lastHeard[peer] = DateTime.UtcNow;
                keyExchange?.Heard(peer);
                if (frame.Type.StartsWith("lobby.", StringComparison.Ordinal)) Lobby.FromPeer(peer, frame);
                else if (frame.Type == "account.key") keyExchange?.FromPeer(peer, frame);
                else if (frame.Type is "presence" or "presence.hello") FromPeerPresence(peer, frame);
                else if (frame.Type.StartsWith("party.", StringComparison.Ordinal)) Party.FromPeer(peer, frame);
                else if (frame.Type.StartsWith("stats.", StringComparison.Ordinal)) Stats?.FromPeer(peer, frame);
                else if (frame.Type.StartsWith("assets.", StringComparison.Ordinal)) Assets?.FromPeer(peer, frame);
                else if (frame.Type.StartsWith("mods.", StringComparison.Ordinal)) Sync?.FromPeer(peer, frame);
                else if (frame.Type == "war.clock" && warClock != null && Lobby.HostsJoinedLobby(peer) && long.TryParse(frame.Text(0), out var seconds))
                {
                    // Any host, a stranger's too, only within a war's possible time: from its start to MaxWarSeconds.
                    if (seconds is < 0 or > MaxWarSeconds) { Log($"Ignored the host's war time ({seconds} s): out of range."); return; }
                    warClock.Set(seconds);
                    Log($"War time set to the host's ({seconds} s).");
                }
            };
    }

    public static MultiplayerHost Start(MultiplayerSettings settings)
    {
        var (pipeName, link, modDigest, evidenceDirectory) = settings;
        var (accountKeys, modsFolder, directory, rejoinFile) = (settings.AccountKeys, settings.ModsFolder, settings.Directory, settings.RejoinFile);
        var host = new MultiplayerHost(settings);
        host.transport?.Start();
        // The session's live-edit pipe is "hellforge-live-" + the run id, the pipe name's own suffix ("hellforge-<run id>").
        if (host.transport != null && modsFolder != null && pipeName.StartsWith("hellforge-", StringComparison.Ordinal))
        {
            try
            {
                host.Stats = new StatsShare(host.transport, host.Lobby, pipeName["hellforge-".Length..], modsFolder, host.FriendName, host.Log,
                    new StatsShareOptions(() => ModSettings.Read(modsFolder).HostStats, settings.SandboxStats));
                var stats = host.Stats;
                host.Assets = new AssetShare(host.transport, host.Lobby, stats.Live, host.Log, () => stats.LocalStats, () => stats.LobbyPlaysHostStats);
                host.Log("Stats: every player keeps their own unless the host shares its (the Mods page); the lobby's packages load on every machine; this game's live-edit pipe is served here.");
            }
            catch (Exception error) when (error is ArgumentException or InvalidDataException or IOException) { host.Log("Stats sharing is off: " + error.Message); }
        }
        if (host.transport != null && modsFolder != null) host.StartSync(modsFolder, settings.ModSetFile, evidenceDirectory);
        host.modsFolder = modsFolder;
        directory ??= host.transport != null && settings.DirectoryFolder != null ? new FolderLobbyDirectory(settings.DirectoryFolder, host.transport.LocalId) : null;
        host.StartDirectory(directory ?? (host.transport as SteamPeerTransport)?.Directory, modsFolder);
        // The transport knows this machine's SteamID only once started.
        if (host.transport != null && accountKeys != null) host.keyExchange = new AccountKeyExchange(host.transport, accountKeys, host.Log);
        // before the pipes: the game's first lobby must not be missed
        if (host.transport != null && rejoinFile != null) host.StartRejoin(rejoinFile, modDigest);
        host.pipes.Start();
        if (host.transport != null) host.heartbeat = new Timer(_ => host.Beat(), null, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(2));
        host.Log($"Multiplayer ready: pipe {pipeName}, link {(host.transport == null ? "none" : link.Split(':')[0])}" +
            (host.transport != null ? $", local peer {LobbyService.EntityId(host.transport.LocalId)}" : "") + ".");
        return host;
    }

    // The session folder is the evidence folder's parent; the next session's mods go to the installation's temp folder.
    void StartSync(string modsFolder, string? modSetFile, string evidenceDirectory)
    {
        ModSet? own = null;
        try { if (modSetFile != null) own = ModSet.Parse(File.ReadAllText(modSetFile), local: true); }
        catch (Exception error) when (error is InvalidDataException or IOException) { Log("Mod sync: this session's mod set could not be read, so joiners cannot download it: " + error.Message); }
        var install = new InstallLayout(Path.GetDirectoryName(Path.GetFullPath(modsFolder))!);
        Sync = new ModSync(transport!, new ModSyncPaths(modsFolder, Path.GetDirectoryName(Path.GetFullPath(evidenceDirectory))!, evidenceDirectory,
            ModSyncSession.PathIn(install)), own, FriendName, Log);
        Lobby.ModsRefused = Sync.OfferMods;
        Sync.Expected = Lobby.JoinedRecently;
        if (Stats?.Live is { } live)
        {
            Sync.Display = new LiveSyncDisplay(live);
            live.Received += parts => { if (parts[0] == "syncanswer") Sync.Answer(parts); };
            live.StateChanged += () => { if (live.Connected) Sync.Republish(); };
        }
        Log($"Mod sync: {(own != null ? own.Mods.Count + " gameplay mods to share" : "nothing to share")}; offers go to {(Sync.Display != null ? "the game" : "the session log only")}.");
    }

    // After a mod sync restart (--rejoin): the game is invited back into the host's lobby once it is on the ship (AutoRejoin).
    void StartRejoin(string file, string modDigest)
    {
        var (target, reason) = AutoRejoin.Read(file, transport!.LocalId, modDigest, DateTime.UtcNow);
        if (target == null) { Log("Rejoin: not rejoining, because " + reason); return; }
        var rejoin = Rejoin = new AutoRejoin(target, modDigest, (peer, frame) => transport.Send(peer, frame.Encode(), reliable: true), Lobby.InviteLocal,
            (kind, title, lines) =>
            {
                if (Stats != null) Stats.Notice(kind, title, lines);
                else Log($"Notice not shown, no live-edit pipe: {title}");
            }, Log);
        rejoin.Attach(Lobby);
        rejoin.Start();
    }

    // The lobby directory and the gate for strangers, who reach this machine through it only.
    // modsFolder: where the Mods page keeps the switch that lists this player's public lobbies (ModSettings.ListPublic; on
    // without one).
    void StartDirectory(ILobbyDirectory? directory, string? modsFolder)
    {
        if (transport == null) return;
        var setting = new StrangerSetting(modsFolder);
        bool StrangersAllowed() => setting.Allowed;
        if (directory != null && presence != null)
            Directory = new DirectoryService(directory, Lobby, presence, transport.LocalId, pipes.SendLobby, Log, new DirectorySettings(Sync,
                () => Stats?.SharesEdited == true, ListPublic: modsFolder != null ? () => ModSettings.Read(modsFolder).ListPublic : null, StrangersAllowed: StrangersAllowed));
        gate = new StrangerGate(transport, () => Directory?.Listing == true, Related, StrangersAllowed, Log, Lobby.Welcome);
        if (transport is SteamPeerTransport steam)
            steam.AcceptStranger = peer => StrangersAllowed() && (Directory?.Listing == true || Related(peer) || Lobby.Welcome(peer));
        var link = transport;
        Lobby.JoinRefusal = peer => link.IsFriend(peer) || StrangersAllowed() ? null :
            ["This player is not a Steam friend, and Play with strangers is off.", "Turn it on in the launcher's Multiplayer settings to join public lobbies."];
        Log((Directory != null ? "Directory: public lobbies are listed and searched" : "Directory: off (no lobby directory for this link)") +
            (StrangersAllowed() ? "; Play with strangers is on." : "; Play with strangers is off: friends only."));
    }

    // A machine this one deals with: in a lobby hosted here, hosting one joined (or being joined) from here, a mod download,
    // or the host this game is rejoining after a mod sync.
    bool Related(ulong peer) => Lobby.InHostedLobby(peer) || Lobby.HostsJoinedLobby(peer) || Lobby.Joining(peer) || Sync?.Related(peer) == true ||
        Rejoin?.Related(peer) == true;

    // A friend's name as Steam shows it, for the joiner's editor ("Host: <name>").
    // A stranger's is the host's player name in the lobby record their listing carried.
    string FriendName(ulong peer) =>
        (transport as IFriendSource)?.HellForgeFriends.FirstOrDefault(f => f.SteamId == peer)?.Name ?? Directory?.HostName(peer) ??
        (transport?.IsFriend(peer) == false ? "Player " : "Steam friend ") + LobbyService.EntityId(peer);

    // HELLFORGE_TEST_STRANGERS=1: friends are treated as strangers (testing the lobby directory's stranger path with two
    // friends' PCs; docs/multiplayer.md step 7).
    static IPeerTransport? CreateTransport(string link, Action<string> log)
    {
        if (link == "none") return null;
        var strangers = Environment.GetEnvironmentVariable("HELLFORGE_TEST_STRANGERS") == "1";
        if (strangers) log("Test mode (HELLFORGE_TEST_STRANGERS): Steam friends are treated as strangers: no presence, the directory's gate.");
        if (link.StartsWith("steam:", StringComparison.Ordinal)) return new SteamPeerTransport(link[6..], log) { TreatFriendsAsStrangers = strangers };
        var parts = link.Split(':');
        if (parts is ["udp", var id, var port, var peers] && LobbyService.TryNode(id, out var localId) && int.TryParse(port, out var localPort))
        {
            var table = new Dictionary<ulong, int>();
            foreach (var entry in peers.Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                var pair = entry.Split('=');
                if (pair.Length != 2 || !LobbyService.TryNode(pair[0], out var peer) || !int.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out var peerPort))
                    throw new ArgumentException("Bad udp peer: " + entry);
                table[peer] = peerPort;
            }
            return new UdpLoopbackTransport(localId, localPort, table) { TreatPeersAsStrangers = strangers };
        }
        throw new ArgumentException("--link must be none, steam:<steam_api64.dll> or udp:<id>:<port>:<peer>=<port>,...");
    }

    // The co-op peers file (PeersFileName), written whole when the set changed: the lobby and Party peers, and the peers
    // listed ahead of them (ListPeer) until those list them too or PinnedFor has passed, so a list read just before an
    // admission cannot take the new peer out again. Each change starts one (Task.Run) and they can reach peersGate in any
    // order: one whose list was read before a later one's is not written after it, so a list from before a kick cannot put
    // the kicked peer back until the next heartbeat. (A list read after a ticket holds every change made before it.)
    void WritePeers()
    {
        try
        {
            var ticket = Interlocked.Increment(ref peersTicket);
            var listed = Lobby.Peers().Concat(Party.Peers()).ToHashSet();
            lock (peersGate)
            {
                if (ticket < peersWrittenTicket) return;
                peersWrittenTicket = ticket;
                foreach (var (peer, since) in pinned.ToList())
                    if (listed.Contains(peer) || DateTime.UtcNow - since > PinnedFor) pinned.Remove(peer);
                WriteListed(listed.Concat(pinned.Keys));
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Log("Could not write the co-op peers for Steam P2P: " + error.Message); }
    }

    // A peer into the file at once, before this machine's game hears of it: a friend admitted to a lobby hosted here
    // (LobbyService.PeerAdmitted, under that service's lock, so only peersGate is taken here), or the host of a lobby this
    // game joins. The host's game sends to a joiner the moment it hears of the join, and the broker passes only listed peers.
    void ListPeer(ulong peer)
    {
        try
        {
            lock (peersGate)
            {
                pinned[peer] = DateTime.UtcNow;
                var written = (peersWritten ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries)
                    .Select(line => ulong.Parse(line, NumberStyles.None, CultureInfo.InvariantCulture));
                WriteListed(written.Append(peer));
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Log("Could not write the co-op peers for Steam P2P: " + error.Message); }
    }

    // Under peersGate.
    void WriteListed(IEnumerable<ulong> peers)
    {
        var text = string.Concat(peers.Distinct().Order().Select(p => p.ToString(CultureInfo.InvariantCulture) + "\n"));
        if (text == peersWritten) return;
        File.WriteAllText(peersFile + ".partial", text);
        File.Move(peersFile + ".partial", peersFile, overwrite: true);
        peersWritten = text;
    }

    // Pings every friend this machine shares a lobby or network with; a friend unheard for PeerTimeout is gone.
    void Beat()
    {
        WritePeers();
        try
        {
            var now = DateTime.UtcNow;
            var peers = Lobby.Peers().Concat(Party.Peers()).ToHashSet();
            List<ulong> lost = [];
            lock (lastHeard)
            {
                foreach (var peer in peers)
                {
                    if (!lastHeard.TryGetValue(peer, out var heard)) lastHeard[peer] = heard = now;
                    if (now - heard > PeerTimeout) lost.Add(peer);
                }
                foreach (var peer in lastHeard.Keys.Where(p => !peers.Contains(p)).ToList()) { lastHeard.Remove(peer); keyExchange?.Forget(peer); }
                foreach (var peer in lost) { lastHeard.Remove(peer); keyExchange?.Forget(peer); }
            }
            foreach (var peer in peers.Except(lost)) transport!.Send(peer, new Frame("peer.ping").Encode(), reliable: false);
            SharePresence(now);
            foreach (var peer in lost)
            {
                Log($"Peer {LobbyService.EntityId(peer)} stopped answering; treating it as gone.");
                Lobby.PeerLost(peer);
                Party.PeerLost(peer);
            }
        }
        catch (Exception error) { Log("Heartbeat failed: " + error.Message); }
    }

    // The social tab: friends running HellForge (from the transport) get this player's profile objects and the game's current
    // lobby every 10 s and at once when either changes: "presence" [objects JSON, lobbyId, connectionString, lobby JSON as
    // the game uploaded it, listing digest], without the lobby fields when in none, and without the digest when the
    // directory lists nothing (FriendPresence.SetLocalListing; older backends send none and ignore it). The first one to a
    // friend is "presence.hello", which is always answered, so two backends know each other at once, also after one
    // restarts.
    void SharePresence(DateTime now)
    {
        if (presence == null || transport is not IFriendSource source) return;
        var friends = source.HellForgeFriends;
        presence.SetFriends(friends);
        List<ulong> hello, periodic = [];
        lock (presenceGate)
        {
            // Friends just found get it at once; everyone again when it changes or every PresenceInterval.
            presenceSentTo.IntersectWith(friends.Select(f => f.SteamId));
            hello = [.. friends.Select(f => f.SteamId).Where(id => !presenceSentTo.Contains(id))];
            presenceSentTo.UnionWith(hello);
            var version = presence.Local.Version;
            if (version != presenceSentVersion || now - presenceSent >= PresenceInterval)
            {
                presenceSentVersion = version;
                presenceSent = now;
                periodic = [.. presenceSentTo.Except(hello)];
            }
        }
        SendPresence(hello, "presence.hello");
        SendPresence(periodic, "presence");
    }

    void SendPresence(IReadOnlyCollection<ulong> targets, string type)
    {
        if (presence == null || transport == null || targets.Count == 0) return;
        var (lobbyId, body, objects, _) = presence.Local;
        var connection = lobbyId != null ? Lobby.ConnectionStringFor(lobbyId) : null;
        var frame = new Frame(type).Add(objects.ToJsonString());
        if (lobbyId != null && connection != null && body != null)
        {
            frame.Add(lobbyId).Add(connection).Add(body.ToJsonString());
            if (presence.LocalListing is { } listing) frame.Add(listing);
        }
        var bytes = frame.Encode();
        foreach (var target in targets) transport.Send(target, bytes, reliable: true);
    }

    // A presence frame's listing digest (DirectoryData.Digest: 64 lowercase hex digits), or null when it has none.
    public static string? ListingOf(Frame frame) =>
        frame.Count > 4 && frame.Text(4) is { Length: 64 } digest && digest.All(c => c is (>= '0' and <= '9') or (>= 'a' and <= 'f')) ? digest : null;

    void FromPeerPresence(ulong peer, Frame frame)
    {
        if (presence == null || frame.Count < 1) return;
        // A friend saying hello, or one this backend has not sent to yet, gets this player's presence in reply.
        bool reply;
        lock (presenceGate) reply = presenceSentTo.Add(peer) || frame.Type == "presence.hello";
        if (reply) SendPresence([peer], "presence");
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(frame.Text(0)) is System.Text.Json.Nodes.JsonObject sent && RecordGuard.Objects(sent) is { } objects)
                presence.SetFriendObjects(peer, objects);
        }
        catch (System.Text.Json.JsonException) { Log("Dropped malformed profile objects from a friend."); return; }
        if (frame.Count < 4) { presence.ClearFriendLobby(peer); return; }
        var lobbyId = frame.Text(1);
        var connection = frame.Text(2);
        // Only a lobby on the sender's own machine or one it joined through HellForge can be offered.
        if (!connection.StartsWith(LobbyService.ConnectionPrefix, StringComparison.Ordinal) || connection.Split(';') is not [_, var id, _] || id != lobbyId)
        {
            Log("Dropped a malformed lobby from a friend.");
            return;
        }
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(frame.Text(3)) is System.Text.Json.Nodes.JsonObject sent && RecordGuard.Lobby(sent) is { } body)
                presence.SetFriendLobby(peer, lobbyId, connection, body, ListingOf(frame));
            else Log("Dropped a friend's lobby record that is not one.");
        }
        catch (System.Text.Json.JsonException) { Log("Dropped a malformed lobby from a friend."); }
    }

    void Log(string message)
    {
        lock (logGate) logFile.WriteLine($"{DateTime.UtcNow.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture)} {HellForge.Core.Privacy.Scrub(message)}");
    }

    public async ValueTask DisposeAsync()
    {
        if (heartbeat != null) await heartbeat.DisposeAsync();
        Rejoin?.Dispose();
        if (Directory != null) await Directory.DisposeAsync();
        if (Sync != null) await Sync.DisposeAsync();
        if (Assets != null) await Assets.DisposeAsync();
        if (Stats != null) await Stats.DisposeAsync();
        await pipes.DisposeAsync();
        transport?.Dispose();
        Log("Multiplayer stopped.");
        logFile.Dispose();
    }
}

// What MultiplayerHost.Start sets up. ModsFolder: the session's mods folder (stats and mod sharing, mod sync).
// ModSetFile: the session's mod set (mods-stage --modset), which joiners lacking its mods may download. Directory: the
// lobby directory (by default Steam's for a Steam link; DirectoryFolder a folder's, for the loopback harness; none otherwise).
// RejoinFile: the session after a mod sync (ModSyncSession), whose host's lobby the game is invited back into.
public sealed record MultiplayerSettings(string PipeName, string Link, string ModDigest, string EvidenceDirectory)
{
    public WarClock? WarClock { get; init; }
    public AccountKeys? AccountKeys { get; init; }
    public FriendPresence? Presence { get; init; }
    public string? ModsFolder { get; init; }
    public string? ModList { get; init; }
    public string? ModSetFile { get; init; }
    public ILobbyDirectory? Directory { get; init; }
    public string? DirectoryFolder { get; init; }
    public string? RejoinFile { get; init; }
    // A sandboxed game's editor saves (the run's game/sandbox-stats), which StatsShare shares over the mods folder's files.
    public string? SandboxStats { get; init; }
}
