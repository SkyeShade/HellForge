using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// Rejoining by itself after a mod sync (docs/multiplayer.md "Mod sync"). The session the launcher restarts with a host's
// mods (--mod-sync) passes the same file to its backend (--rejoin, a ModSyncSession); a session whose running game took
// the host's mods without a restart (LiveMods) starts it at once, past Waiting when its game has its own lobby. Either brings
// the game back into that host's lobby through the game's own invitation path, the one a friend's invitation takes:
//   Waiting   until this machine's game creates its own lobby (LobbyService.LocalLobbyCreated: it is on the ship and has
//             listened for invitations since it booted), at most ReadyTimeout from the start;
//   Settling  Settle more, so the ship's screens are up;
//   Asking    lobby.rejoin to the host (LobbyService: the lobby asked, or the one the host's game owns now, checked as a
//             join would be), at most AnswerTimeout for lobby.rejoined;
//   Invited   on "ok" the lobby stand-in hands the game InviteReceived from the host (LobbyService.InviteLocal), and the
//             game takes it as any invitation; AcceptWindow for its join to start;
//   Joining   the game asked to join the host's lobby, until the answer;
//   Done / GaveUp.
// It gives up once, with a notice in game saying why (the host's lobby closed, is full or locked, the host does not
// answer, the game never reached the ship), and never tries again. The game joining any lobby by itself first ends it
// (into the host's: it is rejoining; another: the player chose). The host keeps the machine expected back
// (LobbyService.ExpectedBack after the mod refusal, RejoinGrace after its "ok"), so the join is admitted whatever the
// lobby's privacy, unless the machine was kicked.
public sealed class AutoRejoin : IDisposable
{
    public static readonly TimeSpan ReadyTimeout = TimeSpan.FromMinutes(3), Settle = TimeSpan.FromSeconds(5), AnswerTimeout = TimeSpan.FromSeconds(10),
        AcceptWindow = TimeSpan.FromMinutes(2);
    // A rejoin file older than this is from a restart long ago: the host's lobby has surely moved on.
    public static readonly TimeSpan MaxAge = TimeSpan.FromMinutes(15);

    public enum Stage { Waiting, Settling, Asking, Invited, Joining, Done, GaveUp }

    // Whom to rejoin: the host's machine and name, the lobby it refused this machine from, and the host's modlist digest.
    public sealed record Target(ulong Host, string HostName, string LobbyId, string Digest);

    readonly Target target;
    readonly string ownDigest;
    readonly Action<ulong, Frame> send;
    readonly Action<string, string, string> invite;
    readonly Action<string, string, IReadOnlyList<string>> notice;
    readonly Action<string> log;
    readonly Func<DateTime> clock;
    readonly object gate = new();
    readonly DateTime started;
    DateTime due;
    Timer? timer;

    // send: a frame to a peer's backend; invite: lobby id, connection string, inviter id to this machine's game
    // (LobbyService.InviteLocal); notice: kind, title and lines for the game (StatsShare.Notice).
    public AutoRejoin(Target target, string ownDigest, Action<ulong, Frame> send, Action<string, string, string> invite,
        Action<string, string, IReadOnlyList<string>> notice, Action<string> log, Func<DateTime>? clock = null)
    {
        this.target = target;
        this.ownDigest = ownDigest;
        this.send = send;
        this.invite = invite;
        this.notice = notice;
        this.log = log;
        this.clock = clock ?? (() => DateTime.UtcNow);
        started = this.clock();
        due = started + ReadyTimeout;
        log($"Rejoin: this session plays {target.HostName}'s mods after a mod sync; once this game is on the ship it is invited back into lobby {target.LobbyId}.");
    }

    public Stage Current { get { lock (gate) return stage; } }
    Stage stage = Stage.Waiting;

    // The rejoin file a session after a mod sync passes (ModSyncSession), or why this session does not rejoin: written by
    // this machine's backend, but checked as if it were not (the host and lobby from its connection string, its age), and
    // only when this session plays the host's mods (its modlist digest is the host's: mods-stage found them all).
    public static (Target? Target, string? Reason) Read(string path, ulong localId, string ownDigest, DateTime now)
    {
        ModSyncSession session;
        try
        {
            if (now - File.GetLastWriteTimeUtc(path) > MaxAge) return (null, "the mod sync is older than " + MaxAge.TotalMinutes + " minutes.");
            session = ModSyncSession.Read(path);
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            return (null, "the mod sync file could not be read (" + error.Message + ").");
        }
        return From(session, localId, ownDigest);
    }

    // Whom a mod sync's session rejoins (the file a restart passes, or the running game's mods changed live: LiveMods), or
    // why it does not: checked as if this machine had not written it.
    public static (Target? Target, string? Reason) From(ModSyncSession session, ulong localId, string ownDigest)
    {
        var parts = session.Connection.Split(';');
        if (!session.Connection.StartsWith(LobbyService.ConnectionPrefix, StringComparison.Ordinal) || parts.Length != 3 || parts[1].Length is 0 or > 100 ||
            !LobbyService.TryNode(parts[2], out var host) || !LobbyService.TryNode(session.Host, out var named) || named != host)
            return (null, "the mod sync file names no host's lobby.");
        if (host == localId) return (null, "the mod sync file names this player's own lobby.");
        if (session.Digest != ownDigest) return (null, "this session plays its own mods, not the host's (see the session's mod problems).");
        return (new Target(host, session.HostName.Length > 0 ? session.HostName : "the host", parts[1], session.Digest), null);
    }

    // Follows this machine's lobby service: its game's own lobby, its joins, and the host's answer.
    public void Attach(LobbyService lobby)
    {
        attached = lobby;
        lobby.LocalLobbyCreated += GameReady;
        lobby.LocalJoinStarted += JoinStarted;
        lobby.LocalJoinFinished += JoinFinished;
        lobby.RejoinAnswered += Answered;
    }

    LobbyService? attached;

    // Ticks once a second.
    public void Start() => timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));

    // Stops following the lobby service too: a later mod sync's rejoin takes over (MultiplayerHost).
    public void Dispose()
    {
        timer?.Dispose();
        if (attached is not { } lobby) return;
        lobby.LocalLobbyCreated -= GameReady;
        lobby.LocalJoinStarted -= JoinStarted;
        lobby.LocalJoinFinished -= JoinFinished;
        lobby.RejoinAnswered -= Answered;
        attached = null;
    }

    // Whether the host's answers and what follows are taken from it (a stranger too: StrangerGate).
    public bool Related(ulong peer)
    {
        lock (gate) return peer == target.Host && stage is Stage.Asking or Stage.Invited or Stage.Joining;
    }

    // This machine's game created a lobby: it is on the ship.
    public void GameReady()
    {
        lock (gate)
        {
            if (stage != Stage.Waiting) return;
            (stage, due) = (Stage.Settling, clock() + Settle);
            log($"Rejoin: this game is on the ship; it asks {target.HostName} in {Settle.TotalSeconds:0} s.");
        }
    }

    public void Tick()
    {
        lock (gate)
        {
            var now = clock();
            if (now < due) return;
            switch (stage)
            {
                case Stage.Waiting:
                    GiveUp($"this game did not reach the ship within {ReadyTimeout.TotalMinutes:0} minutes.",
                        ["Your game took too long to reach the ship.", $"Join {target.HostName} from the social tab or the Galactic Map."]);
                    return;
                case Stage.Settling:
                    (stage, due) = (Stage.Asking, now + AnswerTimeout);
                    log($"Rejoin: asking {LobbyService.EntityId(target.Host)} whether this player may rejoin lobby {target.LobbyId}.");
                    send(target.Host, new Frame("lobby.rejoin").Add(target.LobbyId).Add(ownDigest).Add(LobbyService.Protocol));
                    return;
                case Stage.Asking:
                    GiveUp($"{LobbyService.EntityId(target.Host)} did not answer within {AnswerTimeout.TotalSeconds:0} s.",
                        [$"{target.HostName} did not answer.", "They may have closed the game or lost their connection."]);
                    return;
                case Stage.Invited:
                    stage = Stage.Done;
                    log($"Rejoin: the invitation into {target.HostName}'s lobby was not taken within {AcceptWindow.TotalMinutes:0} minutes; nothing more is tried.");
                    return;
            }
        }
    }

    // The host answered lobby.rejoin (LobbyService.RejoinAnswered).
    public void Answered(ulong peer, Frame frame)
    {
        lock (gate)
        {
            if (peer != target.Host || stage != Stage.Asking || frame.Text(0) != target.LobbyId) return;
            var status = frame.Text(1);
            if (status == "ok")
            {
                var (lobbyId, connection) = (frame.Text(2), frame.Text(3));
                var parts = connection.Split(';');
                if (!connection.StartsWith(LobbyService.ConnectionPrefix, StringComparison.Ordinal) || parts.Length != 3 || parts[1] != lobbyId || lobbyId.Length is 0 or > 100 ||
                    !LobbyService.TryNode(parts[2], out var owner) || owner != target.Host)
                {
                    GiveUp("the host's answer named another machine's lobby.", [$"{target.HostName}'s answer could not be used.", $"Join {target.HostName} from the social tab or the Galactic Map."]);
                    return;
                }
                (stage, due) = (Stage.Invited, clock() + AcceptWindow);
                log($"Rejoin: {target.HostName} lets this player back into lobby {lobbyId}" + (lobbyId != target.LobbyId ? " (their lobby now)" : "") + "; the game is invited.");
                invite(lobbyId, connection, LobbyService.EntityId(target.Host));
                notice("info", "Rejoining " + target.HostName, [$"{target.HostName}'s invitation is waiting in game: accept it to join their lobby with their mods."]);
                return;
            }
            var lines = Enumerable.Range(2, Math.Max(0, frame.Count - 2)).Take(6).Select(frame.Text).Where(l => l.Length is > 0 and <= 200).ToList();
            if (lines.Count == 0) lines.Add($"{target.HostName}'s lobby can't be joined now.");
            if (status is "gone" or "full" or "locked") lines.Add($"Join {target.HostName} from the social tab or the Galactic Map when you can.");
            GiveUp($"{LobbyService.EntityId(target.Host)} answered {status}.", lines);
        }
    }

    // This machine's game asked to join a friend's lobby (owner, lobby id).
    public void JoinStarted(ulong owner, string lobbyId)
    {
        lock (gate)
        {
            if (stage is Stage.Done or Stage.GaveUp or Stage.Joining) return;
            if (owner == target.Host)
            {
                log($"Rejoin: the game is joining {target.HostName}'s lobby {lobbyId}" + (stage == Stage.Invited ? " (the invitation was taken)." : " by itself."));
                (stage, due) = (Stage.Joining, DateTime.MaxValue);
            }
            else
            {
                log($"Rejoin: the game joins another lobby ({lobbyId}); this player is not taken back to {target.HostName}'s.");
                stage = Stage.Done;
            }
        }
    }

    // The answer to that join (an HRESULT; 0 is in).
    public void JoinFinished(ulong owner, string lobbyId, uint result)
    {
        lock (gate)
        {
            // The answer may come before the start: LobbyService runs each thread's after-lock work on that thread, so the
            // host's answer (the peer link's thread) can overtake the game's join (the pipe's thread). Then it starts here.
            if (owner == target.Host && stage is not (Stage.Joining or Stage.Done or Stage.GaveUp)) JoinStarted(owner, lobbyId);
            if (stage != Stage.Joining || owner != target.Host) return;
            stage = Stage.Done;
            log(result == 0 ? $"Rejoin: back in {target.HostName}'s lobby {lobbyId}." : $"Rejoin: joining {target.HostName}'s lobby {lobbyId} failed (0x{result:X8}); nothing more is tried.");
        }
    }

    // Called under the lock.
    void GiveUp(string reason, IReadOnlyList<string> lines)
    {
        stage = Stage.GaveUp;
        log("Rejoin: not rejoining, because " + reason);
        notice("error", "Couldn't rejoin " + target.HostName, lines);
    }
}
