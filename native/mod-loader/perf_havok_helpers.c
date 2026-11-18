/* Havok's task queue helpers, build 25480438 (docs/performance.md, havok-helpers). When a Havok task graph is
 * submitted (exe 0x8A9F40), the queue starts up to one "havok_task_queue_task" job per worker; each runs the queue's
 * ready tasks (0x8A9A60) and returns the first time none is ready (0x8A9A20). In a physics step most tasks become ready
 * only when an earlier one ends, so the helpers are gone by then and the game thread, which waits for the step by
 * running tasks itself, runs them alone or spins. Here a few helpers (HH_LINGER) stay while the queue still holds
 * tasks that are not ready yet, up to HH_IDLE_US without work, so the released tasks are shared out.
 * Included by hellforge_modloader.c after render_guards.c: PinMatches (packages.c), LoaderHook (hooks.c), PerfFixOn. */

#define HH_JOB 0x8A9A20u     /* exe: the helper job: thread setup, the queue's start count -1, run until none ready */
#define HH_POP 0x8A9A60u     /* exe: pop and run one ready task; 0 when none is ready */
#define HH_SETUP 0x816520u   /* exe: the worker's Havok thread setup (once a thread), writes an int thread index */
#define HH_SPAWN 0x8AA1B4u   /* exe: the submit's helper start, the job's only reference */
#define HH_POPPED 0x8A9B35u  /* exe: a pop takes one from the queue's count +8 */
#define HH_LINGER 12         /* helpers that may stay at once: the AI avoidance solve alone has 32 parts a frame */
#define HH_IDLE_US 1000      /* a staying helper leaves after this long without a task */
#define HH_PAUSES 32         /* pause instructions between two looks at the queue */

typedef struct HhPin { uint32_t rva; uint8_t size; const char* what; const uint8_t* bytes; } HhPin;

static const uint8_t kHhJobPin[] = {0x40,0x53, 0x48,0x83,0xec,0x20, 0x48,0x8d,0x4c,0x24,0x48, 0x49,0x8b,0xd9, 0xe8,0xed,0xca,0xf6,0xff,
    0xb8,0xff,0xff,0xff,0xff, 0xf0,0x0f,0xc1,0x43,0x0c, 0x0f,0x1f,0x00, 0x48,0x8b,0xcb, 0xe8,0x18,0x00,0x00,0x00, 0x84,0xc0, 0x75,0xf4,
    0x33,0xc0, 0x48,0x83,0xc4,0x20, 0x5b, 0xc3};
static const uint8_t kHhSpawnPin[] = {0x41,0x8b,0x4e,0x0c, 0x44,0x2b,0xe9, 0x45,0x85,0xed, 0x0f,0x8e,0x87,0x00,0x00,0x00, 0x41,0x8b,0xcd,
    0xf0,0x41,0x0f,0xc1,0x4e,0x0c, 0x48,0x8b,0x0d,0xcc,0x9e,0x26,0x01, 0xff,0x51,0x48, 0x48,0x8b,0x3d,0xc2,0x9e,0x26,0x01,
    0x41,0xb8,0x10,0x00,0x00,0x00, 0x49,0x63,0xdd, 0x48,0x8b,0xc8, 0x48,0xc1,0xe3,0x03, 0x48,0x8b,0xf0, 0x48,0x8b,0xd3, 0xff,0x57,0x58,
    0x48,0xc7,0x44,0x24,0x30,0x00,0x00,0x00,0x00, 0x4c,0x8d,0x05,0x81,0x78,0xdf,0x00, 0x48,0x8b,0xf8, 0x48,0xc1,0xeb,0x03, 0x48,0x8b,0xd0,
    0x44,0x89,0x6c,0x24,0x28, 0x49,0x8b,0xc6, 0xc7,0x44,0x24,0x20,0x03,0x00,0x00,0x00, 0x48,0x8b,0xcb, 0x45,0x33,0xc9, 0xf3,0x48,0xab,
    0x48,0x8b,0x05,0x9f,0x9e,0x26,0x01, 0x48,0x8d,0x0d,0xe8,0xf7,0xff,0xff, 0xff,0x90,0x88,0x00,0x00,0x00};
static const uint8_t kHhPoppedPin[] = {0x4c,0x8b,0x72,0x08, 0xe8,0x72,0xa9,0xf6,0xff, 0x41,0xbf,0xff,0xff,0xff,0xff, 0x41,0x8b,0xc7,
    0xf0,0x0f,0xc1,0x43,0x08, 0x41,0x8b,0xc7};
static const uint8_t kHhSetupPin[] = {0x40,0x56, 0x48,0x83,0xec,0x20, 0x48,0x8b,0x05,0xa3,0xdb,0x2f,0x01, 0x48,0x8b,0xf1,
    0xff,0x90,0xd8,0x00,0x00,0x00, 0x89,0x06, 0x85,0xc0};
static const uint8_t kHhPopPin[] = {0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x48,0x89,0x7c,0x24,0x18, 0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xec,0x20,0x01,0x00,0x00};

static const HhPin kHhPins[] = {
    {HH_JOB, sizeof kHhJobPin, "the helper job", kHhJobPin},
    {HH_SPAWN, sizeof kHhSpawnPin, "the submit's helper start", kHhSpawnPin},
    {HH_POPPED, sizeof kHhPoppedPin, "the pop's count", kHhPoppedPin},
    {HH_SETUP, sizeof kHhSetupPin, "the worker's Havok setup", kHhSetupPin},
    {HH_POP, sizeof kHhPopPin, "the pop", kHhPopPin},
};
#define HH_PINS (sizeof kHhPins / sizeof kHhPins[0])

typedef void* (*HhSetupFn)(uint64_t* index);
typedef uint8_t (*HhPopFn)(uintptr_t queue);

static HhSetupFn g_hhSetup;
static HhPopFn g_hhPop;
static void* g_hhNext;          /* the original job, kept for the hook; never called */
static LONGLONG g_hhIdleTicks;  /* HH_IDLE_US in QueryPerformanceCounter ticks */
static volatile LONG g_hhStaying;
static volatile LONG64 g_hhJobs, g_hhStayed, g_hhLate;
static LONG64 g_hhLateReported;
static int g_hhState; /* 0 not asked yet, 2 waiting for the code, 1 installed, -1 not */
static ULONGLONG g_hhLoadedAt, g_hhReportedAt;

/* The first pin that differs, or NULL. */
static const char* HavokHelpersMismatch(uintptr_t exe)
{
    for (size_t i = 0; i < HH_PINS; i++)
        if (!PinMatches(exe + kHhPins[i].rva, kHhPins[i].bytes, kHhPins[i].size)) return kHhPins[i].what;
    return NULL;
}

/* 1 while this helper may stay: it stays already, or fewer than HH_LINGER do. */
static int HavokHelperStay(int* staying)
{
    if (*staying) return 1;
    if (InterlockedIncrement(&g_hhStaying) > HH_LINGER) { InterlockedDecrement(&g_hhStaying); return 0; }
    InterlockedIncrement64(&g_hhStayed);
    return *staying = 1;
}

/* 1 once HH_IDLE_US have passed since the first miss in a row (0 starts the count). */
static int HavokHelperIdle(LONGLONG* since)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (*since == 0) { *since = now.QuadPart; return 0; }
    return now.QuadPart - *since > g_hhIdleTicks;
}

/* The helper job (the job system passes the queue in r9, the fourth argument). As the game's: the thread's Havok setup,
 * one start less pending (+0xC), tasks while one is ready. Then, while tasks are still waiting (+8 counts the queue's
 * entries not popped yet, ready or not) and fewer than HH_LINGER helpers stay, it stays and looks again. */
static uint64_t HavokHelperJob(uint64_t a, uint64_t b, uint64_t c, uintptr_t queue)
{
    (void)a; (void)b; (void)c;
    uint64_t index = 0;
    LONGLONG idleSince = 0;
    LONG64 late = 0;
    int staying = 0;
    g_hhSetup(&index);
    InterlockedDecrement((volatile LONG*)(queue + 0xC));
    for (;;)
    {
        if (g_hhPop(queue)) { late += staying; idleSince = 0; continue; }
        if (*(volatile LONG*)(queue + 8) <= 0 || !HavokHelperStay(&staying) || HavokHelperIdle(&idleSince)) break;
        for (int i = 0; i < HH_PAUSES; i++) YieldProcessor();
    }
    if (staying) InterlockedDecrement(&g_hhStaying);
    InterlockedIncrement64(&g_hhJobs);
    if (late) InterlockedAdd64(&g_hhLate, late);
    return 0;
}

static void HavokHelpersBind(uintptr_t exe)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_hhIdleTicks = f.QuadPart * HH_IDLE_US / 1000000;
    g_hhSetup = (HhSetupFn)(exe + HH_SETUP);
    g_hhPop = (HhPopFn)(exe + HH_POP);
}

static void HavokHelpersWatchAt(uintptr_t exe)
{
    if (g_hhState == 0)
    {
        if (strcmp(g_build, "25480438")) { g_hhState = -1; Note("performance fix havok-helpers: not installed; not build 25480438"); return; }
        if (!PerfFixOn("havok-helpers")) { g_hhState = -1; return; }
        g_hhState = 2;
    }
    if (g_hhState != 2) return;
    const char* differs = HavokHelpersMismatch(exe);
    if (differs != NULL)
    {
        /* the exe's code is decoded some time after start; a minute after the game loaded, it never will be the reviewed one */
        if (g_hhLoadedAt == 0) g_hhLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_hhLoadedAt < 60000) return;
        g_hhState = -1;
        Note("performance fix havok-helpers: not installed; %s is not the reviewed code", differs);
        return;
    }
    HavokHelpersBind(exe);
    if (LoaderHook("havok-helpers: the task queue's helper job", exe + HH_JOB, (void*)HavokHelperJob, &g_hhNext))
    {
        g_hhState = -1;
        Note("performance fix havok-helpers: not installed; its hook could not be added");
        return;
    }
    g_hhState = 1;
    Note("performance fix havok-helpers: installed; up to %d Havok helpers stay up to %d us for tasks that become ready later",
        HH_LINGER, HH_IDLE_US);
}

/* From the watch thread, once the game has loaded; what the staying helpers ran, at most once a minute. */
static void PerfHavokHelpersWatch(void)
{
    if ((g_hhState == 0 || g_hhState == 2) && g_fired[HF_EVENT_GAME_LOADED]) HavokHelpersWatchAt((uintptr_t)GetModuleHandleW(NULL));
    if (g_hhState != 1 || g_hhLate == g_hhLateReported || GetTickCount64() - g_hhReportedAt < 60000) return;
    g_hhReportedAt = GetTickCount64();
    g_hhLateReported = g_hhLate;
    Note("performance fix havok-helpers: %lld Havok task(s) run by staying helpers so far; %lld of %lld helper job(s) stayed",
        (long long)g_hhLateReported, (long long)g_hhStayed, (long long)g_hhJobs);
}
