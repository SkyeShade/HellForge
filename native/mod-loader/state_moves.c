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

static int g_stateMoves; /* 0 waiting, 1 moved, -1 left where they are */
static int g_moveGuard;  /* the guard and the canary are on */
/* Where the catalogue is, from P (ownership.c reads through this): the compiled places until it moves. */
typedef struct CatalogueLayout { uint32_t count, records, index, starts, catalogue, inventory, items; } CatalogueLayout;
static CatalogueLayout g_catalogue = {MOVE_CATALOGUE_OLD, MOVE_CATALOGUE_OLD + 4u, MOVE_CATALOGUE_OLD + MOVE_AT_HIERARCHY(MOVE_ITEMS_OLD), MOVE_CATALOGUE_OLD + MOVE_AT_STARTS(MOVE_ITEMS_OLD),
    MOVE_CATALOGUE_OLD + MOVE_AT_STATUS(MOVE_ITEMS_OLD), MOVE_CATALOGUE_OLD + MOVE_AT_INVENTORY(MOVE_ITEMS_OLD), MOVE_ITEMS_OLD};

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

static int MoveSitesRead(uintptr_t game, const MoveSite* sites, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        uint8_t bytes[15];
        if (SafeRead(game + sites[i].rva, bytes, sites[i].length) || memcmp(bytes, sites[i].code, sites[i].length) != 0)
        {
            Note("state moves: 0x%x does not read as reviewed", sites[i].rva);
            return 0;
        }
    }
    return 1;
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
    g_catalogue = (CatalogueLayout){MOVE_CATALOGUE_NEW, MOVE_CATALOGUE_NEW + 4u, MOVE_CATALOGUE_NEW + MOVE_AT_HIERARCHY(MOVE_ITEMS), MOVE_CATALOGUE_NEW + MOVE_AT_STARTS(MOVE_ITEMS),
        MOVE_CATALOGUE_NEW + MOVE_AT_STATUS(MOVE_ITEMS), MOVE_CATALOGUE_NEW + MOVE_AT_INVENTORY(MOVE_ITEMS), MOVE_ITEMS};
    g_gameItems = MOVE_ITEMS;
    g_gamePackages = MOVE_PACKAGES;
    WriteRegisteredItems(); /* before the game can ask: the state does not exist yet */
}

static void StateMoves(uintptr_t game)
{
    uint64_t state = 1;
    if (g_stateMoves != 0 || game == 0 || SafeRead(game + MOVE_STATE, &state, 8)) return;
    g_stateMoves = -1;
    if (state != 0) { Note("state moves: the game state was already allocated: its arrays stay"); return; }
    uint8_t call[5];
    if (SafeRead(game + MOVE_RESET_CALL, call, 5) || memcmp(call, kMoveResetCall, 5) != 0 ||
        !MoveSitesRead(game, kMoveStateSizes, MOVE_COUNT(kMoveStateSizes)) || !MoveSitesRead(game, kMovePackageSites, MOVE_COUNT(kMovePackageSites)) ||
        !MoveSitesRead(game, kMoveCatalogueSites, MOVE_COUNT(kMoveCatalogueSites)) || !MoveSitesRead(game, kMoveCapacitySites, MOVE_COUNT(kMoveCapacitySites)))
    {
        Note("state moves: the code is not the reviewed one: the arrays stay");
        return;
    }
    g_moveGuard = MoveGuardWanted();
    uint8_t* stub = MoveResetStub(game);
    if (stub == NULL) { Note("state moves: no stub near game.dll: the arrays stay"); return; }
    int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(game + MOVE_RESET_CALL + 5));
    int failed = MoveSitesWrite(game, kMoveStateSizes, MOVE_COUNT(kMoveStateSizes), (int32_t)MOVE_EXTRA_BYTES) ||
        MoveSitesWrite(game, kMovePackageSites, MOVE_COUNT(kMovePackageSites), (int32_t)(MOVE_SLOTS_NEW - MOVE_SLOTS_OLD)) ||
        MoveCatalogueWrite(game) ||
        ApiWrite(game + MOVE_RESET_CALL + 1, &rel, 4);
    /* The setup can have started during the writes: then it allocated at the old size, or half way through them. */
    if (failed || SafeRead(game + MOVE_STATE, &state, 8) || state != 0)
    {
        ApiWrite(game + MOVE_RESET_CALL + 1, kMoveResetCall + 1, 4);
        MoveSitesWrite(game, kMoveCapacitySites, MOVE_COUNT(kMoveCapacitySites), 0);
        MoveSitesWrite(game, kMoveCatalogueSites, MOVE_COUNT(kMoveCatalogueSites), 0);
        MoveSitesWrite(game, kMovePackageSites, MOVE_COUNT(kMovePackageSites), 0);
        MoveSitesWrite(game, kMoveStateSizes, MOVE_COUNT(kMoveStateSizes), 0);
        Note("state moves: %s: put back", failed ? "a site could not be written" : "the game state was allocated meanwhile");
        return;
    }
    g_stateMoves = 1;
    MoveCatalogueLaidOut();
    Note("state moves: package slots at P+0x%x, room for %u; catalogue at P+0x%x, room for %u items; state %u KB larger, %u sites", MOVE_SLOTS_NEW, MOVE_SLOTS_ROOM, MOVE_CATALOGUE_NEW,
        MOVE_ITEMS, MOVE_EXTRA_BYTES >> 10, (unsigned)(MOVE_COUNT(kMoveStateSizes) + MOVE_COUNT(kMovePackageSites) + MOVE_COUNT(kMoveCatalogueSites) + MOVE_COUNT(kMoveCapacitySites) + 1));
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
    if (g_stateMoves != 1 || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    uint64_t p = 0;
    uint32_t count = 0;
    if (SafeRead(game + MOVE_P, &p, 8) || p == 0) return;
    if (g_moveGuard) MoveGuardWatch(game, (uintptr_t)p);
    else if (!SafeRead((uintptr_t)p + MOVE_SLOT_COUNT, &count, 4)) MoveNoteCounts((uintptr_t)p, count);
}
