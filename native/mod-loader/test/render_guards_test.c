/* The render guards (render_guards.c) run on a synthetic exe image: thunks load the registers each site reads and
 * jump to it, and small markers stand where the game continues, so every cave is executed. A freed slot (-1) takes
 * the site's null path; a live one goes on unchanged. The job pool's real spin loop runs empty: a second thread fills
 * it; with no give-backs it stops early; with give-backs but no free slot the budget runs out. Included by
 * stats_test.c. */

#define RG_IMAGE 0x740000u
#define RG_THUNKS 0x700000u /* free room in the image for the thunks */

typedef uint64_t (*RgCall)(void* mgr, uint32_t index);

static uint8_t* RgPut(uint8_t* image, uint32_t rva, const uint8_t* bytes, size_t size)
{
    memcpy(image + rva, bytes, size);
    return image + rva + size;
}

/* A thunk at rva: its prefix (register moves), then a jump to the site. */
static RgCall RgThunk(uint8_t* image, uint32_t rva, const uint8_t* prefix, size_t size, uint32_t site)
{
    uint8_t* c = RgPut(image, rva, prefix, size);
    RgJump(c, 0xe9, (uintptr_t)image + site);
    return (RgCall)(void*)(image + rva);
}

static volatile LONG* g_rgTestHead;
static volatile LONG* g_rgTestFreed;
static volatile LONG g_rgTestStop;
static LONG g_rgFillAfter; /* the pool's wait count before the test's take: the filler starts its 40 ms once it rises */
static DWORD WINAPI RgFillLater(void* parameter)
{
    /* the wait's clock starts after the guard's spins, which a slow machine (CI) can spend most of 40 ms on: the
     * filler sleeps from the moment the guard waits, so the wait it records is never under 20 ms */
    while (g_rgPoolWaits == g_rgFillAfter) Sleep(1);
    Sleep(40);
    InterlockedIncrement(g_rgTestFreed);
    InterlockedExchange(g_rgTestHead, 0x5);
    return 0;
}

/* Workers finishing jobs that other threads take at once: give-backs, never a free slot. */
static DWORD WINAPI RgFinishing(void* parameter)
{
    while (!g_rgTestStop) { InterlockedIncrement(g_rgTestFreed); Sleep(5); }
    return 0;
}

static void RenderGuardsFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RG_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    for (size_t i = 0; i < RG_SITES; i++) RgPut(image, kRgSites[i].rva, kRgSites[i].pin, kRgSites[i].length + 8);
    RgPut(image, RG_BIND_NULL, kRgBindNullPin, sizeof kRgBindNullPin);
    RgPut(image, RG_RESOLVE_NULL, kRgResolveNullPin, sizeof kRgResolveNullPin);
    RgPut(image, RG_COMPUTE_NULL, kRgComputeNullPin, sizeof kRgComputeNullPin);
    RgPut(image, RG_POOL_FAIL, kRgPoolFailPin, sizeof kRgPoolFailPin);
    RgPut(image, RG_POOL_FREED_AT, kRgPoolFreedPin, sizeof kRgPoolFreedPin);
    /* the allocator's own spin loop, as in the build: free-list head empty -> count r9d to 0x21 -> the give-up */
    static const uint8_t spin[] = {0x41,0x8b,0x7a,0x40, 0x8b,0xc7, 0x25,0xff,0xff,0x7f,0x00, 0x75,0x12, 0x41,0x8b,0xc1,
        0x41,0xff,0xc1, 0x83,0xf8,0x20, 0x76,0xe8};
    RgPut(image, RG_POOL_RETRY, spin, sizeof spin);
    RgPut(image, RG_POOL_EMPTY, kRgPoolPin, sizeof kRgPoolPin);
    assert(RgPinned(exe));
    image[kRgSites[2].rva + 3] ^= 1;
    assert(!RgPinned(exe)); /* another build's code is left alone */
    image[kRgSites[2].rva + 3] ^= 1;

    assert(RgInReach(exe + 0x10000000, exe) && RgInReach(exe - 0x10000000, exe));
    assert(!RgInReach(exe + 0x90000000ull, exe) && !RgInReach(exe - 0x7FFF0000ull, exe)); /* a jump would not reach */
    assert(RenderGuardsInstallAt(exe));
    for (size_t i = 0; i < RG_SITES; i++) assert(image[kRgSites[i].rva] == 0xe9);
    assert(image[RG_POOL_EMPTY] == 0xe9);

    /* the markers, after the install (they replace pinned bytes): a live slot comes back; the binding loop's null path
     * shows r8 (0x77 when it was zeroed); the compute binding's says 0x66 */
    static const uint8_t slotEcx[] = {0x89,0xc8,0xc3}, slotR8d[] = {0x44,0x89,0xc0,0xc3};
    static const uint8_t bindNull[] = {0x49,0x8d,0x40,0x77,0xc3}, computeNull[] = {0xb8,0x66,0x00,0x00,0x00,0xc3};
    for (size_t i = 0; i < RG_SITES; i++)
        if (kRgSites[i].slotIsR8) RgPut(image, kRgSites[i].rva + kRgSites[i].length, slotR8d, sizeof slotR8d);
        else RgPut(image, kRgSites[i].rva + kRgSites[i].length, slotEcx, sizeof slotEcx);
    RgPut(image, RG_BIND_NULL, bindNull, sizeof bindNull);
    RgPut(image, RG_COMPUTE_NULL, computeNull, sizeof computeNull);
    /* each site's registers from (rcx = the manager, edx = the index); r8 set too, so zeroing it shows */
    static const uint8_t r8Mgr[] = {0x49,0x89,0xc8, 0x89,0xd1};                    /* mov r8, rcx; mov ecx, edx */
    static const uint8_t r9Mgr[] = {0x49,0x89,0xc9, 0x49,0x89,0xc8, 0x89,0xd1};    /* mov r9, rcx; mov r8, rcx; mov ecx, edx */
    static const uint8_t rdxMgr[] = {0x48,0x89,0xc8, 0x49,0x89,0xc0, 0x89,0xd1, 0x48,0x89,0xc2}; /* rax = r8 = rcx; ecx = edx; rdx = rax */
    static const uint8_t rcxMgr[] = {0x41,0x89,0xd1};                              /* mov r9d, edx */
    static const uint8_t r8MgrRdx[] = {0x49,0x89,0xc8, 0x89,0xd2};                 /* mov r8, rcx; mov edx, edx */
    static const uint8_t tableEcx[] = {0x49,0x89,0xc8, 0x48,0x8b,0x81,0x28,0x02,0x00,0x00, 0x89,0xd1}; /* r8; rax = table; ecx */
    static const uint8_t tableRdx[] = {0x49,0x89,0xc8, 0x48,0x8b,0x81,0x28,0x02,0x00,0x00, 0x89,0xd2}; /* r8; rax = table; edx */
    const struct { const uint8_t* bytes; size_t size; } prefix[RG_SITES] = {
        {r8Mgr, sizeof r8Mgr}, {r9Mgr, sizeof r9Mgr}, {rdxMgr, sizeof rdxMgr}, {r9Mgr, sizeof r9Mgr}, {rcxMgr, sizeof rcxMgr},
        {r8MgrRdx, sizeof r8MgrRdx}, {tableEcx, sizeof tableEcx}, {tableRdx, sizeof tableRdx}};
    RgCall calls[RG_SITES];
    for (size_t i = 0; i < RG_SITES; i++) calls[i] = RgThunk(image, RG_THUNKS + 0x40 * (uint32_t)i, prefix[i].bytes, prefix[i].size, kRgSites[i].rva);
    FlushInstructionCache(GetCurrentProcess(), image, RG_IMAGE);

    static uint32_t slots[3] = {0x11, 0xFFFFFFFFu, 0x22};
    _Alignas(16) uint8_t mgr[0x300] = {0};
    uint32_t* table = slots;
    memcpy(mgr + 0x228, &table, 8);
    for (size_t i = 0; i < RG_SITES; i++)
    {
        LONG before = g_rgHits[i];
        assert(calls[i](mgr, 0) == 0x11 && calls[i](mgr, 2) == 0x22); /* live slots: unchanged */
        assert(g_rgHits[i] == before);
        uint64_t freed = calls[i](mgr, 1), expect = kRgSites[i].null == 0 ? 0 : kRgSites[i].null == RG_BIND_NULL ? 0x77 : 0x66;
        assert(freed == expect);
        assert(g_rgHits[i] == before + 1);
    }

    /* the pool: thunk = the allocator's frame (push rdi, which the loop writes, leaves rsp 16-aligned as the game's
     * does), r10 the pool, r9d 0. Found: r10 back; failed: 2 */
    static const uint8_t found[] = {0x4c,0x89,0xd0, 0x5f, 0xc3}, failed[] = {0xb8,0x02,0x00,0x00,0x00, 0x5f, 0xc3};
    RgPut(image, RG_POOL_EMPTY + 7, found, sizeof found);
    RgPut(image, RG_POOL_FAIL, failed, sizeof failed);
    static const uint8_t frame[] = {0x57, 0x49,0x89,0xca, 0x45,0x31,0xc9};
    RgCall take = RgThunk(image, RG_THUNKS + 0x400, frame, sizeof frame, RG_POOL_RETRY);
    FlushInstructionCache(GetCurrentProcess(), image, RG_IMAGE);
    _Alignas(64) uint8_t pool[0x180] = {0};
    volatile LONG* head = (volatile LONG*)(pool + 0x40);
    g_rgTestHead = head;
    g_rgTestFreed = (volatile LONG*)(pool + RG_POOL_FREED);

    *head = 0x3;
    LONG waits = g_rgPoolWaits;
    assert(take(pool, 0) == (uintptr_t)pool && g_rgPoolWaits == waits); /* a free slot: no wait */

    *head = 0;
    g_rgFillAfter = waits;
    HANDLE filler = CreateThread(NULL, 0, RgFillLater, NULL, 0, NULL);
    assert(filler != NULL);
    assert(take(pool, 0) == (uintptr_t)pool); /* waited for the worker, r10 kept */
    WaitForSingleObject(filler, INFINITE);
    CloseHandle(filler);
    assert(g_rgPoolWaits == waits + 1 && g_rgPoolLongestMs >= 20);

    /* no give-back at all (the workers stopped): the stall ends it long before the budget */
    *head = 0;
    ULONGLONG budget = g_rgPoolWaitMs, stall = g_rgPoolStallMs;
    LONG stalled = g_rgPoolStalled, gaveUp = g_rgPoolGaveUp;
    g_rgPoolStallMs = 60;
    SetLastError(1234);
    ULONGLONG from = GetTickCount64();
    assert(take(pool, 0) == 2); /* the game's own failure */
    assert(GetTickCount64() - from < 2000 && GetLastError() == 1234);
    assert(g_rgPoolStalled == stalled + 1 && g_rgPoolGaveUp == gaveUp && g_rgPoolWaits == waits + 2);

    /* give-backs keep coming but the pool stays full: the budget ends it */
    g_rgPoolWaitMs = 150;
    g_rgTestStop = 0;
    HANDLE finishing = CreateThread(NULL, 0, RgFinishing, NULL, 0, NULL);
    assert(finishing != NULL);
    from = GetTickCount64();
    assert(take(pool, 0) == 2);
    assert(GetTickCount64() - from >= 140);
    InterlockedExchange(&g_rgTestStop, 1);
    WaitForSingleObject(finishing, INFINITE);
    CloseHandle(finishing);
    assert(g_rgPoolGaveUp == gaveUp + 1 && g_rgPoolStalled == stalled + 1);
    g_rgPoolWaitMs = budget;
    g_rgPoolStallMs = stall;
    puts("PASS render guards: pins, a freed slot on its null path at all eight sites and live ones unchanged, the job pool waited out with r10 kept, ended early when no job finishes, by the budget when jobs finish but none is free, the last error kept");
}
