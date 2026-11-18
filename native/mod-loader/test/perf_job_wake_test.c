/* The job system's wakes (perf_job_wake.c) on a synthetic exe image: the pinned ready callback and worker loop run as
 * the game has them and as HellForge rewrites them, on real Windows semaphores and threads. The job poll is a counter
 * of queued jobs; a burst of readied jobs has to be run within 2 s, or a wake was lost. Included by stats_test.c. */

#define JWT_IMAGE 0x2400000u
#define JWT_LOW_PRIORITY 0x1B135E9u
#define JWT_POLL 0x626900u
#define JWT_ENTER 0x700000u /* a thunk with the worker function's frame, into its loop at 0x625380 */
#define JWT_WORKERS 6
#define JWT_BACKGROUND 2
#define JWT_THREADS (JWT_WORKERS + JWT_BACKGROUND)

typedef struct JwtJobs
{
    uint8_t pad[0xF4];
    volatile LONG threads; /* the loop's exit takes one off +0xF4 */
    volatile LONG queued, done;
} JwtJobs;

typedef struct JwtSem
{
    HANDLE handle;
    const char* name;
    volatile LONG count; /* +0x10 */
} JwtSem;

typedef struct JwtWorker
{
    void* scheduler;     /* +0x00 */
    uint8_t pad8[8];
    JwtJobs* jobs;       /* +0x10 */
    uint8_t pad18[8];
    JwtSem* sem;         /* +0x20 */
    uint8_t pad28[0x7C - 0x28];
    volatile uint8_t exit, flush; /* +0x7C, +0x7D */
    uint8_t pad7e[2];
} JwtWorker;

typedef void (*JwtReadyFn)(void* callback, uint8_t channel, uint32_t count);

static JwtJobs g_jwtJobs[2];
static JwtSem g_jwtSems[2];
static JwtWorker g_jwtWorker[JWT_THREADS];
static uint8_t g_jwtScheduler[0x100];
static void* g_jwtCallback[2];
static DWORD g_jwtSubmitter;
static volatile LONG g_jwtSubmitterReleases;
static FARPROC g_jwtRealRelease;

/* The poll as the loop calls it (jobs, 2): one queued job run, or none. */
static uint8_t JwtPoll(JwtJobs* jobs, int priority)
{
    (void)priority;
    LONG q;
    while ((q = jobs->queued) > 0)
        if (InterlockedCompareExchange(&jobs->queued, q - 1, q) == q)
        {
            for (volatile int spin = 0; spin < 200; spin++) {}
            InterlockedIncrement(&jobs->done);
            return 1;
        }
    return 0;
}

static void JwtNothing(void) {}

/* The import slot before the install: the game's own calls, counted on the readying thread. */
static BOOL WINAPI JwtCountedRelease(HANDLE handle, LONG count, LONG* previous)
{
    if (GetCurrentThreadId() == g_jwtSubmitter) InterlockedIncrement(&g_jwtSubmitterReleases);
    return ((JwReleaseFn)(void*)g_jwtRealRelease)(handle, count, previous);
}

static uintptr_t g_jwtExe;

static DWORD WINAPI JwtWorkerRun(void* worker)
{
    ((void (*)(void*))(void*)(g_jwtExe + JWT_ENTER))(worker);
    return 0;
}

static uint8_t* JwtImage(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, JWT_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    for (size_t i = 0; i < JW_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        size_t count = ParsePattern(kJwPins[i].hex, bytes, mask);
        assert(count > 0);
        memcpy(image + kJwPins[i].rva, bytes, count);
    }
    FarJump(image + JWT_POLL, (uint64_t)(uintptr_t)JwtPoll);
    static const uint32_t nothing[] = {0x5B7700u, 0x6302F0u, 0x5BFA80u, 0x5B7610u};
    for (size_t i = 0; i < sizeof nothing / sizeof nothing[0]; i++) FarJump(image + nothing[i], (uint64_t)(uintptr_t)JwtNothing);
    /* push rbx; push r15; sub rsp, 0x68 (the worker function's frame); mov rbx, rcx; mov r15d, -1; jmp 0x625380 */
    static const uint8_t enter[] = {0x53, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x48, 0x89, 0xCB, 0x41, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE9};
    memcpy(image + JWT_ENTER, enter, sizeof enter);
    int32_t rel = (int32_t)(0x625380 - (JWT_ENTER + (int32_t)sizeof enter + 4));
    memcpy(image + JWT_ENTER + sizeof enter, &rel, 4);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_jwtRealRelease = GetProcAddress(k32, "ReleaseSemaphore");
    uint64_t wait = (uint64_t)(uintptr_t)GetProcAddress(k32, "WaitForSingleObject");
    uint64_t counted = (uint64_t)(uintptr_t)JwtCountedRelease;
    memcpy(image + JW_RELEASE_SLOT, &counted, 8);
    memcpy(image + JW_WAIT_SLOT, &wait, 8);
    image[JWT_LOW_PRIORITY] = 0;
    uint64_t callback = (uint64_t)(uintptr_t)(image + 0x625400u), scheduler = (uint64_t)(uintptr_t)g_jwtScheduler;
    memcpy(image + JW_CALLBACK, &callback, 8);
    memcpy(image + JW_SCHEDULER, &scheduler, 8);
    FlushInstructionCache(GetCurrentProcess(), image, JWT_IMAGE);
    return image;
}

/* The scheduler (+0xD0 workers, +0xD8 low priority, +0xE0 background), the callback {function, scheduler} and the
 * worker threads in the game's loop. */
static HANDLE g_jwtThreads[JWT_THREADS];

static void JwtStart(uintptr_t exe)
{
    memset(g_jwtJobs, 0, sizeof g_jwtJobs);
    for (int i = 0; i < 2; i++)
    {
        g_jwtSems[i].handle = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
        g_jwtSems[i].name = i ? "background" : "workers";
        g_jwtSems[i].count = 0;
    }
    void* workers = &g_jwtSems[0], *background = &g_jwtSems[1];
    memcpy(g_jwtScheduler + 0xD0, &workers, 8);
    memcpy(g_jwtScheduler + 0xD8, &workers, 8);
    memcpy(g_jwtScheduler + 0xE0, &background, 8);
    uint32_t counts[3] = {JWT_WORKERS, 0, JWT_BACKGROUND}; /* +0xE8 workers, +0xEC low priority, +0xF0 background */
    memcpy(g_jwtScheduler + 0xE8, counts, sizeof counts);
    g_jwtCallback[0] = (void*)(exe + 0x625400u);
    g_jwtCallback[1] = g_jwtScheduler;
    for (int i = 0; i < JWT_THREADS; i++)
    {
        int bg = i >= JWT_WORKERS;
        memset(&g_jwtWorker[i], 0, sizeof g_jwtWorker[i]);
        g_jwtWorker[i].scheduler = g_jwtScheduler;
        g_jwtWorker[i].jobs = &g_jwtJobs[bg];
        g_jwtWorker[i].sem = &g_jwtSems[bg];
        InterlockedIncrement(&g_jwtJobs[bg].threads);
        g_jwtThreads[i] = CreateThread(NULL, 0, JwtWorkerRun, &g_jwtWorker[i], 0, NULL);
        assert(g_jwtThreads[i] != NULL);
    }
}

/* What the fix still owes on its semaphores. */
static LONG JwtOwed(void)
{
    LONG owed = 0;
    for (int i = 0; i < JW_SEMS; i++) owed += g_jwSems[i].owed;
    return owed;
}

/* All asleep: each pool's count is minus its threads. */
static int JwtAllAsleep(void)
{
    return g_jwtSems[0].count == -JWT_WORKERS && g_jwtSems[1].count == -JWT_BACKGROUND;
}

static void JwtWaitAsleep(void)
{
    for (int i = 0; i < 4000 && !JwtAllAsleep(); i++) Sleep(1);
    assert(JwtAllAsleep());
}

/* bursts of readied jobs, each run to the end before the next; the slowest burst's ms. */
static double JwtBursts(uintptr_t exe, int bursts, unsigned seed)
{
    JwtReadyFn ready = (JwtReadyFn)(void*)(exe + 0x625400u);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double slowest = 0;
    g_jwtSubmitter = GetCurrentThreadId();
    for (int b = 0; b < bursts; b++)
    {
        seed = seed * 1103515245u + 12345u;
        int jobs = 1 + (int)((seed >> 16) % 12), bg = ((seed >> 8) & 7) == 0;
        LONG target0 = g_jwtJobs[0].done, target1 = g_jwtJobs[1].done;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int j = 0; j < jobs; j++)
        {
            seed = seed * 1103515245u + 12345u;
            int many = ((seed >> 12) & 3) == 0 ? 3 : 1; /* sometimes one call for three jobs */
            JwtJobs* q = &g_jwtJobs[bg];
            InterlockedAdd(&q->queued, many);
            if (bg) target1 += many;
            else target0 += many;
            uint8_t channel = (uint8_t)(bg ? 12 + (seed >> 20) % 3 : (seed >> 20) % 12);
            ready(g_jwtCallback, channel, (uint32_t)many);
        }
        for (;;)
        {
            QueryPerformanceCounter(&t1);
            if (g_jwtJobs[0].done >= target0 && g_jwtJobs[1].done >= target1) break;
            assert((double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart < 2.0); /* a lost wake */
            YieldProcessor();
        }
        double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (ms > slowest) slowest = ms;
        if ((b & 15) == 15) JwtWaitAsleep(); /* every 16th burst from all asleep */
    }
    return slowest;
}

/* As the game's shutdown: the exit flags, then releases until every thread has left its loop. */
static void JwtStop(void)
{
    for (int i = 0; i < JWT_THREADS; i++) g_jwtWorker[i].exit = 1;
    for (int tries = 0; tries < 5000 && (g_jwtJobs[0].threads || g_jwtJobs[1].threads); tries++)
    {
        for (int i = 0; i < 2; i++)
        {
            LONG n = i ? JWT_BACKGROUND : JWT_WORKERS, was = InterlockedExchangeAdd(&g_jwtSems[i].count, n);
            if (was < 0) ((JwReleaseFn)(void*)g_jwtRealRelease)(g_jwtSems[i].handle, -was < n ? -was : n, NULL);
        }
        Sleep(1);
    }
    assert(WaitForMultipleObjects(JWT_THREADS, g_jwtThreads, TRUE, 5000) == WAIT_OBJECT_0);
    for (int i = 0; i < JWT_THREADS; i++) CloseHandle(g_jwtThreads[i]);
    for (int i = 0; i < 2; i++) CloseHandle(g_jwtSems[i].handle);
}

static void JwtStates(uint8_t* image, uintptr_t exe)
{
    for (size_t i = 0; i < JW_PINS; i++)
    {
        uint32_t at = kJwPins[i].rva + 3;
        image[at] ^= 0x10;
        assert(!JwPinned(exe));
        image[at] ^= 0x10;
    }
    assert(JwPinned(exe));
    image[0x625410u] ^= 0x10; /* code not decoded yet: a wait, then a minute after the load it gives up */
    g_jwState = 0;
    g_fired[HF_EVENT_GAME_LOADED] = 0;
    JwWatchAt(exe);
    assert(g_jwState == 2);
    g_fired[HF_EVENT_GAME_LOADED] = 1;
    JwWatchAt(exe);
    g_jwLoadedAt = GetTickCount64() - 60001;
    JwWatchAt(exe);
    assert(g_jwState == -1);
    image[0x625410u] ^= 0x10;
    g_jwLoadedAt = 0;
    uintptr_t callback = *(uintptr_t*)(image + JW_CALLBACK), none = 0;
    memcpy(image + JW_CALLBACK, &none, 8); /* the job system's callback not the game's: refused */
    g_jwState = 0;
    JwWatchAt(exe);
    assert(g_jwState == -1);
    memcpy(image + JW_CALLBACK, &callback, 8);
    g_jwState = 0; /* the import leads outside the kernel and the exe (the counter): refused */
    JwWatchAt(exe);
    assert(g_jwState == -1);
    uint64_t counted, wrong = (uint64_t)(uintptr_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetLastError");
    memcpy(&counted, image + JW_RELEASE_SLOT, 8);
    memcpy(image + JW_RELEASE_SLOT, &wrong, 8); /* in the kernel, but no release: refused */
    g_jwState = 0;
    JwWatchAt(exe);
    assert(g_jwState == -1);
    memcpy(image + JW_RELEASE_SLOT, &counted, 8);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "heap-trim, Job-Wake");
    g_jwState = 0;
    JwWatchAt(exe);
    assert(g_jwState == -1);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    strcpy(g_build, "99999999");
    g_jwState = 0;
    JwWatchAt(exe);
    assert(g_jwState == -1);
    strcpy(g_build, "25480438");
    static const uint8_t call[6] = {0xFF, 0x15};
    assert(!memcmp(image + JW_WAIT_SITE, call, 2) && !memcmp(image + JW_RELEASE, call, 2) && !memcmp(image + JW_RELEASE_LP, call, 2));
}

/* The kick on its own: one wake on its way holds the next back until it arrives or goes stale. */
static void JwtKick(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_jwStaleTicks = f.QuadPart * JW_STALE_MS / 1000;
    JwReleaseFn installed = g_jwRelease;
    g_jwRelease = (JwReleaseFn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ReleaseSemaphore");
    JwSem s = {0};
    s.handle = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    LONG64 made = g_jwBySubmit, stale = g_jwStale;
    s.owed = 3;
    JwKick(&s, 1, 0, &g_jwBySubmit);
    assert(s.owed == 2 && s.inflight == 1 && g_jwBySubmit == made + 1);
    JwKick(&s, 1, 0, &g_jwBySubmit);
    assert(s.owed == 2 && s.inflight == 1); /* held back */
    s.since -= g_jwStaleTicks + 1;
    JwKick(&s, 1, 0, &g_jwBySubmit);
    assert(s.owed == 1 && s.inflight == 1 && g_jwStale == stale + 1); /* stale: one more, the old one taken as lost */
    JwKick(&s, 3, 0, &g_jwBySubmit);
    assert(s.owed == 0 && s.inflight == 2);
    JwKick(&s, 9, 1, &g_jwBySubmit);
    assert(s.owed == 0 && g_jwBySubmit == made + 3); /* nothing owed: nothing released, forced or not */
    int signalled = 0;
    while (WaitForSingleObject(s.handle, 0) == WAIT_OBJECT_0) signalled++;
    assert(signalled == 3);
    CloseHandle(s.handle);
    g_jwRelease = installed;
}

static void JobWakeFixture(void)
{
    uint8_t* image = JwtImage();
    uintptr_t exe = (uintptr_t)image;
    g_jwtExe = exe;
    char build[sizeof g_build];
    memcpy(build, g_build, sizeof build);
    strcpy(g_build, "25480438");
    LONG loaded = g_fired[HF_EVENT_GAME_LOADED];
    JwtKick();
    JwtStart(exe);
    JwtStates(image, exe);

    JwtWaitAsleep();
    g_jwtSubmitterReleases = 0;
    double before = JwtBursts(exe, 3000, 7);
    LONG gameReleases = g_jwtSubmitterReleases;
    JwtWaitAsleep();

    /* installed with every worker asleep in the game's own wait: the install wakes them once, so every later sleep
     * is in HellForge's wait and no wake it makes goes stale */
    LONG64 stale = g_jwStale;
    uint64_t real = (uint64_t)(uintptr_t)g_jwtRealRelease;
    memcpy(image + JW_RELEASE_SLOT, &real, 8);
    g_jwState = 0;
    JwWatchAt(exe);
    assert(g_jwState == 1);
    assert(image[JW_WAIT_SITE] == 0xE8 && image[JW_RELEASE] == 0xE8 && image[JW_RELEASE_LP] == 0xE8 && image[JW_WAIT_SITE + 5] == 0x90);
    JwWatchAt(exe);
    assert(g_jwState == 1); /* once */
    DWORD gameThread = g_gameThread;
    LONG64 direct = g_jwDirect;
    g_gameThread = 0; /* before the game thread is known: every release made at once, as the game makes it */
    JwtBursts(exe, 200, 3);
    assert(g_jwDirect > direct);
    JwtWaitAsleep();
    g_gameThread = GetCurrentThreadId(); /* the readying thread is the game thread */
    LONG64 bySubmit = g_jwBySubmit, asked = g_jwAsked;
    direct = g_jwDirect;
    double after = JwtBursts(exe, 3000, 7);
    assert(g_jwDirect == direct);
    LONG64 hfSubmitter = g_jwBySubmit - bySubmit, hfAsked = g_jwAsked - asked;
    JwtWaitAsleep();
    for (int i = 0; i < 2; i++) assert(g_jwtSems[i].count == (i ? -JWT_BACKGROUND : -JWT_WORKERS));
    assert(JwtOwed() == 0); /* everything owed was paid */
    assert(hfAsked > 0 && hfSubmitter < gameReleases);
    assert(g_jwStale == stale);
    JwtStop();
    g_gameThread = gameThread;

    memcpy(g_build, build, sizeof build);
    g_fired[HF_EVENT_GAME_LOADED] = loaded;
    VirtualFree(image, 0, MEM_RELEASE);
    printf("PASS job wake: the game's ready callback and worker loop run on a synthetic image with real semaphores and "
        "threads, 3000 bursts of 1-12 readied jobs each run to the end (slowest %.1f ms as the game has it, %.1f ms "
        "rewritten); pins refuse any changed byte, wait for decoded code and give up, refuse imports that are not the "
        "kernel's or do not release and wait, off by name and on another build; installed once with every worker asleep in the game's wait, other threads' "
        "releases made at once, no wake lost, all owed paid, the game thread's releases %ld -> %lld (%lld asked for)\n",
        before, after, (long)gameReleases, (long long)hfSubmitter, (long long)hfAsked);
}
