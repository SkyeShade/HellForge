using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace HellForge.LocalBackend.Multiplayer;

// Backend-to-backend messages through Steam networking (ISteamNetworkingMessages over Valve's relays, peers named by
// SteamID). Runs in the backend process, which the network block does not list; the copied game itself only talks to
// loopback. The backend starts the Steam API as the game's app beside the running game (verified with a
// probe program, docs/multiplayer.md). Sessions are accepted from the signed-in account's Steam
// friends, and from someone else only when AcceptStranger says so (the lobby directory: a public lobby is listed, or this
// machine deals with them). Every Steam call happens on one thread, the lobby directory's too (SteamLobbyDirectory).
// Friends who run HellForge are found for the social tab by a rich presence key ("hellforge" = the multiplayer protocol) that the
// backend sets for the game's app beside the game's own keys: only friends who are playing HD2 are looked at, and nothing
// is sent to anyone to find them.
public sealed unsafe class SteamPeerTransport : IPeerTransport, IFriendSource
{
    const int AppId = 553850;
    const int Channel = 0;
    // k_nSteamNetworkingSend_Reliable / _Unreliable, each with _AutoRestartBrokenSession. Unreliable (not NoDelay) is
    // queued while a route is being set up: NoDelay dropped a joining player's first game messages (first two-PC test).
    const int SendReliable = 8 | 32, SendUnreliable = 0 | 32;
    const int SessionRequest = 1251, SessionFailed = 1252; // SteamNetworkingMessagesSessionRequest_t, ..SessionFailed_t
    const int CallCompleted = 703; // SteamAPICallCompleted_t
    const int IdentitySize = 136, IdentitySteamId = 16; // SteamNetworkingIdentity, k_ESteamNetworkingIdentityType_SteamID
    const int FriendRelationshipFriend = 3;
    const int FriendFlagImmediate = 4;
    const string PresenceKey = "hellforge";
    static readonly TimeSpan FriendScanInterval = TimeSpan.FromSeconds(5);
    // Steam networking estimates no bandwidth: a connection sends at SendRateMin..Max, 256 KiB/s each by default
    // (GameNetworkingSockets' steamnetworkingtypes.h and csteamnetworkingsockets.cpp), which held the first two-PC mod
    // sync to 0.24 MB/s. Min = Max: with Min below Max a connection starts at about four packets a round trip and stays
    // there or at Min. SendRate is a ceiling, not a load: game traffic is far below it, and mod sync's own window keeps
    // its queue short (DownloadWindow). The buffers hold a download's whole window (ModSync.MaxWindow chunks, 2 MiB) with
    // room to spare: a message past the send buffer is refused (k_EResultLimitExceeded), and received messages past the
    // receive buffer are dropped. Docs: multiplayer.md "Mod sync speed".
    public const int SendRate = 8 << 20, SendBuffer = 8 << 20, ReceiveBuffer = 8 << 20;
    const int ConfigSendBufferSize = 9, ConfigSendRateMin = 10, ConfigSendRateMax = 11, ConfigRecvBufferSize = 47; // ESteamNetworkingConfigValue
    const int ResultOk = 1, ResultLimitExceeded = 25; // EResult

    readonly string library;
    readonly Action<string> log;
    readonly ConcurrentQueue<(ulong Peer, byte[] Frame, bool Reliable)> outbox = new();
    // Set by Send: the link's thread sends at once instead of at its next turn (a turn waits up to a timer tick, 15.6 ms).
    readonly AutoResetEvent wake = new(false);
    readonly ManualResetEventSlim started = new();
    readonly CancellationTokenSource stop = new();
    Thread? thread;
    string? failure;
    public ulong LocalId { get; private set; }
    public event Action<ulong, byte[]>? Received;
    volatile IReadOnlyList<FriendPresence.Friend> hellforgeFriends = [];
    public IReadOnlyList<FriendPresence.Friend> HellForgeFriends => TreatFriendsAsStrangers ? [] : hellforgeFriends;
    // Test mode (HELLFORGE_TEST_STRANGERS=1, MultiplayerHost): Steam friends are treated as strangers, so two friends' PCs
    // test what strangers go through (the directory's gate, no presence). Steam itself is unchanged.
    public bool TreatFriendsAsStrangers { get; init; }
    // Every Steam friend's SteamID, refreshed with the friend scan.
    volatile HashSet<ulong> friendIds = [];
    public bool IsFriend(ulong peer) => !TreatFriendsAsStrangers && friendIds.Contains(peer);
    // Whether a session from someone who is not a Steam friend is accepted (asked on the Steam thread; must be quick).
    public Func<ulong, bool>? AcceptStranger { get; set; }
    public SteamLobbyDirectory Directory { get; }
    DateTime strangerLogged = DateTime.MinValue, sendFailureLogged = DateTime.MinValue;

    public SteamPeerTransport(string steamApiLibrary, Action<string> log)
    {
        library = Path.GetFullPath(steamApiLibrary);
        this.log = log;
        Directory = new SteamLobbyDirectory(log);
    }

    public void Start()
    {
        thread = new Thread(Run) { IsBackground = true, Name = "HellForge Steam peer link" };
        thread.Start();
        started.Wait();
        if (failure != null) throw new IOException(failure);
    }

    public void Send(ulong peer, byte[] frame, bool reliable)
    {
        outbox.Enqueue((peer, frame, reliable));
        wake.Set();
    }

    void Run()
    {
        nint module = 0;
        var initialized = false;
        try
        {
            Environment.SetEnvironmentVariable("SteamAppId", AppId.ToString());
            Environment.SetEnvironmentVariable("SteamGameId", AppId.ToString());
            module = NativeLibrary.Load(library);
            nint E(string name) => NativeLibrary.GetExport(module, name);
            initialized = ((delegate* unmanaged[Cdecl]<byte>)E("SteamAPI_Init"))() != 0;
            if (!initialized) throw new IOException("SteamAPI_Init failed: is Steam running and signed in?");
            var pipe = ((delegate* unmanaged[Cdecl]<int>)E("SteamAPI_GetHSteamPipe"))();
            ((delegate* unmanaged[Cdecl]<void>)E("SteamAPI_ManualDispatch_Init"))();
            var runFrame = (delegate* unmanaged[Cdecl]<int, void>)E("SteamAPI_ManualDispatch_RunFrame");
            var nextCallback = (delegate* unmanaged[Cdecl]<int, byte*, byte>)E("SteamAPI_ManualDispatch_GetNextCallback");
            var freeCallback = (delegate* unmanaged[Cdecl]<int, void>)E("SteamAPI_ManualDispatch_FreeLastCallback");
            var user = ((delegate* unmanaged[Cdecl]<nint>)E("SteamAPI_SteamUser_v021"))();
            var friends = ((delegate* unmanaged[Cdecl]<nint>)E("SteamAPI_SteamFriends_v017"))();
            var utils = ((delegate* unmanaged[Cdecl]<nint>)E("SteamAPI_SteamNetworkingUtils_SteamAPI_v004"))();
            var messages = ((delegate* unmanaged[Cdecl]<nint>)E("SteamAPI_SteamNetworkingMessages_SteamAPI_v002"))();
            if (user == 0 || friends == 0 || utils == 0 || messages == 0) throw new IOException("Steam networking interfaces are unavailable.");
            LocalId = ((delegate* unmanaged[Cdecl]<nint, ulong>)E("SteamAPI_ISteamUser_GetSteamID"))(user);
            var relationship = (delegate* unmanaged[Cdecl]<nint, ulong, int>)E("SteamAPI_ISteamFriends_GetFriendRelationship");
            var setSteamId = (delegate* unmanaged[Cdecl]<byte*, ulong, void>)E("SteamAPI_SteamNetworkingIdentity_SetSteamID64");
            var send = (delegate* unmanaged[Cdecl]<nint, byte*, byte*, uint, int, int, int>)E("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
            var receive = (delegate* unmanaged[Cdecl]<nint, int, nint*, int, int>)E("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
            var accept = (delegate* unmanaged[Cdecl]<nint, byte*, byte>)E("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
            var release = (delegate* unmanaged[Cdecl]<nint, void>)E("SteamAPI_SteamNetworkingMessage_t_Release");
            // Global: every session this process opens takes them (before any is opened, so none keeps Steam's defaults).
            var setGlobal = (delegate* unmanaged[Cdecl]<nint, int, int, byte>)E("SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
            var configured = setGlobal(utils, ConfigSendRateMin, SendRate) != 0 & setGlobal(utils, ConfigSendRateMax, SendRate) != 0 &
                setGlobal(utils, ConfigSendBufferSize, SendBuffer) != 0 & setGlobal(utils, ConfigRecvBufferSize, ReceiveBuffer) != 0;
            log(configured ? $"Steam link: send rate {SendRate >> 20} MiB/s, send and receive buffers {SendBuffer >> 20} MiB."
                : "Steam link: Steam did not take the link's send rate and buffers; mod downloads stay at Steam's default 256 KiB/s.");
            ((delegate* unmanaged[Cdecl]<nint, void>)E("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"))(utils);
            var setPresence = (delegate* unmanaged[Cdecl]<nint, byte*, byte*, byte>)E("SteamAPI_ISteamFriends_SetRichPresence");
            var friendCount = (delegate* unmanaged[Cdecl]<nint, int, int>)E("SteamAPI_ISteamFriends_GetFriendCount");
            var friendByIndex = (delegate* unmanaged[Cdecl]<nint, int, int, ulong>)E("SteamAPI_ISteamFriends_GetFriendByIndex");
            var gamePlayed = (delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte>)E("SteamAPI_ISteamFriends_GetFriendGamePlayed");
            var requestPresence = (delegate* unmanaged[Cdecl]<nint, ulong, void>)E("SteamAPI_ISteamFriends_RequestFriendRichPresence");
            var friendPresence = (delegate* unmanaged[Cdecl]<nint, ulong, byte*, byte*>)E("SteamAPI_ISteamFriends_GetFriendRichPresence");
            var personaName = (delegate* unmanaged[Cdecl]<nint, ulong, byte*>)E("SteamAPI_ISteamFriends_GetFriendPersonaName");
            Directory.Bind(E, pipe, LocalId);
            log("Steam peer link started.");
            started.Set();

            var key = System.Text.Encoding.UTF8.GetBytes(PresenceKey + "\0");
            var tag = System.Text.Encoding.UTF8.GetBytes(LobbyService.Protocol + "\0");
            var gameInfo = stackalloc byte[32]; // FriendGameInfo_t: CGameID, IP, ports, lobby
            var requested = new Dictionary<ulong, DateTime>();
            var lastScan = DateTime.MinValue;
            // Lists Steam friends playing HD2 whose backend set the tag; re-sets this machine's tag, which the game's own
            // rich presence calls may clear.
            void ScanFriends()
            {
                fixed (byte* k = key, v = tag) setPresence(friends, k, v);
                var found = new List<FriendPresence.Friend>();
                var all = new HashSet<ulong>();
                var count = friendCount(friends, FriendFlagImmediate);
                for (var i = 0; i < count; i++)
                {
                    var friend = friendByIndex(friends, i, FriendFlagImmediate);
                    all.Add(friend);
                    new Span<byte>(gameInfo, 32).Clear();
                    if (gamePlayed(friends, friend, gameInfo) == 0 || (*(ulong*)gameInfo & 0xFFFFFF) != AppId) continue;
                    if (!requested.TryGetValue(friend, out var asked) || DateTime.UtcNow - asked > TimeSpan.FromMinutes(1))
                    {
                        requestPresence(friends, friend);
                        requested[friend] = DateTime.UtcNow;
                    }
                    byte* value;
                    fixed (byte* k = key) value = friendPresence(friends, friend, k);
                    if (value == null || *value == 0) continue;
                    found.Add(new FriendPresence.Friend(friend, Marshal.PtrToStringUTF8((nint)personaName(friends, friend)) ?? "Steam friend"));
                }
                hellforgeFriends = found;
                friendIds = all;
            }

            var callback = stackalloc byte[24]; // CallbackMsg_t
            var identity = stackalloc byte[IdentitySize];
            var inbox = stackalloc nint[32];
            while (!stop.IsCancellationRequested)
            {
                runFrame(pipe);
                while (nextCallback(pipe, callback) != 0)
                {
                    var id = *(int*)(callback + 4);
                    var parameter = *(byte**)(callback + 8);
                    if (id is SessionRequest or SessionFailed && *(int*)parameter == IdentitySteamId)
                    {
                        var remote = *(ulong*)(parameter + 8);
                        if (id == SessionFailed) log("A Steam peer session failed.");
                        else if (!TreatFriendsAsStrangers && relationship(friends, remote) == FriendRelationshipFriend || AcceptStranger?.Invoke(remote) == true)
                            accept(messages, parameter);
                        else if (DateTime.UtcNow - strangerLogged > TimeSpan.FromMinutes(1))
                        {
                            strangerLogged = DateTime.UtcNow;
                            log("Refused a Steam peer session from someone who is not a Steam friend (no public lobby listed here).");
                        }
                    }
                    else if (id == CallCompleted) Directory.CallCompleted(parameter);
                    freeCallback(pipe);
                }
                var busy = false;
                while (outbox.TryDequeue(out var outgoing))
                {
                    busy = true;
                    new Span<byte>(identity, IdentitySize).Clear();
                    setSteamId(identity, outgoing.Peer);
                    int result;
                    fixed (byte* data = outgoing.Frame)
                        result = send(messages, identity, data, (uint)outgoing.Frame.Length, outgoing.Reliable ? SendReliable : SendUnreliable, Channel);
                    if (result != ResultOk && DateTime.UtcNow - sendFailureLogged > TimeSpan.FromMinutes(1))
                    {
                        sendFailureLogged = DateTime.UtcNow;
                        log($"Steam did not take a message to {LobbyService.EntityId(outgoing.Peer)} ({outgoing.Frame.Length} bytes): EResult {result}" +
                            (result == ResultLimitExceeded ? ", its send buffer is full." : "."));
                    }
                }
                int count;
                while ((count = receive(messages, Channel, inbox, 32)) > 0)
                    for (var i = 0; i < count; i++)
                    {
                        busy = true;
                        var message = inbox[i];
                        var peerIdentity = (byte*)message + 16; // SteamNetworkingMessage_t.m_identityPeer
                        if (*(int*)peerIdentity == IdentitySteamId)
                        {
                            var sender = *(ulong*)(peerIdentity + 8);
                            var frame = new ReadOnlySpan<byte>(*(byte**)message, *(int*)(message + 8)).ToArray();
                            try { Received?.Invoke(sender, frame); }
                            catch (Exception error) { log("Peer message handler failed: " + error.Message); }
                        }
                        release(message);
                    }
                Directory.Pump();
                if (DateTime.UtcNow - lastScan > FriendScanInterval)
                {
                    lastScan = DateTime.UtcNow;
                    ScanFriends();
                }
                // While messages flow, the next turn comes at once (a reply's handler may have queued a send already);
                // otherwise at the next send or after a short wait, for what arrives.
                if (!busy) wake.WaitOne(2);
            }
            Directory.Shutdown();
            // The tag goes when the backend does (an empty value deletes the key, leaving the game's own keys alone).
            fixed (byte* k = key, v = "\0"u8) setPresence(friends, k, v);
        }
        catch (Exception error) when (error is IOException or DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            failure = error.Message;
            started.Set();
        }
        finally
        {
            if (initialized) ((delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(module, "SteamAPI_Shutdown"))();
            if (module != 0) NativeLibrary.Free(module);
        }
    }

    public void Dispose()
    {
        stop.Cancel();
        thread?.Join(TimeSpan.FromSeconds(5));
    }
}
