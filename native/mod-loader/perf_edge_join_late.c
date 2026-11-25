/* edge-join-late (docs/performance-enemies.md), build 25480438. The navmesh edge gathers (the largest of the AI query
 * jobs at ~1,000 moving enemies) are read only by the boundary clip, and the clip's first caller in the world update is
 * the vehicle collision pass (0x6F9090); the steering and motion passes come after it. So the world update's join
 * (0xFDB1F0) waits for every query job but the edge gathers, and the edge gathers' wait and count reset move to the
 * vehicle collision pass's start: the workers answer them while the game thread runs the world update's first passes.
 * The game's join runs as before, with its edge batch pointer (entity manager +0x38) aimed at a batch of ours whose
 * rows are all done for the length of the call. If the vehicle collision pass is not reached, the wait ends with the
 * world update; a kick finds no wait left over. Needs ai-query-chunks (its join and job runner hooks). Included by
 * perf_all.c after perf_ai_query_chunks.c: LoaderHook, HookRemove (hooks.c), PinMatches (packages.c), PerfFixOn, Note,
 * g_build, ApiModuleBase. */

#define EJ_WORLD_UPDATE 0x571250u   /* game: (world, dt) */
#define EJ_VEHICLE_PASS 0x6F9090u   /* game: VehicleCollisionDamage_Update (component, dt), the clip's first caller */
#define EJ_PENDING_TASK 0xA4A40u    /* exe: ThreadApi::maybe_run_pending_task, the join's own way to wait */
#define EJ_BATCH_AT 0x38u           /* entity manager: the edge gathers' world batch */
#define EJ_DONE_AT 0x65C10u         /* that batch: 12 rows' done flags, 12 bytes apart */
#define EJ_ROWS 12u

typedef struct EjPin { uint32_t rva; uint16_t size; const char* what; uint8_t bytes[32]; } EjPin;

/* The world update: its start, its join's call (the entity manager in rcx), the vehicle collision, steering and
 * motion passes' calls in that order; the vehicle collision pass's start and its clip call (through 0x59E600); the
 * engine's pending-task call. */
static const EjPin kEjGamePins[] = {
    {0x571250u, 0x20, "the world update's start", {
        0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa8,0x48,0x81,
        0xec,0x20,0x01,0x00,0x00,0x0f,0x29,0x70}},
    {0x57194Cu, 0xC, "the world update's join", {
        0x48,0x8b,0x0d,0x45,0xa6,0xef,0x02,0xe8,0x98,0x98,0xa6,0x00}},
    {0x573928u, 0xF, "the world update's vehicle collision pass", {
        0x48,0x8d,0x8f,0x10,0x38,0xee,0x00,0x0f,0x28,0xce,0xe8,0x59,0x57,0x18,0x00}},
    {0x573D9Au, 0x9, "the world update's steering pass", {
        0x44,0x8b,0x41,0x1c,0xe8,0xfd,0x61,0x26,0x00}},
    {0x573F5Cu, 0xC, "the world update's motion pass", {
        0x44,0x8b,0x80,0x8c,0x48,0x00,0x00,0xe8,0xd8,0xb4,0x02,0x00}},
    {0x6F9090u, 0x10, "the vehicle collision pass's start", {
        0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24}},
    {0x6FAE76u, 0xA, "the vehicle collision pass's clip", {
        0x85,0xf8,0x00,0x00,0x00,0xe8,0x80,0x37,0xea,0xff}},
    {0x59E803u, 0xA, "the vehicle clip's call", {
        0x20,0xff,0xff,0xff,0xff,0xe8,0x53,0xc5,0xda,0x00}},
};
static const EjPin kEjExePins[] = {
    {0xA4A40u, 0x15, "the job runner's pending task", {
        0x48,0x8b,0x05,0x71,0xae,0x96,0x01,0xba,0x02,0x00,0x00,0x00,0x48,0x8b,0x88,0x18,0x03,0x00,0x00,0xe9,0xa8}},
};
#define EJ_GAME_PINS (sizeof kEjGamePins / sizeof kEjGamePins[0])
#define EJ_EXE_PINS (sizeof kEjExePins / sizeof kEjExePins[0])

typedef void (*EjPassFn)(void* self, float dt);

static void* volatile g_ejWorldNext;
static void* volatile g_ejVehicleNext;
static uintptr_t g_ejExe;
static volatile LONG g_ejOn, g_ejInWorld;
static uint8_t* g_ejDone;      /* a batch of ours: count 0, every row done */
static uintptr_t g_ejBatch;    /* the world batch whose wait moved */
static int g_ejState;          /* 0 waiting for ai-query-chunks and the code, 1 installed, -1 not */
static ULONGLONG g_ejFirstTry, g_ejReportedAt;
static LONG64 g_ejLateTicks, g_ejLate, g_ejAtEnd, g_ejAtKick, g_ejLast[4]; /* the game thread's, the watch's */

static const char* EdgeJoinLateMismatch(uintptr_t exe, uintptr_t game)
{
    for (size_t i = 0; i < EJ_GAME_PINS; i++)
        if (!PinMatches(game + kEjGamePins[i].rva, kEjGamePins[i].bytes, kEjGamePins[i].size)) return kEjGamePins[i].what;
    for (size_t i = 0; i < EJ_EXE_PINS; i++)
        if (!PinMatches(exe + kEjExePins[i].rva, kEjExePins[i].bytes, kEjExePins[i].size)) return kEjExePins[i].what;
    return NULL;
}

/* In the join, on the game thread: move the edge gathers' wait when this is the world update's join of a kick. */
static int EdgeJoinLateWanted(void) { return g_ejOn && g_ejInWorld && g_aq[AQ_EDGES].active && !g_ejPending; }

/* The game's join with the edge batch pointer on ours (every row done): it waits for the other jobs and resets them. */
static void EdgeJoinLateJoin(void* entities)
{
    uintptr_t* slot = (uintptr_t*)((uintptr_t)entities + EJ_BATCH_AT);
    uintptr_t batch = *slot;
    if (batch == 0) { ((AqJoinFn)g_aqJoinNext)(entities); return; }
    g_ejBatch = batch;
    *(volatile uint32_t*)g_ejDone = 0;
    *slot = (uintptr_t)g_ejDone;
    ((AqJoinFn)g_aqJoinNext)(entities);
    *slot = batch;
    InterlockedExchange(&g_ejPending, 1);
}

/* The moved wait, as the game's join does it: chunks still left, every row's done flag, then the count to 0. */
static void EdgeJoinLateWait(void)
{
    if (!g_ejPending) return;
    AqBatch* b = &g_aq[AQ_EDGES];
    if (b->active) AiQueryChunksRun(b, NULL, NULL, NULL, 1);
    volatile uint32_t* done = (volatile uint32_t*)(g_ejBatch + EJ_DONE_AT);
    for (uint32_t i = 0; i < EJ_ROWS; i++)
        while (done[i * 3] == 0) ((void (*)(void))(g_ejExe + EJ_PENDING_TASK))();
    *(volatile uint32_t*)g_ejBatch = 0;
    InterlockedExchange(&b->active, 0);
    InterlockedExchange(&g_ejPending, 0);
}

static void EdgeJoinLateVehicle(void* self, float dt)
{
    if (g_ejPending)
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        EdgeJoinLateWait();
        QueryPerformanceCounter(&b);
        g_ejLateTicks += b.QuadPart - a.QuadPart;
        g_ejLate++;
    }
    ((EjPassFn)g_ejVehicleNext)(self, dt);
}

static void EdgeJoinLateWorld(void* self, float dt)
{
    InterlockedExchange(&g_ejInWorld, 1);
    ((EjPassFn)g_ejWorldNext)(self, dt);
    InterlockedExchange(&g_ejInWorld, 0);
    if (g_ejPending) { g_ejAtEnd++; EdgeJoinLateWait(); }
}

/* The vehicle pass first (a wait moved before it is hooked would end with the world update), then the world update. */
static int EdgeJoinLateInstallAt(uintptr_t exe, uintptr_t game)
{
    g_ejDone = (uint8_t*)VirtualAlloc(NULL, EJ_DONE_AT + EJ_ROWS * 12u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_ejDone == NULL) { Note("performance fix edge-join-late: not installed; no memory for its batch"); return 0; }
    for (uint32_t i = 0; i < EJ_ROWS; i++) *(uint32_t*)(g_ejDone + EJ_DONE_AT + i * 12u) = 1;
    g_ejExe = exe;
    int vehicle = LoaderHook("edge-join-late: the vehicle collision pass", game + EJ_VEHICLE_PASS, (void*)EdgeJoinLateVehicle,
        (void**)&g_ejVehicleNext) == 0;
    int world = vehicle && LoaderHook("edge-join-late: the world update", game + EJ_WORLD_UPDATE, (void*)EdgeJoinLateWorld,
        (void**)&g_ejWorldNext) == 0;
    if (world)
    {
        g_ejReportedAt = GetTickCount64();
        InterlockedExchange(&g_ejOn, 1);
        Note("performance fix edge-join-late: installed; the edge gathers are waited for at the vehicle collision pass, not before the world update");
        return 1;
    }
    if (vehicle) HookRemove(NULL, game + EJ_VEHICLE_PASS, (void*)EdgeJoinLateVehicle);
    VirtualFree(g_ejDone, 0, MEM_RELEASE);
    g_ejDone = NULL;
    Note("performance fix edge-join-late: not installed; a function could not be hooked");
    return 0;
}

/* 1 installed, -1 not, 0 not yet. */
static int EdgeJoinLateTry(uintptr_t exe, uintptr_t game, int lastTry)
{
    const char* differs = EdgeJoinLateMismatch(exe, game);
    if (differs != NULL)
    {
        if (!lastTry) return 0;
        Note("performance fix edge-join-late: not installed; %s is not the reviewed code", differs);
        return -1;
    }
    if (!PerfFixOn("edge-join-late")) return -1;
    return EdgeJoinLateInstallAt(exe, game) ? 1 : -1;
}

/* After AiQueryChunksWatch (WatchStats): only with ai-query-chunks installed. */
static void EdgeJoinLateWatch(void)
{
    if (g_ejState == 1 && g_ejLate + g_ejAtEnd > g_ejLast[0] + g_ejLast[1] && GetTickCount64() - g_ejReportedAt >= 60000)
    {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        LONG64 now[4] = {g_ejLate, g_ejAtEnd, g_ejLateTicks, g_aqJoins};
        double late = (double)(now[0] - g_ejLast[0]);
        Note("performance fix edge-join-late: in the last %.0f s, %.0f moved waits took %.3f ms each at the vehicle collision pass; "
            "%lld ended with the world update instead", (double)(GetTickCount64() - g_ejReportedAt) / 1e3, late,
            late > 0 ? 1e3 * (double)(now[2] - g_ejLast[2]) / (double)hz.QuadPart / late : 0.0, (long long)(now[1] - g_ejLast[1]));
        memcpy(g_ejLast, now, sizeof now);
        g_ejReportedAt = GetTickCount64();
    }
    if (g_ejState != 0 || g_aqState == 0) return;
    if (g_aqState < 0)
    {
        g_ejState = -1;
        Note("performance fix edge-join-late: not installed; it needs ai-query-chunks");
        return;
    }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (g_ejFirstTry == 0) g_ejFirstTry = GetTickCount64();
    g_ejState = EdgeJoinLateTry((uintptr_t)GetModuleHandleW(NULL), game, GetTickCount64() - g_ejFirstTry >= 60000);
}
