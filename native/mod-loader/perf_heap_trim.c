/* The game's allocator (rpmalloc) keeps freed memory in per-thread and global caches. The game empties them (exe
 * 0x3245F0) as it enters each loading state, so what the load then frees stays cached until the next one. This calls
 * the same function once more, on the game thread's first update in the ship or a mission. Included by
 * hellforge_modloader.c after render_guards.c: PinMatches, ReadState, g_tickState. */
#include <psapi.h> /* the memory counters' types only; the function is looked up */

#define HT_TRIM 0x3245F0u        /* exe: this thread's heap and the global caches; then flags every other heap */
#define HT_WORKERS 0x324630u     /* exe: each pool worker flagged; it trims its own heap after its job loop */
#define HT_RENDER 0x324690u      /* exe: message 0x29 to the render thread, which trims its own heap */
#define HT_HEAP 0x6302F0u        /* exe: the calling thread's heap (TLS), then each global class */
#define HT_GLOBAL 0x6680C0u      /* exe: one global class, released under its own spinlock */
#define HT_API 0x3326308u        /* game: the engine API; [+0x10] + 0x520 is the trim's slot */
#define HT_SLOT 0x520u
#define HT_SHIP_ENTER 0xAC55F4u  /* game: entering PREPARE_SHIP (0xAC55B0) calls the slot */
#define HT_MISSION_ENTER 0xAC4E30u /* game: entering PREPARE_MISSION (0xAC4C50) calls the slot */

static const uint8_t kHtTrimPin[] = {0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57, 0x48,0x83,0xec,0x30,
    0x48,0x8b,0x35,0x02,0xbc,0x6e,0x01, 0xb1,0x01, 0x48,0x8b,0x9e,0x18,0x03,0x00,0x00, 0xe8,0xdc,0xbc,0x30,0x00};
static const uint8_t kHtWorkersPin[] = {0x48,0x8b,0x08, 0x48,0x83,0xc0,0x08, 0xc6,0x41,0x7d,0x01};
static const uint8_t kHtRenderPin[] = {0xb8,0x29,0x00,0x00,0x00, 0x66,0x89,0x42,0x0c};
static const uint8_t kHtHeapPin[] = {0x40,0x53, 0x57, 0x48,0x83,0xec,0x28, 0x65,0x48,0x8b,0x04,0x25,0x58,0x00,0x00,0x00,
    0x4c,0x89,0x74,0x24,0x58, 0x41,0xbe,0x50,0x43,0x00,0x00};
static const uint8_t kHtGlobalPin[] = {0x40,0x53, 0x48,0x83,0xec,0x20, 0x48,0x8b,0xd9, 0x48,0x89,0x7c,0x24,0x38,
    0x33,0xc9, 0xbf,0x01,0x00,0x00,0x00, 0x33,0xc0, 0xf0,0x0f,0xb1,0x3b};
static const uint8_t kHtShipEnterPin[] = {0x48,0x8b,0x05,0x0d,0x0d,0x86,0x02, 0x4c,0x8b,0x40,0x10, 0x41,0xff,0x90,0x20,0x05,0x00,0x00};
static const uint8_t kHtMissionEnterPin[] = {0x48,0x8b,0x05,0xd1,0x14,0x86,0x02, 0x48,0x8b,0x48,0x10, 0xff,0x91,0x20,0x05,0x00,0x00};

typedef void (*HtTrimFn)(void);
static volatile LONG g_htState; /* 0 waiting for the code, 1 on, -1 off */
static uintptr_t g_htGame;
static HtTrimFn g_htCall;
static uint32_t g_htLast;       /* the state the game thread's last update read */
static LONG g_htTrims;
static ULONGLONG g_htLoadedAt;

/* 1 when the trim, its callees and both of the game's calls read as reviewed and the slot holds the trim. */
static int HeapTrimPinned(uintptr_t exe, uintptr_t game, uintptr_t* call)
{
    uintptr_t api = 0, table = 0, fn = 0;
    if (!PinMatches(exe + HT_TRIM, kHtTrimPin, sizeof kHtTrimPin) || !PinMatches(exe + HT_WORKERS, kHtWorkersPin, sizeof kHtWorkersPin) ||
        !PinMatches(exe + HT_RENDER, kHtRenderPin, sizeof kHtRenderPin) || !PinMatches(exe + HT_HEAP, kHtHeapPin, sizeof kHtHeapPin) ||
        !PinMatches(exe + HT_GLOBAL, kHtGlobalPin, sizeof kHtGlobalPin) ||
        !PinMatches(game + HT_SHIP_ENTER, kHtShipEnterPin, sizeof kHtShipEnterPin) ||
        !PinMatches(game + HT_MISSION_ENTER, kHtMissionEnterPin, sizeof kHtMissionEnterPin)) return 0;
    if (SafeRead(game + HT_API, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0 ||
        SafeRead(table + HT_SLOT, &fn, 8) || fn != exe + HT_TRIM) return 0;
    *call = fn;
    return 1;
}

/* 1 on, 0 the code is not the reviewed one (yet), -1 turned off. */
static int HeapTrimInstallAt(uintptr_t exe, uintptr_t game)
{
    uintptr_t call = 0;
    if (exe == 0 || game == 0 || !HeapTrimPinned(exe, game, &call)) return 0;
    if (!PerfFixOn("heap-trim")) { InterlockedExchange(&g_htState, -1); return -1; }
    g_htGame = game;
    g_htCall = (HtTrimFn)(void*)call;
    g_htLast = HF_STATE_NONE;
    InterlockedExchange(&g_htState, 1);
    Note("heap trim: on; the game's allocator caches are emptied once more when a load ends on the ship or a mission");
    return 1;
}

/* From the watch thread, once the game has loaded and its update is hooked: the code is decoded some time after start. */
static void HeapTrimInstall(void)
{
    if (g_htState != 0 || !g_fired[HF_EVENT_GAME_LOADED] || g_tickState == 0) return;
    const char* why = strcmp(g_build, "25480438") ? "an unsupported build" : g_tickState < 0 ? "the game thread's update is not hooked" : NULL;
    if (why == NULL)
    {
        if (HeapTrimInstallAt((uintptr_t)GetModuleHandleW(NULL), ApiModuleBase("game.dll", NULL))) return;
        if (g_htLoadedAt == 0) g_htLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_htLoadedAt < 60000) return;
        why = "the allocator's trim or the game's calls of it are not the reviewed ones";
    }
    InterlockedExchange(&g_htState, -1);
    Note("heap trim: off; %s", why);
}

/* The process's committed memory in MB as Task Manager counts it, or -1. */
static long long HeapTrimCommitMb(void)
{
    typedef BOOL (WINAPI* InfoFn)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static InfoFn info;
    static int looked;
    if (!looked)
    {
        HMODULE kernel = GetModuleHandleA("kernel32.dll");
        info = kernel ? (InfoFn)(void*)GetProcAddress(kernel, "K32GetProcessMemoryInfo") : NULL;
        looked = 1;
    }
    PROCESS_MEMORY_COUNTERS_EX c;
    memset(&c, 0, sizeof c);
    c.cb = sizeof c;
    return info != NULL && info(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&c, sizeof c) ? (long long)(c.PrivateUsage >> 20) : -1;
}

/* One update's state. A load ends where the state turns to the ship or a mission; the first state read only sets the
 * baseline, so a fix installed in play waits for the next load. */
static void HeapTrimStep(uint32_t state)
{
    uint32_t from = g_htLast;
    if (g_htState != 1 || state == HF_STATE_NONE || state == from) return;
    g_htLast = state;
    if (from == HF_STATE_NONE || (state != HF_STATE_SHIP && state != HF_STATE_MISSION)) return;
    long long before = HeapTrimCommitMb();
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    g_htCall();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    long long after = HeapTrimCommitMb();
    g_htTrims++;
    Note("heap trim %ld, %s reached: %.1f ms on the game thread; commit %lld -> %lld MB; the workers and the renderer trim their own caches next",
        (long)g_htTrims, state == HF_STATE_SHIP ? "the ship" : "a mission", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart,
        before, after);
}

/* From packages.c's TickDetour: the game thread inside the frame update (game 0xAB5000) that makes the game's own
 * hourly trim, holding no allocator or queue lock. */
static void HeapTrimTick(void)
{
    if (g_htState == 1) HeapTrimStep(ReadState(g_htGame));
}
