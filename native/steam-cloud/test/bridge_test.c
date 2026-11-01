// The Steam bridge end to end (docs/steam-bridge.md, phase 3): the broker's serving loop on a thread, with a fake Valve DLL
// (fake_valve.c) behind its generated dispatch, and the bridge build of the stand-in loaded as the game loads it. The test
// calls the stand-in's interfaces through their vtables as the game does, and checks each kind of argument and result, the
// allow-list's checks, the snapshot, callbacks, call results, the accessors' context, and that the stand-in never loads
// Valve's DLL.
// Usage: bridge_test.exe <bridge build of steam_api64.dll> <fake_valve.dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bridge_gen.h"
#include "broker_bridge.h"

static int g_failures;
#define EXPECT(cond, what) do { if (cond) printf("  ok    %s\n", what); else { printf("  FAIL  %s\n", what); g_failures++; } } while (0)

#define STEAM_ID 76561197960287930ull
#define LOBBY 0x0186000000000123ull
#define HOST_LOBBY 0x0186000000000456ull
#define CLOSED_LOBBY 0x0186000000000789ull
#define HOST_PEER 76561197960287931ull

typedef struct FakeValveState {
    int wrongSelf;
    int richPresenceCalls;
    char richKey[80], richValue[300];
    int lobbyDataCalls;
    char lobbyKey[300];
    uint64_t lobbyDataLobby;
    int personaNameSets;
    int createLobbyMax;
    int createLobbyType;
    int setLobbyTypeCalls, setLobbyType;
    uint64_t setLobbyTypeLobby;
    int p2pSends;
    uint64_t p2pTo;
    uint32_t p2pSize;
    uint8_t p2pFirst;
    int p2pAccepts;
} FakeValveState;

// Play with strangers in a mods folder of the test's own: the settings file's text (written anew, so its time stamp moves).
static wchar_t g_mods[MAX_PATH];

static wchar_t g_peers[MAX_PATH + 32];

static void WriteText(const wchar_t* path, const char* text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wrote = 0;
    if (file != INVALID_HANDLE_VALUE) { WriteFile(file, text, (DWORD)strlen(text), &wrote, NULL); CloseHandle(file); }
}

static void Strangers(const char* json)
{
    wchar_t path[MAX_PATH + 32];
    swprintf(path, sizeof path / sizeof path[0], L"%ls\\hellforge-mods.json", g_mods);
    WriteText(path, json);
}

// ---- the broker, on a thread ----

static BrokerChannel g_broker;
static BrokerBridge g_bridge;

static DWORD WINAPI Broker(void* parameter)
{
    (void)parameter;
    if (BrokerChannelAccept(&g_broker, GetCurrentProcessId(), NULL, 5000) != BROKER_ACCEPTED) return 1;
    return (DWORD)BrokerServe(&g_broker, &g_bridge.steam, NULL);
}

// ---- a CCallbackBase as the game's compiler lays it out: vtable, flags at +8, id at +12 ----

typedef struct FakeCallback {
    void* const* vtable;
    uint8_t flags;
    uint8_t pad[3];
    int id;
    int size;
    int runs;
    uint8_t last[64];
    bool ioFailure;
    uint64_t call;
} FakeCallback;

static void Run3(FakeCallback* self, void* param, bool ioFailure, uint64_t call)
{
    self->runs++;
    memcpy(self->last, param, self->size < 64 ? (size_t)self->size : 64);
    self->ioFailure = ioFailure;
    self->call = call;
}
static void Run1(FakeCallback* self, void* param) { Run3(self, param, false, 0); }
static int Size(FakeCallback* self) { return self->size; }
static void* const g_callbackVtable[3] = { (void*)Run3, (void*)Run1, (void*)Size };

typedef void* (*FindFn)(int32_t user, const char* version);
typedef void (*VoidFn)(void);
typedef bool (*BoolFn)(void);
typedef void (*RegisterFn)(void* callback, int id);
typedef void (*RegisterResultFn)(void* callback, uint64_t call);
typedef void (*UnregisterFn)(void* callback);
typedef void* (*CreateFn)(const char* version);
typedef void* (*ContextInitFn)(void* data);

#define SLOT(object, name) ((*(void* const* const*)(object))[BRIDGE_SLOT_##name])

static VoidFn g_runCallbacks;

static void RunUntil(volatile int* runs, int want)
{
    for (int i = 0; i < 1000 && *runs < want; i++) { g_runCallbacks(); Sleep(10); }
}

static int g_contextFills;
static void FillContext(void* ctx) { g_contextFills++; *(void**)ctx = (void*)0x1234; }

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) { fprintf(stderr, "Usage: bridge_test <bridge steam_api64.dll> <fake_valve.dll>\n"); return 2; }
    HMODULE valve = LoadLibraryW(argv[2]);
    if (!valve) { fprintf(stderr, "cannot load the fake Valve DLL (%lu)\n", GetLastError()); return 2; }
    FakeValveState* fake = ((FakeValveState* (*)(void))(void*)GetProcAddress(valve, "FakeValve_State"))();
    void (*queue)(int, const void*, int) = (void (*)(int, const void*, int))(void*)GetProcAddress(valve, "FakeValve_Queue");

    wchar_t suffix[64];
    swprintf(suffix, 64, L"bridgetest-%lu", GetCurrentProcessId());
    if (!BrokerChannelCreate(&g_broker, suffix) || !BrokerBridgeOpen(&g_bridge, valve, 1)) { fprintf(stderr, "broker setup failed\n"); return 2; }
    BrokerBridgeSnapshot(&g_bridge, &g_broker);
    // strangers off: a nested "playWithStrangers" (a mod's own setting) is not the player's
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(g_mods, MAX_PATH, L"%lsbridgetest-mods-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_mods, NULL);
    Strangers("{\"mods\":{\"x\":{\"playWithStrangers\":true}},\"playWithStrangers\":false}");
    BrokerBridgeWatchStrangers(&g_bridge, g_mods);
    // the backend's co-op peers for Steam P2P: none yet
    swprintf(g_peers, MAX_PATH + 32, L"%ls\\p2p-peers.txt", g_mods);
    WriteText(g_peers, "");
    BrokerBridgeWatchPeers(&g_bridge, g_peers);
    HANDLE broker = CreateThread(NULL, 0, Broker, NULL, 0, NULL);

    SetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", suffix);
    HMODULE standIn = LoadLibraryW(argv[1]);
    if (!standIn) { fprintf(stderr, "cannot load the stand-in (%lu)\n", GetLastError()); return 2; }
#define EXPORT(type, name) ((type)(void*)GetProcAddress(standIn, name))
    g_runCallbacks = EXPORT(VoidFn, "SteamAPI_RunCallbacks");
    FindFn find = EXPORT(FindFn, "SteamInternal_FindOrCreateUserInterface");

    printf("Steam bridge\n");
    EXPECT(EXPORT(BoolFn, "SteamAPI_Init")(), "SteamAPI_Init connects to the broker");

    // ISteamUser: snapshot (bool, CSteamID through the hidden pointer), a call with a buffer and an out value
    void* user = find(1, "SteamUser021");
    EXPECT(user != NULL, "SteamUser021 is a proxy");
    EXPECT(((bool (*)(void*))SLOT(user, SteamUser021_BLoggedOn))(user), "BLoggedOn from the snapshot");
    uint64_t id = 0;
    EXPECT(((uint64_t* (*)(void*, uint64_t*))SLOT(user, SteamUser021_GetSteamID))(user, &id) == &id && id == STEAM_ID,
        "GetSteamID returns its CSteamID through the hidden pointer");
    uint8_t ticket[64] = { 0 };
    uint32_t ticketSize = 0;
    typedef uint32_t (*TicketFn)(void*, void*, int, uint32_t*);
    EXPECT(((TicketFn)SLOT(user, SteamUser021_GetAuthSessionTicket))(user, ticket, sizeof ticket, &ticketSize) == 42 &&
        memcmp(ticket, "TICKET", 6) == 0 && ticketSize == 6, "GetAuthSessionTicket: buffer and out value cross the pipe");
    static uint8_t big[2048];
    EXPECT(((TicketFn)SLOT(user, SteamUser021_GetAuthSessionTicket))(user, big, sizeof big, &ticketSize) == 0,
        "a ticket buffer over the table's 1024 bytes is refused");

    // ISteamFriends: a snapshot string, input strings with the printable check, a refused method that never reaches Steam
    void* friends = find(1, "SteamFriends017");
    EXPECT(strcmp(((const char* (*)(void*))SLOT(friends, SteamFriends017_GetPersonaName))(friends), "Helldiver") == 0,
        "GetPersonaName from the snapshot");
    EXPECT(((int (*)(void*, int))SLOT(friends, SteamFriends017_GetFriendCount))(friends, 4) == 3, "GetFriendCount: a value each way");
    typedef bool (*RichFn)(void*, const char*, const char*);
    EXPECT(((RichFn)SLOT(friends, SteamFriends017_SetRichPresence))(friends, "status", "In a mission") &&
        fake->richPresenceCalls == 1 && strcmp(fake->richValue, "In a mission") == 0, "SetRichPresence passes printable strings");
    EXPECT(!((RichFn)SLOT(friends, SteamFriends017_SetRichPresence))(friends, "status", "hidden\x01") && fake->richPresenceCalls == 1,
        "SetRichPresence with a control character is refused by the broker");
    char longKey[100];
    memset(longKey, 'k', sizeof longKey - 1);
    longKey[sizeof longKey - 1] = 0;
    EXPECT(!((RichFn)SLOT(friends, SteamFriends017_SetRichPresence))(friends, longKey, "x") && fake->richPresenceCalls == 1,
        "a key over the table's 64 bytes is refused");
    EXPECT(!((RichFn)SLOT(friends, SteamFriends017_SetRichPresence))(friends, "HellForge", "hellforge-mp-9") && fake->richPresenceCalls == 1,
        "HellForge's own key (how friends' backends find this one) is refused, in any case");
    EXPECT(((RichFn)SLOT(friends, SteamFriends017_SetRichPresence))(friends, "hellforge2", "x") && fake->richPresenceCalls == 2,
        "a key that only starts with it passes");
    ((uint64_t (*)(void*, const char*))SLOT(friends, SteamFriends017_SetPersonaName))(friends, "Someone else");
    EXPECT(fake->personaNameSets == 0, "SetPersonaName (not in the table) never reaches Steam");

    // ISteamUtils: fixed answers, a string in and a buffer out
    void* utils = find(1, "SteamUtils010");
    EXPECT(((uint32_t (*)(void*))SLOT(utils, SteamUtils010_GetAppID))(utils) == 553850, "GetAppID is the game's");
    EXPECT(strcmp(((const char* (*)(void*))SLOT(utils, SteamUtils010_GetIPCountry))(utils), "SE") == 0, "GetIPCountry from the snapshot");
    char filtered[32] = "";
    typedef int (*FilterFn)(void*, int, uint64_t, const char*, char*, uint32_t);
    EXPECT(((FilterFn)SLOT(utils, SteamUtils010_FilterText))(utils, 1, STEAM_ID, "hello", filtered, sizeof filtered) == 5 &&
        strcmp(filtered, "HELLO") == 0, "FilterText: a string in, a buffer out");

    // ISteamApps: the DLC list from the snapshot
    void* apps = find(1, "STEAMAPPS_INTERFACE_VERSION008");
    EXPECT(((int (*)(void*))SLOT(apps, STEAMAPPS_INTERFACE_VERSION008_GetDLCCount))(apps) == 2, "GetDLCCount from the snapshot");
    uint32_t app = 0;
    bool available = true;
    char dlcName[64] = "";
    typedef bool (*DlcFn)(void*, int, uint32_t*, bool*, char*, int);
    EXPECT(((DlcFn)SLOT(apps, STEAMAPPS_INTERFACE_VERSION008_BGetDLCDataByIndex))(apps, 1, &app, &available, dlcName, sizeof dlcName) &&
        app == 1002 && !available && strcmp(dlcName, "Polar Patriots") == 0, "BGetDLCDataByIndex from the snapshot");
    typedef bool (*SubscribedFn)(void*, uint32_t);
    SubscribedFn subscribed = (SubscribedFn)SLOT(apps, STEAMAPPS_INTERFACE_VERSION008_BIsSubscribedApp);
    EXPECT(subscribed(apps, 553850) && subscribed(apps, 1001) && !subscribed(apps, 1002) && !subscribed(apps, 4000), "BIsSubscribedApp: the game and its DLC");

    // ISteamNetworking: answered in the game
    void* networking = find(1, "SteamNetworking006");
    uint32_t packetSize = 99;
    typedef bool (*ReadFn)(void*, void*, uint32_t, uint32_t*, uint64_t*, int);
    uint64_t remote = 7;
    EXPECT(!((ReadFn)SLOT(networking, SteamNetworking006_ReadP2PPacket))(networking, big, sizeof big, &packetSize, &remote, 0) && packetSize == 0,
        "ReadP2PPacket: no packets, no round trip");

    // callbacks: registered by the stand-in, delivered from the ring; an id the table does not list never arrives
    RegisterFn registerCallback = EXPORT(RegisterFn, "SteamAPI_RegisterCallback");
    FakeCallback dlcInstalled = { g_callbackVtable, 0, { 0 }, 0, 4, 0, { 0 }, false, 0 };
    FakeCallback persona = { g_callbackVtable, 0, { 0 }, 0, 12, 0, { 0 }, false, 0 };
    registerCallback(&dlcInstalled, 1005);
    registerCallback(&persona, 304);
    EXPECT(dlcInstalled.flags == 1 && dlcInstalled.id == 1005, "RegisterCallback marks the object registered with its id");
    uint32_t appInstalled = 1002;
    queue(304, "persona-change", 12);
    queue(1005, &appInstalled, 4);
    RunUntil(&dlcInstalled.runs, 1);
    uint32_t delivered = 0;
    memcpy(&delivered, dlcInstalled.last, 4);
    EXPECT(dlcInstalled.runs == 1 && delivered == 1002 && !dlcInstalled.ioFailure, "DlcInstalled_t reaches its CCallback");
    EXPECT(persona.runs == 0, "a callback the table does not list (304) is not passed on");
    EXPORT(UnregisterFn, "SteamAPI_UnregisterCallback")(&dlcInstalled);
    queue(1005, &appInstalled, 4);
    for (int i = 0; i < 20; i++) { g_runCallbacks(); Sleep(10); }
    EXPECT(dlcInstalled.runs == 1 && dlcInstalled.flags == 0, "an unregistered callback gets nothing more");

    // ISteamMatchmaking: a lobby of the game's own, from CreateLobby's call result; any other is refused
    void* matchmaking = find(1, "SteamMatchMaking009");
    typedef bool (*LobbyDataFn)(void*, uint64_t, const char*, const char*);
    LobbyDataFn setLobbyData = (LobbyDataFn)SLOT(matchmaking, SteamMatchMaking009_SetLobbyData);
    EXPECT(!setLobbyData(matchmaking, LOBBY, "mission", "1") && fake->lobbyDataCalls == 0, "SetLobbyData on a lobby the game did not make is refused");
    typedef uint64_t (*CreateLobbyFn)(void*, int, int);
    CreateLobbyFn createLobby = (CreateLobbyFn)SLOT(matchmaking, SteamMatchMaking009_CreateLobby);
    EXPECT(createLobby(matchmaking, 1, 9) == 0 && fake->createLobbyMax == 0 && g_bridge.pendingCount == 0,
        "CreateLobby for 9 members is refused (table: 1-4), before the broker's own rule sees it");
    uint64_t call = createLobby(matchmaking, 2, 4);
    EXPECT(call == 5005 && fake->createLobbyMax == 4, "CreateLobby returns its SteamAPICall_t");
    EXPECT(fake->createLobbyType == 1, "Play with strangers off: the game's Public lobby (2) is made Friends only (1)");
    FakeCallback created = { g_callbackVtable, 0, { 0 }, 513, 16, 0, { 0 }, false, 0 };
    EXPORT(RegisterResultFn, "SteamAPI_RegisterCallResult")(&created, call);
    RunUntil(&created.runs, 1);
    uint64_t lobby = 0;
    memcpy(&lobby, created.last + 8, 8);
    EXPECT(created.runs == 1 && created.call == 5005 && !created.ioFailure && lobby == LOBBY, "LobbyCreated_t reaches its CCallResult");
    EXPECT(setLobbyData(matchmaking, LOBBY, "mission", "1") && fake->lobbyDataCalls == 1 && fake->lobbyDataLobby == LOBBY &&
        strcmp(fake->lobbyKey, "mission") == 0, "SetLobbyData on the game's own lobby passes");
    uint64_t owner = 0;
    EXPECT(((uint64_t* (*)(void*, uint64_t*, uint64_t))SLOT(matchmaking, SteamMatchMaking009_GetLobbyOwner))(matchmaking, &owner, LOBBY) == &owner &&
        owner == STEAM_ID, "GetLobbyOwner: a CSteamID argument, and its result through the hidden pointer");

    // Play with strangers turned on: the game's lobby is Public again, and a new Public lobby stays Public; a private one
    // (0) is never changed. Off again: every lobby the game asked to be Public is Friends only.
    Strangers("{\"playWithStrangers\": true}");
    for (int i = 0; i < 1000 && fake->setLobbyTypeCalls == 0; i++) Sleep(10);
    EXPECT(fake->setLobbyTypeCalls == 1 && fake->setLobbyType == 2 && fake->setLobbyTypeLobby == LOBBY, "Strangers on: the game's lobby is made Public again");
    createLobby(matchmaking, 2, 4);
    EXPECT(fake->createLobbyType == 2, "Strangers on: a Public lobby stays Public");
    createLobby(matchmaking, 0, 4);
    EXPECT(fake->createLobbyType == 0, "a private lobby (0) is left as asked");
    SetEnvironmentVariableW(L"HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY", L"1");
    createLobby(matchmaking, 2, 4);
    EXPECT(fake->createLobbyType == 1, "HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1 (a two-PC test): Friends only with strangers on too");
    SetEnvironmentVariableW(L"HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY", NULL);
    for (int i = 0; i < 1000 && g_bridge.lobbyCount < 4; i++) Sleep(10);
    Strangers("{\"playWithStrangers\":false}");
    for (int i = 0; i < 1000 && fake->setLobbyTypeCalls < 4; i++) Sleep(10);
    Sleep(BROKER_STRANGERS_MS + 300);
    EXPECT(g_bridge.lobbyCount == 4 && fake->setLobbyTypeCalls == 4 && fake->setLobbyType == 1,
        "Strangers off again: the three lobbies asked to be Public are made Friends only, the private one is not touched");

    // A co-op join (census 2026-10-24): after the PlayFab join the joiner enters the host's Steam lobby (JoinLobby, whose
    // LobbyEnter_t says whether it did), then reads its members and data and writes to it. Before entering, and for a
    // lobby Steam would not let it into, those stay refused.
    typedef uint64_t (*JoinLobbyFn)(void*, uint64_t);
    typedef const char* (*GetLobbyDataFn)(void*, uint64_t, const char*);
    typedef int (*MembersFn)(void*, uint64_t);
    JoinLobbyFn joinLobby = (JoinLobbyFn)SLOT(matchmaking, SteamMatchMaking009_JoinLobby);
    GetLobbyDataFn getLobbyData = (GetLobbyDataFn)SLOT(matchmaking, SteamMatchMaking009_GetLobbyData);
    MembersFn members = (MembersFn)SLOT(matchmaking, SteamMatchMaking009_GetNumLobbyMembers);
    const char* before = getLobbyData(matchmaking, HOST_LOBBY, "mission");
    EXPECT(members(matchmaking, HOST_LOBBY) == 0 && before && before[0] == 0, "Before the join, the host's lobby is not the game's: its members and data are refused");
    FakeCallback entered = { g_callbackVtable, 0, { 0 }, 504, 24, 0, { 0 }, false, 0 };
    uint64_t joinCall = joinLobby(matchmaking, HOST_LOBBY);
    EXPORT(RegisterResultFn, "SteamAPI_RegisterCallResult")(&entered, joinCall);
    RunUntil(&entered.runs, 1);
    uint32_t response = 0;
    memcpy(&response, entered.last + 16, 4);
    EXPECT(joinCall == 5006 && entered.runs == 1 && entered.call == 5006 && response == 1, "JoinLobby's LobbyEnter_t (callback 504) reaches its CCallResult");
    const char* mission = getLobbyData(matchmaking, HOST_LOBBY, "mission");
    int writes = fake->lobbyDataCalls;
    EXPECT(members(matchmaking, HOST_LOBBY) == 2 && mission && strcmp(mission, "host-mission") == 0 &&
        setLobbyData(matchmaking, HOST_LOBBY, "player", "2") && fake->lobbyDataCalls == writes + 1 && fake->lobbyDataLobby == HOST_LOBBY,
        "Entered, the host's lobby is the game's: its members, its data, and writes to it pass");
    EXPECT(fake->setLobbyTypeCalls == 4, "A lobby the game entered never has its type changed (the game asked nothing about it)");
    FakeCallback refusedEntry = { g_callbackVtable, 0, { 0 }, 504, 24, 0, { 0 }, false, 0 };
    uint64_t closedCall = joinLobby(matchmaking, CLOSED_LOBBY);
    EXPORT(RegisterResultFn, "SteamAPI_RegisterCallResult")(&refusedEntry, closedCall);
    RunUntil(&refusedEntry.runs, 1);
    writes = fake->lobbyDataCalls;
    EXPECT(refusedEntry.runs == 1 && members(matchmaking, CLOSED_LOBBY) == 0 && !setLobbyData(matchmaking, CLOSED_LOBBY, "player", "2") &&
        fake->lobbyDataCalls == writes, "A lobby Steam did not let the game into (LobbyEnter_t response 2) stays refused");

    // Steam P2P (2026-10-24: the host sends to a joiner when it joins): only with the game's co-op peers, the ones the
    // backend lists (a member of a lobby the game entered is not enough); packets in come through the ring in pieces.
    void* p2pNet = find(1, "SteamNetworking006");
    typedef bool (*SendFn)(void*, uint64_t, const void*, uint32_t, int, int);
    typedef bool (*PeerFn)(void*, uint64_t);
    typedef bool (*StateFn)(void*, uint64_t, void*);
    typedef bool (*AvailableFn)(void*, uint32_t*, int);
    typedef bool (*ReadFn)(void*, void*, uint32_t, uint32_t*, uint64_t*, int);
    SendFn send = (SendFn)SLOT(p2pNet, SteamNetworking006_SendP2PPacket);
    PeerFn accept = (PeerFn)SLOT(p2pNet, SteamNetworking006_AcceptP2PSessionWithUser);
    StateFn state = (StateFn)SLOT(p2pNet, SteamNetworking006_GetP2PSessionState);
    AvailableFn p2pWaiting = (AvailableFn)SLOT(p2pNet, SteamNetworking006_IsP2PPacketAvailable);
    ReadFn readPacket = (ReadFn)SLOT(p2pNet, SteamNetworking006_ReadP2PPacket);
    const uint64_t stranger = 76561197960287999ull, listed = 76561197960288000ull;
    static uint8_t bytes[7001];
    for (int i = 0; i < (int)sizeof bytes; i++) bytes[i] = (uint8_t)(i * 7 + 1);
    EXPECT(p2pNet && !send(p2pNet, HOST_PEER, bytes, 5, 2, 0) && fake->p2pSends == 0,
        "SendP2PPacket to a member of the lobby the game entered is refused while the backend does not list it (JoinLobby takes any lobby)");
    WriteText(g_peers, "76561197960287931\n");
    EXPECT(send(p2pNet, HOST_PEER, bytes, 5, 2, 0) && fake->p2pSends == 1 && fake->p2pTo == HOST_PEER && fake->p2pSize == 5 &&
        fake->p2pFirst == 1, "SendP2PPacket to the host the backend lists reaches Steam, its bytes with it");
    EXPECT(!send(p2pNet, stranger, bytes, 5, 2, 0) && fake->p2pSends == 1, "SendP2PPacket to anyone else is refused (no way out to any Steam user)");
    WriteText(g_peers, "76561197960287931\n76561197960288000\n");
    EXPECT(send(p2pNet, listed, bytes, 7000, 2, 0) && fake->p2pSends == 2 && fake->p2pSize == 7000,
        "a peer the backend just listed (a join seconds old) is reached at once, 7000 bytes in one call");
    EXPECT(!send(p2pNet, listed, bytes, 7001, 2, 0) && fake->p2pSends == 2, "more than 7000 bytes is refused in the game");
    // The backend can rewrite its file twice within one tick of the clock (a lobby change, then a join's peer), and both
    // writes get the same time stamp: the peer written second is reached all the same.
    int reached = 0;
    for (int i = 1; i <= 50; i++)
    {
        char list[64];
        snprintf(list, sizeof list, "76561197960287931\n%llu\n", (unsigned long long)(listed + i));
        WriteText(g_peers, list);
        reached += send(p2pNet, listed + i, bytes, 5, 2, 0);
    }
    EXPECT(reached == 50 && fake->p2pSends == 52, "a peer listed within the same clock tick as the list before it is reached at once");
    uint8_t session[20] = { 0 };
    EXPECT(state(p2pNet, HOST_PEER, session) && session[0] == 1, "GetP2PSessionState: its P2PSessionState_t comes back as bytes");
    EXPECT(accept(p2pNet, HOST_PEER) && !accept(p2pNet, stranger) && fake->p2pAccepts == 1, "AcceptP2PSessionWithUser: a peer, never anyone else");
    void (*queuePacket)(uint64_t, int, const void*, uint32_t) = (void (*)(uint64_t, int, const void*, uint32_t))(void*)GetProcAddress(valve, "FakeValve_QueuePacket");
    queuePacket(stranger, 0, bytes, 10);
    queuePacket(HOST_PEER, 0, bytes, 3000);
    queuePacket(HOST_PEER, 1, bytes + 100, 7);
    uint32_t waiting = 0;
    for (int i = 0; i < 1000 && !p2pWaiting(p2pNet, &waiting, 1); i++) { g_runCallbacks(); Sleep(10); }
    static uint8_t got[4096];
    uint32_t gotSize = 0;
    uint64_t from = 0;
    bool firstPacket = p2pWaiting(p2pNet, &waiting, 0) && waiting == 3000 && readPacket(p2pNet, got, sizeof got, &gotSize, &from, 0);
    EXPECT(firstPacket && gotSize == 3000 && from == HOST_PEER && memcmp(got, bytes, 3000) == 0,
        "a 3000-byte packet from the peer arrives whole, through the ring in pieces, and ReadP2PPacket gives it with its sender");
    EXPECT(!p2pWaiting(p2pNet, &waiting, 0) && readPacket(p2pNet, got, sizeof got, &gotSize, &from, 1) && gotSize == 7 && got[0] == bytes[100] &&
        !p2pWaiting(p2pNet, &waiting, 1), "a stranger's packet never reaches the game; another channel's comes on its own");

    // the inline accessors' context, ISteamClient, and an interface the table does not know
    ContextInitFn contextInit = EXPORT(ContextInitFn, "SteamInternal_ContextInit");
    struct { void (*fn)(void*); uintptr_t counter; void* ptr; } context = { FillContext, 0, NULL };
    void* first = contextInit(&context);
    void* second = contextInit(&context);
    EXPECT(first == &context.ptr && second == first && g_contextFills == 1 && context.ptr == (void*)0x1234, "ContextInit fills once, then returns");
    void* client = EXPORT(CreateFn, "SteamInternal_CreateInterface")("SteamClient020");
    typedef void* (*GetInterfaceFn)(void*, int32_t, int32_t, const char*);
    EXPECT(client && ((GetInterfaceFn)SLOT(client, SteamClient020_GetISteamUser))(client, 1, 1, "SteamUser021") == user,
        "ISteamClient::GetISteamUser hands out the same proxy");
    typedef void* (*FindGameServerFn)(int32_t user, const char* version);
    EXPECT(EXPORT(FindGameServerFn, "SteamInternal_FindOrCreateGameServerInterface")(0, "SteamGameServerUtils010") == NULL,
        "a game-server interface is NULL, as Valve's without a game server (the game checks for it)");
    void* http = find(1, "STEAMHTTP_INTERFACE_VERSION003");
    EXPECT(http && ((uint64_t (*)(void*))(*(void* const* const*)http)[0])(http) == 0, "an interface the table does not know answers 0");

    EXPECT(fake->wrongSelf == 0, "every flat function got its own interface's object");
    EXPECT(GetModuleHandleW(L"steam_api64_hellforge.dll") == NULL, "the stand-in never loaded Valve's DLL");
    EXPORT(VoidFn, "SteamAPI_Shutdown")();
    EXPECT(WaitForSingleObject(broker, 5000) == WAIT_OBJECT_0, "SteamAPI_Shutdown ends the broker's loop");
    // the control character, HellForge's key, the 9-member lobby and the foreign lobby (the long key never left the game),
    // the host's lobby twice before the join, the lobby Steam refused twice, the send before the backend listed the host,
    // and the stranger's send and accept
    EXPECT(g_bridge.refusals == 11, "the broker counted its eleven refusals");

    printf("%s Steam bridge: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
