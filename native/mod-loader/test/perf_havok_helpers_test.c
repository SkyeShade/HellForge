/* Havok's task queue helpers (perf_havok_helpers.c) on a synthetic exe image: the pins and their refusals, the hook
 * installed through the loader's chain over the job's pinned prologue, then the job run through it against a stub
 * queue: tasks while ready, gone at once when nothing waits, staying for tasks released later, the HH_LINGER cap and
 * the idle limit. Included by stats_test.c. */

#define HH_TEST_EXE 0x8AB000u

static volatile LONG g_hhTestReady, g_hhTestRun, g_hhTestSetups, g_hhTestReleaseAfter;
static uint8_t g_hhTestQueue[0x40];

static void* HhTestSetup(uint64_t* index) { *index = 7; InterlockedIncrement(&g_hhTestSetups); return index; }

/* One ready task a pop; a waiting one (the queue's +8 holds both) becomes ready after g_hhTestReleaseAfter misses. */
static uint8_t HhTestPop(uintptr_t queue)
{
    if (g_hhTestReady > 0)
    {
        InterlockedDecrement(&g_hhTestReady);
        InterlockedDecrement((volatile LONG*)(queue + 8));
        InterlockedIncrement(&g_hhTestRun);
        return 1;
    }
    if (*(volatile LONG*)(queue + 8) > 0 && g_hhTestReleaseAfter > 0 && InterlockedDecrement(&g_hhTestReleaseAfter) == 0)
        InterlockedIncrement(&g_hhTestReady);
    return 0;
}

static void HhTestQueue(LONG ready, LONG waiting, LONG releaseAfter)
{
    memset(g_hhTestQueue, 0, sizeof g_hhTestQueue);
    *(LONG*)(g_hhTestQueue + 8) = ready + waiting;
    *(LONG*)(g_hhTestQueue + 0xC) = 1; /* one start pending */
    g_hhTestReady = ready;
    g_hhTestRun = 0;
    g_hhTestReleaseAfter = releaseAfter;
}

typedef uint64_t (*HhTestJobFn)(uint64_t, uint64_t, uint64_t, uintptr_t);

static void PerfHavokHelpersFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, HH_TEST_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    for (size_t i = 0; i < HH_PINS; i++) memcpy(image + kHhPins[i].rva, kHhPins[i].bytes, kHhPins[i].size);
    FlushInstructionCache(GetCurrentProcess(), image, HH_TEST_EXE);
    assert(HavokHelpersMismatch(exe) == NULL);
    for (size_t i = 0; i < HH_PINS; i++)
    {
        uint8_t* at = image + kHhPins[i].rva + kHhPins[i].size - 1;
        *at ^= 0x10;
        assert(HavokHelpersMismatch(exe) == kHhPins[i].what);
        *at ^= 0x10;
    }
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", L"havok-helpers");
    HavokHelpersWatchAt(exe);
    assert(g_hhState == -1);
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", NULL);
    g_hhState = 0;
    image[HH_POP + 3] ^= 0x10; /* not the reviewed code: it waits, then refuses without a hook */
    g_hhLoadedAt = GetTickCount64();
    HavokHelpersWatchAt(exe);
    assert(g_hhState == 2);
    g_hhLoadedAt = GetTickCount64() - 61000;
    HavokHelpersWatchAt(exe);
    assert(g_hhState == -1 && g_hhNext == NULL);
    image[HH_POP + 3] ^= 0x10;
    g_hhState = 0;
    HavokHelpersWatchAt(exe);
    assert(g_hhState == 1 && g_hhNext != NULL && g_hhIdleTicks > 0);
    assert(g_hhSetup == (HhSetupFn)(exe + HH_SETUP) && g_hhPop == (HhPopFn)(exe + HH_POP));
    g_hhSetup = HhTestSetup;
    g_hhPop = HhTestPop;
    HhTestJobFn job = (HhTestJobFn)(exe + HH_JOB); /* through the hook */

    /* ready tasks only: all run, then it leaves without staying */
    HhTestQueue(5, 0, 0);
    assert(job(0, 0, 0, (uintptr_t)g_hhTestQueue) == 0);
    assert(g_hhTestRun == 5 && *(LONG*)(g_hhTestQueue + 0xC) == 0 && g_hhTestSetups == 1 && g_hhStayed == 0 && g_hhStaying == 0);
    /* one task released after 50 misses: it stays and runs it */
    HhTestQueue(2, 1, 50);
    job(0, 0, 0, (uintptr_t)g_hhTestQueue);
    assert(g_hhTestRun == 3 && g_hhStayed == 1 && g_hhLate == 1 && g_hhStaying == 0 && *(LONG*)(g_hhTestQueue + 8) == 0);
    /* one that never comes: it leaves after the idle limit */
    HhTestQueue(0, 1, 0);
    ULONGLONG t0 = GetTickCount64();
    job(0, 0, 0, (uintptr_t)g_hhTestQueue);
    assert(g_hhTestRun == 0 && g_hhStayed == 2 && g_hhStaying == 0 && GetTickCount64() - t0 < 1000);
    /* the cap: with HH_LINGER staying already, the next one leaves like the game's */
    g_hhStaying = HH_LINGER;
    HhTestQueue(0, 1, 0);
    job(0, 0, 0, (uintptr_t)g_hhTestQueue);
    assert(g_hhStayed == 2 && g_hhStaying == HH_LINGER);
    g_hhStaying = 0;

    assert(HookRemove(NULL, exe + HH_JOB, (void*)HavokHelperJob) == 0);
    g_hhState = 0; g_hhNext = NULL; g_hhSetup = NULL; g_hhPop = NULL; g_hhLoadedAt = 0;
    g_hhStayed = g_hhLate = g_hhJobs = 0;
    VirtualFree(image, 0, MEM_RELEASE);
    puts("PASS havok helpers: pins and their refusals, HELLFORGE_PERF_OFF, the hook over the job's prologue, ready tasks then gone, staying for a released task, the idle limit, the cap");
}
