using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;

namespace HellForge.SdkShimGen;

// Two players on one PC: two real backend processes linked over loopback UDP (each with its own named pipe), and two
// copies of each stand-in, each connected to one backend. The "host" copies create a lobby and a Party network through
// the PlayFab C APIs; the "friend" copies join the lobby with its connection string, update their member data, connect
// to the network with its descriptor, create an endpoint, and exchange messages with the host, exactly as two games on
// two machines would, except that Steam networking is replaced by loopback UDP. Then the host invites the friend (who
// listens for invitations) and kicks it: the kicked game is removed and disconnected, and its join back is refused. Last,
// the friend comes back as after a mod sync: a new backend given --rejoin and a new stand-in copy, whose game is invited
// back into the host's new lobby once it has one of its own, and joins.
public static unsafe class LobbyLinkSmoke
{
    const string HostId = "110000100000001", FriendId = "110000100000002";

    [StructLayout(LayoutKind.Sequential)] internal struct EntityKey { public byte* Id; public byte* Type; }
    [StructLayout(LayoutKind.Sequential)] internal struct CreateConfiguration
    {
        public uint MaxMemberCount, OwnerMigrationPolicy, AccessPolicy, SearchPropertyCount;
        public byte** SearchKeys, SearchValues;
        public uint LobbyPropertyCount;
        public byte** LobbyKeys, LobbyValues;
    }
    [StructLayout(LayoutKind.Sequential)] internal struct JoinConfiguration { public uint Count; public byte** Keys, Values; }

    internal sealed class Player
    {
        public string Name = "";
        public nint Handle;
        public delegate* unmanaged<nint, uint*, nint*, int> Start;
        public delegate* unmanaged<nint, uint, nint, int> Finish;
        public readonly List<string> Seen = [];
        // The connection strings of the invitations received (InviteReceived).
        public readonly List<string> Invitations = [];

        // Delivers state changes until one matches (type, or type:hresult for completions), for at most five seconds.
        public bool WaitFor(string wanted, int milliseconds = 5000)
        {
            var clock = Stopwatch.StartNew();
            while (clock.ElapsedMilliseconds < milliseconds)
            {
                uint count;
                nint changes;
                if (Start(Handle, &count, &changes) != 0) return false;
                for (var i = 0; i < count; i++)
                {
                    var change = (uint*)((nint*)changes)[i];
                    Seen.Add(change[0] is 0 or 1 or 5 or 8 or 12 or 15 ? $"{change[0]}:{change[1]:x}" : $"{change[0]}");
                    // PFLobbyInviteReceivedStateChange: the connection string after the listening and inviting entities
                    if (change[0] == 13) Invitations.Add(Marshal.PtrToStringUTF8(*(nint*)((byte*)change + 40)) ?? "");
                }
                Finish(Handle, count, changes);
                var at = Seen.IndexOf(wanted);
                if (at >= 0) { Seen.RemoveRange(0, at + 1); return true; }
                Thread.Sleep(10);
            }
            return false;
        }
    }

    public static int Run(string standIns, string backend)
    {
        var root = Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "hellforge-lobby-link-" + Guid.NewGuid().ToString("N"))).FullName;
        var problems = new List<string>();
        void Expect(bool condition, string description) { if (!condition) problems.Add(description); }
        Environment.SetEnvironmentVariable("HELLFORGE_SHIM_LOG_DIR", Path.Combine(root, "logs"));
        var ports = (FreePort(), FreePort());
        var pipes = ($"hellforge-link-host-{Guid.NewGuid():N}", $"hellforge-link-friend-{Guid.NewGuid():N}");
        var processes = new List<Process>();
        try
        {
            processes.Add(StartBackend(backend, Path.Combine(root, "host"), pipes.Item1, $"udp:{HostId}:{ports.Item1}:{FriendId}={ports.Item2}"));
            processes.Add(StartBackend(backend, Path.Combine(root, "friend"), pipes.Item2, $"udp:{FriendId}:{ports.Item2}:{HostId}={ports.Item1}"));
            foreach (var directory in new[] { "host", "friend" })
            {
                var ready = Path.Combine(root, directory, "ready.json");
                var clock = Stopwatch.StartNew();
                while (!File.Exists(ready) && clock.ElapsedMilliseconds < 20000) Thread.Sleep(50);
                Expect(File.Exists(ready), $"The {directory} backend starts");
            }
            if (problems.Count > 0) return Report(problems, root);

            // Two separate copies of the stand-in, so each has its own state, as in two games.
            var host = Load(Path.Combine(standIns, "PlayFabMultiplayerWin.dll"), Path.Combine(root, "lobby-host.dll"), pipes.Item1, "host");
            var friend = Load(Path.Combine(standIns, "PlayFabMultiplayerWin.dll"), Path.Combine(root, "lobby-friend.dll"), pipes.Item2, "friend");
            var hostLibrary = host.Library;
            var friendLibrary = friend.Library;
            var create = (delegate* unmanaged<nint, EntityKey*, CreateConfiguration*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(hostLibrary, "PFMultiplayerCreateAndJoinLobby");
            var connection = (delegate* unmanaged<nint, nint*, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbyGetConnectionString");
            var hostMembers = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbyGetMembers");
            var hostMemberProperty = (delegate* unmanaged<nint, EntityKey*, byte*, nint*, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbyGetMemberProperty");
            var hostLeave = (delegate* unmanaged<nint, EntityKey*, nint, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbyLeave");
            var join = (delegate* unmanaged<nint, EntityKey*, byte*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(friendLibrary, "PFMultiplayerJoinLobby");
            var friendMembers = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(friendLibrary, "PFLobbyGetMembers");
            var friendProperty = (delegate* unmanaged<nint, byte*, nint*, int>)NativeLibrary.GetExport(friendLibrary, "PFLobbyGetLobbyProperty");
            var friendOwner = (delegate* unmanaged<nint, EntityKey**, int>)NativeLibrary.GetExport(friendLibrary, "PFLobbyGetOwner");
            var friendUpdate = (delegate* unmanaged<nint, EntityKey*, nint, JoinConfiguration*, nint, int>)NativeLibrary.GetExport(friendLibrary, "PFLobbyPostUpdate");
            var friendLeave = (delegate* unmanaged<nint, EntityKey*, nint, int>)NativeLibrary.GetExport(friendLibrary, "PFLobbyLeave");
            var invite = (delegate* unmanaged<nint, EntityKey*, EntityKey*, nint, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbySendInvite");
            var kick = (delegate* unmanaged<nint, EntityKey*, byte, nint, int>)NativeLibrary.GetExport(hostLibrary, "PFLobbyForceRemoveMember");
            var listen = (delegate* unmanaged<nint, EntityKey*, int>)NativeLibrary.GetExport(friendLibrary, "PFMultiplayerStartListeningForLobbyInvites");

            fixed (byte* hostId = Encoding.ASCII.GetBytes(HostId + "\0"), friendId = Encoding.ASCII.GetBytes(FriendId + "\0"),
                type = "title_player_account\0"u8, k1 = "1\0"u8, v1 = "f8c9037f96047f7a\0"u8, platform = "platform_lobby\0"u8,
                hostPlatform = "2:109775245170738417::\0"u8, friendPlatform = "2:109775245170738418::\0"u8)
            {
                var hostKey = new EntityKey { Id = hostId, Type = type };
                var friendKey = new EntityKey { Id = friendId, Type = type };
                var lobbyKeys = stackalloc byte*[] { k1 };
                var lobbyValues = stackalloc byte*[] { v1 };
                var memberKeys = stackalloc byte*[] { platform };
                var hostMemberValues = stackalloc byte*[] { hostPlatform };
                var configuration = new CreateConfiguration { MaxMemberCount = 4, LobbyPropertyCount = 1, LobbyKeys = lobbyKeys, LobbyValues = lobbyValues };
                var hostJoin = new JoinConfiguration { Count = 1, Keys = memberKeys, Values = hostMemberValues };
                nint hostLobby = 0, friendLobby = 0, text = 0;
                Expect(create(host.Player.Handle, &hostKey, &configuration, &hostJoin, 0x11, &hostLobby) == 0 && hostLobby != 0, "The host creates a lobby");
                Expect(host.Player.WaitFor("0:0"), "The host's lobby is created by its backend");
                Expect(connection(hostLobby, &text) == 0 && Marshal.PtrToStringUTF8(text)?.EndsWith(";" + HostId) == true, "The connection string names the host");
                var connectionString = Marshal.PtrToStringUTF8(text) ?? "";

                var friendJoin = new JoinConfiguration { Count = 0 };
                fixed (byte* connectionBytes = Encoding.UTF8.GetBytes(connectionString + "\0"))
                    Expect(join(friend.Player.Handle, &friendKey, connectionBytes, &friendJoin, 0x22, &friendLobby) == 0 && friendLobby != 0, "The friend starts joining");
                Expect(friend.Player.WaitFor("1:0"), "The friend joins the host's lobby through both backends");
                uint count = 0;
                nint list = 0;
                Expect(friendMembers(friendLobby, &count, &list) == 0 && count == 2, "The friend sees two members");
                EntityKey* owner = null;
                Expect(friendOwner(friendLobby, &owner) == 0 && owner != null && Marshal.PtrToStringUTF8((nint)owner->Id) == HostId, "The friend sees the host as owner");
                Expect(friendProperty(friendLobby, k1, &text) == 0 && Marshal.PtrToStringUTF8(text) == "f8c9037f96047f7a", "The friend reads the host's lobby properties");
                Expect(host.Player.WaitFor("2"), "The host sees the friend join");
                Expect(hostMembers(hostLobby, &count, &list) == 0 && count == 2, "The host sees two members");
                // each backend lists the other as a co-op peer for a sandboxed game's Steam P2P (MultiplayerHost.PeersFileName)
                static bool Lists(string file, string hexId)
                {
                    for (var i = 0; i < 40; i++)
                    {
                        try
                        {
                            if (File.Exists(file) && File.ReadAllLines(file).Contains(ulong.Parse(hexId, NumberStyles.HexNumber).ToString(CultureInfo.InvariantCulture))) return true;
                        }
                        catch (IOException) { }
                        Thread.Sleep(100);
                    }
                    return false;
                }
                Expect(Lists(Path.Combine(root, "host", "p2p-peers.txt"), FriendId) && Lists(Path.Combine(root, "friend", "p2p-peers.txt"), HostId),
                    "Each backend lists the other among its co-op peers for Steam P2P");

                var friendValues = stackalloc byte*[] { friendPlatform };
                var memberUpdate = new JoinConfiguration { Count = 1, Keys = memberKeys, Values = friendValues };
                Expect(friendUpdate(friendLobby, &friendKey, 0, &memberUpdate, 0x33) == 0, "The friend posts its member data");
                Expect(friend.Player.WaitFor("8:0") && friend.Player.WaitFor("7"), "The friend's update completes and is applied");
                Expect(host.Player.WaitFor("7"), "The host receives the friend's update");
                Expect(hostMemberProperty(hostLobby, &friendKey, platform, &text) == 0 && Marshal.PtrToStringUTF8(text) == "2:109775245170738418::",
                    "The host reads the friend's platform lobby");

                PartyFlow(standIns, root, pipes, Expect);
                KeyFlow(root, Expect);
                PresenceFlow(root, connectionString, Expect);

                // An invitation: the host's game invites the friend, whose listening game hears it through both backends.
                Expect(listen(friend.Player.Handle, &friendKey) == 0 && friend.Player.WaitFor("14"), "The friend listens for invitations");
                Expect(invite(hostLobby, &hostKey, &friendKey, 0x66) == 0 && host.Player.WaitFor("15:0"), "The host's invitation is taken");
                Expect(friend.Player.WaitFor("13"), "The friend's game receives the invitation");

                // The kick (PFLobbyForceRemoveMember): MemberRemoved then the completion for the host; for the kicked game
                // its member removed, then Disconnecting and Disconnected; a join back is refused as not joinable.
                Expect(kick(hostLobby, &friendKey, 1, 0x77) == 0, "The host kicks the friend");
                Expect(host.Player.WaitFor("4") && host.Player.WaitFor("5:0") && hostMembers(hostLobby, &count, &list) == 0 && count == 1,
                    "The host sees the friend removed, then the kick complete");
                Expect(friend.Player.WaitFor("4") && friend.Player.WaitFor("9") && friend.Player.WaitFor("10"), "The kicked friend is removed and disconnected");
                nint rejoined = 0;
                fixed (byte* connectionBytes = Encoding.UTF8.GetBytes(connectionString + "\0"))
                    Expect(join(friend.Player.Handle, &friendKey, connectionBytes, &friendJoin, 0x88, &rejoined) == 0 && friend.Player.WaitFor("1:89236227"),
                        "The kicked friend cannot join again");
                Expect(hostMembers(hostLobby, &count, &list) == 0 && count == 1, "The host's lobby keeps one member");
                _ = friendLeave;
                Expect(hostLeave(hostLobby, &hostKey, 0x55) == 0 && host.Player.WaitFor("10"), "The host leaves its lobby");

                // Rejoining after a mod sync: the friend's next session (a new backend on the friend's port, given the sync
                // file naming the host's new lobby, and a new copy of the stand-in) is invited back once its game has a
                // lobby of its own, and joins with the invitation's connection string.
                nint secondLobby = 0, ownLobby = 0, back = 0;
                Expect(create(host.Player.Handle, &hostKey, &configuration, &hostJoin, 0x91, &secondLobby) == 0 && host.Player.WaitFor("0:0"), "The host opens a new lobby");
                Expect(connection(secondLobby, &text) == 0, "The new lobby has a connection string");
                var secondConnection = Marshal.PtrToStringUTF8(text) ?? "";
                processes[1].Kill(true);
                processes[1].WaitForExit(10000);
                var syncFile = Path.Combine(root, "mod-sync", "session.json");
                new HellForge.Core.ModSyncSession(HostId, "Host", secondConnection, "smoke", []).Write(syncFile);
                var againPipe = $"hellforge-link-again-{Guid.NewGuid():N}";
                processes.Add(StartBackend(backend, Path.Combine(root, "friend-again"), againPipe, $"udp:{FriendId}:{ports.Item2}:{HostId}={ports.Item1}", ["--rejoin", syncFile]));
                var againReady = Path.Combine(root, "friend-again", "ready.json");
                var readyClock = Stopwatch.StartNew();
                while (!File.Exists(againReady) && readyClock.ElapsedMilliseconds < 20000) Thread.Sleep(50);
                Expect(File.Exists(againReady), "The friend's next session's backend starts with --rejoin");
                var again = Load(Path.Combine(standIns, "PlayFabMultiplayerWin.dll"), Path.Combine(root, "lobby-friend-again.dll"), againPipe, "friend again");
                var againCreate = (delegate* unmanaged<nint, EntityKey*, CreateConfiguration*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(again.Library, "PFMultiplayerCreateAndJoinLobby");
                var againListen = (delegate* unmanaged<nint, EntityKey*, int>)NativeLibrary.GetExport(again.Library, "PFMultiplayerStartListeningForLobbyInvites");
                var againJoin = (delegate* unmanaged<nint, EntityKey*, byte*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(again.Library, "PFMultiplayerJoinLobby");
                var againMembers = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(again.Library, "PFLobbyGetMembers");
                Expect(againListen(again.Player.Handle, &friendKey) == 0 && again.Player.WaitFor("14"), "The friend's next game listens for invitations from the start");
                var friendOwnJoin = new JoinConfiguration { Count = 0 };
                Expect(againCreate(again.Player.Handle, &friendKey, &configuration, &friendOwnJoin, 0x92, &ownLobby) == 0 && again.Player.WaitFor("0:0"),
                    "The friend's next game reaches the ship (its own lobby)");
                Expect(again.Player.WaitFor("13", 20000) && again.Player.Invitations.LastOrDefault() == secondConnection,
                    "The friend's next game receives the host's invitation into its new lobby");
                fixed (byte* connectionBytes = Encoding.UTF8.GetBytes(secondConnection + "\0"))
                    Expect(againJoin(again.Player.Handle, &friendKey, connectionBytes, &friendJoin, 0x93, &back) == 0 && again.Player.WaitFor("1:0"),
                        "The friend's next game takes the invitation and joins");
                Expect(againMembers(back, &count, &list) == 0 && count == 2 && host.Player.WaitFor("2") && hostMembers(secondLobby, &count, &list) == 0 && count == 2,
                    "Both games see the friend back in the host's lobby");
                // the backend keeps its log open
                string FriendLog()
                {
                    using var stream = new FileStream(Path.Combine(root, "friend-again", "multiplayer.log"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    return new StreamReader(stream).ReadToEnd();
                }
                var logClock = Stopwatch.StartNew();
                while (!FriendLog().Contains("Rejoin: back in Host's lobby", StringComparison.Ordinal) && logClock.ElapsedMilliseconds < 5000) Thread.Sleep(50);
                Expect(FriendLog().Contains("Rejoin: back in Host's lobby", StringComparison.Ordinal), "The friend's backend logs the rejoin");
            }
            return Report(problems, root);
        }
        finally
        {
            foreach (var process in processes)
            {
                try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { }
                process.Dispose();
            }
        }
    }

    [StructLayout(LayoutKind.Sequential)] struct DataBuffer { public byte* Buffer; public uint Size; }

    // Delivers Party state changes until one matches ("type" or "type:result" for completions), keeping message bytes.
    sealed class PartyPlayer
    {
        public nint Library, Handle;
        public delegate* unmanaged<nint, uint*, nint*, uint> Start;
        public delegate* unmanaged<nint, uint, nint, uint> Finish;
        public readonly List<string> Seen = [];
        public readonly List<byte[]> Messages = [];
        static readonly HashSet<uint> Completions = [1, 2, 3, 4, 10, 11, 19];

        public bool WaitFor(string wanted)
        {
            var clock = Stopwatch.StartNew();
            while (clock.ElapsedMilliseconds < 5000)
            {
                uint count;
                nint changes;
                if (Start(Handle, &count, &changes) != 0) return false;
                for (var i = 0; i < count; i++)
                {
                    var change = (byte*)((nint*)changes)[i];
                    var type = *(uint*)change;
                    Seen.Add(Completions.Contains(type) ? $"{type}:{*(uint*)(change + 4)}" : $"{type}");
                    if (type == 21) Messages.Add(new ReadOnlySpan<byte>(*(byte**)(change + 48), *(int*)(change + 44)).ToArray());
                }
                Finish(Handle, count, changes);
                var at = Seen.IndexOf(wanted);
                if (at >= 0) { Seen.RemoveRange(0, at + 1); return true; }
                Thread.Sleep(10);
            }
            return false;
        }
    }

    static PartyPlayer LoadParty(string original, string copy, string pipe)
    {
        File.Copy(original, copy, overwrite: true);
        Environment.SetEnvironmentVariable("HELLFORGE_PIPE", pipe);
        var library = NativeLibrary.Load(copy);
        var initialize = (delegate* unmanaged<byte*, nint*, uint>)NativeLibrary.GetExport(library, "PartyInitialize");
        var player = new PartyPlayer
        {
            Library = library,
            Start = (delegate* unmanaged<nint, uint*, nint*, uint>)NativeLibrary.GetExport(library, "PartyStartProcessingStateChanges"),
            Finish = (delegate* unmanaged<nint, uint, nint, uint>)NativeLibrary.GetExport(library, "PartyFinishProcessingStateChanges")
        };
        nint handle = 0;
        fixed (byte* title = "974A9\0"u8) initialize(title, &handle);
        player.Handle = handle;
        return player;
    }

    // The Party network between the two machines: the host creates and joins it, the friend connects with its serialized
    // descriptor, each creates an endpoint, and messages travel both ways through the backends.
    static void PartyFlow(string standIns, string root, (string Host, string Friend) pipes, Action<bool, string> expect)
    {
        var host = LoadParty(Path.Combine(standIns, "PartyWin.dll"), Path.Combine(root, "party-host.dll"), pipes.Host);
        var friend = LoadParty(Path.Combine(standIns, "PartyWin.dll"), Path.Combine(root, "party-friend.dll"), pipes.Friend);
        nint E(PartyPlayer player, string name) => NativeLibrary.GetExport(player.Library, name);
        var createUser = (delegate* unmanaged<nint, byte*, byte*, nint*, uint>)E(host, "PartyCreateLocalUser");
        var createNetwork = (delegate* unmanaged<nint, nint, uint*, uint, nint, nint, nint, byte*, byte*, uint>)E(host, "PartyCreateNewNetwork");
        var serialize = (delegate* unmanaged<byte*, byte*, uint>)E(host, "PartySerializeNetworkDescriptor");
        var friendCreateUser = (delegate* unmanaged<nint, byte*, byte*, nint*, uint>)E(friend, "PartyCreateLocalUser");
        var deserialize = (delegate* unmanaged<byte*, byte*, uint>)E(friend, "PartyDeserializeNetworkDescriptor");

        nint hostUser = 0, friendUser = 0, hostNetwork = 0, friendNetwork = 0, hostEndpoint = 0, friendEndpoint = 0;
        var configuration = stackalloc uint[6] { 4, 4, 1, 1, 8, 0 };
        var descriptor = stackalloc byte[512];
        var invitation = stackalloc byte[128];
        var serialized = stackalloc byte[449];
        var parsed = stackalloc byte[512];
        fixed (byte* hostId = Encoding.ASCII.GetBytes(HostId + "\0"), friendId = Encoding.ASCII.GetBytes(FriendId + "\0"), token = "hellforge-token\0"u8)
        {
            expect(createUser(host.Handle, hostId, token, &hostUser) == 0 && friendCreateUser(friend.Handle, friendId, token, &friendUser) == 0, "Party local users are created");
            expect(createNetwork(host.Handle, hostUser, configuration, 0, 0, 0, 0x101, descriptor, invitation) == 0, "The host creates a Party network");
            Join(host, E(host, "PartyConnectToNetwork"), E(host, "PartyNetworkAuthenticateLocalUser"), E(host, "PartyNetworkCreateEndpoint"), descriptor, hostUser, invitation,
                &hostNetwork, &hostEndpoint);
            expect(host.WaitFor("10:0"), "The host's network connects and its endpoint is created");
            expect(serialize(descriptor, serialized) == 0 && deserialize(serialized, parsed) == 0, "The descriptor travels as text");
            Join(friend, E(friend, "PartyConnectToNetwork"), E(friend, "PartyNetworkAuthenticateLocalUser"), E(friend, "PartyNetworkCreateEndpoint"), parsed, friendUser, invitation,
                &friendNetwork, &friendEndpoint);
            expect(friend.WaitFor("3:0"), "The friend connects to the host's network through both backends");
            expect(friend.WaitFor("16") && friend.WaitFor("12"), "The friend sees the host's machine and endpoint");
            expect(friend.WaitFor("10:0"), "The friend's endpoint is created with an id from the host");
            expect(host.WaitFor("16") && host.WaitFor("12"), "The host sees the friend's machine and endpoint");

            var getEndpoints = (delegate* unmanaged<nint, uint*, nint**, uint>)E(friend, "PartyNetworkGetEndpoints");
            var isLocal = (delegate* unmanaged<nint, byte*, uint>)E(friend, "PartyEndpointIsLocal");
            uint count = 0;
            nint* endpoints = null;
            nint hostSeenByFriend = 0;
            expect(getEndpoints(friendNetwork, &count, &endpoints) == 0 && count == 2, "The friend's network lists both endpoints");
            for (var i = 0; i < count; i++)
            {
                byte local = 1;
                if (isLocal(endpoints[i], &local) == 0 && local == 0) hostSeenByFriend = endpoints[i];
            }
            expect(hostSeenByFriend != 0, "The host's endpoint is remote for the friend");

            var payload = Enumerable.Range(0, 1200).Select(i => (byte)(i * 7)).ToArray();
            var friendSend = (delegate* unmanaged<nint, uint, nint*, uint, nint, uint, DataBuffer*, nint, uint>)E(friend, "PartyEndpointSendMessage");
            var hostSend = (delegate* unmanaged<nint, uint, nint*, uint, nint, uint, DataBuffer*, nint, uint>)E(host, "PartyEndpointSendMessage");
            fixed (byte* data = payload)
            {
                var buffer = new DataBuffer { Buffer = data, Size = (uint)payload.Length };
                expect(friendSend(friendEndpoint, 1, &hostSeenByFriend, 1, 0, 1, &buffer, 0) == 0, "The friend sends a message to the host's endpoint");
            }
            expect(host.WaitFor("21") && host.Messages.Count == 1 && host.Messages[0].SequenceEqual(payload), "The host receives the friend's message intact");
            var reply = "hello helldiver"u8.ToArray();
            fixed (byte* data = reply)
            {
                var buffer = new DataBuffer { Buffer = data, Size = (uint)reply.Length };
                expect(hostSend(hostEndpoint, 0, null, 0, 0, 1, &buffer, 0) == 0, "The host broadcasts a message");
            }
            expect(friend.WaitFor("21") && friend.Messages.Count == 1 && friend.Messages[0].SequenceEqual(reply), "The friend receives the host's broadcast");

            var leave = (delegate* unmanaged<nint, nint, uint>)E(friend, "PartyNetworkLeaveNetwork");
            expect(leave(friendNetwork, 0x202) == 0 && friend.WaitFor("19:0"), "The friend leaves the network");
            expect(host.WaitFor("17"), "The host sees the friend's machine leave");
            // Like the SDK, a machine that shares no network any more is destroyed (15) and is created anew (14) when
            // it comes back: HD2 drops a join whose host machine reappears without REMOTE_DEVICE_CREATED.
            expect(host.WaitFor("15"), "The host destroys the friend's machine once no network is shared");
            nint rejoinNetwork = 0, rejoinEndpoint = 0;
            Join(friend, E(friend, "PartyConnectToNetwork"), E(friend, "PartyNetworkAuthenticateLocalUser"), E(friend, "PartyNetworkCreateEndpoint"), parsed, friendUser, invitation,
                &rejoinNetwork, &rejoinEndpoint);
            expect(friend.WaitFor("3:0") && friend.WaitFor("14") && friend.WaitFor("16"), "A returning friend sees the host's machine created again");
            expect(host.WaitFor("14") && host.WaitFor("16"), "The host sees the returning friend's machine created again");
            expect(leave(rejoinNetwork, 0x203) == 0 && friend.WaitFor("19:0"), "The returning friend leaves again");
        }
    }

    // Each game's login key reaches the other backend, which serves it to its game's /Account/Keys lookup, as a host's
    // game asks for a joining player's key.
    static void KeyFlow(string root, Action<bool, string> expect)
    {
        string Url(string directory)
        {
            using var ready = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(root, directory, "ready.json")));
            return ready.RootElement.GetProperty("urls")[0].GetString()!;
        }
        using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { Timeout = TimeSpan.FromSeconds(10) };
        var keys = new Dictionary<string, string> { ["host"] = Convert.ToBase64String(new byte[32]), ["friend"] = Convert.ToBase64String(Enumerable.Repeat((byte)0xfb, 32).ToArray()) };
        foreach (var (directory, key) in keys)
            using (var login = client.PostAsync(Url(directory) + "/Account/Login",
                new StringContent("{\"publicKey\":\"" + key + "\"}", Encoding.UTF8, "application/json")).GetAwaiter().GetResult())
                expect(login.IsSuccessStatusCode, $"The {directory} game logs in with its key");
        foreach (var (asker, about, id) in new[] { ("host", "friend", FriendId), ("friend", "host", HostId) })
        {
            var account = Convert.ToUInt64(id, 16).ToString(System.Globalization.CultureInfo.InvariantCulture);
            var body = client.GetStringAsync(Url(asker) + "/Account/Keys?id=" + account).GetAwaiter().GetResult();
            using var json = System.Text.Json.JsonDocument.Parse(body);
            var entries = json.RootElement.GetProperty("accountKeys");
            expect(entries.GetArrayLength() == 1 && entries[0].GetProperty("platformAccountId").GetString() == account &&
                entries[0].GetProperty("key").GetString() == keys[about], $"The {asker}'s backend serves the {about}'s key");
        }
    }

    // The social tab: the host's game publishes its lobby, and the friend's backend lists the host as a friend and returns
    // that lobby, with the connection string to join it, for the host's account.
    static void PresenceFlow(string root, string connectionString, Action<bool, string> expect)
    {
        string Url(string directory)
        {
            using var ready = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(root, directory, "ready.json")));
            return ready.RootElement.GetProperty("urls")[0].GetString()!;
        }
        using var client = new HttpClient(new HttpClientHandler { UseProxy = false }) { Timeout = TimeSpan.FromSeconds(10) };
        var lobbyId = connectionString.Split(';')[1];
        using (var put = client.PutAsync(Url("host") + "/lobby", new StringContent(
            "{\"onboardingState\":0,\"memberAccountIds\":[\"1\"],\"players\":[{\"playerName\":\"Host\",\"isHost\":true}],\"numLobbyMembers\":2,\"regionIndex\":3}",
            Encoding.UTF8, "application/json")).GetAwaiter().GetResult())
            expect(put.IsSuccessStatusCode, "The host's game publishes its lobby");
        using (var member = client.PostAsync(Url("host") + "/lobby/" + lobbyId + "/member", new StringContent("")).GetAwaiter().GetResult())
            expect(member.IsSuccessStatusCode, "The host's game says which lobby it is in");
        var account = Convert.ToUInt64(HostId, 16).ToString(System.Globalization.CultureInfo.InvariantCulture);
        System.Text.Json.JsonElement found = default;
        var clock = Stopwatch.StartNew();
        while (clock.ElapsedMilliseconds < 15000)
        {
            using var answer = client.PostAsync(Url("friend") + "/lobby/accounts", new StringContent("{\"ids\":[\"" + account + "\"]}", Encoding.UTF8, "application/json")).GetAwaiter().GetResult();
            using var json = System.Text.Json.JsonDocument.Parse(answer.Content.ReadAsStringAsync().GetAwaiter().GetResult());
            if (json.RootElement.GetProperty("lobbies").GetArrayLength() == 1) { found = json.RootElement.Clone(); break; }
            Thread.Sleep(250);
        }
        expect(found.ValueKind == System.Text.Json.JsonValueKind.Object &&
            found.GetProperty("accountLobbies")[0].GetProperty("account").GetString() == account &&
            found.GetProperty("accountLobbies")[0].GetProperty("lobbyId").GetString() == lobbyId &&
            found.GetProperty("lobbies")[0].GetProperty("connectionString").GetString() == connectionString &&
            found.GetProperty("lobbies")[0].GetProperty("regionIndex").GetInt32() == 3, "The friend's backend returns the host's lobby for the host's account");
        var friends = client.PostAsync(Url("friend") + "/playfab/Client/GetFriendsList", new StringContent("{\"ExternalPlatformFriends\": \"None\", }", Encoding.UTF8, "application/json"))
            .GetAwaiter().GetResult().Content.ReadAsStringAsync().GetAwaiter().GetResult();
        expect(friends.Contains("\"FriendPlayFabId\"", StringComparison.Ordinal) && friends.Contains("Friend " + HostId, StringComparison.Ordinal),
            "The friend's friends list has the host");
        using (var clear = client.DeleteAsync(Url("host") + "/lobby/clear").GetAwaiter().GetResult())
            expect(clear.IsSuccessStatusCode, "The host's game clears its lobby");
        clock.Restart();
        var cleared = false;
        while (!cleared && clock.ElapsedMilliseconds < 15000)
        {
            using var answer = client.PostAsync(Url("friend") + "/lobby/accounts", new StringContent("{\"ids\":[\"" + account + "\"]}", Encoding.UTF8, "application/json")).GetAwaiter().GetResult();
            using var json = System.Text.Json.JsonDocument.Parse(answer.Content.ReadAsStringAsync().GetAwaiter().GetResult());
            cleared = json.RootElement.GetProperty("lobbies").GetArrayLength() == 0;
            if (!cleared) Thread.Sleep(250);
        }
        expect(cleared, "A cleared lobby disappears from the friend's social tab");
    }

    static void Join(PartyPlayer player, nint connectExport, nint authenticateExport, nint endpointExport, byte* descriptor, nint user, byte* invitation,
        nint* network, nint* endpoint)
    {
        var connect = (delegate* unmanaged<nint, byte*, nint, nint*, uint>)connectExport;
        var authenticate = (delegate* unmanaged<nint, nint, byte*, nint, uint>)authenticateExport;
        var createEndpoint = (delegate* unmanaged<nint, nint, uint, nint, nint, nint, nint*, uint>)endpointExport;
        connect(player.Handle, descriptor, 0x301, network);
        authenticate(*network, user, invitation, 0x302);
        createEndpoint(*network, user, 0, 0, 0, 0x303, endpoint);
    }

    internal static (nint Library, Player Player) Load(string original, string copy, string pipe, string name)
    {
        File.Copy(original, copy, overwrite: true);
        Environment.SetEnvironmentVariable("HELLFORGE_PIPE", pipe);
        var library = NativeLibrary.Load(copy);
        var initialize = (delegate* unmanaged<byte*, nint*, int>)NativeLibrary.GetExport(library, "PFMultiplayerInitialize");
        var player = new Player
        {
            Name = name,
            Start = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(library, "PFMultiplayerStartProcessingLobbyStateChanges"),
            Finish = (delegate* unmanaged<nint, uint, nint, int>)NativeLibrary.GetExport(library, "PFMultiplayerFinishProcessingLobbyStateChanges")
        };
        nint handle = 0;
        fixed (byte* title = "974A9\0"u8) initialize(title, &handle);
        player.Handle = handle;
        return (library, player);
    }

    internal static Process StartBackend(string backend, string evidence, string pipe, string link, IEnumerable<string>? more = null,
        IReadOnlyDictionary<string, string>? environment = null)
    {
        var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in new[] { backend, "--port", "0", "--evidence", evidence, "--lifetime-seconds", "120", "--pipe", pipe, "--link", link, "--mod-digest", "smoke" }.Concat(more ?? []))
            start.ArgumentList.Add(argument);
        foreach (var (name, value) in environment ?? new Dictionary<string, string>()) start.Environment[name] = value;
        var process = Process.Start(start) ?? throw new IOException("Could not start the backend.");
        process.OutputDataReceived += (_, _) => { };
        process.ErrorDataReceived += (_, _) => { };
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        return process;
    }

    internal static int FreePort()
    {
        using var probe = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)probe.Client.LocalEndPoint!).Port;
    }

    static int Report(List<string> problems, string root)
    {
        Console.WriteLine($"Lobby link smoke (two backends, two stand-ins): {(problems.Count == 0 ? "passed" : problems.Count + " problem(s)")}; logs in {root}");
        foreach (var problem in problems) Console.Error.WriteLine("  failed: " + problem);
        return problems.Count == 0 ? 0 : 1;
    }
}
