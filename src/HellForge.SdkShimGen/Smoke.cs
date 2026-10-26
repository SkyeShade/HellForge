using System.Runtime.InteropServices;

namespace HellForge.SdkShimGen;

// Loads built stand-ins into this process and checks the phase-1 contract: initialization hands out a handle, the
// state-change pumps report nothing, an export with outputs fails without writing them, HELLFORGE_SHIM_POLICY overrides a
// default, and every call is logged. Must run in a fresh process (the policy is read once per load).
public static unsafe class Smoke
{
    const uint PartyOk = 0, PartyUnavailable = 0x10D0;
    const int SOk = 0, ENotImpl = unchecked((int)0x80004001);

    public static int Run(string directory)
    {
        var logs = Path.Combine(Path.GetTempPath(), "hellforge-shim-smoke-" + Guid.NewGuid().ToString("N"));
        Environment.SetEnvironmentVariable("HELLFORGE_SHIM_LOG_DIR", logs);
        Environment.SetEnvironmentVariable("HELLFORGE_SHIM_POLICY", "PartyDoWork=fail");
        var problems = new List<string>();
        void Expect(bool condition, string description) { if (!condition) problems.Add(description); }

        var party = NativeLibrary.Load(Path.Combine(directory, "PartyWin.dll"));
        PartyContract(party, Expect);
        var lobby = NativeLibrary.Load(Path.Combine(directory, "PlayFabMultiplayerWin.dll"));
        LobbyContract(lobby, Expect);

        var pid = Environment.ProcessId;
        var partyLog = Path.Combine(logs, $"PartyWin.dll-{pid}.log");
        var lobbyLog = Path.Combine(logs, $"PlayFabMultiplayerWin.dll-{pid}.log");
        var partyText = ReadShared(partyLog);
        var lobbyText = ReadShared(lobbyLog);
        Expect(partyText.Contains("HellForge stand-in loaded") && partyText.Contains("PartyInitialize #1(\"974A9\""), "Party log records load and calls");
        Expect(lobbyText.Contains("PFLobbyLeaveWithEntityHandle #1()"), "Lobby log records undeclared calls");

        Console.WriteLine($"Smoke test: {problems.Count} problem(s); logs in {logs}");
        foreach (var problem in problems) Console.Error.WriteLine("  failed: " + problem);
        return problems.Count == 0 ? 0 : 1;
    }

    // PartyWin: initialize, an empty pump, the local device, a local user and a created network, then hosting and the stubs.
    static void PartyContract(nint party, Action<bool, string> Expect)
    {
        var partyInitialize = (delegate* unmanaged<byte*, nint*, uint>)NativeLibrary.GetExport(party, "PartyInitialize");
        var partyStart = (delegate* unmanaged<nint, uint*, nint*, uint>)NativeLibrary.GetExport(party, "PartyStartProcessingStateChanges");
        var partyLocalDevice = (delegate* unmanaged<nint, nint*, uint>)NativeLibrary.GetExport(party, "PartyGetLocalDevice");
        var partyCreateLocalUser = (delegate* unmanaged<nint, byte*, byte*, nint*, uint>)NativeLibrary.GetExport(party, "PartyCreateLocalUser");
        var partyEntityId = (delegate* unmanaged<nint, nint*, uint>)NativeLibrary.GetExport(party, "PartyLocalUserGetEntityId");
        var partyCreateNetwork = (delegate* unmanaged<nint, nint, uint*, uint, nint, nint, nint, byte*, byte*, uint>)NativeLibrary.GetExport(party, "PartyCreateNewNetwork");
        nint handle = 0, changes = 0, device = 0, user = 0, entityId = 0;
        uint count = 99;
        fixed (byte* title = "974A9\0"u8)
            Expect(partyInitialize(title, &handle) == PartyOk && handle != 0, "PartyInitialize returns a handle");
        Expect(partyStart(handle, &count, &changes) == PartyOk && count == 0 && changes != 0, "Party state changes are empty");
        Expect(partyLocalDevice(handle, &device) == PartyOk && device != 0, "PartyGetLocalDevice returns the local device");
        fixed (byte* id = "110000100000001\0"u8, token = "hellforge-entity-test\0"u8)
            Expect(partyCreateLocalUser(handle, id, token, &user) == PartyOk && user != 0, "PartyCreateLocalUser returns a user");
        Expect(partyEntityId(user, &entityId) == PartyOk && Marshal.PtrToStringUTF8(entityId) == "110000100000001", "Local user keeps its entity id");
        // PARTY_NETWORK_CONFIGURATION is six uint32 fields; the descriptor 37 + 20 + 300 bytes; invitation ids 128.
        var configuration = stackalloc uint[6] { 4, 4, 1, 1, 8, 0 };
        var descriptor = stackalloc byte[512];
        var invitation = stackalloc byte[128];
        Expect(partyCreateNetwork(handle, user, configuration, 0, 0, 0, 0x77, descriptor, invitation) == PartyOk && descriptor[8] == (byte)'-' && invitation[0] != 0,
            "PartyCreateNewNetwork is accepted and writes a descriptor and the initial invitation");
        PartyHosting(party, handle, user, descriptor, invitation, Expect);
        PartyStubs(party, handle, Expect);
    }

    // HD2 hosts by creating a network, connecting, authenticating with the initial invitation and creating an endpoint,
    // all in one frame (stand-in boots, 2026-10-14); then the regions, lossless descriptors, leaving, and a friend's network.
    static void PartyHosting(nint party, nint handle, nint user, byte* descriptor, byte* invitation, Action<bool, string> Expect)
    {
        var partyStart = (delegate* unmanaged<nint, uint*, nint*, uint>)NativeLibrary.GetExport(party, "PartyStartProcessingStateChanges");
        var partyFinish = (delegate* unmanaged<nint, uint, nint, uint>)NativeLibrary.GetExport(party, "PartyFinishProcessingStateChanges");
        var partyRegions = (delegate* unmanaged<nint, uint*, nint*, uint>)NativeLibrary.GetExport(party, "PartyGetRegions");
        var connect = (delegate* unmanaged<nint, byte*, nint, nint*, uint>)NativeLibrary.GetExport(party, "PartyConnectToNetwork");
        var authenticate = (delegate* unmanaged<nint, nint, byte*, nint, uint>)NativeLibrary.GetExport(party, "PartyNetworkAuthenticateLocalUser");
        var createEndpoint = (delegate* unmanaged<nint, nint, uint, nint, nint, nint, nint*, uint>)NativeLibrary.GetExport(party, "PartyNetworkCreateEndpoint");
        var endpointId = (delegate* unmanaged<nint, ushort*, uint>)NativeLibrary.GetExport(party, "PartyEndpointGetUniqueIdentifier");
        var leave = (delegate* unmanaged<nint, nint, uint>)NativeLibrary.GetExport(party, "PartyNetworkLeaveNetwork");
        var serialize = (delegate* unmanaged<byte*, byte*, uint>)NativeLibrary.GetExport(party, "PartySerializeNetworkDescriptor");
        var deserialize = (delegate* unmanaged<byte*, byte*, uint>)NativeLibrary.GetExport(party, "PartyDeserializeNetworkDescriptor");
        nint changes = 0, network = 0, endpoint = 0, regions = 0x1234;
        uint count = 0;
        Expect(connect(handle, descriptor, 0x88, &network) == PartyOk && network != 0, "PartyConnectToNetwork returns a network at once");
        Expect(authenticate(network, user, invitation, 0) == PartyOk, "PartyNetworkAuthenticateLocalUser queues on the connecting network");
        Expect(createEndpoint(network, user, 0, 0, 0, 0x99, &endpoint) == PartyOk && endpoint != 0, "PartyNetworkCreateEndpoint queues on the connecting network");
        Expect(partyStart(handle, &count, &changes) == PartyOk && string.Join(",", Types(changes, count)) == "0:0,2:0,3:0,5,53,4:0,12,10:0",
            "Hosting succeeds in the documented order: regions, created, connected, configuration, invitation, authenticated, endpoint");
        Expect(partyFinish(handle, count, changes) == PartyOk && partyStart(handle, &count, &changes) == PartyOk && count == 0, "Finished state changes are not delivered again");
        ushort uniqueId = 0;
        Expect(endpointId(endpoint, &uniqueId) == PartyOk && uniqueId == 1, "The created endpoint has an identifier");
        count = 0;
        Expect(partyRegions(handle, &count, &regions) == PartyOk && count == 1 && Marshal.PtrToStringUTF8(regions) == "HellForgeLocal", "The region list is filled");
        var serialized = stackalloc byte[449];
        var parsed = stackalloc byte[512];
        Expect(serialize(descriptor, serialized) == PartyOk && deserialize(serialized, parsed) == PartyOk && new Span<byte>(parsed, 357).SequenceEqual(new Span<byte>(descriptor, 357)) &&
            Marshal.PtrToStringUTF8((nint)serialized)!.StartsWith("hellforge1;", StringComparison.Ordinal), "Descriptors serialize and deserialize losslessly");
        Expect(leave(network, 0xAA) == PartyOk && partyStart(handle, &count, &changes) == PartyOk && string.Join(",", Types(changes, count)) == "13,54,20,19:0",
            "Leaving tears down endpoint, invitation and network before the leave completes");
        partyFinish(handle, count, changes);
        // A network created on another machine cannot be reached yet.
        nint foreign = 0;
        fixed (byte* text = "hellforge1;f00df00d-0000-0000-0000-000000000000;HellForgeLocal;\0"u8)
            Expect(deserialize(text, parsed) == PartyOk && connect(handle, parsed, 0, &foreign) == PartyOk && partyStart(handle, &count, &changes) == PartyOk &&
                string.Join(",", Types(changes, count)) == "3:3,20", "Connecting to a friend's network fails cleanly");
        partyFinish(handle, count, changes);
    }

    static void PartyStubs(nint party, nint handle, Action<bool, string> Expect)
    {
        var partySetOption = (delegate* unmanaged<nint, uint, nint, uint>)NativeLibrary.GetExport(party, "PartySetOption");
        var partyDoWork = (delegate* unmanaged<nint, uint, uint>)NativeLibrary.GetExport(party, "PartyDoWork");
        var memoryCallbacks = (delegate* unmanaged<nint*, nint*, uint>)NativeLibrary.GetExport(party, "PartyGetMemoryCallbacks");
        nint allocate = 0x1234, free = 0x1234;
        Expect(memoryCallbacks(&allocate, &free) == PartyUnavailable && allocate == 0 && free == 0, "A generated stub with outputs fails and clears them");
        Expect(partySetOption(0, 0, 0) == PartyOk, "PartySetOption (no outputs) succeeds by default");
        Expect(partyDoWork(handle, 0) == PartyUnavailable, "HELLFORGE_SHIM_POLICY turns PartyDoWork into a failure");
    }

    // PlayFabMultiplayerWin: initialize, an empty pump, failing reads that clear their outputs, a named error and an
    // undeclared export, then hosting a lobby.
    static void LobbyContract(nint lobby, Action<bool, string> Expect)
    {
        var initialize = (delegate* unmanaged<byte*, nint*, int>)NativeLibrary.GetExport(lobby, "PFMultiplayerInitialize");
        var lobbyStart = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(lobby, "PFMultiplayerStartProcessingLobbyStateChanges");
        var members = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(lobby, "PFLobbyGetMembers");
        var message = (delegate* unmanaged<int, byte*>)NativeLibrary.GetExport(lobby, "PFMultiplayerGetErrorMessage");
        var undeclared = (delegate* unmanaged<int>)NativeLibrary.GetExport(lobby, "PFLobbyLeaveWithEntityHandle");
        nint multiplayer = 0, changes = 0, list = 0x5678;
        uint count = 99;
        fixed (byte* title = "974A9\0"u8)
            Expect(initialize(title, &multiplayer) == SOk && multiplayer != 0, "PFMultiplayerInitialize returns a handle");
        Expect(lobbyStart(multiplayer, &count, &changes) == SOk && count == 0, "Lobby state changes are empty");
        Expect(members(0, &count, &list) < 0 && list == 0 && count == 0, "PFLobbyGetMembers on no lobby fails and clears its outputs");
        // Boot 3 crashed in game.dll's strlen on a lobby property the stub left unwritten (garbage 0x8).
        var lobbyProperty = (delegate* unmanaged<nint, byte*, nint*, int>)NativeLibrary.GetExport(lobby, "PFLobbyGetLobbyProperty");
        nint value = 8;
        fixed (byte* key = "8\0"u8)
            Expect(lobbyProperty(0, key, &value) < 0 && value == 0, "PFLobbyGetLobbyProperty on no lobby returns a null value");
        Expect(Marshal.PtrToStringUTF8((nint)message(ENotImpl))?.StartsWith("HellForge:", StringComparison.Ordinal) == true, "Error message names the stand-in");
        Expect(undeclared() == ENotImpl, "Undeclared export answers E_NOTIMPL");
        HostLobby(lobby, multiplayer, Expect);
    }

    [StructLayout(LayoutKind.Sequential)] struct EntityKey { public byte* Id; public byte* Type; }
    [StructLayout(LayoutKind.Sequential)] struct CreateConfiguration
    {
        public uint MaxMemberCount, OwnerMigrationPolicy, AccessPolicy, SearchPropertyCount;
        public byte** SearchKeys, SearchValues;
        public uint LobbyPropertyCount;
        public byte** LobbyKeys, LobbyValues;
    }
    [StructLayout(LayoutKind.Sequential)] struct JoinConfiguration { public uint Count; public byte** Keys, Values; }
    [StructLayout(LayoutKind.Sequential)] struct DataUpdate
    {
        public EntityKey* NewOwner;
        public uint* MaxMemberCount, AccessPolicy, MembershipLock;
        public uint SearchPropertyCount;
        public byte** SearchKeys, SearchValues;
        public uint LobbyPropertyCount;
        public byte** LobbyKeys, LobbyValues;
    }

    // Hosting a lobby: create, read, update, leave, and a friend's lobby that cannot be joined yet.
    static void HostLobby(nint library, nint multiplayer, Action<bool, string> expect)
    {
        var create = (delegate* unmanaged<nint, EntityKey*, CreateConfiguration*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(library, "PFMultiplayerCreateAndJoinLobby");
        var join = (delegate* unmanaged<nint, EntityKey*, byte*, JoinConfiguration*, nint, nint*, int>)NativeLibrary.GetExport(library, "PFMultiplayerJoinLobby");
        var start = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(library, "PFMultiplayerStartProcessingLobbyStateChanges");
        var finish = (delegate* unmanaged<nint, uint, nint, int>)NativeLibrary.GetExport(library, "PFMultiplayerFinishProcessingLobbyStateChanges");
        var property = (delegate* unmanaged<nint, byte*, nint*, int>)NativeLibrary.GetExport(library, "PFLobbyGetLobbyProperty");
        var memberProperty = (delegate* unmanaged<nint, EntityKey*, byte*, nint*, int>)NativeLibrary.GetExport(library, "PFLobbyGetMemberProperty");
        var owner = (delegate* unmanaged<nint, EntityKey**, int>)NativeLibrary.GetExport(library, "PFLobbyGetOwner");
        var members = (delegate* unmanaged<nint, uint*, nint*, int>)NativeLibrary.GetExport(library, "PFLobbyGetMembers");
        var connection = (delegate* unmanaged<nint, nint*, int>)NativeLibrary.GetExport(library, "PFLobbyGetConnectionString");
        var postUpdate = (delegate* unmanaged<nint, EntityKey*, DataUpdate*, nint, nint, int>)NativeLibrary.GetExport(library, "PFLobbyPostUpdate");
        var leave = (delegate* unmanaged<nint, EntityKey*, nint, int>)NativeLibrary.GetExport(library, "PFLobbyLeave");
        string? Text(nint pointer) => pointer == 0 ? null : Marshal.PtrToStringUTF8(pointer);
        string Deliver()
        {
            uint count;
            nint changes;
            if (start(multiplayer, &count, &changes) != 0) return "start failed";
            var types = new List<string>();
            for (var i = 0; i < count; i++)
            {
                var change = (uint*)((nint*)changes)[i];
                // Completions carry an HRESULT as their second field.
                types.Add(change[0] is 0 or 1 or 8 or 12 or 15 ? $"{change[0]}:{change[1]:x}" : $"{change[0]}");
            }
            finish(multiplayer, count, changes);
            return string.Join(",", types);
        }

        fixed (byte* id = "110000100000001\0"u8, type = "title_player_account\0"u8, k0 = "0\0"u8, k8 = "8\0"u8, k5 = "5\0"u8,
            v0 = "zero\0"u8, v8 = "eight\0"u8, v8b = "EIGHT\0"u8, platform = "platform_lobby\0"u8, platformValue = "1:76561198000000000:a:b\0"u8,
            foreignLobby = "hellforge1-lobby;f00d;10000000000000\0"u8)
        {
            var creator = new EntityKey { Id = id, Type = type };
            var lobbyKeys = stackalloc byte*[] { k0, k8 };
            var lobbyValues = stackalloc byte*[] { v0, v8 };
            var memberKeys = stackalloc byte*[] { platform };
            var memberValues = stackalloc byte*[] { platformValue };
            var configuration = new CreateConfiguration { MaxMemberCount = 4, AccessPolicy = 0, LobbyPropertyCount = 2, LobbyKeys = lobbyKeys, LobbyValues = lobbyValues };
            var joinConfiguration = new JoinConfiguration { Count = 1, Keys = memberKeys, Values = memberValues };
            nint handle = 0, text = 0;
            expect(create(multiplayer, &creator, &configuration, &joinConfiguration, 0x55, &handle) == 0 && handle != 0, "PFMultiplayerCreateAndJoinLobby returns a lobby at once");
            expect(Deliver() == "2,0:0", "Creating a lobby delivers MemberAdded, then CreateAndJoinLobbyCompleted(S_OK)");
            expect(property(handle, k8, &text) == 0 && Text(text) == "eight" && property(handle, k5, &text) == 0 && text == 0,
                "Lobby properties read back, and a missing one is null");
            EntityKey* ownerKey = null;
            uint count = 0;
            nint list = 0;
            expect(owner(handle, &ownerKey) == 0 && ownerKey != null && Text((nint)ownerKey->Id) == "110000100000001", "The creator owns the lobby");
            expect(members(handle, &count, &list) == 0 && count == 1, "The creator is the only member");
            expect(memberProperty(handle, &creator, platform, &text) == 0 && Text(text) == "1:76561198000000000:a:b", "Member properties read back");
            expect(connection(handle, &text) == 0 && Text(text)?.StartsWith("hellforge1-lobby;", StringComparison.Ordinal) == true, "The connection string names a HellForge lobby");

            var updateKeys = stackalloc byte*[] { k8, k0 };
            var updateValues = stackalloc byte*[] { v8b, null };
            var update = new DataUpdate { LobbyPropertyCount = 2, LobbyKeys = updateKeys, LobbyValues = updateValues };
            expect(postUpdate(handle, &creator, &update, 0, 0x66) == 0 && Deliver() == "8:0,7", "PostUpdate completes, then Updated follows");
            expect(property(handle, k8, &text) == 0 && Text(text) == "EIGHT" && property(handle, k0, &text) == 0 && text == 0,
                "An update changes a property and a null value deletes one");
            expect(leave(handle, null, 0x77) == 0 && Deliver() == "4,6,9,10", "Leaving delivers MemberRemoved, LeaveLobbyCompleted, Disconnecting, Disconnected");

            nint foreign = 0;
            expect(join(multiplayer, &creator, foreignLobby, null, 0x88, &foreign) == 0 && foreign != 0 && Deliver() == "1:89236226",
                "Joining a friend's lobby fails with 'lobby doesn't exist'");
        }
    }

    // "type:result" for completions (their second field) and "type" for notifications, which have no result:
    // configuration made available (5), endpoint created/destroyed (12, 13), network destroyed (20) and invitation
    // created/destroyed (53, 54).
    static List<string> Types(nint changes, uint count)
    {
        var types = new List<string>();
        for (var i = 0; i < count; i++)
        {
            var change = (uint*)((nint*)changes)[i];
            types.Add(change[0] is 5 or 12 or 13 or 20 or 53 or 54 ? $"{change[0]}" : $"{change[0]}:{change[1]}");
        }
        return types;
    }

    // The stand-ins keep their log open for appending until the process ends.
    static string ReadShared(string path)
    {
        if (!File.Exists(path)) return "";
        using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
        return reader.ReadToEnd();
    }
}
