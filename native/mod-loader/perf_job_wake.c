/* job-wake (docs/performance.md#job-wake). The job system's ready callback (exe 0x625400) calls ReleaseSemaphore for
 * each job a thread readies while pool workers sleep. On the game thread HellForge keeps the game's count and moves
 * those calls: the releases asked for are owed, the game thread pays one only while none it owed is on its way, and
 * each worker that wakes pays the next before it looks for work. Other threads release as the game does. Every release
 * is still made on the same semaphore, no more of them; why none is lost is in the doc. Included by hellforge_modloader.c after render_guards.c: AllocateNearGame (damage.c),
 * JournalPut (live_code.c), ApiCodeMatches and FarJump (hooks.c), PerfFixOn, Note, g_build, g_fired. */

#define JW_NAME "job-wake"
#define JW_WAIT_SITE 0x625399u    /* the worker loop's WaitForSingleObject(semaphore, INFINITE): call [rip] */
#define JW_RELEASE_LP 0x625483u   /* the ready callback's ReleaseSemaphore on the low-priority semaphore */
#define JW_RELEASE 0x6254BCu      /* and on the workers' or the background semaphore */
#define JW_RELEASE_SLOT 0x13F68D0u /* the exe's import of ReleaseSemaphore */
#define JW_WAIT_SLOT 0x13F66F8u   /* and of WaitForSingleObject */
#define JW_CALLBACK 0x23B0CF0u    /* the ready callback {0x625400, scheduler} bikeshed calls */
#define JW_SCHEDULER 0x23B0CF8u
#define JW_WAKE_ALL 0x625F80u     /* the game's wake of every worker: each semaphore signalled by its worker count */
#define JW_SEMS 8                 /* semaphores known; the game has 2 or 3 */
#define JW_SUBMIT_LIMIT 1         /* a readying thread releases only while fewer wakes than this are on their way */
#define JW_WORKER_LIMIT 2         /* a woken worker releases until this many are on their way */
#define JW_STALE_MS 2             /* a wake on its way longer than this is taken as lost */
#define JW_WATCH_MS 50            /* a sleeping worker looks at what is owed this often */

typedef struct JwPin
{
    uint32_t rva;
    const char* hex;
} JwPin;

/* The ready callback whole, the worker loop whole (its wait to its return), bikeshed's two calls of the callback in
 * ReadyTasks (0x6249D0: the job is in its channel before the call), the callback's install in 0x625500 and the
 * wake of every worker (scheduler +0xE8 / +0xEC / +0xF0: the worker counts of the three semaphores). */
static const JwPin kJwPins[] = {
    {0x625400u, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 0F B6 F2 B8 AB AA AA AA F7 E6 41 8B D8 48 8B F9 D1 EA 44 0F B6 C2 8D 04 52 2B F0 84 D2 74 25 41 83"},
    {0x625430u, "E8 01 74 1F 41 83 E8 01 74 19 41 83 E8 01 74 13 41 83 F8 01 75 0D 48 8B 41 08 48 8B 88 E0 00 00 00 EB 4F 0F B6 05 8F E1 4E 01 48 8B 49 08 84 C0"},
    {0x625460u, "74 39 48 8B 89 D8 00 00 00 8B D3 F0 0F C1 51 10 85 D2 79 15 48 8B 09 4C 8D 44 24 30 F7 DA 3B D3 0F 4F D3 FF 15 47 14 DD 00 83 FE 02 74 34 48 8B"},
    {0x625490u, "47 08 48 8B 88 D0 00 00 00 EB 07 48 8B 89 D0 00 00 00 8B D3 F0 0F C1 51 10 85 D2 79 15 48 8B 09 4C 8D 44 24 38 F7 DA 3B D3 0F 4F D3 FF 15 0E 14"},
    {0x6254C0u, "DD 00 48 8B 5C 24 40 48 8B 74 24 48 48 83 C4 20 5F C3"},
    {0x625380u, "48 8B 4B 20 41 8B C7 F0 0F C1 41 10 83 F8 01 7D 0F 48 8B 09 BA FF FF FF FF FF 15 59 13 DD 00 90 48 8B 4B 10 BA 02 00 00 00 E8 52 15 00 00 84 C0"},
    {0x6253B0u, "75 EE E8 49 23 F9 FF 0F B6 43 7C 84 C0 75 15 0F B6 43 7D 84 C0 74 B9 33 C9 E8 22 AF 00 00 C6 43 7D 00 EB AC 48 8B 0B E8 A4 A6 F9 FF E8 2F 22 F9"},
    {0x6253E0u, "FF 48 8B 43 10 F0 44 0F C1 B8 F4 00 00 00 48 83 C4 68 41 5F 5B C3"},
    {0x624C16u, "48 8B 87 70 01 00 00 48 85 C0 74 0C 45 8B C3 40 0F B6 D5 48 8B C8 FF 10"},
    {0x624C78u, "4C 8B 8F 70 01 00 00 4D 85 C9 74 0D 45 8B C3 40 0F B6 D5 49 8B C9 41 FF 11"},
    {0x6255EAu, "48 89 35 07 B7 D8 01 48 8D 05 08 FE FF FF 48 89 05 F1 B6 D8 01"},
    {0x625F80u, "40 53 48 83 EC 20 8B 81 E8 00 00 00 48 8B D9 48 8B 89 D0 00 00 00 8B D0 F0 0F C1 51 10 85 D2 79 15 48 8B 09 4C 8D 44 24 30 F7 DA 3B D0 0F 4F D0"},
    {0x625FB0u, "FF 15 1A 09 DD 00 8B 83 EC 00 00 00 8B D0 48 8B 8B D8 00 00 00 F0 0F C1 51 10 85 D2 79 15 48 8B 09 4C 8D 44 24 38 F7 DA 3B D0 0F 4F D0 FF 15 ED"},
    {0x625FE0u, "08 DD 00 8B 83 F0 00 00 00 8B D0 48 8B 8B E0 00 00 00 F0 0F C1 51 10 85 D2 79 15 48 8B 09 4C 8D 44 24 40 F7 DA 3B D0 0F 4F D0 FF 15 C0 08 DD 00"},
    {0x626010u, "48 83 C4 20 5B C3"},
};
#define JW_PINS (sizeof kJwPins / sizeof kJwPins[0])

typedef BOOL(WINAPI* JwReleaseFn)(HANDLE, LONG, LONG*);
typedef DWORD(WINAPI* JwWaitFn)(HANDLE, DWORD);

typedef struct JwSem
{
    HANDLE volatile handle;
    volatile LONG owed;     /* releases the game asked for, not made yet */
    volatile LONG inflight; /* releases made by the fix whose worker has not woken yet */
    volatile LONG64 since;  /* QueryPerformanceCounter of the last one made */
} JwSem;

static JwSem g_jwSems[JW_SEMS];
static JwReleaseFn g_jwRelease;
static JwWaitFn g_jwWait;
static LONG64 g_jwStaleTicks;
static volatile LONG64 g_jwAsked, g_jwBySubmit, g_jwByWorker, g_jwByWatch, g_jwStale, g_jwDirect;
static int g_jwState; /* 0 not asked, 2 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_jwLoadedAt, g_jwReportedAt;
static LONG64 g_jwReportedAsked;

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

static LONG64 JwNow(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* The semaphore's slot, taken on first sight; NULL when all are taken (then the game's own calls). */
static JwSem* JwFind(HANDLE handle)
{
    for (int i = 0; i < JW_SEMS; i++)
    {
        HANDLE seen = g_jwSems[i].handle;
        if (seen == handle) return &g_jwSems[i];
        if (seen != NULL) continue;
        seen = InterlockedCompareExchangePointer((PVOID volatile*)&g_jwSems[i].handle, handle, NULL);
        if (seen == NULL || seen == handle) return &g_jwSems[i];
    }
    return NULL;
}

/* Pays what is owed while fewer than limit wakes are on their way. force: one release whatever is on its way (what is
 * on its way is then taken as lost). Each release first takes one off owed, so no more is released than was owed. */
static void JwKick(JwSem* s, LONG limit, int force, volatile LONG64* counter)
{
    for (;;)
    {
        LONG owed = s->owed;
        if (owed <= 0) return;
        if (!force && s->inflight >= limit)
        {
            if (JwNow() - s->since < g_jwStaleTicks) return;
            InterlockedIncrement64(&g_jwStale);
            force = 1;
        }
        if (InterlockedCompareExchange(&s->owed, owed - 1, owed) != owed) continue;
        if (force) InterlockedExchange(&s->inflight, 1);
        else InterlockedIncrement(&s->inflight);
        force = 0;
        s->since = JwNow();
        g_jwRelease(s->handle, 1, NULL);
        InterlockedIncrement64(counter);
    }
}

/* The ready callback's ReleaseSemaphore(handle, min(sleepers, jobs), &unused), on the readying thread. Only the game
 * thread's are owed: the render thread and the workers wait on their own job chains, so theirs wake at once. */
static BOOL WINAPI JwSubmitRelease(HANDLE handle, LONG count, LONG* previous)
{
    DWORD game = g_gameThread; /* events.c: set by the game thread's update */
    JwSem* s = count > 0 && game != 0 && GetCurrentThreadId() == game ? JwFind(handle) : NULL;
    if (s == NULL)
    {
        InterlockedIncrement64(&g_jwDirect);
        return g_jwRelease(handle, count, previous);
    }
    InterlockedAdd(&s->owed, count);
    InterlockedAdd64(&g_jwAsked, count);
    JwKick(s, JW_SUBMIT_LIMIT, 0, &g_jwBySubmit);
    if (previous != NULL) *previous = 0; /* the callback never reads it */
    return TRUE;
}

/* The worker loop's WaitForSingleObject(handle, INFINITE): the same wait in 50 ms steps, each step paying what is
 * owed; once woken, the worker takes itself off the wakes on their way and pays the next ones. */
static DWORD WINAPI JwWorkerWait(HANDLE handle, DWORD ms)
{
    JwSem* s = ms == INFINITE ? JwFind(handle) : NULL;
    if (s == NULL) return g_jwWait(handle, ms);
    DWORD result;
    while ((result = g_jwWait(handle, JW_WATCH_MS)) == WAIT_TIMEOUT)
        if (s->owed > 0) JwKick(s, 1, 1, &g_jwByWatch);
    if (result != WAIT_OBJECT_0) return result;
    LONG on;
    while ((on = s->inflight) > 0 && InterlockedCompareExchange(&s->inflight, on - 1, on) != on) {}
    MemoryBarrier(); /* the decrement before the look at owed, as the readying thread adds before it looks */
    JwKick(s, JW_WORKER_LIMIT, 0, &g_jwByWorker);
    return result;
}

static int JwPinned(uintptr_t exe)
{
    for (size_t i = 0; i < JW_PINS; i++)
        if (!ApiCodeMatches(exe + kJwPins[i].rva, kJwPins[i].hex)) return 0;
    return 1;
}

static HMODULE JwModuleOf(uintptr_t address)
{
    HMODULE module = NULL;
    if (address == 0 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)address, &module)) return NULL;
    return module;
}

/* The two imports lead into the kernel or the exe's own import stubs (its protector redirects them there), and on a
 * semaphore of HellForge's own they release, wait without a count and time out as ReleaseSemaphore and
 * WaitForSingleObject do. */
static int JwImportsWork(uintptr_t exe, JwReleaseFn release, JwWaitFn wait)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll"), base = GetModuleHandleA("kernelbase.dll"), own = JwModuleOf(exe);
    HMODULE r = JwModuleOf((uintptr_t)release), w = JwModuleOf((uintptr_t)wait);
    if (r == NULL || w == NULL || (r != k32 && r != base && r != own) || (w != k32 && w != base && w != own)) return 0;
    HANDLE probe = CreateSemaphoreA(NULL, 0, 4, NULL);
    if (probe == NULL) return 0;
    LONG previous = -1;
    int works = release(probe, 2, &previous) && previous == 0 && wait(probe, 0) == WAIT_OBJECT_0 &&
        wait(probe, 0) == WAIT_OBJECT_0 && wait(probe, 1) == WAIT_TIMEOUT;
    CloseHandle(probe);
    return works;
}

/* call rel32 to a stub in the page near the exe, then a nop: the 6-byte call [rip] it replaces. */
static int JwCallTo(uintptr_t exe, uint32_t site, uint8_t* stub)
{
    int64_t rel = (int64_t)((uintptr_t)stub - (exe + site + 5));
    if (rel >= 0x7FFF0000 || rel <= -0x7FFF0000) return 1;
    uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    int32_t rel32 = (int32_t)rel;
    memcpy(call + 1, &rel32, 4);
    return JournalPut(exe + site, call, sizeof call);
}

/* After the pins matched. The wait first, then every sleeping worker woken once (the game's own wake of all, which
 * keeps its count: a worker woken for no job finds none and sleeps again, now in HellForge's wait), then the releases.
 * 1 on, -1 not. */
static int JwInstallAt(uintptr_t exe)
{
    uintptr_t scheduler = *(volatile uintptr_t*)(exe + JW_SCHEDULER);
    if (*(volatile uintptr_t*)(exe + JW_CALLBACK) != exe + 0x625400u || scheduler == 0 || *(volatile uintptr_t*)(scheduler + 0xD0) == 0 ||
        *(volatile uintptr_t*)(scheduler + 0xD8) == 0 || *(volatile uintptr_t*)(scheduler + 0xE0) == 0)
    {
        Note("performance fix %s: not installed; the job system's ready callback is not the game's", JW_NAME);
        return -1;
    }
    uintptr_t release = *(volatile uintptr_t*)(exe + JW_RELEASE_SLOT), wait = *(volatile uintptr_t*)(exe + JW_WAIT_SLOT);
    if (!JwImportsWork(exe, (JwReleaseFn)release, (JwWaitFn)wait))
    {
        Note("performance fix %s: not installed; the job system's imports are not ReleaseSemaphore and WaitForSingleObject", JW_NAME);
        return -1;
    }
    g_jwRelease = (JwReleaseFn)release;
    g_jwWait = (JwWaitFn)wait;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_jwStaleTicks = f.QuadPart * JW_STALE_MS / 1000;
    uint8_t* page = AllocateNearGame(exe, 0x1000);
    if (page == NULL) { Note("performance fix %s: not installed; no memory near the game's exe", JW_NAME); return -1; }
    FarJump(page, (uint64_t)(uintptr_t)JwWorkerWait);
    FarJump(page + 16, (uint64_t)(uintptr_t)JwSubmitRelease);
    DWORD previous;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    if (JwCallTo(exe, JW_WAIT_SITE, page))
    {
        Note("performance fix %s: not installed; the worker loop could not be rewritten", JW_NAME);
        return -1;
    }
    ((void (*)(uintptr_t))(exe + JW_WAKE_ALL))(scheduler);
    int lowPriority = !JwCallTo(exe, JW_RELEASE_LP, page + 16), workers = !JwCallTo(exe, JW_RELEASE, page + 16);
    if (lowPriority && workers)
        Note("performance fix %s: installed; the game thread wakes one worker for the jobs it readies, the woken workers wake the rest", JW_NAME);
    else
        Note("performance fix %s: installed in part; the ready callback's %s release could not be rewritten", JW_NAME,
            lowPriority ? "workers'" : workers ? "low-priority" : "both");
    return 1;
}

/* 1 when the game has loaded (its job system long running) and the pins match. Otherwise 0, and a minute after the
 * game loaded the code never will be the reviewed one. */
static int JwReady(uintptr_t exe)
{
    if (!g_fired[HF_EVENT_GAME_LOADED]) return 0;
    if (JwPinned(exe)) return 1;
    if (g_jwLoadedAt == 0) g_jwLoadedAt = GetTickCount64();
    if (GetTickCount64() - g_jwLoadedAt < 60000) return 0;
    g_jwState = -1;
    Note("performance fix %s: not installed; the job system's code is not the reviewed one", JW_NAME);
    return 0;
}

static void JwWatchAt(uintptr_t exe)
{
    if (g_jwState == 0 && strcmp(g_build, "25480438"))
    {
        g_jwState = -1;
        Note("performance fix %s: not installed; not build 25480438", JW_NAME);
    }
    if (g_jwState == 0) g_jwState = PerfFixOn(JW_NAME) ? 2 : -1;
    if (g_jwState == 2 && JwReady(exe)) g_jwState = JwInstallAt(exe);
}

/* A line a minute while jobs are readied: how many releases were asked for and who made them. */
static void JwReport(void)
{
    LONG64 asked = g_jwAsked;
    ULONGLONG now = GetTickCount64();
    if (g_jwState != 1 || asked == g_jwReportedAsked || now - g_jwReportedAt < 60000) return;
    g_jwReportedAt = now;
    g_jwReportedAsked = asked;
    Note("performance fix %s: %lld wakes asked for; made by the game thread %lld, by woken workers %lld, by waiting "
        "workers %lld; %lld taken as lost; %lld made at once by other threads", JW_NAME, (long long)asked, (long long)g_jwBySubmit,
        (long long)g_jwByWorker, (long long)g_jwByWatch, (long long)g_jwStale, (long long)g_jwDirect);
}

/* From the start; it installs once the game has loaded. */
static void PerfJobWakeWatch(void)
{
    if (g_jwState == 0 || g_jwState == 2) JwWatchAt((uintptr_t)GetModuleHandleW(NULL));
    JwReport();
}
