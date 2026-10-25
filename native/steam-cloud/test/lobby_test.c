// The game's own Steam lobby between several installations on one PC (broker_lobby.c): two instances' brokers share a
// folder, one makes the lobby, the other enters it, and each sees the other as Steam would show two players. Run by
// Build-Engine.ps1 with the bridge.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "broker_lobby.h"

static int g_failures;
#define EXPECT(cond, what) do { if (cond) printf("  ok    %s\n", what); else { printf("  FAIL  %s\n", what); g_failures++; } } while (0)

typedef uint64_t (*CreateFn)(void*, int32_t, int32_t);
typedef uint64_t (*JoinFn)(void*, uint64_t);
typedef void (*LeaveFn)(void*, uint64_t);
typedef int32_t (*CountFn)(void*, uint64_t);
typedef uint64_t (*MemberFn)(void*, uint64_t, int32_t);
typedef uint64_t (*OwnerFn)(void*, uint64_t);
typedef const char* (*GetDataFn)(void*, uint64_t, const char*);
typedef bool (*SetDataFn)(void*, uint64_t, const char*, const char*);
typedef bool (*JoinableFn)(void*, uint64_t, bool);

#define A 76561199000000002ull
#define B 76561199000000003ull
#define C 76561199000000004ull

// The next result waiting for that instance with this callback id (others are skipped), or false.
static bool Next(BrokerLobbies* s, int32_t id, BrokerLocalResult* out)
{
    for (int i = 0; i < 8; i++)
    {
        if (!BrokerLobbiesNext(s, out)) return false;
        if (out->id == id) return true;
    }
    return false;
}

static uint32_t Response(const BrokerLocalResult* r) { uint32_t v; memcpy(&v, r->data + 16, 4); return v; }

int wmain(int argc, wchar_t** argv)
{
    (void)argc; (void)argv;
    wchar_t folder[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    wchar_t name[64];
    swprintf(name, 64, L"lobbytest-%lu", GetCurrentProcessId());
    wcscat(folder, name);
    CreateFn create = (CreateFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_CreateLobby");
    JoinFn join = (JoinFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_JoinLobby");
    LeaveFn leave = (LeaveFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_LeaveLobby");
    CountFn members = (CountFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    MemberFn member = (MemberFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    OwnerFn owner = (OwnerFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    GetDataFn getData = (GetDataFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_GetLobbyData");
    SetDataFn setData = (SetDataFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_SetLobbyData");
    JoinableFn joinable = (JoinableFn)BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    printf("Local Steam lobby (several installations)\n");
    EXPECT(create && join && leave && members && member && owner && getData && setData && joinable &&
        !BrokerLobbiesResolve("SteamAPI_ISteamMatchmaking_RequestLobbyList"), "the nine bridged matchmaking calls are answered here, nothing else");

    BrokerLobbies* a = BrokerLobbiesOpen(folder, A);
    BrokerLobbies* b = BrokerLobbiesOpen(folder, B);
    BrokerLobbies* c = BrokerLobbiesOpen(folder, C);
    EXPECT(a && b && c, "three instances share the folder");

    // A hosts: its lobby, with data only it may set
    BrokerLobbiesUse(a);
    BrokerLocalResult r;
    uint64_t call = create(NULL, 2, 4);
    int32_t result = 0;
    uint64_t lobby = 0;
    bool made = Next(a, 513, &r) && r.call == call;
    if (made) { memcpy(&result, r.data, 4); memcpy(&lobby, r.data + 8, 8); }
    EXPECT(made && result == 1 && lobby >> 52 == 0x018, "CreateLobby's LobbyCreated_t comes as its call result: k_EResultOK and a lobby id");
    EXPECT(setData(NULL, lobby, "mission", "planet\t261\nback\\slash") && owner(NULL, lobby) == A && members(NULL, lobby) == 1,
        "the host owns it and sets its data");

    // B joins: both see two members, the host as owner, the same data
    BrokerLobbiesUse(b);
    call = join(NULL, lobby);
    bool entered = Next(b, 504, &r) && r.call == call && Response(&r) == 1;
    uint64_t enteredLobby = 0;
    memcpy(&enteredLobby, r.data, 8);
    EXPECT(entered && enteredLobby == lobby, "JoinLobby's LobbyEnter_t says it entered");
    EXPECT(members(NULL, lobby) == 2 && member(NULL, lobby, 0) == A && member(NULL, lobby, 1) == B && owner(NULL, lobby) == A,
        "the joiner sees the host and itself, the host as owner");
    EXPECT(strcmp(getData(NULL, lobby, "mission"), "planet\t261\nback\\slash") == 0 && getData(NULL, lobby, "absent")[0] == 0,
        "it reads the host's data, tabs, newlines and backslashes kept");
    EXPECT(!setData(NULL, lobby, "mission", "mine") && strcmp(getData(NULL, lobby, "mission"), "planet\t261\nback\\slash") == 0,
        "as on Steam, only the owner sets the lobby's data");
    BrokerLobbiesUse(a);
    EXPECT(members(NULL, lobby) == 2 && member(NULL, lobby, 1) == B, "the host sees the joiner among its lobby's members (what the game waits for)");
    EXPECT(Next(a, 505, &r) && memcmp(r.data, &lobby, 8) == 0 && r.data[16] == 1 && r.call == 0, "a change to the lobby's file comes as LobbyDataUpdate_t");

    // Steam P2P between the instances, over their brokers' loopback sockets: the host sends, the joiner reads it with its
    // sender, each channel apart
    typedef bool (*SendFn)(void*, uint64_t, const void*, uint32_t, int32_t, int32_t);
    typedef bool (*AvailableFn)(void*, uint32_t*, int32_t);
    typedef bool (*ReadFn)(void*, void*, uint32_t, uint32_t*, uint64_t*, int32_t);
    typedef bool (*StateFn)(void*, uint64_t, void*);
    SendFn send = (SendFn)BrokerLobbiesResolve("SteamAPI_ISteamNetworking_SendP2PPacket");
    AvailableFn available = (AvailableFn)BrokerLobbiesResolve("SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    ReadFn readPacket = (ReadFn)BrokerLobbiesResolve("SteamAPI_ISteamNetworking_ReadP2PPacket");
    StateFn state = (StateFn)BrokerLobbiesResolve("SteamAPI_ISteamNetworking_GetP2PSessionState");
    static uint8_t bytes[7000];
    for (int i = 0; i < (int)sizeof bytes; i++) bytes[i] = (uint8_t)(i * 13 + 5);
    BrokerLobbiesUse(a);
    uint8_t session[20] = { 0 };
    EXPECT(send && available && readPacket && state && send(NULL, B, bytes, sizeof bytes, 2, 0) && send(NULL, B, "two", 3, 2, 3) &&
        state(NULL, B, session) && session[0] == 1, "the host sends 7000 bytes on channel 0 and 3 on channel 3; the session is active");
    BrokerLobbiesUse(b);
    uint32_t size = 0;
    uint64_t from = 0;
    static uint8_t got[7000];
    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; i++) { arrived = available(NULL, &size, 3); if (!arrived) Sleep(10); }
    EXPECT(arrived && size == 3 && readPacket(NULL, got, sizeof got, &size, &from, 3) && size == 3 && memcmp(got, "two", 3) == 0 && from == A,
        "the joiner reads channel 3's packet, from the host");
    EXPECT(available(NULL, &size, 0) && size == sizeof bytes && readPacket(NULL, got, sizeof got, &size, &from, 0) && memcmp(got, bytes, sizeof bytes) == 0 &&
        from == A && !available(NULL, &size, 0), "and channel 0's 7000 bytes, whole, then nothing more");
    BrokerLobbiesUse(a);
    EXPECT(!available(NULL, &size, 0), "the sender's own socket holds nothing of it");

    // closed, missing and full lobbies
    EXPECT(joinable(NULL, lobby, false), "the host closes it");
    BrokerLobbiesUse(c);
    join(NULL, lobby);
    EXPECT(Next(c, 504, &r) && Response(&r) == 3 && members(NULL, lobby) == 2, "a closed lobby refuses a third instance (k_EChatRoomEnterResponseNotAllowed)");
    join(NULL, lobby + 1);
    EXPECT(Next(c, 504, &r) && Response(&r) == 2, "a lobby no instance made does not exist (k_EChatRoomEnterResponseDoesntExist)");

    // leaving: the host's leaving hands the lobby to the joiner; the last one out removes it
    BrokerLobbiesUse(a);
    leave(NULL, lobby);
    BrokerLobbiesUse(b);
    EXPECT(members(NULL, lobby) == 1 && owner(NULL, lobby) == B, "the host leaves: the joiner owns the lobby");
    BrokerLobbiesClose(b);
    BrokerLobbiesUse(c);
    EXPECT(members(NULL, lobby) == 0 && owner(NULL, lobby) == 0, "the last instance closing leaves it: the lobby is gone");
    BrokerLobbiesClose(a);
    BrokerLobbiesClose(c);

    // the folder holds nothing more
    wchar_t pattern[MAX_PATH + 8];
    swprintf(pattern, MAX_PATH + 8, L"%ls\\*.lobby", folder);
    WIN32_FIND_DATAW found;
    HANDLE any = FindFirstFileW(pattern, &found);
    EXPECT(any == INVALID_HANDLE_VALUE, "no lobby file is left behind");
    if (any != INVALID_HANDLE_VALUE) FindClose(any);
    RemoveDirectoryW(folder);

    printf("%s local Steam lobby: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
