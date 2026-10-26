using System.Globalization;
using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// The PlayFab Lobby service for HellForge. A lobby lives in the backend of the player who created it (its owner node); that
// backend is its only authority. The lobby stand-in DLL of every member mirrors it from the events below and turns them
// into the SDK's state changes. Requests from this machine's DLL for a friend's lobby go to the friend's backend over
// the peer link, and its answers and events come back the same way; the frames are the same on both hops.
//
// DLL -> backend requests (field 0 is an operation id the answer repeats):
//   lobby.create [op, creatorId, creatorType, maxMembers, access, migration, lock, lobbyProps, searchProps, memberProps]
//   lobby.join   [op, memberId, memberType, connectionString, memberProps]  (+ modDigest, Protocol, modList when forwarded
//                to the owner; modList, ModlistDiff's form, only from a backend given --mod-list)
//   lobby.update [op, lobbyId, memberId, newOwner?, newOwnerId, maxMembers?, maxMembers, access?, access, lock?, lock,
//                 lobbyChanges, searchChanges, memberUpdate?, memberChanges]
//   lobby.leave  [op, lobbyId, memberId ("" = every member of the requesting machine)]
//   lobby.kick   [op, lobbyId, memberId, preventRejoin 0|1]  (PFLobbyForceRemoveMember; the owner's own DLL only)
//   lobby.invite [op, lobbyId, senderId, inviteeId]          (PFLobbySendInvite; a member's DLL, forwarded to the owner)
// Backend -> DLL events:
//   lobby.result        [op, kind, hresult, snapshot (create and join successes)]
//   lobby.memberAdded   [lobbyId, memberId, memberType, props]
//   lobby.memberRemoved [lobbyId, memberId, reason]  (the SDK's PFLobbyMemberRemovedReason: 1 to a kicked machine)
//   lobby.updated       [lobbyId, ownerId ("" = unchanged), maxMembers ("" = unchanged), access, lock, lobbyChanges,
//                        searchChanges, memberUpdateCount, (memberId, changes)...]
//   lobby.disconnected  [lobbyId, reason]
//   lobby.inviteReceived [lobbyId, connectionString, inviterId]  (a friend's invitation; the DLL tells its listeners)
// Between backends only: lobby.refused (below), lobby.invited [lobbyId, connectionString, inviterId] from the inviting
// player's backend to the invitee's, taken from Steam friends only.
// snapshot = lobbyId, connectionString, ownerId, maxMembers, access, migration, lock, lobbyProps, searchProps,
//            memberCount, (memberId, memberType, props)...
//
// Who may join a lobby hosted here (Admission, before the protocol and mod checks, so a refused stranger is never offered
// this host's mods) follows the game's own privacy setting as the lobby advertises it: lobby property "19"
// (PrivacyMode), mirrored in search property number_key6, 0 Public, 1 Friends Only, 2 Invite Only, 3 Friends and Clan
// (HD2Runtime research "matchmaking safety", build F5FEE03DCFDB: settings privacy_mode, name table 0x21D3F28, the
// game's own join gate 0x108BC70; an active SOS Beacon advertises 0). Captured in stand-in logs: 0, 1 and 2 at
// creation, "19" and number_key6 always alike, access policy always 0, so the policy says nothing.
//   Public       anyone who reaches this machine (a stranger only through the lobby directory: StrangerGate);
//   Friends Only this machine's Steam friends, and players invited to the lobby; Friends and Clan the same (no clans);
//   Invite Only  players invited to the lobby (PFLobbySendInvite by any member), friends included only when invited.
//   Unknown (no "19" nor number_key6): Friends Only.
// Under every setting a player already in the lobby, or admitted to it before (rejoining after a crash), stays welcome,
// so a privacy change never throws anyone out; a machine kicked from the lobby (lobby.kick) never comes back while it lasts.
// A machine refused only for its gameplay mods (mod sync) is expected back for ExpectedBack, whatever the privacy then.
//
// Rejoining after a mod sync (AutoRejoin, on the joiner's machine): the joiner's next session asks the host, once its
// game is on the ship, whether it may come back, then invites its own game into the lobby the answer names:
//   lobby.rejoin   [lobbyId, modDigest, Protocol]                        joiner -> host
//   lobby.rejoined [lobbyId asked, "ok", lobbyId, connectionString]      host -> joiner, or
//                  [lobbyId asked, "gone"|"full"|"locked"|"refused"|"mods"|"version", line...] (the lines its game shows)
// The host answers for the lobby asked, or, when that one has closed, the one lobby its own game hosts now; an "ok"
// keeps the machine expected there for RejoinGrace, so the join that follows is admitted.
public sealed class LobbyService
{
    public const ulong LocalNode = 0;
    public const string ConnectionPrefix = "hellforge1-lobby;";
    // HRESULTs from the SDK's error table (docs/multiplayer.md).
    public const uint LobbyNotFound = 0x89236226, AccessDenied = 0x80070005, InvalidArgument = 0x80070057;
    // "the requested lobby wasn't in a joinable state" (SDK error table, PFMultiplayerGetErrorMessage 0x38cb0). Joins this
    // host will not take (full, locked, other gameplay mods or HellForge version) fail with it: a code of HellForge's own
    // (0x8923B701) left the joining game retrying, stuck on "connecting". The reason goes to the joiner's backend in
    // "lobby.refused" [reason, line...] and its multiplayer.log; the lines (what the joiner's game shows: which mods differ,
    // say) go to its game (Refused), and the host's game is told too (RefusedFriend). For other gameplay mods the host's
    // mod set goes to the joiner just before, in its own frame (ModSync: "mods.offer"), which older backends ignore.
    public const uint LobbyNotJoinable = 0x89236227;
    public const uint ModlistMismatch = LobbyNotJoinable;
    // The HellForge multiplayer protocol a joiner's backend speaks; a host refuses other versions, since mixed builds fail in
    // confusing ways (second two-PC test: an outdated friend got no missions and could not rejoin). Raise it whenever
    // backends or stand-ins change behaviour the other side relies on.
    public const string Protocol = "hellforge-mp-8"; // 8: every player keeps their own stats unless the host shares its ("stats.set" says which), and the lobby's packages are loaded on every machine (AssetShare); 7: a refusal for different gameplay mods carries the host's mod set, and joiners download it (ModSync); 6: mods set up in id order and added entities numbered by resource id (engine 0.1.4: content numbered alike whatever the load order), the editor's mod content values in stats.set; 5: the host's stats, Local Stats left the mod digest
    const int MemberRemovedForcibly = 1, MemberRemovedRemoteLeft = 2, DisconnectingNoLocalMembers = 0, DisconnectingLobbyDeleted = 1;
    static readonly HashSet<string> Requests = ["lobby.create", "lobby.join", "lobby.update", "lobby.leave", "lobby.kick", "lobby.invite"];
    // PlayFab's limits (PFLobby.h: PFLobbyMaxLobbyPropertyCount, ...MemberPropertyCount, ...SearchPropertyCount, 30 each),
    // and HellForge's own on a key's and a value's length, far above the game's (keys "0".."26", "descriptor", values up
    // to 123 characters in the stand-in logs). Frames from other machines are held to them; the local DLL is not.
    public const int MaxProperties = 30, MaxKeyLength = 64, MaxValueLength = 1024;
    // Invitations a lobby remembers, and those this machine passes on to its game from one friend in a minute.
    const int MaxInvited = 64, InvitesPerMinute = 10;
    // A machine refused only for its gameplay mods is expected back for ExpectedBack (a download of up to ModSync.GrantTime,
    // then the restart); one the host told it may rejoin, for RejoinGrace (its player accepting the invitation). At most
    // MaxExpected per lobby, the oldest dropped.
    public static readonly TimeSpan ExpectedBack = TimeSpan.FromMinutes(15), RejoinGrace = TimeSpan.FromMinutes(3);
    const int MaxExpected = 64;

    public enum Privacy { Public = 0, FriendsOnly = 1, InviteOnly = 2, FriendsAndClan = 3 }
    static readonly HashSet<string> Events = ["lobby.result", "lobby.memberAdded", "lobby.memberRemoved", "lobby.updated", "lobby.disconnected"];

    readonly object gate = new();
    readonly IPeerTransport? transport;
    readonly Action<Frame> toLocal;
    readonly Action<string> log;
    readonly string modDigest;
    // The digest's mods one by one (ModlistDiff's form), or null when the session did not give them.
    readonly string? modList;
    readonly WarClock? warClock;
    readonly Dictionary<string, HostedLobby> hosted = new(StringComparer.Ordinal);
    // Lobbies on a friend's machine that this machine's DLL joined or is joining: lobby id -> owner node.
    readonly Dictionary<string, ulong> remote = new(StringComparer.Ordinal);
    // Forwarded requests awaiting the owner's answer: op -> owner node and lobby.
    readonly Dictionary<string, (ulong Owner, string LobbyId)> forwarded = new(StringComparer.Ordinal);
    // Friends' machines this machine's game last asked to join, and when: a refusal ("lobby.refused") may come before or
    // after the failed join's answer, so it is taken from them for a while either way.
    readonly Dictionary<ulong, DateTime> joining = [];
    static readonly TimeSpan RefusalWindow = TimeSpan.FromSeconds(30);
    // Invitations this machine's game sent for a friend's lobby, passed on once its owner took them: op -> invitee, lobby.
    readonly Dictionary<string, (ulong Invitee, string LobbyId, string Inviter)> invitesSent = new(StringComparer.Ordinal);
    // Invitations friends sent this machine in the last minute, by friend.
    readonly Dictionary<ulong, Queue<DateTime>> invitesHeard = [];
    // Work for other services that must not run under this service's lock (PartyService calls into it under its own).
    readonly List<Action> afterLock = [];

    // Mod sync (ModSync). Host: a joiner with other gameplay mods is refused; ModsRefused (joiner, lobby id) offers them this
    // host's mods first and says whether they may download them (the joiner's game is then told so). Called under this
    // service's lock: it must not call back into it.
    public Func<ulong, string, bool>? ModsRefused { get; set; }
    // Joins this machine asked for in the last minute: lobby id -> owner, kept whatever their answers (a mod offer, a
    // refusal or the failed join's result, in any order).
    readonly Dictionary<string, (ulong Owner, DateTime At)> joins = new(StringComparer.Ordinal);
    static readonly TimeSpan JoinMemory = TimeSpan.FromMinutes(1);

    // The lobby directory (DirectoryService). A lobby hosted here changed (created, joined, updated, left, closed): its
    // listing is looked at again. The game's lobby search from this machine's DLL: lobby.find [op, entity id, entity type,
    // filter, sort, result count], answered with lobby.found. Both are called under this service's lock.
    public event Action? HostedChanged;
    // A friend's machine admitted to a lobby hosted here, raised under this service's lock BEFORE any game hears of the
    // member: a sandboxed host's Steam broker lets the game's P2P reach only listed peers, and the host's game sends to a
    // joiner the moment it hears of it (MultiplayerHost.ListPeer). A handler must not take this service's or the Party
    // service's lock.
    public event Action<ulong>? PeerAdmitted;
    public Action<Frame>? Find { get; set; }
    public sealed record Hosted(string Id, string ConnectionString, uint MaxMembers, int Members, int Access, bool Locked, bool LocalOwner,
        IReadOnlyDictionary<string, string> SearchProperties, Privacy Privacy = Privacy.FriendsOnly);

    public LobbyService(IPeerTransport? transport, Action<Frame> toLocal, Action<string> log, string modDigest, WarClock? warClock = null,
        string? modList = null)
    {
        this.warClock = warClock;
        this.transport = transport;
        this.toLocal = toLocal;
        this.log = log;
        this.modDigest = modDigest;
        this.modList = modList;
    }

    // A friend's lobby refused this machine's join (the host's machine, and lines saying why for this player), and this
    // host refused a friend's (their machine, and lines for this host); the game is told (MultiplayerHost). Raised under
    // the service's lock: a handler must not call back into it.
    public event Action<ulong, IReadOnlyList<string>>? Refused;
    public event Action<ulong, IReadOnlyList<string>>? RefusedFriend;
    // This host kicked a machine from a lobby hosted here (its Party traffic is dropped: PartyService.Expel), and a host
    // kicked this machine from its lobby (the game is told). Raised after the service's lock is released.
    public event Action<ulong>? Kicked;
    public event Action<ulong>? KickedByHost;

    // For rejoining after a mod sync (AutoRejoin), raised after the service's lock is released: this machine's game created
    // a lobby (it is on the ship), asked to join a friend's lobby (owner, lobby id) and heard the answer (owner, lobby id,
    // HRESULT), and a host answered lobby.rejoin (the host, the frame).
    public event Action? LocalLobbyCreated;
    public event Action<ulong, string>? LocalJoinStarted;
    public event Action<ulong, string, uint>? LocalJoinFinished;
    public event Action<ulong, Frame>? RejoinAnswered;

    // The time Expected entries are held to (tests set their own).
    public Func<DateTime> Clock { get; set; } = () => DateTime.UtcNow;

    // The privacy a lobby advertises (see the top of this file): search property number_key6, else lobby property "19";
    // Friends Only when neither holds a known value.
    public static Privacy PrivacyOf(IReadOnlyDictionary<string, string> searchProperties, IReadOnlyDictionary<string, string> lobbyProperties)
    {
        var text = searchProperties.GetValueOrDefault("number_key6") ?? lobbyProperties.GetValueOrDefault("19");
        return int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var value) && value is >= 0 and <= 3 ? (Privacy)value : Privacy.FriendsOnly;
    }

    public static string Describe(Privacy privacy) => privacy switch
    {
        Privacy.Public => "Public", Privacy.InviteOnly => "Invite only", Privacy.FriendsAndClan => "Friends and clan", _ => "Friends only"
    };

    // Runs deferred work once the lock is released.
    void Drain()
    {
        List<Action> work;
        lock (gate)
        {
            if (afterLock.Count == 0) return;
            work = [.. afterLock];
            afterLock.Clear();
        }
        foreach (var action in work) action();
    }

    // Why this machine's game may not join a lobby owned by that machine (lines for the player), or null: a stranger's
    // while Play with strangers is off (MultiplayerHost). The join is answered at once, and Refused says why.
    public Func<ulong, IReadOnlyList<string>?>? JoinRefusal { get; set; }

    ulong LocalId => transport?.LocalId ?? 0;
    public static string EntityId(ulong node) => node.ToString("X", CultureInfo.InvariantCulture);
    public static bool TryNode(string entityId, out ulong node) =>
        ulong.TryParse(entityId, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out node) && node != 0;

    // A frame from this machine's DLL.
    public void FromLocal(Frame frame)
    {
        lock (gate)
        {
            if (frame.Type == "lobby.find" && Find != null) { Find(frame); return; }
            if (!Requests.Contains(frame.Type)) { log("Ignored a lobby frame from the DLL: " + frame.Type); return; }
            try { Route(frame); }
            catch (InvalidDataException error) { log($"Bad {frame.Type} from the DLL: {error.Message}"); }
        }
        Drain();
    }

    // A frame from a friend's backend.
    public void FromPeer(ulong peer, Frame frame)
    {
        lock (gate)
        {
            try
            {
                if (Requests.Contains(frame.Type)) Handle(peer, frame);
                else if (Events.Contains(frame.Type)) RelayEvent(peer, frame);
                else if (frame.Type == "lobby.refused" && joining.Remove(peer, out var asked) && DateTime.UtcNow - asked < RefusalWindow)
                {
                    // the lines for this player after the reason (a host from before them: the reason alone)
                    var lines = Enumerable.Range(1, Math.Max(0, frame.Count - 1)).Take(8).Select(frame.Text).ToList();
                    if (lines.Count == 0) lines.Add("The host refused the join: " + frame.Text(0));
                    log($"{EntityId(peer)} refused this player's join: {frame.Text(0)}" + (frame.Count > 1 ? " | " + string.Join(" | ", lines) : ""));
                    Refused?.Invoke(peer, lines);
                }
                else if (frame.Type == "lobby.invited") Invited(peer, frame);
                else if (frame.Type == "lobby.rejoin") RejoinAsked(peer, frame);
                else if (frame.Type == "lobby.rejoined") afterLock.Add(() => RejoinAnswered?.Invoke(peer, frame));
            }
            catch (InvalidDataException error) { log($"Bad {frame.Type} from a peer: {error.Message}"); }
        }
        Drain();
    }

    // A friend invited this machine's player to a lobby (theirs, or one they are in): the game's invite listeners are told
    // (the lobby stand-in's InviteReceived), at most InvitesPerMinute from one friend. Strangers never invite.
    void Invited(ulong peer, Frame frame)
    {
        if (transport?.IsFriend(peer) != true) { log($"Dropped an invitation from {EntityId(peer)}, who is not a Steam friend."); return; }
        var (lobbyId, connection, inviter) = (frame.Text(0), frame.Text(1), frame.Text(2));
        var parts = connection.Split(';');
        if (!connection.StartsWith(ConnectionPrefix, StringComparison.Ordinal) || parts.Length != 3 || parts[1] != lobbyId || lobbyId.Length is 0 or > 100 ||
            !TryNode(parts[2], out _) || !TryNode(inviter, out var inviterNode) || inviterNode != peer)
        {
            log($"Dropped a malformed invitation from {EntityId(peer)}.");
            return;
        }
        var now = DateTime.UtcNow;
        if (!invitesHeard.TryGetValue(peer, out var times)) invitesHeard[peer] = times = new Queue<DateTime>();
        while (times.Count > 0 && now - times.Peek() > TimeSpan.FromMinutes(1)) times.Dequeue();
        foreach (var quiet in invitesHeard.Where(i => i.Value.Count == 0 && i.Key != peer).Select(i => i.Key).ToList()) invitesHeard.Remove(quiet);
        if (times.Count >= InvitesPerMinute) return;
        times.Enqueue(now);
        log($"{EntityId(peer)} invited this player to lobby {lobbyId}.");
        toLocal(new Frame("lobby.inviteReceived").Add(lobbyId).Add(connection).Add(inviter));
    }

    // This machine's DLL went away (the game closed): its members leave every lobby, and lobbies it owned close.
    public void LocalDisconnected()
    {
        lock (gate)
        {
            foreach (var lobby in hosted.Values.ToList())
            {
                var leaving = lobby.Members.Where(m => m.Node == LocalNode).ToList();
                if (leaving.Count > 0) Leave(lobby, leaving);
            }
            foreach (var (id, owner) in remote.ToList())
                Send(owner, new Frame("lobby.leave").Add("hellforge-disconnect-" + id).Add(id).Add(""));
            remote.Clear();
            forwarded.Clear();
            joining.Clear();
            invitesSent.Clear();
        }
    }

    // The lobbies hosted here as the directory lists them.
    public IReadOnlyList<Hosted> HostedLobbies()
    {
        lock (gate)
            return [.. hosted.Values.Select(l => new Hosted(l.Id, l.ConnectionString, l.MaxMembers, l.Members.Count, l.Access, l.Lock == 1,
                l.Members.Any(m => m.Node == LocalNode && SameEntity(m.Id, l.OwnerId)), new Dictionary<string, string>(l.SearchProperties, StringComparer.Ordinal),
                PrivacyOf(l.SearchProperties, l.LobbyProperties)))];
    }

    // Whether this machine asked the peer for a join in the last minute (its answers may come from a stranger).
    public bool Joining(ulong peer)
    {
        lock (gate) return joins.Values.Any(j => j.Owner == peer && DateTime.UtcNow - j.At <= JoinMemory);
    }

    // Machines this backend shares a lobby with, for the peer heartbeat.
    public IReadOnlyCollection<ulong> Peers()
    {
        lock (gate) return hosted.Values.SelectMany(l => l.Nodes()).Concat(remote.Values).Where(n => n != LocalNode).ToHashSet();
    }

    // The string a friend's game joins a lobby this machine's game is in with (social tab), or null when unknown.
    public string? ConnectionStringFor(string lobbyId)
    {
        lock (gate)
        {
            if (hosted.TryGetValue(lobbyId, out var lobby)) return lobby.ConnectionString;
            if (remote.TryGetValue(lobbyId, out var owner)) return ConnectionPrefix + lobbyId + ";" + EntityId(owner);
            return null;
        }
    }

    // Whether a friend's machine hosts a lobby this machine is in (whose war time it may set).
    public bool HostsJoinedLobby(ulong node)
    {
        lock (gate) return remote.ContainsValue(node);
    }

    // The friends' machines hosting the lobbies this machine is in.
    public IReadOnlyCollection<ulong> JoinedHosts()
    {
        lock (gate) return remote.Values.Where(n => n != LocalNode).ToHashSet();
    }

    // Friends' machines in lobbies hosted here (they play with this machine's stats: StatsShare).
    public IReadOnlyCollection<ulong> HostedFriends()
    {
        lock (gate) return hosted.Values.SelectMany(l => l.Nodes()).Where(n => n != LocalNode).ToHashSet();
    }

    // Whether a lobby hosted here expects this machine (back): admitted before, invited, or back after a mod sync, and not
    // kicked. A stranger it expects may open a Steam session and send a join although no lobby is listed (StrangerGate).
    public bool Welcome(ulong node)
    {
        lock (gate) return hosted.Values.Any(l => !l.Kicked.Contains(node) && (l.Admitted.Contains(node) || l.Invited.Contains(node) || ExpectedIn(l, node)));
    }

    // Whether a machine refused for its gameplay mods (or told it may rejoin) is still expected back in a lobby.
    bool ExpectedIn(HostedLobby lobby, ulong node) => lobby.Expected.TryGetValue(node, out var until) && Clock() < until;

    void Expect(HostedLobby lobby, ulong node, TimeSpan time)
    {
        var until = Clock() + time;
        foreach (var gone in lobby.Expected.Where(e => e.Value <= Clock()).Select(e => e.Key).ToList()) lobby.Expected.Remove(gone);
        if (lobby.Expected.TryGetValue(node, out var already) && already >= until) return;
        if (!lobby.Expected.ContainsKey(node) && lobby.Expected.Count >= MaxExpected) lobby.Expected.Remove(lobby.Expected.MinBy(e => e.Value).Key);
        lobby.Expected[node] = until;
    }

    // This machine's game invites itself into a lobby (AutoRejoin: the host said it may come back): the lobby stand-in hands
    // its invitation listeners InviteReceived, as for a friend's invitation.
    public void InviteLocal(string lobbyId, string connectionString, string inviterId)
    {
        lock (gate)
        {
            log($"Passed {inviterId}'s lobby {lobbyId} to this player's game as an invitation (rejoining after a mod sync).");
            toLocal(new Frame("lobby.inviteReceived").Add(lobbyId).Add(connectionString).Add(inviterId));
        }
    }

    // Whether a friend's machine is a member of a lobby hosted here (the Party network admits only them).
    public bool InHostedLobby(ulong node)
    {
        lock (gate) return hosted.Values.Any(l => l.Members.Any(m => m.Node == node));
    }

    // A friend's machine stopped answering: its members leave lobbies hosted here, and its own lobbies are gone.
    public void PeerLost(ulong peer)
    {
        lock (gate)
        {
            foreach (var lobby in hosted.Values.ToList())
            {
                var leaving = lobby.Members.Where(m => m.Node == peer).ToList();
                if (leaving.Count > 0) Leave(lobby, leaving);
            }
            foreach (var (id, owner) in remote.Where(r => r.Value == peer).ToList())
            {
                remote.Remove(id);
                toLocal(new Frame("lobby.disconnected").Add(id).Add(2)); // ConnectionInterruption
            }
        }
    }

    // Requests from the local DLL go to the lobby's owner: this backend, or a friend's.
    void Route(Frame frame)
    {
        var op = frame.Text(0);
        (ulong Owner, string LobbyId)? to;
        switch (frame.Type)
        {
            case "lobby.create":
                Handle(LocalNode, frame);
                return;
            case "lobby.join":
                to = RouteJoin(frame, op);
                break;
            default:
                to = RouteToLobby(frame, op);
                break;
        }
        if (to is not { } target) return;
        forwarded[op] = target;
        Send(target.Owner, frame);
    }

    // A join from the local DLL: one of this machine's lobbies is joined here, a friend's lobby through its owner (its owner
    // and the lobby; null when answered here).
    (ulong Owner, string LobbyId)? RouteJoin(Frame frame, string op)
    {
        var connection = frame.Text(3);
        var parts = connection.StartsWith(ConnectionPrefix, StringComparison.Ordinal) ? connection.Split(';') : [];
        if (parts.Length != 3 || !TryNode(parts[2], out var owner) || transport == null && owner != LocalId)
        {
            Answer(LocalNode, op, "join", LobbyNotFound);
            return null;
        }
        if (owner == LocalId) { Handle(LocalNode, frame); return null; }
        if (JoinRefusal?.Invoke(owner) is { } why)
        {
            log($"Did not join {parts[1]} of {EntityId(owner)}: " + string.Join(" ", why));
            Answer(LocalNode, op, "join", LobbyNotJoinable, parts[1]);
            Refused?.Invoke(owner, why);
            return null;
        }
        var lobbyId = parts[1];
        remote[lobbyId] = owner;
        joining[owner] = DateTime.UtcNow;
        foreach (var old in joins.Where(j => DateTime.UtcNow - j.Value.At > JoinMemory).Select(j => j.Key).ToList()) joins.Remove(old);
        joins[lobbyId] = (owner, DateTime.UtcNow);
        var (joinOwner, joinLobby) = (owner, lobbyId);
        afterLock.Add(() => LocalJoinStarted?.Invoke(joinOwner, joinLobby));
        frame.Add(modDigest);
        frame.Add(Protocol);
        if (modList != null) frame.Add(modList);
        return (owner, lobbyId);
    }

    // Any other request from the local DLL: handled here for a lobby hosted here, else sent to the friend who owns it
    // (its owner and the lobby; null when answered here).
    (ulong Owner, string LobbyId)? RouteToLobby(Frame frame, string op)
    {
        var lobbyId = frame.Text(1);
        if (hosted.ContainsKey(lobbyId)) { Handle(LocalNode, frame); return null; }
        // only the owner kicks, and only from its own machine
        if (frame.Type == "lobby.kick" || !remote.TryGetValue(lobbyId, out var owner))
        {
            Answer(LocalNode, op, Kind(frame.Type), frame.Type == "lobby.kick" && remote.ContainsKey(lobbyId) ? AccessDenied : LobbyNotFound);
            return null;
        }
        if (frame.Type == "lobby.leave") remote.Remove(lobbyId);
        // an invitation to a friend's lobby: its owner records it, then this machine passes it on (RelayEvent)
        if (frame.Type == "lobby.invite" && TryNode(frame.Text(3), out var invitee))
            invitesSent[op] = (invitee, lobbyId, frame.Text(2));
        return (owner, lobbyId);
    }

    // Whether this machine asked owner to join lobbyId in the last minute (a mod offer from them counts only then).
    public bool JoinedRecently(ulong owner, string lobbyId)
    {
        lock (gate) return joins.TryGetValue(lobbyId, out var join) && join.Owner == owner && DateTime.UtcNow - join.At <= JoinMemory;
    }

    static string Kind(string type) => type switch
    {
        "lobby.update" => "update", "lobby.kick" => "kick", "lobby.invite" => "invite", "lobby.join" => "join", "lobby.create" => "create", _ => "leave"
    };

    // An event from a friend's backend reaches this machine's DLL only if that friend owns the lobby it is about.
    void RelayEvent(ulong peer, Frame frame)
    {
        var relayed = frame.Type == "lobby.result" ? ForwardedResult(peer, frame) : OwnersEvent(peer, frame);
        if (relayed) toLocal(frame);
    }

    // The answer to a request forwarded to that friend (false: not one).
    bool ForwardedResult(ulong peer, Frame frame)
    {
        var op = frame.Text(0);
        if (!forwarded.Remove(op, out var request) || request.Owner != peer) return false;
        if (frame.Text(1) == "join" && frame.Text(2) != "0") remote.Remove(request.LobbyId);
        if (frame.Text(1) == "join" && frame.Text(2) == "0") joining.Remove(peer); /* in: no refusal to wait for */
        if (frame.Text(1) == "join" && uint.TryParse(frame.Text(2), NumberStyles.None, CultureInfo.InvariantCulture, out var joinResult))
            afterLock.Add(() => LocalJoinFinished?.Invoke(peer, request.LobbyId, joinResult));
        if (invitesSent.Remove(op, out var invite) && frame.Text(1) == "invite" && frame.Text(2) == "0" && ConnectionStringFor(invite.LobbyId) is { } connection)
            Deliver(invite.Invitee, invite.LobbyId, connection, invite.Inviter);
        return true;
    }

    // An event about a lobby that friend owns (false: not one).
    bool OwnersEvent(ulong peer, Frame frame)
    {
        if (!remote.TryGetValue(frame.Text(0), out var owner) || owner != peer) return false;
        if (frame.Type == "lobby.disconnected") remote.Remove(frame.Text(0));
        // the owner removed this machine's player (its own copy goes to this machine only with this reason)
        if (frame.Type == "lobby.memberRemoved" && frame.Text(2) == MemberRemovedForcibly.ToString(CultureInfo.InvariantCulture))
        {
            log($"The host {EntityId(peer)} removed this player from lobby {frame.Text(0)}.");
            afterLock.Add(() => KickedByHost?.Invoke(peer));
        }
        return true;
    }

    // An invitation goes from the inviting player's backend to the invitee's (lobby.invited); its game shows it.
    void Deliver(ulong invitee, string lobbyId, string connection, string inviter)
    {
        if (invitee == LocalId || transport == null) return;
        log($"Invited {EntityId(invitee)} to lobby {lobbyId}.");
        transport.Send(invitee, new Frame("lobby.invited").Add(lobbyId).Add(connection).Add(inviter).Encode(), reliable: true);
    }

    // Requests for a lobby this backend owns (or creates), from the local DLL or a friend's backend.
    void Handle(ulong node, Frame frame)
    {
        var op = frame.Text(0);
        switch (frame.Type)
        {
            case "lobby.create": HandleCreate(node, frame, op); return;
            case "lobby.join": HandleJoin(node, frame, op); return;
            case "lobby.update": HandleUpdate(node, frame, op); return;
            case "lobby.leave": HandleLeave(node, frame, op); return;
            case "lobby.kick": HandleKick(node, frame, op); return;
            case "lobby.invite": HandleInvite(node, frame, op); return;
        }
    }

    void HandleCreate(ulong node, Frame frame, string op)
    {
        if (node != LocalNode) { Answer(node, op, "create", AccessDenied); return; }
        var index = 7;
        var lobby = new HostedLobby
        {
            Id = Guid.NewGuid().ToString(),
            MaxMembers = frame.UInt(3), Access = frame.Int(4), Migration = frame.Int(5), Lock = frame.Int(6)
        };
        lobby.LobbyProperties = frame.Properties(ref index);
        lobby.SearchProperties = frame.Properties(ref index);
        var memberProperties = frame.Properties(ref index);
        lobby.ConnectionString = ConnectionPrefix + lobby.Id + ";" + EntityId(LocalId != 0 ? LocalId : NodeOf(frame.Text(1)));
        var creator = new LobbyMember(frame.Text(1), frame.Text(2), LocalNode) { Properties = memberProperties };
        lobby.Members.Add(creator);
        lobby.OwnerId = creator.Id;
        hosted[lobby.Id] = lobby;
        log($"Lobby {lobby.Id} created, at most {lobby.MaxMembers} members.");
        HostedChanged?.Invoke();
        Send(LocalNode, Snapshot(new Frame("lobby.result").Add(op).Add("create").Add(0), lobby));
        afterLock.Add(() => LocalLobbyCreated?.Invoke());
    }

    void HandleJoin(ulong node, Frame frame, string op)
    {
        var memberId = frame.Text(1);
        var connection = frame.Text(3).Split(';');
        var index = 4;
        var memberProperties = frame.Properties(ref index);
        var digest = node == LocalNode ? modDigest : frame.Count > index ? frame.Text(index) : "";
        var protocol = node == LocalNode ? Protocol : frame.Count > index + 1 ? frame.Text(index + 1) : "an older HellForge";
        if (connection.Length != 3 || !hosted.TryGetValue(connection[1], out var lobby)) { Answer(node, op, "join", LobbyNotFound, connection.ElementAtOrDefault(1)); return; }
        if (!Admits(node, frame, op, lobby, memberId, memberProperties)) return;
        if (protocol != Protocol)
        {
            Refuse(node, op, lobby.Id, $"different HellForge versions ({protocol} joining, {Protocol} hosting); both players need the same installer.",
                ["Your HellForge version is not the host's.", "Both players need the same HellForge installer (its version is in the launcher's corner)."],
                ["Their HellForge version is not yours: both players need the same HellForge installer."]);
            return;
        }
        if (digest != modDigest)
        {
            RefuseForMods(node, frame, op, lobby, index);
            return;
        }
        AddMember(node, frame, op, lobby, memberId, memberProperties);
    }

    // A friend's join, before the version and mod checks: as themselves, within PlayFab's limits, and admitted (false:
    // answered, refused).
    bool Admits(ulong node, Frame frame, string op, HostedLobby lobby, string memberId, Dictionary<string, string> memberProperties)
    {
        if (node == LocalNode) return true;
        // A friend joins as themselves: the member id must be the requesting machine's own SteamID.
        if (!TryNode(memberId, out var claimed) || claimed != node) { Answer(node, op, "join", AccessDenied, lobby.Id); return false; }
        if (!WithinLimits(memberProperties) || frame.Text(2).Length > MaxKeyLength) { Answer(node, op, "join", InvalidArgument, lobby.Id); return false; }
        // The lobby's privacy and kicked players first: someone refused here is never offered this host's mods.
        if (Admission(lobby, node) is { } refusal)
        {
            Refuse(node, op, lobby.Id, refusal.Reason, refusal.JoinerLines, refusal.HostLines);
            return false;
        }
        return true;
    }

    // A join with other gameplay mods than this host's (index: the frame's field after the member properties).
    void RefuseForMods(ulong node, Frame frame, string op, HostedLobby lobby, int index)
    {
        // the joiner's mods one by one (after its digest and protocol; a joiner from before sends none)
        var theirs = node != LocalNode && frame.Count > index + 2 ? ModlistDiff.Decode(frame.Text(index + 2)) : null;
        var mine = modList != null ? ModlistDiff.Decode(modList) : null;
        // mod sync: the joiner's backend gets this host's mod set first; when it may download it, its game is told so
        var offered = node != LocalNode && ModsRefused?.Invoke(node, lobby.Id) == true;
        // it passed the privacy check, so it may come back with this host's mods (AutoRejoin), invited or not
        if (node != LocalNode) Expect(lobby, node, ExpectedBack);
        Refuse(node, op, lobby.Id, "different gameplay mods; enable the same gameplay mods (see the launcher's MODS page) and start a new session.",
            ["Your gameplay mods don't match the host's.", .. Differences(theirs, mine, asHost: false),
                offered ? "HellForge's MOD SYNC prompt shows which of the host's mods it can download for you."
                    : "Turn on the same gameplay mods on the launcher's MODS page, then start a new session."],
            ["Their gameplay mods don't match yours.", .. Differences(mine, theirs, asHost: true)]);
    }

    // A join that passed the checks: the member joins unless the lobby is locked, full, or has them already.
    void AddMember(ulong node, Frame frame, string op, HostedLobby lobby, string memberId, Dictionary<string, string> memberProperties)
    {
        // A friend joining again while still listed (their game crashed, or they came back before this host noticed
        // they were gone): the earlier membership leaves first, so the others see them leave and join again.
        if (node != LocalNode && lobby.Members.Where(m => m.Node == node && SameEntity(m.Id, memberId)).ToList() is { Count: > 0 } stale)
        {
            log($"A friend joined lobby {lobby.Id} again; their earlier membership was removed first.");
            Leave(lobby, stale);
        }
        if (lobby.Lock == 1 || lobby.Members.Count >= lobby.MaxMembers)
        {
            Refuse(node, op, lobby.Id, lobby.Lock == 1 ? "the lobby is locked." : "the lobby is full.", [lobby.Lock == 1 ? "The host's lobby is locked." : "The host's lobby is full."]);
            return;
        }
        if (lobby.Members.Any(m => SameEntity(m.Id, memberId))) { Answer(node, op, "join", InvalidArgument, lobby.Id); return; }
        var member = new LobbyMember(memberId, frame.Text(2), node) { Properties = memberProperties };
        lobby.Members.Add(member);
        if (node != LocalNode) lobby.Admitted.Add(node);
        if (node != LocalNode) PeerAdmitted?.Invoke(node);
        log($"{(node == LocalNode ? "A local player" : "A friend")} joined lobby {lobby.Id} ({lobby.Members.Count} members).");
        Send(node, Snapshot(new Frame("lobby.result").Add(op).Add("join").Add(0), lobby));
        // A friend's backend adopts this war time, so both games see the same war.
        if (node != LocalNode && warClock != null) Send(node, new Frame("war.clock").Add(warClock.Seconds));
        var added = new Frame("lobby.memberAdded").Add(lobby.Id).Add(member.Id).Add(member.Type).Add(member.Properties);
        foreach (var other in lobby.Nodes().Where(n => n != node)) Send(other, added);
        HostedChanged?.Invoke();
    }

    // A lobby.update's changes: the lobby's properties, its search properties, and the member's (when MemberUpdate).
    sealed record LobbyChanges(List<PropertyChange> Lobby, List<PropertyChange> Search, bool MemberUpdate, List<PropertyChange> Member)
    {
        public static LobbyChanges Read(Frame frame)
        {
            var index = 11;
            var lobbyChanges = frame.Changes(ref index);
            var searchChanges = frame.Changes(ref index);
            var memberUpdate = frame.Text(index++) == "1";
            var memberChanges = frame.Changes(ref index);
            return new LobbyChanges(lobbyChanges, searchChanges, memberUpdate, memberChanges);
        }
    }

    void HandleUpdate(ulong node, Frame frame, string op)
    {
        if (!hosted.TryGetValue(frame.Text(1), out var lobby)) { Answer(node, op, "update", LobbyNotFound); return; }
        var member = lobby.Members.FirstOrDefault(m => SameEntity(m.Id, frame.Text(2)) && m.Node == node);
        var changes = LobbyChanges.Read(frame);
        var newOwner = frame.Text(3) == "1" ? lobby.Members.FirstOrDefault(m => SameEntity(m.Id, frame.Text(4))) : null;
        var result = UpdateResult(node, frame, lobby, member, newOwner, changes);
        Answer(node, op, "update", result);
        if (result != 0) return;
        var updated = Updated(lobby, frame, newOwner, member, changes);
        foreach (var target in lobby.Nodes()) Send(target, updated);
        HostedChanged?.Invoke();
    }

    // Whether an update is taken (0): only the owner changes the lobby, and a new owner is one of its members.
    static uint UpdateResult(ulong node, Frame frame, HostedLobby lobby, LobbyMember? member, LobbyMember? newOwner, LobbyChanges changes)
    {
        var changesLobby = frame.Text(3) == "1" || frame.Text(5) == "1" || frame.Text(7) == "1" || frame.Text(9) == "1" ||
            changes.Lobby.Count > 0 || changes.Search.Count > 0;
        if (member == null) return LobbyNotFound;
        if (changesLobby && !SameEntity(lobby.OwnerId, member.Id)) return AccessDenied;
        if (frame.Text(3) == "1" && newOwner == null) return InvalidArgument;
        // another machine's update keeps to PlayFab's limits (the SDK fails a whole update that breaks them)
        if (node != LocalNode && (!Fits(lobby.LobbyProperties, changes.Lobby) || !Fits(lobby.SearchProperties, changes.Search, search: true) ||
            !Fits(member.Properties, changes.Member) || frame.Text(5) == "1" && frame.UInt(6) is < 2 or > 128)) return InvalidArgument;
        return 0;
    }

    // A taken update made to the lobby, and the lobby.updated its members are sent.
    static Frame Updated(HostedLobby lobby, Frame frame, LobbyMember? newOwner, LobbyMember? member, LobbyChanges changes)
    {
        var updated = new Frame("lobby.updated").Add(lobby.Id);
        if (newOwner != null) lobby.OwnerId = newOwner.Id;
        updated.Add(newOwner?.Id ?? "");
        if (frame.Text(5) == "1") lobby.MaxMembers = frame.UInt(6);
        updated.Add(frame.Text(5) == "1" ? lobby.MaxMembers.ToString(CultureInfo.InvariantCulture) : "");
        if (frame.Text(7) == "1") lobby.Access = frame.Int(8);
        updated.Add(frame.Text(7) == "1" ? lobby.Access.ToString(CultureInfo.InvariantCulture) : "");
        if (frame.Text(9) == "1") lobby.Lock = frame.Int(10);
        updated.Add(frame.Text(9) == "1" ? lobby.Lock.ToString(CultureInfo.InvariantCulture) : "");
        Apply(lobby.LobbyProperties, changes.Lobby);
        Apply(lobby.SearchProperties, changes.Search);
        updated.Add(changes.Lobby).Add(changes.Search);
        if (changes.MemberUpdate)
        {
            Apply(member!.Properties, changes.Member);
            updated.Add(1).Add(member.Id).Add(changes.Member);
        }
        else updated.Add(0);
        return updated;
    }

    void HandleLeave(ulong node, Frame frame, string op)
    {
        if (!hosted.TryGetValue(frame.Text(1), out var lobby)) { Answer(node, op, "leave", LobbyNotFound); return; }
        var memberId = frame.Text(2);
        var leaving = lobby.Members.Where(m => m.Node == node && (memberId.Length == 0 || SameEntity(m.Id, memberId))).ToList();
        Answer(node, op, "leave", 0);
        if (leaving.Count > 0) Leave(lobby, leaving);
    }

    void HandleKick(ulong node, Frame frame, string op)
    {
        // The owner's game removes a member (PFLobbyForceRemoveMember): only from this machine, as the owner, never
        // itself. The kicked machine leaves, its Party traffic with it, and never rejoins this lobby.
        if (!hosted.TryGetValue(frame.Text(1), out var lobby)) { Answer(node, op, "kick", LobbyNotFound); return; }
        if (node != LocalNode || !lobby.Members.Any(m => m.Node == LocalNode && SameEntity(m.Id, lobby.OwnerId))) { Answer(node, op, "kick", AccessDenied); return; }
        var target = lobby.Members.FirstOrDefault(m => SameEntity(m.Id, frame.Text(2)));
        if (target == null || target.Node == LocalNode) { Answer(node, op, "kick", InvalidArgument); return; }
        Kick(lobby, target.Node, frame.Count > 3 && frame.Text(3) == "1");
        Answer(node, op, "kick", 0);
    }

    void HandleInvite(ulong node, Frame frame, string op)
    {
        // A member invites someone (PFLobbySendInvite): the lobby admits them whatever its privacy (Admission), and the
        // inviting machine passes the invitation on (Deliver; RelayEvent for a friend's lobby).
        if (!hosted.TryGetValue(frame.Text(1), out var lobby)) { Answer(node, op, "invite", LobbyNotFound); return; }
        if (!lobby.Members.Any(m => m.Node == node && SameEntity(m.Id, frame.Text(2)))) { Answer(node, op, "invite", AccessDenied); return; }
        if (!TryNode(frame.Text(3), out var invitee) || invitee == (node == LocalNode ? LocalId : node)) { Answer(node, op, "invite", InvalidArgument); return; }
        if (lobby.Kicked.Contains(invitee)) { Answer(node, op, "invite", AccessDenied); return; }
        if (lobby.Invited.Count >= MaxInvited && !lobby.Invited.Contains(invitee)) lobby.Invited.Remove(lobby.Invited.First());
        lobby.Invited.Add(invitee);
        log($"{(node == LocalNode ? "This player" : EntityId(node))} invited {EntityId(invitee)} to lobby {lobby.Id}.");
        Answer(node, op, "invite", 0);
        if (node == LocalNode) Deliver(invitee, lobby.Id, lobby.ConnectionString, frame.Text(2));
    }

    // Whether a machine may join (null) or why not: kicked, then the lobby's privacy (see the top of this file).
    (string Reason, string[] JoinerLines, string[]? HostLines)? Admission(HostedLobby lobby, ulong node)
    {
        if (lobby.Kicked.Contains(node))
            return ("the host removed this player from the lobby.", ["The host removed you from this lobby.", "You can't join it again while it lasts."], null);
        if (lobby.Admitted.Contains(node) || lobby.Members.Any(m => m.Node == node) || lobby.Invited.Contains(node) || ExpectedIn(lobby, node)) return null;
        var privacy = PrivacyOf(lobby.SearchProperties, lobby.LobbyProperties);
        var friend = transport?.IsFriend(node) == true;
        if (privacy == Privacy.Public || privacy is Privacy.FriendsOnly or Privacy.FriendsAndClan && friend) return null;
        // a friend refused is told how to get in, and the host who; a stranger's refusal is only logged here (no notice spam)
        return privacy == Privacy.InviteOnly
            ? ("the lobby is Invite only and this player was not invited.", ["The host's lobby is Invite only.", "Ask the host to invite you from the game's social menu."],
                friend ? ["Your lobby is Invite only: invite them from the game's social menu."] : null)
            : ("the lobby is Friends only and this player is not the host's Steam friend.", ["The host's lobby is Friends only.", "Only the host's Steam friends, or players they invite, can join."], null);
    }

    // A machine back after a mod sync asks whether it may rejoin (lobby.rejoin, see the top of this file): the lobby asked,
    // or the one lobby this machine's game owns now when that one has closed, checked as a join would be (kicked, privacy
    // with the machine expected back, version, mods, lock, room). An "ok" keeps it expected there for RejoinGrace.
    void RejoinAsked(ulong node, Frame frame)
    {
        var asked = frame.Text(0);
        var (digest, protocol) = (frame.Count > 1 ? frame.Text(1) : "", frame.Count > 2 ? frame.Text(2) : "");
        var owned = hosted.Values.Where(l => l.Members.Any(m => m.Node == LocalNode && SameEntity(m.Id, l.OwnerId))).ToList();
        var lobby = hosted.TryGetValue(asked, out var same) && owned.Contains(same) ? same : owned.Count == 1 ? owned[0] : null;
        void Tell(string status, IReadOnlyList<string> lines)
        {
            log($"{EntityId(node)} asked to rejoin lobby {asked} after a mod sync: {status}" + (lobby != null && lobby.Id != asked ? $" (this host's lobby is now {lobby.Id})" : "") +
                (lines.Count > 0 ? " | " + string.Join(" | ", lines) : "") + ".");
            var answer = new Frame("lobby.rejoined").Add(asked).Add(status);
            foreach (var line in lines) answer.Add(line);
            Send(node, answer);
        }
        if (asked.Length is 0 or > 100) return;
        if (lobby == null) { Tell("gone", ["The host's lobby has closed."]); return; }
        if (Admission(lobby, node) is { } refusal) { Tell("refused", refusal.JoinerLines); return; }
        if (protocol != Protocol) { Tell("version", ["Your HellForge version is not the host's.", "Both players need the same HellForge installer."]); return; }
        if (digest != modDigest) { Tell("mods", ["The host's gameplay mods changed after your restart.", "Join them again to see which differ."]); return; }
        if (lobby.Lock == 1) { Tell("locked", ["The host's lobby is locked."]); return; }
        if (lobby.Members.Count(m => m.Node != node) >= lobby.MaxMembers) { Tell("full", ["The host's lobby is full."]); return; }
        Expect(lobby, node, RejoinGrace);
        log($"{EntityId(node)} may rejoin lobby {lobby.Id} after a mod sync" + (lobby.Id != asked ? $" (it asked for {asked}, which has closed)" : "") + ".");
        Send(node, new Frame("lobby.rejoined").Add(asked).Add("ok").Add(lobby.Id).Add(lobby.ConnectionString));
    }

    // The host kicks a machine from one of its lobbies: the kicked machine's game hears that its member was removed by the
    // owner (LocalUserForciblyRemoved) and the lobby is gone for it (NoLocalMembers), as the SDK reports a force removal;
    // the others hear that a member left (RemoteUserLeftLobby). The machine stays refused for the lobby's lifetime
    // (preventRejoin or not: decided 2026-10-23), and its Party traffic is dropped (Kicked: PartyService.Expel).
    void Kick(HostedLobby lobby, ulong node, bool preventRejoin)
    {
        var leaving = lobby.Members.Where(m => m.Node == node).ToList();
        foreach (var member in leaving) lobby.Members.Remove(member);
        lobby.Kicked.Add(node);
        lobby.Admitted.Remove(node);
        lobby.Invited.Remove(node);
        lobby.Expected.Remove(node);
        log($"Kicked {EntityId(node)} from lobby {lobby.Id} (the game asked {(preventRejoin ? "to" : "not to")} prevent a rejoin; it is prevented either way).");
        foreach (var member in leaving) Send(node, new Frame("lobby.memberRemoved").Add(lobby.Id).Add(member.Id).Add(MemberRemovedForcibly));
        Send(node, new Frame("lobby.disconnected").Add(lobby.Id).Add(DisconnectingNoLocalMembers));
        foreach (var member in leaving)
        {
            var removed = new Frame("lobby.memberRemoved").Add(lobby.Id).Add(member.Id).Add(MemberRemovedRemoteLeft);
            foreach (var other in lobby.Nodes()) Send(other, removed);
        }
        HostedChanged?.Invoke();
        afterLock.Add(() => Kicked?.Invoke(node));
    }

    // Kicks a machine from every lobby hosted here (the Party stand-in's KickDevice and KickUser go here); false when it was in none.
    public bool Kick(ulong node)
    {
        bool any;
        lock (gate)
        {
            var lobbies = hosted.Values.Where(l => l.Members.Any(m => m.Node == node) && node != LocalNode).ToList();
            foreach (var lobby in lobbies) Kick(lobby, node, preventRejoin: true);
            any = lobbies.Count > 0;
        }
        Drain();
        return any;
    }

    // Another machine's properties within PlayFab's limits.
    static bool WithinLimits(IReadOnlyDictionary<string, string> properties) =>
        properties.Count <= MaxProperties && properties.All(p => p.Key.Length is > 0 and <= MaxKeyLength && p.Value.Length <= MaxValueLength);

    // Changes from another machine: at most twice the limit in one update (half of them removals, as the SDK allows), and
    // the result within the limits; search keys only string_key1..30 and number_key1..30, number_keys numbers.
    static bool Fits(Dictionary<string, string> properties, List<PropertyChange> changes, bool search = false)
    {
        if (changes.Count > 2 * MaxProperties) return false;
        var after = new Dictionary<string, string>(properties, StringComparer.Ordinal);
        Apply(after, changes);
        return WithinLimits(after) && (!search || after.All(p => SearchValue(p.Key, p.Value)));
    }

    // A search property as PlayFab takes it: string_keyN or number_keyN, N 1..30; a number_key's value a finite number.
    public static bool SearchValue(string key, string value)
    {
        var number = key.StartsWith("number_key", StringComparison.Ordinal);
        var digits = number ? key["number_key".Length..] : key.StartsWith("string_key", StringComparison.Ordinal) ? key["string_key".Length..] : "";
        if (!int.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out var n) || n is < 1 or > MaxProperties || digits[0] == '0') return false;
        if (!number) return value.Length <= 256;
        return value.Length <= 32 && double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) && double.IsFinite(parsed);
    }

    // Members leave. When the owner leaves, the lobby closes for everyone else (HD2's host ends the session).
    void Leave(HostedLobby lobby, List<LobbyMember> leaving)
    {
        foreach (var member in leaving) lobby.Members.Remove(member);
        HostedChanged?.Invoke();
        var leavingNodes = leaving.Select(m => m.Node).ToHashSet();
        if (leaving.Any(m => SameEntity(m.Id, lobby.OwnerId)) || lobby.Members.Count == 0)
        {
            hosted.Remove(lobby.Id);
            log($"Lobby {lobby.Id} closed.");
            var closed = new Frame("lobby.disconnected").Add(lobby.Id).Add(DisconnectingLobbyDeleted);
            foreach (var other in lobby.Nodes().Where(n => !leavingNodes.Contains(n))) Send(other, closed);
            return;
        }
        foreach (var member in leaving)
        {
            var removed = new Frame("lobby.memberRemoved").Add(lobby.Id).Add(member.Id).Add(MemberRemovedRemoteLeft);
            foreach (var other in lobby.Nodes()) Send(other, removed);
        }
    }

    // A join this host will not take: the joiner's backend is told why (the reason, then the lines its game shows), then the
    // join fails like a non-joinable lobby. hostLines: what this host's game shows, when it should know.
    void Refuse(ulong node, string op, string lobbyId, string reason, IReadOnlyList<string> joinerLines, IReadOnlyList<string>? hostLines = null)
    {
        log($"Refused a join to {lobbyId}: {reason}" + (hostLines is { Count: > 1 } ? " | " + string.Join(" | ", hostLines.Skip(1)) : ""));
        if (node != LocalNode)
        {
            var refused = new Frame("lobby.refused").Add(reason);
            foreach (var line in joinerLines) refused.Add(line);
            Send(node, refused);
            if (hostLines != null) RefusedFriend?.Invoke(node, hostLines);
        }
        Answer(node, op, "join", LobbyNotJoinable, lobbyId);
    }

    // The mods that differ, a line each, as one player reads them (ModlistDiff), at most 6 lines; none when either list is
    // unknown (a backend from before modlists).
    static List<string> Differences(List<ModFingerprint>? mine, List<ModFingerprint>? theirs, bool asHost)
    {
        if (mine == null || theirs == null) return [];
        var lines = ModlistDiff.Describe(mine, theirs, asHost);
        return lines.Count <= 6 ? lines : [.. lines.Take(5), $"... and {lines.Count - 5} more"];
    }

    void Answer(ulong node, string op, string kind, uint result, string? lobbyId = null)
    {
        var frame = new Frame("lobby.result").Add(op).Add(kind).Add(result);
        if (lobbyId != null) frame.Add(lobbyId);
        Send(node, frame);
    }

    static Frame Snapshot(Frame frame, HostedLobby lobby)
    {
        frame.Add(lobby.Id).Add(lobby.ConnectionString).Add(lobby.OwnerId).Add(lobby.MaxMembers).Add(lobby.Access).Add(lobby.Migration)
            .Add(lobby.Lock).Add(lobby.LobbyProperties).Add(lobby.SearchProperties).Add(lobby.Members.Count);
        foreach (var member in lobby.Members) frame.Add(member.Id).Add(member.Type).Add(member.Properties);
        return frame;
    }

    void Send(ulong node, Frame frame)
    {
        if (node == LocalNode) toLocal(frame);
        else transport?.Send(node, frame.Encode(), reliable: true);
    }

    static void Apply(Dictionary<string, string> properties, List<PropertyChange> changes)
    {
        foreach (var change in changes)
            if (change.Value == null) properties.Remove(change.Key);
            else properties[change.Key] = change.Value;
    }

    static bool SameEntity(string a, string b) =>
        TryNode(a, out var x) && TryNode(b, out var y) ? x == y : string.Equals(a, b, StringComparison.Ordinal);

    static ulong NodeOf(string entityId) => TryNode(entityId, out var node) ? node : 0;

    sealed class HostedLobby
    {
        public string Id = "";
        public string ConnectionString = "";
        public string OwnerId = "";
        public uint MaxMembers;
        public int Access, Migration, Lock;
        public Dictionary<string, string> LobbyProperties = new(StringComparer.Ordinal);
        public Dictionary<string, string> SearchProperties = new(StringComparer.Ordinal);
        public List<LobbyMember> Members = [];
        // Machines admitted before (welcome back whatever the privacy), invited by a member, and kicked (never back): for
        // the lobby's lifetime, not stored anywhere.
        public HashSet<ulong> Admitted = [], Invited = [], Kicked = [];
        // Machines expected back after a mod sync, until when (Expect).
        public Dictionary<ulong, DateTime> Expected = [];
        public IEnumerable<ulong> Nodes() => Members.Select(m => m.Node).Distinct();
    }

    sealed class LobbyMember(string id, string type, ulong node)
    {
        public string Id { get; } = id;
        public string Type { get; } = type;
        public ulong Node { get; } = node;
        public Dictionary<string, string> Properties { get; init; } = new(StringComparer.Ordinal);
    }
}
