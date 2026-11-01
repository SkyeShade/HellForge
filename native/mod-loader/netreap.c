/* Rejoining after a crash (co-op research 2026-10-21, fix 5).
 * The engine's transport (helldivers2.exe) keeps one connection object per peer id. A crashed friend's connection goes
 * dead (+0x1878C) on its pong timeout, and every packet from that peer id is then dropped (0x2AB2F0, 0x2AAEC6, 0x2AB0F8),
 * the friend's connect request too: the friend relaunches and cannot reach the mission. The transport's cleanup
 * (0x2ABE20, transport vtable +0xA0, called by the network manager's update 0x172080 at 0x172466) destroys a dead
 * connection only when its peer is in none of the "wanted" lists: the game session, the PlayFab lobby, the Steam lobby
 * and an explicit list. HellForge's peer id is the Steam id, and the game's own Steam lobby keeps listing the crashed
 * friend until the mission ends, so the dead connection stays. (In vanilla a peer id is a PlayFab id, which a Steam lobby
 * never lists. HellForge keeps Steam ids: the friends-only join check hands the peer id to Steam's friend list,
 * game.dll 0x108BCFF -> 0x13EFEA0.)
 * After the game's cleanup, on its thread, HellForge destroys a dead connection whose peer is no longer in the game
 * session (the host removed the peer: 0x108D190), as the cleanup itself does for a peer nobody wants: 0x2AA8A0
 * (transport, connection), then the session's vtable +0xE0 with the removed peers ({count, capacity, peers}, read only).
 * A friend's next connect request then makes a new connection (0x2AB27B), which answers. */

#define REAP_MANAGER 0x1A10278u      /* the network manager: +0x98 the transport, +0xA8 the session */
#define REAP_CLEANUP 0x2ABE20u       /* (transport, wanted set, removed peers) */
#define REAP_DESTROY 0x2AA8A0u       /* (transport, connection) */
#define REAP_TRANSPORT_VT 0x1676270u /* +0xA0 the cleanup */
#define REAP_SESSION_VT 0x1675B70u   /* +0x80 is a member (0x292C60), +0xE0 peers removed (0x292FC0) */
#define REAP_MEMBER 0x292C60u
#define REAP_REMOVED 0x292FC0u
#define REAP_DEAD 0x1878Cu           /* connection: dead */
#define REAP_KEPT 0x1878Du           /* ... wanted, as the cleanup found it */
#define REAP_PEER 0x10u              /* ... its peer id */
#define REAP_MAX 16u

static const uint8_t kReapCleanupPin[14] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81};
static const uint8_t kReapDestroyPin[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20};
static const uint8_t kReapMemberPin[10] = {0x44, 0x8B, 0x89, 0x38, 0x01, 0x00, 0x00, 0x45, 0x33, 0xC0};
static const uint8_t kReapRemovedPin[11] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x33, 0xFF, 0x48, 0x8B, 0xF2};
/* the cleanup's reads of the fields: count +0xA0, array +0xA8, peer +0x10, kept and dead */
static const uint8_t kReapCountPin[6] = {0x8B, 0x81, 0xA0, 0x00, 0x00, 0x00};                 /* 0x2ABE2C */
static const uint8_t kReapKeptPin[7] = {0x80, 0xBB, 0x8D, 0x87, 0x01, 0x00, 0x00};            /* 0x2ABF4C */
static const uint8_t kReapDeadPin[7] = {0x80, 0xBB, 0x8C, 0x87, 0x01, 0x00, 0x00};            /* 0x2ABF59 */
static const uint8_t kReapDropPin[9] = {0x80, 0xBF, 0x8C, 0x87, 0x01, 0x00, 0x00, 0x75, 0x27}; /* 0x2AB2F0: a dead one's packets dropped */
static const uint8_t kReapCallPin[6] = {0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00};                  /* 0x172466: call [rax+0xA0] */

typedef void (*ReapCleanupFn)(void* transport, void* wanted, void* removed);
typedef void (*ReapDestroyFn)(void* transport, void* connection);
typedef uint8_t (*ReapMemberFn)(void* session, uint64_t peer);
typedef void (*ReapRemovedFn)(void* session, void* removed);

static int g_reapState; /* 0 waiting, 1 installed, -1 refused */
static uintptr_t g_reapExe;
static ReapCleanupFn g_reapCleanup;
static volatile LONG g_reaped;

/* The game's cleanup, then the dead connections of peers the session no longer has. */
static void ReapCleanupDetour(void* transport, void* wanted, void* removed)
{
    g_reapCleanup(transport, wanted, removed);
    uintptr_t exe = g_reapExe, manager = 0, session = 0, sessionVt = 0, transportVt = 0;
    if (SafeRead(exe + REAP_MANAGER, &manager, 8) || manager == 0 || SafeRead(manager + 0x98, &transportVt, 8) ||
        transportVt != (uintptr_t)transport || SafeRead((uintptr_t)transport, &transportVt, 8) || transportVt != exe + REAP_TRANSPORT_VT ||
        SafeRead(manager + 0xA8, &session, 8) || session == 0 || SafeRead(session, &sessionVt, 8) || sessionVt != exe + REAP_SESSION_VT)
        return; /* another transport, or no session: the game's cleanup alone */
    uintptr_t member = 0, gone = 0;
    if (SafeRead(sessionVt + 0x80, &member, 8) || member != exe + REAP_MEMBER || SafeRead(sessionVt + 0xE0, &gone, 8) || gone != exe + REAP_REMOVED)
        return;
    /* The game's cleanup has just walked these: read directly from here on (this runs on every network update). The
     * explicit list (manager +0xD0 count, +0xD8 peers) is a peer the game itself asked to keep: left alone too. */
    uint32_t keptCount = *(const uint32_t*)(manager + 0xD0);
    const uint64_t* kept = *(const uint64_t* const*)(manager + 0xD8);
    uint64_t peers[REAP_MAX];
    struct { uint32_t count, capacity; uint64_t* peers; } list = {0, REAP_MAX, peers};
    for (uint32_t i = 0; i < *(const uint32_t*)((uintptr_t)transport + 0xA0) && list.count < REAP_MAX; i++)
    {
        uintptr_t connection = (*(const uintptr_t* const*)((uintptr_t)transport + 0xA8))[i];
        uint64_t peer = *(const uint64_t*)(connection + REAP_PEER);
        if (!*(const uint8_t*)(connection + REAP_DEAD) || ((ReapMemberFn)member)((void*)session, peer)) continue;
        int wanted = 0;
        for (uint32_t k = 0; kept != NULL && k < keptCount && !wanted; k++) wanted = kept[k] == peer;
        if (wanted) continue;
        ((ReapDestroyFn)(exe + REAP_DESTROY))(transport, (void*)connection); /* the array closes up: the same index again */
        peers[list.count++] = peer;
        i--;
        InterlockedIncrement(&g_reaped);
        Note("network: the dead connection to peer %016llx (no longer in the game session) is closed, so it can connect again", (unsigned long long)peer);
        CoopDiag("rejoin: dead connection to peer %016llx closed (no longer in the game session; still wanted by a lobby)", (unsigned long long)peer);
    }
    if (list.count > 0) ((ReapRemovedFn)gone)((void*)session, &list);
}

static int RefuseReaper(const char* why)
{
    g_reapState = -1;
    Note("network: dead connections are left to the game (%s): a friend whose game crashed may not rejoin the mission it left", why);
    return 1;
}

/* Whether the transport's code reads as reviewed. */
static int ReapPinsMatch(uintptr_t exe)
{
    static const struct { uint32_t rva; const uint8_t* pin; size_t size; } pins[] = {
        {REAP_CLEANUP, kReapCleanupPin, sizeof kReapCleanupPin}, {REAP_DESTROY, kReapDestroyPin, sizeof kReapDestroyPin},
        {REAP_MEMBER, kReapMemberPin, sizeof kReapMemberPin}, {REAP_REMOVED, kReapRemovedPin, sizeof kReapRemovedPin},
        {0x2ABE2Cu, kReapCountPin, sizeof kReapCountPin}, {0x2ABF4Cu, kReapKeptPin, sizeof kReapKeptPin},
        {0x2ABF59u, kReapDeadPin, sizeof kReapDeadPin}, {0x2AB2F0u, kReapDropPin, sizeof kReapDropPin}, {0x172466u, kReapCallPin, sizeof kReapCallPin}};
    uint8_t code[16];
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++)
        if (SafeRead(exe + pins[i].rva, code, pins[i].size) || memcmp(code, pins[i].pin, pins[i].size)) return 0;
    return 1;
}

/* Polled from the watch loop until it returns 1. pinsReady: the grace period for code still being decoded is over. */
static int InstallReaper(int pinsReady)
{
    if (g_reapState != 0) return 1;
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    if (exe == 0) return pinsReady ? RefuseReaper("helldivers2.exe is not loaded") : 0;
    if (!ReapPinsMatch(exe)) return pinsReady ? RefuseReaper("the transport is not the reviewed code") : 0;
    uintptr_t cleanup = 0;
    if (SafeRead(exe + REAP_TRANSPORT_VT + 0xA0, &cleanup, 8) || cleanup != exe + REAP_CLEANUP)
        return pinsReady ? RefuseReaper("the transport's vtable is not the reviewed one") : 0;
    g_reapExe = exe;
    if (LoaderHook("dead connection reaper", exe + REAP_CLEANUP, (void*)ReapCleanupDetour, (void**)&g_reapCleanup)) return RefuseReaper("its cleanup could not be hooked");
    g_reapState = 1;
    Note("network: a friend's dead connection is closed once the game session has let them go (rejoin after a crash)");
    return 1;
}

/* Co-op diagnostics, once a second on the game thread: the transport's connections when they change (read only). */
static void ReaperSample(void (*line)(const char* format, ...))
{
    static char last[512];
    uintptr_t exe = g_reapExe, manager = 0, transport = 0, connections = 0;
    uint32_t count = 0;
    if (exe == 0 || SafeRead(exe + REAP_MANAGER, &manager, 8) || manager == 0 || SafeRead(manager + 0x98, &transport, 8) || transport == 0 ||
        SafeRead(transport + 0xA0, &count, 4) || SafeRead(transport + 0xA8, &connections, 8))
        return;
    char now[512];
    int used = snprintf(now, sizeof now, "%u:", count);
    for (uint32_t i = 0; i < count && i < 8 && connections != 0 && used > 0 && (size_t)used < sizeof now; i++)
    {
        uintptr_t connection = 0;
        uint64_t peer = 0;
        uint8_t dead = 0, kept = 0;
        if (SafeRead(connections + (uintptr_t)i * 8, &connection, 8) || connection == 0 || SafeRead(connection + REAP_PEER, &peer, 8) ||
            SafeRead(connection + REAP_DEAD, &dead, 1) || SafeRead(connection + REAP_KEPT, &kept, 1))
            break;
        used += snprintf(now + used, sizeof now - (size_t)used, " %016llx%s%s", (unsigned long long)peer, dead ? " DEAD" : "", kept ? " wanted" : "");
    }
    if (!strcmp(now, last)) return;
    memcpy(last, now, sizeof last);
    line("rejoin: transport connections %s (dead ones closed so far: %ld)", now, g_reaped);
}
