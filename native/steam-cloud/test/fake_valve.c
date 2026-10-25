// A stand-in for Valve's steam_api64.dll on the broker's side, for bridge_test.c: the flat functions the allow-list
// bridges and the snapshot reads, the interface accessors, and manual dispatch with a queue the test fills
// (FakeValve_Queue). Each flat function checks it was given its own interface's object, and records what it received
// (FakeValve_State), so the test can tell a bridged call from a refused one.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FAKE_API __declspec(dllexport)
#define STEAM_ID 76561197960287930ull
#define LOBBY 0x0186000000000123ull
#define CREATE_LOBBY_CALL 5005ull
#define HOST_LOBBY 0x0186000000000456ull
#define CLOSED_LOBBY 0x0186000000000789ull
#define HOST_PEER 76561197960287931ull
#define JOIN_LOBBY_CALL 5006ull
#define JOIN_CLOSED_CALL 5007ull

typedef struct FakeValveState {
    int wrongSelf;              // a flat function got another interface's object
    int richPresenceCalls;
    char richKey[80], richValue[300];
    int lobbyDataCalls;
    char lobbyKey[300];
    uint64_t lobbyDataLobby;
    int personaNameSets;        // SetPersonaName: refused, so never reached
    int createLobbyMax;
    int createLobbyType;        // the type Steam was asked for (the broker may change the game's)
    int setLobbyTypeCalls, setLobbyType;
    uint64_t setLobbyTypeLobby;
    int p2pSends;               // SendP2PPacket calls that reached Steam
    uint64_t p2pTo;
    uint32_t p2pSize;
    uint8_t p2pFirst;
    int p2pAccepts;
} FakeValveState;

static FakeValveState g_state;
static int g_user, g_friends, g_utils, g_apps, g_matchmaking, g_stats, g_input, g_networking;

FAKE_API FakeValveState* FakeValve_State(void) { return &g_state; }

FAKE_API void* SteamAPI_SteamUser_v021(void) { return &g_user; }
FAKE_API void* SteamAPI_SteamFriends_v017(void) { return &g_friends; }
FAKE_API void* SteamAPI_SteamUtils_v010(void) { return &g_utils; }
FAKE_API void* SteamAPI_SteamApps_v008(void) { return &g_apps; }
FAKE_API void* SteamAPI_SteamMatchmaking_v009(void) { return &g_matchmaking; }
FAKE_API void* SteamAPI_SteamUserStats_v012(void) { return &g_stats; }
FAKE_API void* SteamAPI_SteamInput_v006(void) { return &g_input; }
FAKE_API void* SteamAPI_SteamNetworking_v006(void) { return &g_networking; }

static void Check(void* self, void* expected) { if (self != expected) g_state.wrongSelf++; }

FAKE_API bool SteamAPI_ISteamUser_BLoggedOn(void* self) { Check(self, &g_user); return true; }
FAKE_API uint64_t SteamAPI_ISteamUser_GetSteamID(void* self) { Check(self, &g_user); return STEAM_ID; }
FAKE_API uint32_t SteamAPI_ISteamUser_GetAuthSessionTicket(void* self, void* ticket, int cbMax, uint32_t* pcb)
{
    Check(self, &g_user);
    if (!ticket || cbMax < 6) return 0;
    memcpy(ticket, "TICKET", 6);
    if (pcb) *pcb = 6;
    return 42;
}

FAKE_API const char* SteamAPI_ISteamFriends_GetPersonaName(void* self) { Check(self, &g_friends); return "Helldiver"; }
FAKE_API int SteamAPI_ISteamFriends_GetFriendCount(void* self, int flags) { Check(self, &g_friends); return flags == 4 ? 3 : -1; }
FAKE_API bool SteamAPI_ISteamFriends_SetRichPresence(void* self, const char* key, const char* value)
{
    Check(self, &g_friends);
    g_state.richPresenceCalls++;
    strncpy(g_state.richKey, key ? key : "(null)", sizeof g_state.richKey - 1);
    strncpy(g_state.richValue, value ? value : "(null)", sizeof g_state.richValue - 1);
    return true;
}
FAKE_API uint64_t SteamAPI_ISteamFriends_SetPersonaName(void* self, const char* name) { (void)name; Check(self, &g_friends); g_state.personaNameSets++; return 1; }

FAKE_API const char* SteamAPI_ISteamUtils_GetIPCountry(void* self) { Check(self, &g_utils); return "SE"; }
FAKE_API bool SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(void* self) { Check(self, &g_utils); return false; }
FAKE_API int SteamAPI_ISteamUtils_FilterText(void* self, int context, uint64_t source, const char* in, char* out, uint32_t size)
{
    Check(self, &g_utils);
    if (context != 1 || source != STEAM_ID || !in || !out || size == 0) return -1;
    uint32_t n = 0;
    for (; in[n] && n + 1 < size; n++) out[n] = (in[n] >= 'a' && in[n] <= 'z') ? (char)(in[n] - 32) : in[n];
    out[n] = 0;
    return (int)n;
}

FAKE_API int SteamAPI_ISteamApps_GetDLCCount(void* self) { Check(self, &g_apps); return 2; }
FAKE_API bool SteamAPI_ISteamApps_BGetDLCDataByIndex(void* self, int i, uint32_t* app, bool* available, char* name, int capacity)
{
    Check(self, &g_apps);
    static const char* names[2] = { "Viper Commandos", "Polar Patriots" };
    if (i < 0 || i > 1) return false;
    *app = 1001 + (uint32_t)i;
    *available = i == 0;
    strncpy(name, names[i], (size_t)capacity);
    return true;
}
FAKE_API bool SteamAPI_ISteamApps_BIsSubscribedApp(void* self, uint32_t app) { Check(self, &g_apps); return app == 1001; }
FAKE_API const char* SteamAPI_ISteamApps_GetCurrentGameLanguage(void* self) { Check(self, &g_apps); return "english"; }
FAKE_API bool SteamAPI_ISteamApps_GetCurrentBetaName(void* self, char* name, int capacity) { Check(self, &g_apps); (void)name; (void)capacity; return false; }

FAKE_API bool SteamAPI_ISteamUserStats_RequestCurrentStats(void* self) { Check(self, &g_stats); return true; }

// ---- manual dispatch: a queue of callbacks, and CreateLobby's call result ----

#pragma pack(push, 8)
typedef struct CallbackMsg { int32_t steamUser; int32_t callbackId; uint8_t* data; int32_t dataSize; } CallbackMsg;
#pragma pack(pop)

#define QUEUE 32
static struct { int id; int size; uint8_t data[1024]; } g_queue[QUEUE];
static volatile LONG g_head, g_tail;
static CRITICAL_SECTION g_lock;

FAKE_API void FakeValve_Queue(int id, const void* data, int size)
{
    EnterCriticalSection(&g_lock);
    if (g_tail - g_head < QUEUE && size >= 0 && size <= 1024)
    {
        int at = g_tail % QUEUE;
        g_queue[at].id = id;
        g_queue[at].size = size;
        memcpy(g_queue[at].data, data, (size_t)size);
        g_tail++;
    }
    LeaveCriticalSection(&g_lock);
}

FAKE_API uint64_t SteamAPI_ISteamMatchmaking_CreateLobby(void* self, int type, int maxMembers)
{
    Check(self, &g_matchmaking);
    g_state.createLobbyType = type;
    g_state.createLobbyMax = maxMembers;
    // SteamAPICallCompleted_t: the call, LobbyCreated_t's id (513), its size
    uint8_t completed[16];
    uint64_t call = CREATE_LOBBY_CALL;
    int32_t id = 513;
    uint32_t size = 16;
    memcpy(completed, &call, 8);
    memcpy(completed + 8, &id, 4);
    memcpy(completed + 12, &size, 4);
    FakeValve_Queue(703, completed, 16);
    return CREATE_LOBBY_CALL;
}
// Called by the broker itself (a change of Play with strangers), never by the game: the table refuses it.
FAKE_API bool SteamAPI_ISteamMatchmaking_SetLobbyType(void* self, uint64_t lobby, int type)
{
    Check(self, &g_matchmaking);
    g_state.setLobbyTypeCalls++;
    g_state.setLobbyType = type;
    g_state.setLobbyTypeLobby = lobby;
    return true;
}
FAKE_API bool SteamAPI_ISteamMatchmaking_SetLobbyData(void* self, uint64_t lobby, const char* key, const char* value)
{
    Check(self, &g_matchmaking);
    (void)value;
    g_state.lobbyDataCalls++;
    g_state.lobbyDataLobby = lobby;
    strncpy(g_state.lobbyKey, key ? key : "(null)", sizeof g_state.lobbyKey - 1);
    return true;
}
FAKE_API uint64_t SteamAPI_ISteamMatchmaking_GetLobbyOwner(void* self, uint64_t lobby) { Check(self, &g_matchmaking); return lobby == LOBBY ? STEAM_ID : 0; }
FAKE_API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void* self, uint64_t lobby)
{
    Check(self, &g_matchmaking);
    return lobby == LOBBY ? 1 : lobby == HOST_LOBBY ? 2 : lobby == CLOSED_LOBBY ? 3 : 0;
}

// The host's lobby: its host (a co-op peer for Steam P2P) and this player.
FAKE_API uint64_t SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void* self, uint64_t lobby, int index)
{
    Check(self, &g_matchmaking);
    return lobby == HOST_LOBBY ? (index == 0 ? HOST_PEER : index == 1 ? STEAM_ID : 0) : lobby == LOBBY && index == 0 ? STEAM_ID : 0;
}

// ---- Steam P2P: sends recorded, packets in from FakeValve_QueuePacket ----

#define PACKETS 8
static struct { uint64_t remote; int channel; uint32_t size; uint8_t data[4096]; } g_packets[PACKETS];
static int g_packetCount;

FAKE_API void FakeValve_QueuePacket(uint64_t remote, int channel, const void* data, uint32_t size)
{
    EnterCriticalSection(&g_lock);
    if (g_packetCount < PACKETS && size <= sizeof g_packets[0].data)
    {
        g_packets[g_packetCount].remote = remote;
        g_packets[g_packetCount].channel = channel;
        g_packets[g_packetCount].size = size;
        memcpy(g_packets[g_packetCount].data, data, size);
        g_packetCount++;
    }
    LeaveCriticalSection(&g_lock);
}
static int FirstPacket(int channel)
{
    for (int i = 0; i < g_packetCount; i++) if (g_packets[i].channel == channel) return i;
    return -1;
}
FAKE_API bool SteamAPI_ISteamNetworking_IsP2PPacketAvailable(void* self, uint32_t* size, int channel)
{
    Check(self, &g_networking);
    EnterCriticalSection(&g_lock);
    int at = FirstPacket(channel);
    if (size) *size = at >= 0 ? g_packets[at].size : 0;
    LeaveCriticalSection(&g_lock);
    return at >= 0;
}
FAKE_API bool SteamAPI_ISteamNetworking_ReadP2PPacket(void* self, void* dest, uint32_t capacity, uint32_t* size, uint64_t* remote, int channel)
{
    Check(self, &g_networking);
    EnterCriticalSection(&g_lock);
    int at = FirstPacket(channel);
    if (at >= 0)
    {
        memcpy(dest, g_packets[at].data, g_packets[at].size < capacity ? g_packets[at].size : capacity);
        *size = g_packets[at].size;
        *remote = g_packets[at].remote;
        memmove(&g_packets[at], &g_packets[at + 1], sizeof g_packets[0] * (size_t)(g_packetCount - at - 1));
        g_packetCount--;
    }
    LeaveCriticalSection(&g_lock);
    return at >= 0;
}
FAKE_API bool SteamAPI_ISteamNetworking_SendP2PPacket(void* self, uint64_t remote, const void* data, uint32_t size, int type, int channel)
{
    Check(self, &g_networking);
    (void)type; (void)channel;
    g_state.p2pSends++;
    g_state.p2pTo = remote;
    g_state.p2pSize = size;
    g_state.p2pFirst = size && data ? *(const uint8_t*)data : 0;
    return true;
}
FAKE_API bool SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(void* self, uint64_t remote) { Check(self, &g_networking); (void)remote; g_state.p2pAccepts++; return true; }
FAKE_API bool SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(void* self, uint64_t remote) { Check(self, &g_networking); (void)remote; return true; }
// P2PSessionState_t: connection active, then the rest zero
FAKE_API bool SteamAPI_ISteamNetworking_GetP2PSessionState(void* self, uint64_t remote, uint8_t* state)
{
    Check(self, &g_networking);
    (void)remote;
    memset(state, 0, 20);
    state[0] = 1;
    return true;
}

// A co-op join: entering the host's lobby succeeds, the closed one is refused by Steam (LobbyEnter_t's response).
FAKE_API uint64_t SteamAPI_ISteamMatchmaking_JoinLobby(void* self, uint64_t lobby)
{
    Check(self, &g_matchmaking);
    uint64_t call = lobby == HOST_LOBBY ? JOIN_LOBBY_CALL : JOIN_CLOSED_CALL;
    uint8_t completed[16];
    int32_t id = 504;
    uint32_t size = 24;
    memcpy(completed, &call, 8);
    memcpy(completed + 8, &id, 4);
    memcpy(completed + 12, &size, 4);
    FakeValve_Queue(703, completed, 16);
    return call;
}
FAKE_API const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void* self, uint64_t lobby, const char* key)
{
    Check(self, &g_matchmaking);
    return lobby == HOST_LOBBY && key && strcmp(key, "mission") == 0 ? "host-mission" : "";
}

FAKE_API int SteamAPI_IsSteamRunning(void) { return 1; }
FAKE_API void SteamAPI_ManualDispatch_RunFrame(int pipe) { (void)pipe; }

static int g_current = -1;

FAKE_API bool SteamAPI_ManualDispatch_GetNextCallback(int pipe, CallbackMsg* msg)
{
    (void)pipe;
    EnterCriticalSection(&g_lock);
    bool any = g_head != g_tail;
    if (any)
    {
        g_current = g_head % QUEUE;
        msg->steamUser = 1;
        msg->callbackId = g_queue[g_current].id;
        msg->data = g_queue[g_current].data;
        msg->dataSize = g_queue[g_current].size;
    }
    LeaveCriticalSection(&g_lock);
    return any;
}

FAKE_API void SteamAPI_ManualDispatch_FreeLastCallback(int pipe)
{
    (void)pipe;
    EnterCriticalSection(&g_lock);
    if (g_current >= 0) { g_head++; g_current = -1; }
    LeaveCriticalSection(&g_lock);
}

// LobbyCreated_t: k_EResultOK, then the lobby. LobbyEnter_t (a join): the lobby, then the response at 16 (1 entered,
// 2 k_EChatRoomEnterResponseDoesntExist).
FAKE_API bool SteamAPI_ManualDispatch_GetAPICallResult(int pipe, uint64_t call, void* data, int size, int expected, bool* failed)
{
    (void)pipe;
    if ((call == JOIN_LOBBY_CALL || call == JOIN_CLOSED_CALL) && expected == 504 && size >= 24)
    {
        uint64_t lobby = call == JOIN_LOBBY_CALL ? HOST_LOBBY : CLOSED_LOBBY;
        uint32_t response = call == JOIN_LOBBY_CALL ? 1 : 2;
        memset(data, 0, (size_t)size);
        memcpy(data, &lobby, 8);
        memcpy((uint8_t*)data + 16, &response, 4);
        *failed = false;
        return true;
    }
    if (call != CREATE_LOBBY_CALL || expected != 513 || size < 16) return false;
    int32_t result = 1;
    uint64_t lobby = LOBBY;
    memset(data, 0, (size_t)size);
    memcpy(data, &result, 4);
    memcpy((uint8_t*)data + 8, &lobby, 8);
    *failed = false;
    return true;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved)
{
    (void)instance; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
    return TRUE;
}
