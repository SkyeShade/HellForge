/* The broker's Steam side of the bridge (broker_bridge.h). Every Steam call here runs on the broker's one serving
 * thread: BrokerServe calls the pump between the game's calls. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge_gen.h"
#include "broker_bridge.h"

#define STEAM_API_CALL_COMPLETED 703
#define LOBBY_CREATED 513
#define LOBBY_ENTER 504
#define RESULT_OK 1
/* LobbyEnter_t (pack 8): uint64 m_ulSteamIDLobby, uint32 m_rgfChatPermissions, bool m_bLocked, uint32
 * m_EChatRoomEnterResponse at 16; k_EChatRoomEnterResponseSuccess is 1 */
#define LOBBY_ENTER_RESPONSE 16
#define ENTER_SUCCESS 1
/* ELobbyType */
#define LOBBY_FRIENDS_ONLY 1
#define LOBBY_PUBLIC 2

/* CallbackMsg_t (steam_api_common.h), what ManualDispatch_GetNextCallback fills. */
#pragma pack(push, 8)
typedef struct CallbackMsg { int32_t steamUser; int32_t callbackId; uint8_t* data; int32_t dataSize; } CallbackMsg;
#pragma pack(pop)

typedef void (*RunFrameFn)(int hPipe);
typedef bool (*NextFn)(int hPipe, CallbackMsg* msg);
typedef void (*FreeFn)(int hPipe);
typedef bool (*CallResultFn)(int hPipe, uint64_t call, void* data, int size, int expectedId, bool* failed);
typedef int (*IsRunningFn)(void);

static BrokerBridge* Of(const BrokerApi* api) { return (BrokerBridge*)api->context; }

/* Several installations on one PC (BrokerBridgeMultiInstance): the game's own Steam lobby is answered locally. */
static void* Resolve(const BrokerApi* api, const char* flat)
{
    void* local = Of(api)->localLobbies ? BrokerLobbiesResolve(flat) : NULL;
    return local ? local : (void*)GetProcAddress(Of(api)->valve, flat);
}

/* The accessors of the interfaces the table bridges, each looked up and called once. */
static void* Self(const BrokerApi* api, const char* accessor)
{
    BrokerBridge* b = Of(api);
    for (int i = 0; i < 16 && b->selves[i].name; i++) if (strcmp(b->selves[i].name, accessor) == 0) return b->selves[i].object;
    typedef void* (*AccessorFn)(void);
    AccessorFn fn = (AccessorFn)(void*)GetProcAddress(b->valve, accessor);
    void* object = fn ? fn() : NULL;
    for (int i = 0; i < 16; i++) if (!b->selves[i].name) { b->selves[i].name = accessor; b->selves[i].object = object; break; }
    return object;
}

static int OwnLobby(const BrokerApi* api, uint64_t lobby)
{
    BrokerBridge* b = Of(api);
    for (int i = 0; i < b->lobbyCount; i++) if (b->lobbies[i] == lobby) return 1;
    return 0;
}

static uint32_t Refuse(const BrokerApi* api, const char* why)
{
    Of(api)->refusals++;
    fprintf(stderr, "broker: refused %s\n", why);
    return BRIDGE_STATUS_REFUSED;
}

static void LogValue(const BrokerApi* api, const char* what, const char* value)
{
    (void)api;
    fprintf(stderr, "broker: %s = %.64s\n", what, value ? value : "(null)");
}

/* ---- Play with strangers: the game's own Steam lobby (docs/steam-bridge.md, "The game's own Steam lobby") ----
 * A Public lobby is in every Steam search, with its host's id, PlayFab lobby id and connection string. Strangers can
 * reach no one with Play with strangers off, so the game's Public lobby is made Friends only then (friends and invitees
 * still join), and turned back when it is switched on. Every other type is left as asked. */

static size_t SkipSpace(const char* json, size_t length, size_t at)
{
    while (at < length && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) at++;
    return at;
}

/* Where the string whose text starts at json[i] ends: its closing quote, or length (or past it) when it has none. */
static size_t StringEnd(const char* json, size_t length, size_t i)
{
    while (i < length && json[i] != '"') i += json[i] == '\\' ? 2 : 1;
    return i;
}

/* A string json[start..end) at depth: -1 when it is not the top-level key or no ':' follows it, else whether the
 * key's value is true. */
static int StrangersValue(const char* json, size_t length, size_t start, size_t end, int depth)
{
    static const char key[] = "playWithStrangers";
    if (depth != 1 || end - start != sizeof key - 1 || memcmp(json + start, key, sizeof key - 1) != 0) return -1;
    size_t at = SkipSpace(json, length, end + 1);
    if (at >= length || json[at] != ':') return -1;
    at = SkipSpace(json, length, at + 1);
    return length - at >= 4 && memcmp(json + at, "true", 4) == 0;
}

int BrokerStrangersIn(const char* json, size_t length)
{
    int depth = 0;
    for (size_t i = 0; i < length; i++)
    {
        char c = json[i];
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        else if (c == '"')
        {
            size_t start = ++i;
            i = StringEnd(json, length, i);
            if (i >= length) return 0;
            int value = StrangersValue(json, length, start, i, depth);
            if (value >= 0) return value;
        }
    }
    return 0;
}

/* The settings file as the backend finds it (ModSettings: hellforge-mods.json, else the older bts-mods.json). */
static int SettingsFile(const BrokerBridge* b, wchar_t* path, size_t capacity, WIN32_FILE_ATTRIBUTE_DATA* data)
{
    static const wchar_t* const names[] = { L"hellforge-mods.json", L"bts-mods.json" };
    for (int i = 0; i < 2; i++)
    {
        if (swprintf(path, capacity, L"%ls\\%ls", b->modsFolder, names[i]) < 0) return 0;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, data)) return 1;
    }
    return 0;
}

static int ReadStrangers(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    static char text[1 << 20];
    DWORD read = 0;
    BOOL ok = ReadFile(file, text, sizeof text, &read, NULL);
    CloseHandle(file);
    return ok ? BrokerStrangersIn(text, read) : 0;
}

/* HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1 (a two-PC test, docs/multiplayer.md step 7): Friends only with strangers on too. */
static int AlwaysFriendsOnly(void)
{
    char value[8];
    DWORD n = GetEnvironmentVariableA("HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY", value, sizeof value);
    return n == 1 && value[0] == '1';
}

static void LogLobbyType(const BrokerBridge* b, uint64_t lobby, int type, bool set)
{
    fprintf(stderr, "broker: lobby %llu made %s (Play with strangers %s)%s\n", (unsigned long long)lobby,
        type == LOBBY_PUBLIC ? "Public" : "Friends only", b->strangers ? "on" : "off", set ? "" : ": SetLobbyType failed");
}

/* The game's lobbies it asked to be Public get Friends only or Public again, as the setting now says. */
static void ApplyLobbyTypes(BrokerBridge* b)
{
    typedef bool (*SetTypeFn)(void*, uint64_t, int);
    if (b->localLobbies) return; /* local lobbies are in no Steam search whatever their type */
    SetTypeFn setType = (SetTypeFn)(void*)GetProcAddress(b->valve, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    void* matchmaking = Self(&b->api, "SteamAPI_SteamMatchmaking_v009");
    int type = b->strangers && !AlwaysFriendsOnly() ? LOBBY_PUBLIC : LOBBY_FRIENDS_ONLY;
    for (int i = 0; i < b->lobbyCount; i++)
        if (b->lobbyAsked[i] == LOBBY_PUBLIC)
        {
            bool set = setType && matchmaking && setType(matchmaking, b->lobbies[i], type);
            LogLobbyType(b, b->lobbies[i], type, set);
        }
}

/* Looks at the settings file (at most every BROKER_STRANGERS_MS unless now), reading it only when its time stamp moved. */
static void CheckStrangers(BrokerBridge* b, int now)
{
    if (!b->modsFolder[0]) return;
    DWORD tick = GetTickCount();
    if (!now && tick - b->strangersChecked < BROKER_STRANGERS_MS) return;
    b->strangersChecked = tick;
    wchar_t path[600];
    WIN32_FILE_ATTRIBUTE_DATA data;
    int found = SettingsFile(b, path, sizeof path / sizeof path[0], &data);
    FILETIME stamp = found ? data.ftLastWriteTime : (FILETIME){ 0, 0 };
    if (!now && CompareFileTime(&stamp, &b->strangersStamp) == 0) return;
    b->strangersStamp = stamp;
    int strangers = found && ReadStrangers(path);
    if (strangers == b->strangers && !now) return;
    int changed = strangers != b->strangers;
    b->strangers = strangers;
    fprintf(stderr, "broker: Play with strangers %s\n", strangers ? "on" : "off");
    if (changed) ApplyLobbyTypes(b);
}

void BrokerBridgeWatchStrangers(BrokerBridge* b, const wchar_t* modsFolder)
{
    if (!modsFolder || wcslen(modsFolder) >= sizeof b->modsFolder / sizeof b->modsFolder[0]) return;
    wcscpy(b->modsFolder, modsFolder);
    CheckStrangers(b, 1);
}

static int64_t Adjust(const BrokerApi* api, uint32_t method, const char* argument, int64_t value)
{
    BrokerBridge* b = Of(api);
    if (method != BRIDGE_M_SteamMatchMaking009_CreateLobby || strcmp(argument, "eLobbyType") != 0) return value;
    CheckStrangers(b, 0);
    int64_t given = value == LOBBY_PUBLIC && (!b->strangers || AlwaysFriendsOnly()) ? LOBBY_FRIENDS_ONLY : value;
    if (b->pendingCount == BROKER_PENDING_LOBBIES)
    {
        memmove(b->pendingAsked, b->pendingAsked + 1, sizeof b->pendingAsked[0] * (BROKER_PENDING_LOBBIES - 1));
        b->pendingCount--;
    }
    b->pendingAsked[b->pendingCount++] = (int32_t)value;
    fprintf(stderr, "broker: CreateLobby asks lobby type %lld; Steam gets %lld (Play with strangers %s)\n", (long long)value, (long long)given,
        b->strangers ? "on" : "off");
    return given;
}

/* A lobby the game entered (LobbyEnter_t, the result of its JoinLobby or a callback) is the game's too, for the table's
 * ownLobby checks: a co-op joiner reads the host's Steam lobby's data and members (census 2026-10-24). Only a
 * successful entry counts; the game asked nothing about its type, so a change of Play with strangers leaves it alone. */
static void RememberEntered(BrokerBridge* b, const uint8_t* data, uint32_t size)
{
    if (!data || size < LOBBY_ENTER_RESPONSE + 4) return;
    uint64_t lobby;
    uint32_t response;
    memcpy(&lobby, data, 8);
    memcpy(&response, data + LOBBY_ENTER_RESPONSE, 4);
    if (response != ENTER_SUCCESS || !lobby || OwnLobby(&b->api, lobby) || b->lobbyCount >= BROKER_MAX_LOBBIES) return;
    b->lobbyAsked[b->lobbyCount] = -1;
    b->lobbies[b->lobbyCount++] = lobby;
    fprintf(stderr, "broker: the game entered lobby %llu\n", (unsigned long long)lobby);
}

/* ---- Steam P2P (docs/steam-bridge.md "Steam P2P"): only with the game's co-op peers ---- */

static uint64_t FileTicks(FILETIME t) { return (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime; }

/* The backend's peers file, read when its time stamp moved (looked at every 500 ms, or now). Writes within one tick of
 * the system clock (about 16 ms: a lobby change and a join's peer) get the same stamp, so an unchanged stamp is trusted
 * only once it was read more than a second after that stamp: a read between two such writes is not kept. */
static void LoadPeers(BrokerBridge* b, int now)
{
    if (!b->peersFile[0]) return;
    DWORD tick = GetTickCount();
    if (!now && tick - b->peersChecked < 500) return;
    b->peersChecked = tick;
    WIN32_FILE_ATTRIBUTE_DATA data;
    FILETIME stamp = GetFileAttributesExW(b->peersFile, GetFileExInfoStandard, &data) ? data.ftLastWriteTime : (FILETIME){ 0, 0 };
    if (CompareFileTime(&stamp, &b->peersStamp) == 0 && FileTicks(b->peersRead) > FileTicks(stamp) + 10000000ull) return;
    b->peersStamp = stamp;
    GetSystemTimeAsFileTime(&b->peersRead);
    b->peerCount = 0;
    HANDLE file = CreateFileW(b->peersFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[BROKER_MAX_PEERS * 24 + 1];
    DWORD read = 0;
    BOOL ok = ReadFile(file, text, sizeof text - 1, &read, NULL);
    CloseHandle(file);
    if (!ok) return;
    text[read] = 0;
    for (char* at = text; *at && b->peerCount < BROKER_MAX_PEERS;)
    {
        char* end = NULL;
        unsigned long long id = strtoull(at, &end, 10);
        if (end == at) { at++; continue; }
        if (id) b->peers[b->peerCount++] = id;
        at = end;
    }
}

static int Listed(const BrokerBridge* b, uint64_t id)
{
    for (int i = 0; i < b->peerCount; i++) if (b->peers[i] == id) return 1;
    return 0;
}

/* One of the game's co-op peers: a SteamID the backend lists, nothing else. A member of a lobby the game entered is not
 * enough (reviewed 2026-10-24): JoinLobby takes any lobby id, so a compromised game could enter any lobby it knows and
 * reach its members. The backend lists a join's peers before the game sees the join; a join is often seconds old when
 * the host first sends, so a miss reads the backend's file again at once. */
static int P2PPeer(const BrokerApi* api, uint64_t id)
{
    BrokerBridge* b = Of(api);
    if (!id) return 0;
    LoadPeers(b, 0);
    if (Listed(b, id)) return 1;
    LoadPeers(b, 1);
    return Listed(b, id);
}

void BrokerBridgeWatchPeers(BrokerBridge* b, const wchar_t* peersFile)
{
    if (!peersFile || wcslen(peersFile) >= sizeof b->peersFile / sizeof b->peersFile[0]) return;
    wcscpy(b->peersFile, peersFile);
    LoadPeers(b, 1);
}

/* A packet the game does not get, read only so it never piles up in Steam: larger than capacity or BRIDGE_P2P_MAX, or
 * not from a co-op peer. The first few are logged. */
static bool DropUnwanted(BrokerBridge* b, uint32_t size, uint32_t capacity, uint64_t remote, int32_t channel)
{
    if (size <= capacity && size <= BRIDGE_P2P_MAX && P2PPeer(&b->api, remote)) return false;
    if (b->p2pDropped++ < 8)
        fprintf(stderr, "broker: P2P packet of %u bytes from %llu on channel %d dropped (%s)\n", size, (unsigned long long)remote, channel,
            size > BRIDGE_P2P_MAX ? "too large" : "not a co-op peer");
    return true;
}

/* A co-op peer's packet into the ring, in pieces (BridgeP2PPiece); a full ring loses the rest of it. */
static void PassPacket(BrokerBridge* b, BrokerChannel* ch, const uint8_t* packet, uint32_t size, uint64_t remote, int32_t channel)
{
    uint8_t piece[BRIDGE_RING_DATA_SIZE];
    BridgeP2PPiece head = { remote, channel, size, 0, 0 };
    do
    {
        uint32_t part = size - head.offset < BRIDGE_P2P_PIECE ? size - head.offset : BRIDGE_P2P_PIECE;
        memcpy(piece, &head, sizeof head);
        memcpy(piece + sizeof head, packet + head.offset, part);
        if (!BrokerChannelPushCallback(ch, BRIDGE_P2P_PACKET, piece, (uint32_t)sizeof head + part))
        {
            if (b->p2pDropped++ < 8) fprintf(stderr, "broker: the ring is full; a P2P packet of %u bytes is lost\n", size);
            break;
        }
        head.offset += part;
    } while (head.offset < size);
}

/* Packets waiting for the game, from its co-op peers, into the ring in pieces (BridgeP2PPiece). Others are read and
 * dropped, so they never pile up in Steam. */
static void PollP2P(BrokerBridge* b, BrokerChannel* ch)
{
    typedef bool (*AvailableFn)(void*, uint32_t*, int32_t);
    typedef bool (*ReadFn)(void*, void*, uint32_t, uint32_t*, uint64_t*, int32_t);
    static AvailableFn available;
    static ReadFn read;
    if (!available) available = (AvailableFn)Resolve(&b->api, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    if (!read) read = (ReadFn)Resolve(&b->api, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    void* networking = Self(&b->api, "SteamAPI_SteamNetworking_v006");
    if (!available || !read || !networking) return;
    static uint8_t packet[64 * 1024];
    for (int32_t channel = 0; channel < BROKER_P2P_CHANNELS; channel++)
        for (int n = 0; n < BROKER_P2P_PER_PUMP; n++)
        {
            uint32_t size = 0;
            uint64_t remote = 0;
            if (!available(networking, &size, channel) || !read(networking, packet, sizeof packet, &size, &remote, channel)) break;
            if (DropUnwanted(b, size, sizeof packet, remote, channel)) continue;
            PassPacket(b, ch, packet, size, remote, channel);
        }
}

static bool Allowed(int id)
{
    for (int i = 0; i < BRIDGE_CALLBACK_COUNT; i++) if (g_bridgeCallbacks[i] == id) return true;
    return false;
}

/* SteamAPICallCompleted_t: the call, the result's callback id and size. Its result goes to the game behind the call-result
 * header; a LobbyCreated_t that succeeded names a lobby the game may then use. */
static void PassCallResult(BrokerBridge* b, BrokerChannel* ch, uint64_t call, int32_t id, bool failed, uint8_t* slot, uint32_t resultSize);

static void CallCompleted(BrokerBridge* b, BrokerChannel* ch, const uint8_t* data, int32_t size)
{
    if (size < 16) return;
    uint64_t call;
    int32_t id;
    uint32_t resultSize;
    memcpy(&call, data, 8);
    memcpy(&id, data + 8, 4);
    memcpy(&resultSize, data + 12, 4);
    uint8_t slot[BRIDGE_RING_DATA_SIZE];
    if (resultSize > sizeof slot - BRIDGE_CALL_RESULT_HEADER || !Allowed(id))
    {
        fprintf(stderr, "broker: call result %d (%u bytes) not passed on\n", id, resultSize);
        return;
    }
    CallResultFn result = (CallResultFn)(void*)GetProcAddress(b->valve, "SteamAPI_ManualDispatch_GetAPICallResult");
    bool failed = false;
    memset(slot, 0, BRIDGE_CALL_RESULT_HEADER + resultSize);
    if (!result || !result(b->hPipe, call, slot + BRIDGE_CALL_RESULT_HEADER, (int)resultSize, id, &failed)) failed = true;
    PassCallResult(b, ch, call, id, failed, slot, resultSize);
}

/* A call result for the game (slot: the header's room, then the result): the lobbies it names are remembered for the
 * table's ownLobby checks, then it goes into the ring behind the call-result header. */
static void PassCallResult(BrokerBridge* b, BrokerChannel* ch, uint64_t call, int32_t id, bool failed, uint8_t* slot, uint32_t resultSize)
{
    memcpy(slot, &call, 8);
    slot[8] = failed ? 1 : 0;
    if (id == LOBBY_CREATED)
    {
        /* the oldest CreateLobby waiting is this one's */
        int32_t asked = b->pendingCount > 0 ? b->pendingAsked[0] : -1;
        if (b->pendingCount > 0) memmove(b->pendingAsked, b->pendingAsked + 1, sizeof b->pendingAsked[0] * (size_t)--b->pendingCount);
        int32_t eResult = 0;
        uint64_t lobby = 0;
        if (!failed && resultSize >= 16)
        {
            memcpy(&eResult, slot + BRIDGE_CALL_RESULT_HEADER, 4);
            memcpy(&lobby, slot + BRIDGE_CALL_RESULT_HEADER + 8, 8);
        }
        if (eResult == RESULT_OK && lobby && b->lobbyCount < BROKER_MAX_LOBBIES)
        {
            b->lobbyAsked[b->lobbyCount] = asked;
            b->lobbies[b->lobbyCount++] = lobby;
        }
    }
    else if (id == LOBBY_ENTER && !failed) RememberEntered(b, slot + BRIDGE_CALL_RESULT_HEADER, resultSize);
    BrokerChannelPushCallback(ch, (uint32_t)id | BRIDGE_CALL_RESULT_FLAG, slot, BRIDGE_CALL_RESULT_HEADER + resultSize);
}

/* One callback of Steam's manual dispatch: a call's completion, or a callback for the game when the table lets it through. */
static void PassCallback(BrokerBridge* b, BrokerChannel* ch, const CallbackMsg* msg)
{
    uint32_t size = msg->dataSize > 0 ? (uint32_t)msg->dataSize : 0;
    if (msg->callbackId == STEAM_API_CALL_COMPLETED) { CallCompleted(b, ch, msg->data, msg->dataSize); return; }
    if (msg->callbackId == LOBBY_ENTER) RememberEntered(b, msg->data, size);
    if (Allowed(msg->callbackId) && !BrokerChannelPushCallback(ch, (uint32_t)msg->callbackId, msg->data, size) && size > BRIDGE_RING_DATA_SIZE)
        fprintf(stderr, "broker: callback %d of %u bytes does not fit a ring slot; dropped\n", msg->callbackId, size);
}

/* The local lobby's call results and data updates (BrokerBridgeMultiInstance). */
static void PassLocalResults(BrokerBridge* b, BrokerChannel* ch)
{
    BrokerLocalResult local;
    while (b->localLobbies && BrokerLobbiesNext(b->localLobbies, &local))
    {
        uint8_t slot[BRIDGE_CALL_RESULT_HEADER + sizeof local.data];
        memset(slot, 0, sizeof slot);
        memcpy(slot + BRIDGE_CALL_RESULT_HEADER, local.data, local.size);
        if (local.call) PassCallResult(b, ch, local.call, local.id, false, slot, local.size);
        else if (Allowed(local.id)) BrokerChannelPushCallback(ch, (uint32_t)local.id, local.data, local.size);
    }
}

static void Pump(void* context, BrokerChannel* ch)
{
    BrokerBridge* b = (BrokerBridge*)context;
    NextFn next = (NextFn)b->next;
    b->runFrame(b->hPipe);
    CallbackMsg msg;
    static uint32_t reportedDrops;
    while (next(b->hPipe, &msg))
    {
        PassCallback(b, ch, &msg);
        b->freeLast(b->hPipe);
    }
    PassLocalResults(b, ch);
    if (ch->dropped > 0 && reportedDrops == 0) { fprintf(stderr, "broker: the ring is full; callbacks are being dropped\n"); reportedDrops = 1; }
    PollP2P(b, ch);
    CheckStrangers(b, 0);
    DWORD now = GetTickCount();
    if (!b->snapshotDone || now - b->lastSnapshot >= BROKER_SNAPSHOT_MS) BrokerBridgeSnapshot(b, ch);
}

static int IsRunning(void* context)
{
    IsRunningFn fn = (IsRunningFn)(void*)GetProcAddress(((BrokerBridge*)context)->valve, "SteamAPI_IsSteamRunning");
    return fn ? fn() : 0;
}

#define CALL(type, name, ...) ((type)(void*)GetProcAddress(b->valve, name))(__VA_ARGS__)
#define HAVE(name) (GetProcAddress(b->valve, name) != NULL)

static void Put(char* to, size_t capacity, const char* from)
{
    if (!from) { to[0] = 0; return; }
    size_t n = strnlen(from, capacity - 1);
    memcpy(to, from, n);
    to[n] = 0;
}

/* The player's part of the snapshot: signed on, SteamID, persona name, IP country, Steam Deck. */
static void SnapshotPlayer(BrokerBridge* b, BridgeSnapshot* next, void* user, void* friends, void* utils)
{
    if (user && HAVE("SteamAPI_ISteamUser_BLoggedOn")) next->loggedOn = CALL(bool (*)(void*), "SteamAPI_ISteamUser_BLoggedOn", user) ? 1 : 0;
    if (user && HAVE("SteamAPI_ISteamUser_GetSteamID")) next->steamId = CALL(uint64_t (*)(void*), "SteamAPI_ISteamUser_GetSteamID", user);
    if (friends && HAVE("SteamAPI_ISteamFriends_GetPersonaName"))
        Put(next->personaName, sizeof next->personaName, CALL(const char* (*)(void*), "SteamAPI_ISteamFriends_GetPersonaName", friends));
    if (utils && HAVE("SteamAPI_ISteamUtils_GetIPCountry"))
        Put(next->ipCountry, sizeof next->ipCountry, CALL(const char* (*)(void*), "SteamAPI_ISteamUtils_GetIPCountry", utils));
    if (utils && HAVE("SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck"))
        next->steamDeck = CALL(bool (*)(void*), "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck", utils) ? 1 : 0;
}

/* The DLCs Steam lists for the game, up to BRIDGE_DLC_MAX: app id, name, available, subscribed. */
static void SnapshotDlc(BrokerBridge* b, BridgeSnapshot* next, void* apps)
{
    int count = CALL(int (*)(void*), "SteamAPI_ISteamApps_GetDLCCount", apps);
    for (int i = 0; i < count && next->dlcCount < BRIDGE_DLC_MAX; i++)
    {
        BridgeSnapshotDlc* d = &next->dlc[next->dlcCount];
        bool available = false;
        if (!CALL(bool (*)(void*, int, uint32_t*, bool*, char*, int), "SteamAPI_ISteamApps_BGetDLCDataByIndex", apps, i, &d->appId, &available,
            d->name, (int)sizeof d->name - 1)) continue;
        d->name[sizeof d->name - 1] = 0;
        d->available = available ? 1 : 0;
        d->subscribed = HAVE("SteamAPI_ISteamApps_BIsSubscribedApp") &&
            CALL(bool (*)(void*, uint32_t), "SteamAPI_ISteamApps_BIsSubscribedApp", apps, d->appId) ? 1 : 0;
        next->dlcCount++;
    }
}

/* The game's part of the snapshot: language, beta branch, DLCs. */
static void SnapshotApps(BrokerBridge* b, BridgeSnapshot* next, void* apps)
{
    if (apps && HAVE("SteamAPI_ISteamApps_GetCurrentGameLanguage"))
        Put(next->language, sizeof next->language, CALL(const char* (*)(void*), "SteamAPI_ISteamApps_GetCurrentGameLanguage", apps));
    if (apps && HAVE("SteamAPI_ISteamApps_GetCurrentBetaName"))
        next->hasBeta = CALL(bool (*)(void*, char*, int), "SteamAPI_ISteamApps_GetCurrentBetaName", apps, next->betaName, (int)sizeof next->betaName - 1) ? 1 : 0;
    if (apps && HAVE("SteamAPI_ISteamApps_GetDLCCount") && HAVE("SteamAPI_ISteamApps_BGetDLCDataByIndex")) SnapshotDlc(b, next, apps);
}

/* next into the shared snapshot under its seqlock: odd while written. */
static void PublishSnapshot(BridgeSnapshot* s, const BridgeSnapshot* next)
{
    uint32_t version = s->version;
    __atomic_store_n(&s->version, version | 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy((uint8_t*)s + sizeof s->version, (const uint8_t*)next + sizeof next->version, sizeof *next - sizeof next->version);
    s->ready = 1;
    __atomic_store_n(&s->version, (version | 1) + 1, __ATOMIC_RELEASE);
}

void BrokerBridgeSnapshot(BrokerBridge* b, BrokerChannel* ch)
{
    BridgeSnapshot* s = BridgeSnapshotOf(ch->ring);
    BridgeSnapshot next;
    memset(&next, 0, sizeof next);
    void* user = Self(&b->api, "SteamAPI_SteamUser_v021");
    void* friends = Self(&b->api, "SteamAPI_SteamFriends_v017");
    void* utils = Self(&b->api, "SteamAPI_SteamUtils_v010");
    void* apps = Self(&b->api, "SteamAPI_SteamApps_v008");
    SnapshotPlayer(b, &next, user, friends, utils);
    SnapshotApps(b, &next, apps);
    next.betaName[sizeof next.betaName - 1] = 0;
    /* several installations on one PC: the game is this instance's player, not the Steam account's */
    if (b->identity)
    {
        next.steamId = b->identity;
        Put(next.personaName, sizeof next.personaName, b->persona);
    }
    PublishSnapshot(s, &next);
    b->lastSnapshot = GetTickCount();
    b->snapshotDone = 1;
}

int BrokerBridgeOpen(BrokerBridge* b, HMODULE valve, int hPipe)
{
    memset(b, 0, sizeof *b);
    b->valve = valve;
    b->hPipe = hPipe;
    if (!valve) return 0;
    b->runFrame = (RunFrameFn)(void*)GetProcAddress(valve, "SteamAPI_ManualDispatch_RunFrame");
    b->next = (void*)GetProcAddress(valve, "SteamAPI_ManualDispatch_GetNextCallback");
    b->freeLast = (FreeFn)(void*)GetProcAddress(valve, "SteamAPI_ManualDispatch_FreeLastCallback");
    if (!b->runFrame || !b->next || !b->freeLast) return 0;
    b->api = (BrokerApi){ b, Resolve, Self, OwnLobby, Refuse, LogValue, P2PPeer, Adjust, BrokerDispatchMethod };
    b->steam = (BrokerSteam){ b, Pump, IsRunning, &b->api };
    return 1;
}

int BrokerBridgeMultiInstance(BrokerBridge* b, const wchar_t* lobbyFolder, uint64_t identity, const char* persona)
{
    /* a SteamID64 of an individual account in the public universe, as HELLFORGE_IDENTITY must be (PlayFabService) */
    if (identity >> 56 != 1 || (identity >> 52 & 0xF) != 1) return 0;
    b->localLobbies = BrokerLobbiesOpen(lobbyFolder, identity);
    if (!b->localLobbies) return 0;
    BrokerLobbiesUse(b->localLobbies);
    b->identity = identity;
    Put(b->persona, sizeof b->persona, persona);
    fprintf(stderr, "broker: several installations: this game is %llu (%s); its Steam lobby is kept with the others' locally\n",
        (unsigned long long)identity, b->persona);
    return 1;
}
