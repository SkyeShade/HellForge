/* Moving arrays out of the game state: docs/internals.md, "The game state's arrays". The progression arrays sit in the one
 * large state object (P = state + 0x2D5A850; state allocated at 0x4EE51B and 0x4EFE3D), reached by displacements compiled
 * into every access. All of it happens before the plugin setup allocates the state, so no thread can be in this code; every
 * site is checked against its reviewed bytes first, and a set that does not match moves nothing. The guard is for tests:
 * off unless HELLFORGE_MOVE_GUARD=1 (MoveGuardWanted), and off it costs nothing. */
#include "state_move_sites.h"

#define MOVE_STATE 0x3326340u       /* the state, 0 until 0x4EE160 allocates it */
#define MOVE_P 0x347CEF8u
#define MOVE_STATE_BYTES 0x7AF7628u
#define MOVE_P_IN_STATE 0x2D5A850u
#define MOVE_TAIL (MOVE_STATE_BYTES - MOVE_P_IN_STATE) /* the state's old end, from P */
#define MOVE_SLOTS_OLD 0x4D4u
#define MOVE_SLOT_BYTES 0x18u
#define MOVE_SLOTS_OLD_BYTES (256u * MOVE_SLOT_BYTES)
#define MOVE_PACKAGES_OLD 256u
#define MOVE_PACKAGES 1024u
#define MOVE_SLOTS_ROOM MOVE_PACKAGES
#define MOVE_SLOTS_NEW (((MOVE_TAIL + 0xFu) & ~0xFu) + 4u) /* as the old ones: 4 past a 16-byte boundary */
#define MOVE_CATALOGUE_OLD 0x1CE0u
/* A field's offset from the block's start, for n items: count, definitions (0xB8 each), hierarchy (0x18 each), 24 category
 * starts and their total, index (u32 each), two statuses. */
#define MOVE_ITEMS_OLD 4096u
#define MOVE_ITEMS 40960u /* a multiple of 16, so each field keeps its alignment; and 57 items x 700 weapons fit */
#define MOVE_AT_HIERARCHY(n) (4u + 0xB8u * (n))
#define MOVE_AT_STARTS(n) (MOVE_AT_HIERARCHY(n) + 0x18u * (n))
#define MOVE_AT_TOTAL(n) (MOVE_AT_STARTS(n) + 0x60u)
#define MOVE_AT_INDEX(n) (MOVE_AT_TOTAL(n) + 4u)
#define MOVE_AT_STATUS(n) (MOVE_AT_INDEX(n) + 4u * (n))
#define MOVE_AT_INVENTORY(n) (MOVE_AT_STATUS(n) + 4u)
#define MOVE_AT_END(n) (MOVE_AT_INVENTORY(n) + 4u)
#define MOVE_CATALOGUE_OLD_BYTES MOVE_AT_END(MOVE_ITEMS_OLD)
#define MOVE_CATALOGUE_BYTES MOVE_AT_END(MOVE_ITEMS)
#define MOVE_CATALOGUE_NEW ((MOVE_SLOTS_NEW + MOVE_SLOTS_ROOM * MOVE_SLOT_BYTES + 0xFu) & ~0xFu) /* 16-byte aligned, as the old one */
#define MOVE_CATALOGUE_DELTA (MOVE_CATALOGUE_NEW - MOVE_CATALOGUE_OLD)
#define MOVE_EXTRA_BYTES (((MOVE_CATALOGUE_NEW + MOVE_CATALOGUE_BYTES - MOVE_TAIL) + 0xFFFu) & ~0xFFFu)
#define MOVE_SLOT_COUNT 0x1CD4u
#define MOVE_ITEM_PARSE_BYTES 0x70u    /* /Progression/items' parse buffer, a record */
#define MOVE_PACKAGE_PARSE_BYTES 0x30u /* /Progression/ProgressionPackages' */
_Static_assert(MOVE_ITEMS % 16 == 0 && MOVE_CATALOGUE_DELTA % 16 == 0, "the moved fields keep their alignment, and a scaled offset stays a multiple of 4");
_Static_assert((uint64_t)MOVE_CATALOGUE_NEW + MOVE_CATALOGUE_BYTES < 0x7FFFFFFFull, "a disp32 off P reaches all of it");
_Static_assert((uint64_t)MOVE_STATE_BYTES + MOVE_EXTRA_BYTES < 0x7FFFFFFFull, "the allocation's size is an imm32");
#define MOVE_COUNT(sites) (sizeof sites / sizeof *sites)

/* game.dll is still being decoded when its pin first reads (WinLicense decodes it upward, and the pin is half way up):
 * a site above the pin can read as its encoded bytes for a few ticks (bug report 0401: 0x175b030, 0x18cc90c, 0x190eee7;
 * 2 of 103 local starts, those with the watch thread up before the decoding ended). Such code gets as long as the
 * difficulty and network passes give it, unless the game allocates its state first. */
#define MOVE_DECODE_WAIT 20000u
/* game.dll's setup_game (functions.json SetupPhysicsConfig): the engine calls it once, through its plugin table, and it
 * allocates the state (MOVE_STATE, at 0x4EE51B). While a site still does not read as reviewed it is held (MoveBeforeSetup):
 * the move goes ahead or refuses there, before the allocation, never after it. It sits below the pin, so it reads as
 * reviewed when the pin does. */
#define MOVE_SETUP 0x4EE160u
#define MOVE_SETUP_WAIT 10000u
static const uint8_t kMoveSetupCode[16] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};

static int g_stateMoves; /* 0 waiting, 1 moved, -1 left where they are */
static int g_moveGuard;  /* the guard and the canary are on */
static ULONGLONG g_moveWaitFrom, g_moveMovedAt; /* the first try that found a site unread; the move */
static uint32_t g_moveTries;      /* the tries that found one */
static uint32_t g_moveUndecoded;  /* tests: HELLFORGE_MOVE_UNDECODED=n, the first n tries find one */
static int32_t g_moveResetRel;    /* the reset call's displacement to the stub, as written */
static int g_moveStateSeen;       /* the game has allocated its state since the move */
static int g_moveSetupHook;       /* setup_game held: 0 not tried, 1 hooked, -1 not */
static void (*g_moveSetupOriginal)(void* getApi);
static uint32_t g_moveUnreadRva;  /* the last site found unread, and what it read */
static char g_moveUnreadWhy[64];
static SRWLOCK g_moveLock = SRWLOCK_INIT; /* the loader's thread and, held, the game's main thread */
static ULONGLONG g_moveSetupWait = MOVE_SETUP_WAIT; /* tests shorten it */
/* Where the catalogue is, from P (ownership.c reads through this): the compiled places until it moves. */
typedef struct CatalogueLayout { uint32_t count, records, index, starts, catalogue, inventory, items; } CatalogueLayout;
#define MOVE_LAYOUT(at, n) {(at), (at) + 4u, (at) + MOVE_AT_HIERARCHY(n), (at) + MOVE_AT_STARTS(n), (at) + MOVE_AT_STATUS(n), (at) + MOVE_AT_INVENTORY(n), (n)}
static CatalogueLayout g_catalogue = MOVE_LAYOUT(MOVE_CATALOGUE_OLD, MOVE_ITEMS_OLD);

/* A field's offset from the block's start, for `items` items. */
static uint32_t MoveFieldAt(uint32_t field, uint32_t items)
{
    switch (field)
    {
    case MOVE_F_COUNT: return 0;
    case MOVE_F_DEFINITIONS: return 4;
    case MOVE_F_HIERARCHY: return MOVE_AT_HIERARCHY(items);
    case MOVE_F_STARTS: return MOVE_AT_STARTS(items);
    case MOVE_F_TOTAL: return MOVE_AT_TOTAL(items);
    case MOVE_F_INDEX: return MOVE_AT_INDEX(items);
    case MOVE_F_STATUS: return MOVE_AT_STATUS(items);
    default: return MOVE_AT_INVENTORY(items);
    }
}

/* What a catalogue site's constant gains: its field's growth, plus the block's move for a site off P, in the constant's
 * units. */
static int32_t MoveSiteDelta(uint8_t kind)
{
    uint32_t field = kind & 0xFu;
    int64_t delta = (int64_t)MoveFieldAt(field, MOVE_ITEMS) - MoveFieldAt(field, MOVE_ITEMS_OLD);
    if (!(kind & MOVE_SITE_OFF_BLOCK)) delta += MOVE_CATALOGUE_DELTA;
    return (int32_t)(kind & MOVE_SITE_SCALED ? delta / 4 : delta);
}

/* A capacity constant's value at the new capacities. */
static int32_t MoveCapacity(uint8_t formula)
{
    switch (formula)
    {
    case MOVE_C_ITEMS: return MOVE_ITEMS;
    case MOVE_C_ITEMS_PARSE: return MOVE_ITEMS * MOVE_ITEM_PARSE_BYTES;
    case MOVE_C_RESET: return MOVE_CATALOGUE_BYTES - 4;
    case MOVE_C_PACKAGES: return MOVE_PACKAGES;
    default: return MOVE_PACKAGES * MOVE_PACKAGE_PARSE_BYTES;
    }
}

/* The first site of these that does not read as reviewed, or NULL; what it reads, in `why`. */
static const MoveSite* MoveSitesUnread(uintptr_t game, const MoveSite* sites, size_t count, char* why, size_t size)
{
    for (size_t i = 0; i < count; i++)
    {
        uint8_t bytes[15];
        if (SafeRead(game + sites[i].rva, bytes, sites[i].length)) { snprintf(why, size, "it cannot be read"); return &sites[i]; }
        if (memcmp(bytes, sites[i].code, sites[i].length) == 0) continue;
        int n = snprintf(why, size, "it reads");
        for (uint8_t b = 0; b < sites[i].length && n > 0 && (size_t)n < size; b++) n += snprintf(why + n, size - n, " %02x", bytes[b]);
        return &sites[i];
    }
    return NULL;
}

/* The first site the move needs that does not read as reviewed (the reset call included), or 0. */
static uint32_t MoveUnread(uintptr_t game, char* why, size_t size)
{
    MoveSite reset = {MOVE_RESET_CALL, 0, 5, 1, 0, {0}};
    memcpy(reset.code, kMoveResetCall, 5);
    if (g_moveTries < g_moveUndecoded) { snprintf(why, size, "HELLFORGE_MOVE_UNDECODED"); return reset.rva; }
    const struct { const MoveSite* sites; size_t count; } sets[] = {{&reset, 1}, {kMoveStateSizes, MOVE_COUNT(kMoveStateSizes)},
        {kMovePackageSites, MOVE_COUNT(kMovePackageSites)}, {kMoveCatalogueSites, MOVE_COUNT(kMoveCatalogueSites)}, {kMoveCapacitySites, MOVE_COUNT(kMoveCapacitySites)}};
    for (size_t i = 0; i < MOVE_COUNT(sets); i++)
    {
        const MoveSite* site = MoveSitesUnread(game, sets[i].sites, sets[i].count, why, size);
        if (site != NULL) return site->rva;
    }
    return 0;
}

/* Whether a site's constant is `value`. */
static int MoveSiteHolds(uintptr_t game, const MoveSite* site, int32_t value)
{
    int32_t now;
    return !SafeRead(game + site->rva + site->at, &now, 4) && now == value;
}

/* The first site that no longer holds what the move wrote there (a second decoding over it), or 0. */
static uint32_t MoveWrittenLost(uintptr_t game)
{
    int32_t rel;
    if (SafeRead(game + MOVE_RESET_CALL + 1, &rel, 4) || rel != g_moveResetRel) return MOVE_RESET_CALL;
    for (size_t i = 0; i < MOVE_COUNT(kMoveStateSizes); i++)
        if (!MoveSiteHolds(game, &kMoveStateSizes[i], kMoveStateSizes[i].value + (int32_t)MOVE_EXTRA_BYTES)) return kMoveStateSizes[i].rva;
    for (size_t i = 0; i < MOVE_COUNT(kMovePackageSites); i++)
        if (!MoveSiteHolds(game, &kMovePackageSites[i], kMovePackageSites[i].value + (int32_t)(MOVE_SLOTS_NEW - MOVE_SLOTS_OLD))) return kMovePackageSites[i].rva;
    for (size_t i = 0; i < MOVE_COUNT(kMoveCatalogueSites); i++)
        if (!MoveSiteHolds(game, &kMoveCatalogueSites[i], kMoveCatalogueSites[i].value + MoveSiteDelta(kMoveCatalogueSites[i].kind))) return kMoveCatalogueSites[i].rva;
    for (size_t i = 0; i < MOVE_COUNT(kMoveCapacitySites); i++)
        if (!MoveSiteHolds(game, &kMoveCapacitySites[i], MoveCapacity(kMoveCapacitySites[i].kind))) return kMoveCapacitySites[i].rva;
    return 0;
}

/* Each site's constant as reviewed, plus delta. 0, or nonzero when one could not be written. */
static int MoveSitesWrite(uintptr_t game, const MoveSite* sites, size_t count, int32_t delta)
{
    for (size_t i = 0; i < count; i++)
    {
        int32_t value = sites[i].value + delta;
        if (ApiWrite(game + sites[i].rva + sites[i].at, &value, 4)) return 1;
    }
    return 0;
}

/* The catalogue's sites at their new places, and the capacities' constants. 0, or nonzero when one could not be written. */
static int MoveCatalogueWrite(uintptr_t game)
{
    for (size_t i = 0; i < MOVE_COUNT(kMoveCatalogueSites); i++)
    {
        const MoveSite* site = &kMoveCatalogueSites[i];
        int32_t value = site->value + MoveSiteDelta(site->kind);
        if (ApiWrite(game + site->rva + site->at, &value, 4)) return 1;
    }
    for (size_t i = 0; i < MOVE_COUNT(kMoveCapacitySites); i++)
    {
        int32_t value = MoveCapacity(kMoveCapacitySites[i].kind);
        if (ApiWrite(game + kMoveCapacitySites[i].rva + kMoveCapacitySites[i].at, &value, 4)) return 1;
    }
    return 0;
}

/* Whether this session runs under the guard: HELLFORGE_MOVE_GUARD=1. */
static int MoveGuardWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", value, 8);
    return length == 1 && value[0] == L'1';
}

/* Tests: HELLFORGE_MOVE_UNDECODED=n, how many tries find game.dll's code still being decoded (0 when unset). */
static uint32_t MoveUndecodedWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_MOVE_UNDECODED", value, 8);
    return length > 0 && length < 8 ? (uint32_t)wcstoul(value, NULL, 10) : 0;
}

/* The reset's memset, then the new slots', then (the guard on) the old slots' canary. rsi is P at the call. */
static uint8_t* MoveResetStub(uintptr_t game)
{
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return NULL;
    uint64_t target = (uint64_t)game + MOVE_MEMSET;
    uint32_t slots = MOVE_SLOTS_NEW, bytes = MOVE_SLOTS_ROOM * MOVE_SLOT_BYTES, old = MOVE_SLOTS_OLD, oldBytes = MOVE_SLOTS_OLD_BYTES;
    static const uint8_t kEnter[] = {0x48, 0x83, 0xEC, 0x28}, kLeave[] = {0x48, 0x83, 0xC4, 0x28, 0xC3}; /* sub/add rsp, 0x28; ret */
    static const uint8_t kMovRax[] = {0x48, 0xB8}, kCallRax[] = {0xFF, 0xD0};
    static const uint8_t kLeaRcx[] = {0x48, 0x8D, 0x8E}, kZeroEdx[] = {0x31, 0xD2}, kMovEdxCanary[] = {0xBA, 0xCD, 0, 0, 0}, kMovR8d[] = {0x41, 0xB8};
    uint8_t* p = code;
#define MOVE_PUT(data, size) (memcpy(p, data, size), p += size)
#define MOVE_MEMSET_CALL() (MOVE_PUT(kMovRax, 2), MOVE_PUT(&target, 8), MOVE_PUT(kCallRax, 2))
    MOVE_PUT(kEnter, 4);
    MOVE_MEMSET_CALL();
    MOVE_PUT(kLeaRcx, 3); MOVE_PUT(&slots, 4);
    MOVE_PUT(kZeroEdx, 2);
    MOVE_PUT(kMovR8d, 2); MOVE_PUT(&bytes, 4);
    MOVE_MEMSET_CALL();
    if (g_moveGuard)
    {
        MOVE_PUT(kLeaRcx, 3); MOVE_PUT(&old, 4);
        MOVE_PUT(kMovEdxCanary, 5);
        MOVE_PUT(kMovR8d, 2); MOVE_PUT(&oldBytes, 4);
        MOVE_MEMSET_CALL();
    }
    MOVE_PUT(kLeave, 5);
#undef MOVE_MEMSET_CALL
#undef MOVE_PUT
    DWORD protect;
    return VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect) ? code : NULL;
}

/* The catalogue is at its new place: HellForge's readers follow, and the backend hears what the game now keeps. */
static void MoveCatalogueLaidOut(void)
{
    g_catalogue = (CatalogueLayout)MOVE_LAYOUT(MOVE_CATALOGUE_NEW, MOVE_ITEMS);
    g_gameItems = MOVE_ITEMS;
    g_gamePackages = MOVE_PACKAGES;
    WriteRegisteredItems(); /* before the game can ask: the state does not exist yet */
}

/* Every site as reviewed again, the reset call last written first. */
static void MovePutBack(uintptr_t game)
{
    ApiWrite(game + MOVE_RESET_CALL + 1, kMoveResetCall + 1, 4);
    MoveSitesWrite(game, kMoveCapacitySites, MOVE_COUNT(kMoveCapacitySites), 0);
    MoveSitesWrite(game, kMoveCatalogueSites, MOVE_COUNT(kMoveCatalogueSites), 0);
    MoveSitesWrite(game, kMovePackageSites, MOVE_COUNT(kMovePackageSites), 0);
    MoveSitesWrite(game, kMoveStateSizes, MOVE_COUNT(kMoveStateSizes), 0);
}

static void MoveHoldSetup(uintptr_t game);

/* A site still unread at `now`: noted (and setup_game held) at the first, refused once the wait runs out. */
static void MoveWaitOrRefuse(uintptr_t game, uint32_t unread, const char* why, ULONGLONG now)
{
    g_moveUnreadRva = unread;
    snprintf(g_moveUnreadWhy, sizeof g_moveUnreadWhy, "%s", why);
    if (g_moveWaitFrom == 0)
    {
        g_moveWaitFrom = now;
        Note("state moves: 0x%x does not read as reviewed (%s): waiting for game.dll's decoding", unread, why);
        MoveHoldSetup(game);
    }
    g_moveTries++;
    ULONGLONG wait = g_moveSetupHook == 1 ? 6 * MOVE_DECODE_WAIT : MOVE_DECODE_WAIT;
    if (now - g_moveWaitFrom < wait) return;
    g_stateMoves = -1;
    Note("state moves: 0x%x still does not read as reviewed after %llu ms (%s): the code is not the reviewed one: the arrays stay", unread, now - g_moveWaitFrom, why);
}

/* Every site as moved, all or none; the game state still unallocated after the writes. */
static void MoveWrite(uintptr_t game)
{
    g_stateMoves = -1;
    g_moveGuard = MoveGuardWanted();
    uint8_t* stub = MoveResetStub(game);
    if (stub == NULL) { Note("state moves: no stub near game.dll: the arrays stay"); return; }
    int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(game + MOVE_RESET_CALL + 5));
    int failed = MoveSitesWrite(game, kMoveStateSizes, MOVE_COUNT(kMoveStateSizes), (int32_t)MOVE_EXTRA_BYTES) ||
        MoveSitesWrite(game, kMovePackageSites, MOVE_COUNT(kMovePackageSites), (int32_t)(MOVE_SLOTS_NEW - MOVE_SLOTS_OLD)) ||
        MoveCatalogueWrite(game) ||
        ApiWrite(game + MOVE_RESET_CALL + 1, &rel, 4);
    /* The setup can have started during the writes: then it allocated at the old size, or half way through them. */
    uint64_t state = 1;
    if (failed || SafeRead(game + MOVE_STATE, &state, 8) || state != 0)
    {
        MovePutBack(game);
        Note("state moves: %s: put back", failed ? "a site could not be written" : "the game state was allocated meanwhile");
        return;
    }
    g_stateMoves = 1;
    g_moveResetRel = rel;
    g_moveMovedAt = GetTickCount64();
    MoveCatalogueLaidOut();
    Note("state moves: package slots at P+0x%x, room for %u; catalogue at P+0x%x, room for %u items; state %u KB larger, %u sites", MOVE_SLOTS_NEW, MOVE_SLOTS_ROOM, MOVE_CATALOGUE_NEW,
        MOVE_ITEMS, MOVE_EXTRA_BYTES >> 10, (unsigned)(MOVE_COUNT(kMoveStateSizes) + MOVE_COUNT(kMovePackageSites) + MOVE_COUNT(kMoveCatalogueSites) + MOVE_COUNT(kMoveCapacitySites) + 1));
    if (g_moveWaitFrom != 0) Note("state moves: game.dll's code read as reviewed %llu ms after the first try (%u tries)", g_moveMovedAt - g_moveWaitFrom, g_moveTries);
}

/* Called each tick until it settles: a site that does not read as reviewed yet is tried again on the next tick, as long
 * as the game has not allocated its state and MOVE_DECODE_WAIT has not passed (six times that while setup_game is held:
 * the hold decides). */
static void StateMovesTry(uintptr_t game)
{
    uint64_t state = 1;
    if (g_stateMoves != 0 || game == 0 || SafeRead(game + MOVE_STATE, &state, 8)) return;
    ULONGLONG now = GetTickCount64();
    if (state != 0)
    {
        g_stateMoves = -1;
        if (g_moveWaitFrom == 0) Note("state moves: the game state was already allocated: its arrays stay");
        else Note("state moves: the game allocated its state while game.dll's code was still being decoded (%llu ms, %u tries): its arrays stay", now - g_moveWaitFrom, g_moveTries);
        return;
    }
    if (g_moveTries == 0) g_moveUndecoded = MoveUndecodedWanted();
    char why[64];
    uint32_t unread = MoveUnread(game, why, sizeof why);
    if (unread != 0) MoveWaitOrRefuse(game, unread, why, now);
    else MoveWrite(game);
}

static void StateMoves(uintptr_t game)
{
    AcquireSRWLockExclusive(&g_moveLock);
    StateMovesTry(game);
    ReleaseSRWLockExclusive(&g_moveLock);
}

/* On the game's main thread, as setup_game is about to allocate the state: the move is tried until it goes ahead or
 * MOVE_SETUP_WAIT passes, and settles here, so the game never sets up with a move still to come. */
static void MoveBeforeSetup(uintptr_t game)
{
    ULONGLONG from = GetTickCount64();
    uint32_t tries = 0;
    for (;; tries++)
    {
        StateMoves(game);
        if (g_stateMoves != 0 || GetTickCount64() - from >= g_moveSetupWait) break;
        Sleep(5);
    }
    ULONGLONG held = GetTickCount64() - from;
    AcquireSRWLockExclusive(&g_moveLock);
    if (g_stateMoves == 0)
    {
        g_stateMoves = -1;
        Note("state moves: 0x%x still does not read as reviewed (%s) as the game sets up, after %llu ms held: the code is not the reviewed one: the arrays stay",
            g_moveUnreadRva, g_moveUnreadWhy, held);
    }
    ReleaseSRWLockExclusive(&g_moveLock);
    if (tries > 0) Note("state moves: the game's setup was held %llu ms (%u tries): %s", held, tries, g_stateMoves == 1 ? "moved before it" : "the arrays stay");
}

static void MoveSetupDetour(void* getApi)
{
    MoveBeforeSetup(ApiModuleBase("game.dll", NULL));
    g_moveSetupOriginal(getApi);
}

/* setup_game held while the move waits (once): only on a start whose first try found a site unread. */
static void MoveHoldSetup(uintptr_t game)
{
    if (g_moveSetupHook != 0) return;
    g_moveSetupHook = -1;
    uint8_t code[sizeof kMoveSetupCode];
    if (SafeRead(game + MOVE_SETUP, code, sizeof code) || memcmp(code, kMoveSetupCode, sizeof code) != 0)
    {
        Note("state moves: setup_game does not read as reviewed: not held; the move waits for game.dll's decoding on its own");
        return;
    }
    if (LoaderHook("state moves: setup hold", game + MOVE_SETUP, (void*)MoveSetupDetour, (void**)&g_moveSetupOriginal))
    {
        Note("state moves: setup_game could not be hooked: not held; the move waits for game.dll's decoding on its own");
        return;
    }
    g_moveSetupHook = 1;
    Note("state moves: setup_game held until the move goes ahead or refuses");
}

/* Each tick from the move until the game allocates its state: every written site still holds its write (a second
 * decoding over one would split the catalogue, so then all of it goes back while that can still be done), and once the
 * state is there, how long after the move it came: the room a late move has. */
static void StateMovesSettleLocked(uintptr_t game)
{
    if (g_stateMoves != 1 || g_moveStateSeen) return;
    uint64_t state = 0;
    uint32_t lost = MoveWrittenLost(game);
    if (SafeRead(game + MOVE_STATE, &state, 8)) return;
    ULONGLONG since = GetTickCount64() - g_moveMovedAt;
    if (state != 0)
    {
        g_moveStateSeen = 1;
        if (lost != 0) Note("state moves: 0x%x lost its write before the game allocated its state: the catalogue is split", lost);
        else Note("state moves: the game allocated its state %llu ms after the move", since);
        return;
    }
    if (lost == 0) return;
    MovePutBack(game);
    g_stateMoves = -1;
    g_catalogue = (CatalogueLayout)MOVE_LAYOUT(MOVE_CATALOGUE_OLD, MOVE_ITEMS_OLD);
    g_gameItems = g_gamePackages = 0;
    WriteRegisteredItems();
    Note("state moves: 0x%x lost its write %llu ms after the move, before the game allocated its state: put back", lost, since);
}

static void StateMovesSettle(uintptr_t game)
{
    AcquireSRWLockExclusive(&g_moveLock);
    StateMovesSettleLocked(game);
    ReleaseSRWLockExclusive(&g_moveLock);
}

/* The old catalogue's pages, no-access, the two it shares with live data too (the slot count below it, the state's next
 * field above it). Each access to the moved bytes is noted once per instruction; any access runs with its pages open and
 * the next instruction closes them, one thread at a time. */
#define MOVE_GUARD_HITS 512
static struct { uint64_t rip, address; uint32_t write; } g_moveHits[MOVE_GUARD_HITS];
static volatile LONG g_moveHitCount;
static LONG g_moveHitsNoted;
static uintptr_t g_moveGuardFrom, g_moveGuardTo;
static uintptr_t g_moveOldFrom, g_moveOldTo, g_moveLiveFrom, g_moveLiveTo; /* what moved; the live words inside it */
static uintptr_t g_moveCrtFrom; /* the page of the CRT's memset */
static volatile DWORD g_moveGuardOwner;
static uintptr_t g_moveGuardOpen[4];
static int g_moveGuardOpened;

/* The instruction let through has run: its pages close and the next thread may come in. */
static void MoveGuardClose(void)
{
    DWORD protect;
    for (int i = 0; i < g_moveGuardOpened; i++) VirtualProtect((void*)g_moveGuardOpen[i], 0x1000, PAGE_NOACCESS, &protect);
    g_moveGuardOpened = 0;
    InterlockedExchange((volatile LONG*)&g_moveGuardOwner, 0);
}

/* Whether an access touches bytes that moved. */
static int MoveGuardMoved(uintptr_t address, uint64_t rip)
{
    if (address < g_moveOldFrom || address >= g_moveOldTo || (address >= g_moveLiveFrom && address < g_moveLiveTo)) return 0;
    /* a reset's own memsets of the old slots, which the canary then fills */
    return !(address < g_moveLiveFrom && rip >= g_moveCrtFrom && rip < g_moveCrtFrom + 0x1000);
}

/* Notes an instruction the first time it is caught. */
static void MoveGuardRecord(uintptr_t address, uint64_t rip, uint32_t write)
{
    LONG count = g_moveHitCount;
    if (count >= MOVE_GUARD_HITS) return;
    for (LONG i = 0; i < count; i++) if (g_moveHits[i].rip == rip) return;
    g_moveHits[count].rip = rip;
    g_moveHits[count].address = address;
    g_moveHits[count].write = write;
    InterlockedIncrement(&g_moveHitCount);
}

static LONG CALLBACK MoveGuard(EXCEPTION_POINTERS* e)
{
    DWORD thread = GetCurrentThreadId(), protect;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && g_moveGuardOwner == thread)
    {
        MoveGuardClose();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t address = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    if (address < g_moveGuardFrom || address >= g_moveGuardTo) return EXCEPTION_CONTINUE_SEARCH;
    if (g_moveGuardOwner != thread)
        while (InterlockedCompareExchange((volatile LONG*)&g_moveGuardOwner, (LONG)thread, 0) != 0) YieldProcessor();
    uint64_t rip = e->ContextRecord->Rip;
    if (MoveGuardMoved(address, rip)) MoveGuardRecord(address, rip, (uint32_t)e->ExceptionRecord->ExceptionInformation[0]);
    uintptr_t page = address & ~(uintptr_t)0xFFF;
    if (g_moveGuardOpened < 4) g_moveGuardOpen[g_moveGuardOpened++] = page;
    VirtualProtect((void*)page, 0x1000, PAGE_READWRITE, &protect);
    e->ContextRecord->EFlags |= 0x100; /* the trap flag: back here after this instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* The state exists (P does): close the old catalogue, once. */
static void MoveGuardInstall(uintptr_t p, uintptr_t game)
{
    static int installed;
    if (installed) return;
    installed = 1;
    uintptr_t from = (p + MOVE_CATALOGUE_OLD) & ~(uintptr_t)0xFFF, to = (p + MOVE_CATALOGUE_OLD + MOVE_CATALOGUE_OLD_BYTES + 0xFFF) & ~(uintptr_t)0xFFF;
    g_moveOldFrom = from > p + MOVE_SLOTS_OLD ? from : p + MOVE_SLOTS_OLD; /* the old slots' end of that page moved too */
    g_moveOldTo = p + MOVE_CATALOGUE_OLD + MOVE_CATALOGUE_OLD_BYTES;
    g_moveLiveFrom = p + MOVE_SLOT_COUNT;
    g_moveLiveTo = p + MOVE_CATALOGUE_OLD;
    g_moveCrtFrom = (game + MOVE_MEMSET) & ~(uintptr_t)0xFFF;
    g_moveGuardFrom = from;
    g_moveGuardTo = to;
    AddVectoredExceptionHandler(1, MoveGuard);
    DWORD protect;
    if (!VirtualProtect((void*)from, to - from, PAGE_NOACCESS, &protect)) Note("state moves: the old catalogue could not be closed (%lu)", GetLastError());
    else Note("state moves: the old catalogue is closed, %llu pages from P+0x%llx", (unsigned long long)((to - from) >> 12), (unsigned long long)(from - p));
    /* The control: one access of our own, which the handler has to catch for its silence to mean anything. */
    (void)*(volatile uint32_t*)(p + MOVE_CATALOGUE_OLD);
    Note("state moves: the guard %s its control", g_moveHitCount >= 1 ? "caught" : "MISSED");
    g_moveHitsNoted = 1;
}

static void MoveGuardReport(uintptr_t game, uintptr_t p)
{
    LONG count = g_moveHitCount;
    for (; g_moveHitsNoted < count && g_moveHitsNoted < MOVE_GUARD_HITS; g_moveHitsNoted++)
    {
        uint64_t rip = g_moveHits[g_moveHitsNoted].rip;
        Note("state moves: MISSED SITE: %s game.dll+0x%llx (rip 0x%llx), P+0x%llx", g_moveHits[g_moveHitsNoted].write ? "write" : "read", (unsigned long long)(rip - game),
            (unsigned long long)rip, (unsigned long long)(g_moveHits[g_moveHitsNoted].address - p));
    }
}

/* The counts in the arrays' new places, when they change. */
static void MoveNoteCounts(uintptr_t p, uint32_t count)
{
    static uint32_t lastCount, lastItems;
    uint32_t first[4] = {0}, items = 0;
    SafeRead(p + MOVE_CATALOGUE_NEW, &items, 4);
    if (count == lastCount && items == lastItems) return;
    SafeRead(p + MOVE_SLOTS_NEW, first, sizeof first);
    Note("state moves: %u package slots in use, the first {id 0x%x, index %u, level %u}; %u catalogue items", count, first[0], first[1], first[2], items);
    lastCount = count;
    lastItems = items;
}

/* Whether the old slots' canary still reads whole, and what changed it once it did. */
static void MoveCanaryCheck(const uint8_t* old, size_t bytes)
{
    static int armed, reported;
    size_t canary = 0, changed = bytes;
    for (size_t i = 0; i < bytes; i++)
    {
        if (old[i] == 0xCD) canary++;
        else if (changed == bytes) changed = i;
    }
    if (canary == bytes)
    {
        if (!armed) Note("state moves: the old package slots hold the canary");
        armed = 1;
        return;
    }
    if (armed && canary != 0 && reported < 16)
    {
        reported++;
        Note("state moves: MISSED SITE: the old package slots changed at P+0x%llx (slot %llu), %llu bytes, now 0x%02x", (unsigned long long)(MOVE_SLOTS_OLD + changed),
            (unsigned long long)(changed / MOVE_SLOT_BYTES), (unsigned long long)(bytes - canary), old[changed]);
    }
}

/* The guard's poll: its first run closes the catalogue, each one reports what the handler caught and reads the canary. */
static void MoveGuardWatch(uintptr_t game, uintptr_t p)
{
    static uint8_t old[MOVE_SLOTS_OLD_BYTES];
    size_t bytes = sizeof old; /* up to the page the guard closes; the rest of the old slots is the guard's */
    MoveGuardInstall(p, game);
    MoveGuardReport(game, p);
    if (p + MOVE_SLOTS_OLD + bytes > g_moveGuardFrom) bytes = g_moveGuardFrom - (p + MOVE_SLOTS_OLD);
    if (SafeRead(p + MOVE_SLOTS_OLD, old, bytes)) return;
    uint32_t count = *(volatile uint32_t*)(p + MOVE_SLOT_COUNT); /* in a closed page: the handler lets it through */
    MoveNoteCounts(p, count);
    MoveCanaryCheck(old, bytes);
}

/* Polled: that the new slots are in use and, the guard on, what has touched the old ones since a reset filled them. */
static void StateMovesWatch(uintptr_t game)
{
    static ULONGLONG next;
    StateMovesSettle(game);
    if (g_stateMoves != 1 || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    uint64_t p = 0;
    uint32_t count = 0;
    if (SafeRead(game + MOVE_P, &p, 8) || p == 0) return;
    if (g_moveGuard) MoveGuardWatch(game, (uintptr_t)p);
    else if (!SafeRead((uintptr_t)p + MOVE_SLOT_COUNT, &count, 4)) MoveNoteCounts((uintptr_t)p, count);
}
