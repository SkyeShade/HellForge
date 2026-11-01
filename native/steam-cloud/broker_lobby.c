/* The game's own Steam lobby between several installations on one PC (broker_lobby.h). A lobby is a text file in the
 * shared folder, <lobby id in hex>.lobby:
 *   owner <id>, max <n>, joinable 0|1, a "member <id>" line each, a "data <key>\t<value>" line each (\\ \t \n \r escaped)
 * and every change takes <id>.lock first, reads the file, and writes it again whole (a new file moved over the old). All
 * of it runs on the broker's one serving thread. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "broker_lobby.h"

#define LOBBY_CREATED 513
#define LOBBY_ENTER 504
#define LOBBY_DATA_UPDATE 505
/* a lobby's SteamID: universe 1, a chat account, the lobby instance flag (as Steam's own lobby ids read) */
#define LOBBY_BASE 0x0186000000000000ull
/* EChatRoomEnterResponse */
#define ENTER_SUCCESS 1
#define ENTER_DOESNT_EXIST 2
#define ENTER_NOT_ALLOWED 3
#define ENTER_FULL 4
#define FILE_LIMIT (2u << 20)

typedef struct Lobby {
    uint64_t id, owner;
    int max;
    bool joinable;
    int memberCount;
    uint64_t members[BROKER_LOBBY_MEMBERS];
    int keyCount;
    char keys[BROKER_LOBBY_KEYS][BROKER_LOBBY_KEY];
    char values[BROKER_LOBBY_KEYS][BROKER_LOBBY_VALUE];
} Lobby;

struct BrokerLobbies {
    wchar_t folder[520];
    uint64_t self;
    uint32_t counter;
    int inCount;
    uint64_t in[BROKER_LOBBY_MEMBERS];      /* the lobbies this instance made or entered */
    FILETIME seen[BROKER_LOBBY_MEMBERS];    /* each one's file time when last reported (LobbyDataUpdate_t) */
    int pendingCount;
    BrokerLocalResult pending[BROKER_LOBBY_PENDING];
    /* Steam P2P between the instances: this broker's loopback socket, and the packets that arrived on it */
    SOCKET udp;
    int packetCount;
    struct { uint64_t remote; int32_t channel; uint32_t size; uint8_t data[BROKER_LOCAL_PACKET]; } packets[BROKER_LOCAL_PACKETS];
};

/* A datagram between two instances' brokers: the sender's SteamID, the channel, the size, then the packet. */
typedef struct LocalDatagram { uint64_t from; int32_t channel; uint32_t size; } LocalDatagram;

static BrokerLobbies* g_current;
static Lobby g_lobby;                       /* the lobby last read, or being changed */
static char g_answer[BROKER_LOBBY_VALUE];   /* GetLobbyData's answer, valid until the next call */

static void PathOf(const BrokerLobbies* s, uint64_t lobby, const wchar_t* suffix, wchar_t* path, size_t capacity)
{
    swprintf(path, capacity, L"%ls\\%016llX%ls", s->folder, (unsigned long long)lobby, suffix);
}

/* The lobby's lock: a file only one broker can hold, gone when closed. Waits up to a second. */
static HANDLE Lock(const BrokerLobbies* s, uint64_t lobby)
{
    wchar_t path[600];
    PathOf(s, lobby, L".lock", path, 600);
    for (int i = 0; i < 200; i++)
    {
        HANDLE lock = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (lock != INVALID_HANDLE_VALUE) return lock;
        Sleep(5);
    }
    return INVALID_HANDLE_VALUE;
}

static void UnescapeText(char* text)
{
    char* to = text;
    for (const char* from = text; *from; from++)
    {
        if (*from != '\\' || !from[1]) { *to++ = *from; continue; }
        from++;
        *to++ = *from == 't' ? '\t' : *from == 'n' ? '\n' : *from == 'r' ? '\r' : *from;
    }
    *to = 0;
}

static size_t EscapeText(char* to, size_t capacity, const char* from)
{
    size_t n = 0;
    for (; *from && n + 2 < capacity; from++)
    {
        char c = *from;
        if (c == '\\' || c == '\t' || c == '\n' || c == '\r')
        {
            to[n++] = '\\';
            to[n++] = c == '\t' ? 't' : c == '\n' ? 'n' : c == '\r' ? 'r' : '\\';
        }
        else to[n++] = c;
    }
    to[n] = 0;
    return n;
}

/* Reads the lobby's file into g_lobby; false when there is none (or it is not one). */
static bool Read(const BrokerLobbies* s, uint64_t lobby)
{
    wchar_t path[600];
    PathOf(s, lobby, L".lobby", path, 600);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    char* text = (char*)malloc(FILE_LIMIT + 1);
    DWORD size = 0;
    BOOL ok = text && ReadFile(file, text, FILE_LIMIT, &size, NULL);
    CloseHandle(file);
    if (!ok) { free(text); return false; }
    text[size] = 0;
    memset(&g_lobby, 0, sizeof g_lobby);
    g_lobby.id = lobby;
    char* save = NULL;
    for (char* line = strtok_s(text, "\n", &save); line; line = strtok_s(NULL, "\n", &save))
    {
        unsigned long long id;
        int number;
        if (sscanf(line, "owner %llx", &id) == 1) g_lobby.owner = id;
        else if (sscanf(line, "max %d", &number) == 1) g_lobby.max = number;
        else if (sscanf(line, "joinable %d", &number) == 1) g_lobby.joinable = number != 0;
        else if (sscanf(line, "member %llx", &id) == 1 && g_lobby.memberCount < BROKER_LOBBY_MEMBERS) g_lobby.members[g_lobby.memberCount++] = id;
        else if (strncmp(line, "data ", 5) == 0 && g_lobby.keyCount < BROKER_LOBBY_KEYS)
        {
            char* tab = strchr(line + 5, '\t');
            if (!tab) continue;
            *tab = 0;
            UnescapeText(line + 5);
            UnescapeText(tab + 1);
            strncpy(g_lobby.keys[g_lobby.keyCount], line + 5, BROKER_LOBBY_KEY - 1);
            strncpy(g_lobby.values[g_lobby.keyCount], tab + 1, BROKER_LOBBY_VALUE - 1);
            g_lobby.keyCount++;
        }
    }
    free(text);
    return g_lobby.owner != 0;
}

/* Writes g_lobby to its file (a new file moved over the old), or removes the file when it has no members left. */
static bool Write(const BrokerLobbies* s)
{
    wchar_t path[600], partial[620];
    PathOf(s, g_lobby.id, L".lobby", path, 600);
    if (g_lobby.memberCount == 0) return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
    size_t capacity = 1024 + (size_t)g_lobby.keyCount * 2 * (BROKER_LOBBY_KEY + BROKER_LOBBY_VALUE) + 64 * BROKER_LOBBY_MEMBERS;
    char* text = (char*)malloc(capacity);
    if (!text) return false;
    size_t n = (size_t)snprintf(text, capacity, "owner %016llX\nmax %d\njoinable %d\n", (unsigned long long)g_lobby.owner, g_lobby.max, g_lobby.joinable ? 1 : 0);
    for (int i = 0; i < g_lobby.memberCount; i++) n += (size_t)snprintf(text + n, capacity - n, "member %016llX\n", (unsigned long long)g_lobby.members[i]);
    for (int i = 0; i < g_lobby.keyCount; i++)
    {
        memcpy(text + n, "data ", 5);
        n += 5;
        n += EscapeText(text + n, capacity - n, g_lobby.keys[i]);
        text[n++] = '\t';
        n += EscapeText(text + n, capacity - n, g_lobby.values[i]);
        text[n++] = '\n';
    }
    swprintf(partial, 620, L"%ls.%016llX", path, (unsigned long long)s->self);
    HANDLE file = CreateFileW(partial, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wrote = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, text, (DWORD)n, &wrote, NULL) && wrote == n;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(text);
    return ok && MoveFileExW(partial, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static int MemberIndex(uint64_t member)
{
    for (int i = 0; i < g_lobby.memberCount; i++) if (g_lobby.members[i] == member) return i;
    return -1;
}

static void Track(BrokerLobbies* s, uint64_t lobby, bool in)
{
    for (int i = 0; i < s->inCount; i++)
        if (s->in[i] == lobby)
        {
            if (!in) { s->in[i] = s->in[--s->inCount]; s->seen[i] = s->seen[s->inCount]; }
            return;
        }
    if (in && s->inCount < BROKER_LOBBY_MEMBERS) { s->in[s->inCount] = lobby; memset(&s->seen[s->inCount], 0, sizeof s->seen[0]); s->inCount++; }
}

static uint64_t Queue(BrokerLobbies* s, int32_t id, const void* data, uint32_t size)
{
    uint64_t call = 0x4846000000000000ull | ((s->self & 0xFFFF) << 24) | (++s->counter & 0xFFFFFF);
    if (s->pendingCount < BROKER_LOBBY_PENDING && size <= sizeof s->pending[0].data)
    {
        BrokerLocalResult* r = &s->pending[s->pendingCount++];
        r->call = call;
        r->id = id;
        r->size = size;
        memset(r->data, 0, sizeof r->data);
        memcpy(r->data, data, size);
    }
    return call;
}

/* ---- the flat ISteamMatchmaking functions (self is the interface object, unused) ---- */

static uint64_t CreateLobby(void* self, int32_t type, int32_t max)
{
    (void)self; (void)type; /* unlisted between the instances whatever the type: the shared folder is the only way in */
    BrokerLobbies* s = g_current;
    uint8_t created[16] = { 0 };
    uint64_t lobby = LOBBY_BASE | ((s->self & 0xFFFFF) << 12) | ((s->counter + 1) & 0xFFF);
    HANDLE lock = Lock(s, lobby);
    if (lock != INVALID_HANDLE_VALUE)
    {
        memset(&g_lobby, 0, sizeof g_lobby);
        g_lobby.id = lobby;
        g_lobby.owner = s->self;
        g_lobby.max = max;
        g_lobby.joinable = true;
        g_lobby.members[g_lobby.memberCount++] = s->self;
        if (Write(s))
        {
            int32_t ok = 1;
            memcpy(created, &ok, 4);
            memcpy(created + 8, &lobby, 8);
            Track(s, lobby, true);
        }
        CloseHandle(lock);
    }
    if (created[0] == 0) { int32_t fail = 2; memcpy(created, &fail, 4); } /* k_EResultFail */
    fprintf(stderr, "broker: local lobby %016llX made for %016llX\n", (unsigned long long)lobby, (unsigned long long)s->self);
    return Queue(s, LOBBY_CREATED, created, sizeof created);
}

static uint64_t JoinLobby(void* self, uint64_t lobby)
{
    (void)self;
    BrokerLobbies* s = g_current;
    uint32_t response = ENTER_DOESNT_EXIST;
    HANDLE lock = Lock(s, lobby);
    if (lock != INVALID_HANDLE_VALUE)
    {
        if (Read(s, lobby))
        {
            if (MemberIndex(s->self) >= 0) response = ENTER_SUCCESS;
            else if (!g_lobby.joinable) response = ENTER_NOT_ALLOWED;
            else if (g_lobby.memberCount >= g_lobby.max || g_lobby.memberCount >= BROKER_LOBBY_MEMBERS) response = ENTER_FULL;
            else
            {
                g_lobby.members[g_lobby.memberCount++] = s->self;
                if (Write(s)) response = ENTER_SUCCESS;
            }
        }
        CloseHandle(lock);
    }
    if (response == ENTER_SUCCESS) Track(s, lobby, true);
    fprintf(stderr, "broker: local lobby %016llX entered by %016llX: response %u\n", (unsigned long long)lobby, (unsigned long long)s->self, response);
    /* LobbyEnter_t: the lobby, permissions, locked, the response at 16 */
    uint8_t entered[24] = { 0 };
    memcpy(entered, &lobby, 8);
    memcpy(entered + 16, &response, 4);
    return Queue(s, LOBBY_ENTER, entered, sizeof entered);
}

static void LeaveLobby(void* self, uint64_t lobby)
{
    (void)self;
    BrokerLobbies* s = g_current;
    Track(s, lobby, false);
    HANDLE lock = Lock(s, lobby);
    if (lock == INVALID_HANDLE_VALUE) return;
    int at;
    if (Read(s, lobby) && (at = MemberIndex(s->self)) >= 0)
    {
        g_lobby.members[at] = g_lobby.members[--g_lobby.memberCount];
        if (g_lobby.owner == s->self && g_lobby.memberCount > 0) g_lobby.owner = g_lobby.members[0];
        Write(s);
    }
    CloseHandle(lock);
}

static int32_t GetNumLobbyMembers(void* self, uint64_t lobby) { (void)self; return Read(g_current, lobby) ? g_lobby.memberCount : 0; }

static uint64_t GetLobbyMemberByIndex(void* self, uint64_t lobby, int32_t index)
{
    (void)self;
    return Read(g_current, lobby) && index >= 0 && index < g_lobby.memberCount ? g_lobby.members[index] : 0;
}

static uint64_t GetLobbyOwner(void* self, uint64_t lobby) { (void)self; return Read(g_current, lobby) ? g_lobby.owner : 0; }

static const char* GetLobbyData(void* self, uint64_t lobby, const char* key)
{
    (void)self;
    g_answer[0] = 0;
    if (key && Read(g_current, lobby))
        for (int i = 0; i < g_lobby.keyCount; i++)
            if (strcmp(g_lobby.keys[i], key) == 0) { strncpy(g_answer, g_lobby.values[i], sizeof g_answer - 1); break; }
    return g_answer;
}

/* As on Steam, only the owner sets a lobby's data and whether it can be joined. */
static bool SetLobbyData(void* self, uint64_t lobby, const char* key, const char* value)
{
    (void)self;
    BrokerLobbies* s = g_current;
    if (!key || !value || !*key || strlen(key) >= BROKER_LOBBY_KEY || strlen(value) >= BROKER_LOBBY_VALUE) return false;
    HANDLE lock = Lock(s, lobby);
    if (lock == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    if (Read(s, lobby) && g_lobby.owner == s->self)
    {
        int at = 0;
        while (at < g_lobby.keyCount && strcmp(g_lobby.keys[at], key) != 0) at++;
        if (at < BROKER_LOBBY_KEYS)
        {
            if (at == g_lobby.keyCount) { strcpy(g_lobby.keys[at], key); g_lobby.keyCount++; }
            strcpy(g_lobby.values[at], value);
            ok = Write(s);
        }
    }
    CloseHandle(lock);
    return ok;
}

static bool SetLobbyJoinable(void* self, uint64_t lobby, bool joinable)
{
    (void)self;
    BrokerLobbies* s = g_current;
    HANDLE lock = Lock(s, lobby);
    if (lock == INVALID_HANDLE_VALUE) return false;
    bool ok = Read(s, lobby) && g_lobby.owner == s->self;
    if (ok) { g_lobby.joinable = joinable; ok = Write(s); }
    CloseHandle(lock);
    return ok;
}

/* ---- Steam P2P between the instances (loopback UDP; the bridge's own checks still decide who may be reached) ---- */

static uint16_t DefaultPortOf(uint64_t id) { return (uint16_t)(BROKER_P2P_PORT_BASE + id % 1000); }
static BrokerP2PPortFn g_portOf = DefaultPortOf;
static uint16_t PortOf(uint64_t id) { return g_portOf(id); }
void BrokerLobbiesPorts(BrokerP2PPortFn portOf) { g_portOf = portOf ? portOf : DefaultPortOf; }

uint16_t BrokerLobbiesPort(const BrokerLobbies* s)
{
    struct sockaddr_in at;
    int length = sizeof at;
    if (!s || s->udp == INVALID_SOCKET || getsockname(s->udp, (struct sockaddr*)&at, &length) != 0) return 0;
    return ntohs(at.sin_port);
}

/* Whatever arrived on this broker's socket since, into its packet list (a full list drops the rest). */
static void Drain(BrokerLobbies* s)
{
    if (s->udp == INVALID_SOCKET) return;
    static uint8_t datagram[sizeof(LocalDatagram) + BROKER_LOCAL_PACKET];
    for (;;)
    {
        struct sockaddr_in from;
        int fromLength = sizeof from;
        int got = recvfrom(s->udp, (char*)datagram, sizeof datagram, 0, (struct sockaddr*)&from, &fromLength);
        if (got == SOCKET_ERROR) return; /* WSAEWOULDBLOCK: nothing more */
        LocalDatagram head;
        if (got < (int)sizeof head || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        memcpy(&head, datagram, sizeof head);
        if (head.size > BROKER_LOCAL_PACKET || (int)(sizeof head + head.size) != got || s->packetCount >= BROKER_LOCAL_PACKETS) continue;
        s->packets[s->packetCount].remote = head.from;
        s->packets[s->packetCount].channel = head.channel;
        s->packets[s->packetCount].size = head.size;
        memcpy(s->packets[s->packetCount].data, datagram + sizeof head, head.size);
        s->packetCount++;
    }
}

static int FirstPacket(const BrokerLobbies* s, int32_t channel)
{
    for (int i = 0; i < s->packetCount; i++) if (s->packets[i].channel == channel) return i;
    return -1;
}

static bool SendP2PPacket(void* self, uint64_t remote, const void* data, uint32_t size, int32_t type, int32_t channel)
{
    (void)self; (void)type;
    BrokerLobbies* s = g_current;
    if (s->udp == INVALID_SOCKET || size > BROKER_LOCAL_PACKET) return false;
    static uint8_t datagram[sizeof(LocalDatagram) + BROKER_LOCAL_PACKET];
    LocalDatagram head = { s->self, channel, size };
    memcpy(datagram, &head, sizeof head);
    if (size) memcpy(datagram + sizeof head, data, size);
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(PortOf(remote));
    return sendto(s->udp, (const char*)datagram, (int)(sizeof head + size), 0, (struct sockaddr*)&to, sizeof to) == (int)(sizeof head + size);
}

static bool IsP2PPacketAvailable(void* self, uint32_t* size, int32_t channel)
{
    (void)self;
    BrokerLobbies* s = g_current;
    Drain(s);
    int at = FirstPacket(s, channel);
    if (size) *size = at >= 0 ? s->packets[at].size : 0;
    return at >= 0;
}

static bool ReadP2PPacket(void* self, void* dest, uint32_t capacity, uint32_t* size, uint64_t* remote, int32_t channel)
{
    (void)self;
    BrokerLobbies* s = g_current;
    Drain(s);
    int at = FirstPacket(s, channel);
    if (at < 0) return false;
    if (dest) memcpy(dest, s->packets[at].data, s->packets[at].size < capacity ? s->packets[at].size : capacity);
    if (size) *size = s->packets[at].size;
    if (remote) *remote = s->packets[at].remote;
    memmove(&s->packets[at], &s->packets[at + 1], sizeof s->packets[0] * (size_t)(s->packetCount - at - 1));
    s->packetCount--;
    return true;
}

/* A session between two instances needs nothing set up: it is open whenever both brokers run. */
static bool P2PSessionOk(void* self, uint64_t remote) { (void)self; (void)remote; return true; }
static bool CloseP2PChannel(void* self, uint64_t remote, int32_t channel) { (void)self; (void)remote; (void)channel; return true; }
static bool AllowP2PPacketRelay(void* self, bool allow) { (void)self; (void)allow; return true; }
/* P2PSessionState_t: connection active, the rest zero */
static bool GetP2PSessionState(void* self, uint64_t remote, void* state)
{
    (void)self; (void)remote;
    if (state) { memset(state, 0, 20); ((uint8_t*)state)[0] = 1; }
    return true;
}

/* ---- the module ---- */

BrokerLobbies* BrokerLobbiesOpen(const wchar_t* folder, uint64_t self)
{
    if (!folder || !self || wcslen(folder) >= 500) return NULL;
    BrokerLobbies* s = (BrokerLobbies*)calloc(1, sizeof *s);
    if (!s) return NULL;
    wcscpy(s->folder, folder);
    s->self = self;
    CreateDirectoryW(folder, NULL);
    DWORD attributes = GetFileAttributesW(folder);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) { free(s); return NULL; }
    /* this instance's P2P socket; without it, P2P sends fail and nothing arrives (the lobby still works) */
    WSADATA wsa;
    s->udp = WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
    struct sockaddr_in at = { 0 };
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons(PortOf(self));
    u_long nonblocking = 1;
    if (s->udp != INVALID_SOCKET && (bind(s->udp, (struct sockaddr*)&at, sizeof at) != 0 || ioctlsocket(s->udp, FIONBIO, &nonblocking) != 0))
    {
        closesocket(s->udp);
        s->udp = INVALID_SOCKET;
    }
    if (s->udp == INVALID_SOCKET) fprintf(stderr, "broker: Steam P2P between the instances is off: 127.0.0.1:%u is not free\n", PortOf(self));
    return s;
}

void BrokerLobbiesUse(BrokerLobbies* lobbies) { g_current = lobbies; }

void* BrokerLobbiesResolve(const char* flat)
{
    static const struct { const char* name; void* fn; } table[] = {
        { "SteamAPI_ISteamMatchmaking_CreateLobby", (void*)CreateLobby },
        { "SteamAPI_ISteamMatchmaking_JoinLobby", (void*)JoinLobby },
        { "SteamAPI_ISteamMatchmaking_LeaveLobby", (void*)LeaveLobby },
        { "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", (void*)GetNumLobbyMembers },
        { "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", (void*)GetLobbyMemberByIndex },
        { "SteamAPI_ISteamMatchmaking_GetLobbyOwner", (void*)GetLobbyOwner },
        { "SteamAPI_ISteamMatchmaking_GetLobbyData", (void*)GetLobbyData },
        { "SteamAPI_ISteamMatchmaking_SetLobbyData", (void*)SetLobbyData },
        { "SteamAPI_ISteamMatchmaking_SetLobbyJoinable", (void*)SetLobbyJoinable },
        { "SteamAPI_ISteamNetworking_SendP2PPacket", (void*)SendP2PPacket },
        { "SteamAPI_ISteamNetworking_IsP2PPacketAvailable", (void*)IsP2PPacketAvailable },
        { "SteamAPI_ISteamNetworking_ReadP2PPacket", (void*)ReadP2PPacket },
        { "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser", (void*)P2PSessionOk },
        { "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser", (void*)P2PSessionOk },
        { "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser", (void*)CloseP2PChannel },
        { "SteamAPI_ISteamNetworking_AllowP2PPacketRelay", (void*)AllowP2PPacketRelay },
        { "SteamAPI_ISteamNetworking_GetP2PSessionState", (void*)GetP2PSessionState },
    };
    if (!flat) return NULL;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) if (strcmp(table[i].name, flat) == 0) return table[i].fn;
    return NULL;
}

bool BrokerLobbiesNext(BrokerLobbies* s, BrokerLocalResult* out)
{
    if (s->pendingCount > 0)
    {
        *out = s->pending[0];
        memmove(s->pending, s->pending + 1, sizeof s->pending[0] * (size_t)--s->pendingCount);
        return true;
    }
    /* LobbyDataUpdate_t (pack 8): the lobby, the member (the lobby itself for its data), success at 16 */
    for (int i = 0; i < s->inCount; i++)
    {
        wchar_t path[600];
        WIN32_FILE_ATTRIBUTE_DATA data;
        PathOf(s, s->in[i], L".lobby", path, 600);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || CompareFileTime(&data.ftLastWriteTime, &s->seen[i]) == 0) continue;
        s->seen[i] = data.ftLastWriteTime;
        memset(out, 0, sizeof *out);
        out->id = LOBBY_DATA_UPDATE;
        out->size = 24;
        memcpy(out->data, &s->in[i], 8);
        memcpy(out->data + 8, &s->in[i], 8);
        out->data[16] = 1;
        return true;
    }
    return false;
}

void BrokerLobbiesClose(BrokerLobbies* s)
{
    if (!s) return;
    BrokerLobbies* before = g_current;
    g_current = s;
    while (s->inCount > 0) LeaveLobby(NULL, s->in[0]);
    g_current = before == s ? NULL : before;
    if (s->udp != INVALID_SOCKET) closesocket(s->udp);
    free(s);
}
