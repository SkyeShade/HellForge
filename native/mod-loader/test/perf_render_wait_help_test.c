/* The render thread's waits (perf_render_wait_help.c) on a synthetic exe image: the pinned poll and counter wait run
 * as the game has them and as HellForge rewrites them, the job pop a recorder of the channels asked, the thread index
 * a fake TLS block. Included by stats_test.c. */

#define RWH_IMAGE 0x2361000u
#define RWH_POP 0x624D00u
#define RWH_WORKERS 0x23608E4u /* the pool's worker count */
#define RWH_LOW_POWER 0x1B135E9u

typedef uint8_t (*RwhPollFn)(void* jobs, int priority);
typedef void (*RwhWaitFn)(void* jobs, volatile int32_t* counter, int unused, int priority);

static uint8_t g_rwhSeen[64];
static int g_rwhSeenCount, g_rwhRunOn, g_rwhDoneAfter;
static volatile int32_t* g_rwhCounter;
static _Alignas(16) uint8_t g_rwhTlsBlock[0x200];
static void* g_rwhTlsArray[1] = {g_rwhTlsBlock};

/* The job pop as the game calls it (shed, channel): only globals, as the fake TLS is in place meanwhile. A job on
 * g_rwhRunOn runs and ends the wait; any wait also ends after g_rwhDoneAfter pops. */
static int RwhFakePop(void* shed, uint32_t channel)
{
    if (g_rwhSeenCount < (int)sizeof g_rwhSeen) g_rwhSeen[g_rwhSeenCount] = (uint8_t)channel;
    g_rwhSeenCount++;
    int ran = (int)(uint8_t)channel == g_rwhRunOn;
    if (g_rwhCounter != NULL && (ran || g_rwhSeenCount >= g_rwhDoneAfter)) *g_rwhCounter = 1;
    return ran;
}

/* One call on thread index `thread`; the channels asked land in g_rwhSeen. Returns the poll's result (0 for a wait). */
static int RwhRun(uintptr_t exe, int wait, uint32_t thread, int priority, int runOn, int doneAfter)
{
    uint8_t jobs[0x100] = {0};
    void* shed = (void*)(uintptr_t)0x5eed;
    memcpy(jobs + 0xF8, &shed, 8);
    volatile int32_t counter = 0;
    g_rwhSeenCount = 0;
    g_rwhRunOn = runOn;
    g_rwhDoneAfter = doneAfter;
    g_rwhCounter = wait ? &counter : NULL;
    memcpy(g_rwhTlsBlock + 0x1D8, &thread, 4);
    void** slot = (void**)((uint8_t*)NtCurrentTeb() + 0x58);
    void* saved = *slot;
    int result = 0;
    *slot = g_rwhTlsArray;
    if (wait) ((RwhWaitFn)(void*)(exe + 0x627160u))(jobs, &counter, 0, priority);
    else result = ((RwhPollFn)(void*)(exe + 0x626900u))(jobs, priority);
    *slot = saved;
    g_rwhCounter = NULL;
    return result;
}

static int RwhSaw(const uint8_t* expect, int count) { return g_rwhSeenCount == count && !memcmp(g_rwhSeen, expect, (size_t)count); }

/* What other threads ask is the game's either way: the main thread, a worker, a background worker. */
static void RwhOthersAsBefore(uintptr_t exe)
{
    static const uint8_t mainP1[] = {0x0f, 0x00, 0x03, 0x10};
    static const uint8_t workerP2[] = {0x09, 0x00, 0x03, 0x06, 0x0a, 0x01, 0x04, 0x07, 0x0b, 0x02, 0x05, 0x08};
    static const uint8_t backgroundP1[] = {0x0c, 0x0d};
    assert(RwhRun(exe, 0, 0, 1, -1, 99) == 0 && RwhSaw(mainP1, 4));
    assert(RwhRun(exe, 0, 2, 2, -1, 99) == 0 && RwhSaw(workerP2, 12));
    assert(RwhRun(exe, 0, 30, 1, -1, 99) == 0 && RwhSaw(backgroundP1, 2));
    RwhRun(exe, 1, 0, 1, -1, 8);
    assert(RwhSaw((const uint8_t[]){0x0f, 0x00, 0x03, 0x10, 0x0f, 0x00, 0x03, 0x10}, 8));
}

static void RwhGameRuns(uintptr_t exe)
{
    static const uint8_t p0[] = {0x12, 0x00}, p1[] = {0x12, 0x00, 0x06, 0x13}, p2[] = {0x12, 0x00, 0x06, 0x13, 0x01, 0x07};
    assert(RwhRun(exe, 0, 1, 0, 6, 99) == 0 && RwhSaw(p0, 2)); /* a render-affine priority-0 job left to the workers */
    assert(RwhRun(exe, 0, 1, 1, 7, 99) == 0 && RwhSaw(p1, 4));
    assert(RwhRun(exe, 0, 1, 2, 7, 99) == 1 && RwhSaw(p2, 6));
    RwhRun(exe, 1, 1, 1, 7, 8); /* the culling wait: passes over channel 7 until a worker's job ends it */
    assert(RwhSaw((const uint8_t[]){0x12, 0x00, 0x06, 0x13, 0x12, 0x00, 0x06, 0x13}, 8));
    RwhOthersAsBefore(exe);
}

static void RwhHellForgeRuns(uintptr_t exe)
{
    static const uint8_t p0[] = {0x12, 0x00, 0x06}, p1[] = {0x12, 0x00, 0x06, 0x13, 0x01, 0x07};
    assert(RwhRun(exe, 0, 1, -1, 0x12, 99) == 0 && g_rwhSeenCount == 0); /* no channels, as before */
    assert(RwhRun(exe, 0, 1, 0, 6, 99) == 1 && RwhSaw(p0, 3));
    assert(RwhRun(exe, 0, 1, 0, -1, 99) == 0 && RwhSaw(p0, 3));
    assert(RwhRun(exe, 0, 1, 1, 7, 99) == 1 && RwhSaw(p1, 6));
    for (int priority = 2; priority <= 9; priority++) /* never more than the idle loop's 6: no channel it never took */
        assert(RwhRun(exe, 0, 1, priority, -1, 99) == 0 && RwhSaw(p1, 6));
    RwhRun(exe, 1, 1, 1, 7, 99); /* the culling wait runs its own job, which ends it */
    assert(RwhSaw(p1, 6));
    RwhRun(exe, 1, 1, 0, -1, 6);
    assert(RwhSaw((const uint8_t[]){0x12, 0x00, 0x06, 0x12, 0x00, 0x06}, 6));
    RwhRun(exe, 1, 1, 3, -1, 6);
    assert(RwhSaw(p1, 6));
    RwhOthersAsBefore(exe);
}

static uint8_t* RwhImage(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RWH_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    for (size_t i = 0; i < RWH_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        size_t count = ParsePattern(kRwhPins[i].hex, bytes, mask);
        assert(count > 0);
        memcpy(image + kRwhPins[i].rva, bytes, count);
    }
    FarJump(image + RWH_POP, (uint64_t)(uintptr_t)RwhFakePop);
    uint32_t workers = 28;
    memcpy(image + RWH_WORKERS, &workers, 4);
    image[RWH_LOW_POWER] = 0;
    FlushInstructionCache(GetCurrentProcess(), image, RWH_IMAGE);
    return image;
}

static void RwhStates(uint8_t* image, uintptr_t exe)
{
    static const uint32_t changed[] = {0x626900u, 0x626942u, 0x626B28u, 0x627160u, 0x6271BAu, 0x6273D9u, 0x6261D2u, 0x113E2Eu};
    for (size_t i = 0; i < sizeof changed / sizeof changed[0]; i++)
    {
        image[changed[i]] ^= 0x10;
        assert(!RwhPinned(exe));
        image[changed[i]] ^= 0x10;
    }
    assert(RwhPinned(exe));
    LONG loaded = g_fired[HF_EVENT_GAME_LOADED];
    image[0x6261D2u] ^= 0x10; /* code not decoded yet: a wait, then a minute after the load it gives up */
    g_rwhState = 0;
    g_fired[HF_EVENT_GAME_LOADED] = 0;
    RwhWatchAt(exe);
    assert(g_rwhState == 2);
    g_fired[HF_EVENT_GAME_LOADED] = 1;
    RwhWatchAt(exe);
    g_rwhLoadedAt = GetTickCount64() - 60001;
    RwhWatchAt(exe);
    assert(g_rwhState == -1);
    image[0x6261D2u] ^= 0x10;
    g_rwhLoadedAt = 0;
    g_fired[HF_EVENT_GAME_LOADED] = loaded;
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "heap-trim, Render-Wait-Help");
    g_rwhState = 0;
    RwhWatchAt(exe);
    assert(g_rwhState == -1 && RwhPinned(exe));
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    strcpy(g_build, "99999999");
    g_rwhState = 0;
    RwhWatchAt(exe);
    assert(g_rwhState == -1 && RwhPinned(exe));
    strcpy(g_build, "25480438");
}

static void RenderWaitHelpFixture(void)
{
    uint8_t* image = RwhImage();
    uintptr_t exe = (uintptr_t)image;
    char build[sizeof g_build];
    memcpy(build, g_build, sizeof build);
    strcpy(g_build, "25480438");
    RwhGameRuns(exe);
    RwhStates(image, exe);
    RwhGameRuns(exe); /* refused or off: the game's code still */

    g_rwhState = 0;
    RwhWatchAt(exe);
    assert(g_rwhState == 1);
    assert(!memcmp(image + RWH_POLL_SITE, kRwhPollCode, RWH_POLL_LENGTH) && !memcmp(image + RWH_WAIT_SITE, kRwhWaitCode, RWH_WAIT_LENGTH));
    RwhWatchAt(exe);
    assert(g_rwhState == 1); /* once */
    RwhHellForgeRuns(exe);

    memcpy(g_build, build, sizeof build);
    VirtualFree(image, 0, MEM_RELEASE);
    puts("PASS render wait help: the game's poll and counter wait run on a synthetic image, the render thread skipping its channel 6 and 7 jobs; "
        "pins refuse any changed byte, wait for decoded code and give up, off by name and on another build; installed once, it runs them, "
        "never more than the idle loop's 6 channels, and every other thread asks what it asked before");
}
