/* The render guards (render_guards.c) run on a synthetic exe image: thunks load the registers each site reads and
 * jump to it, and small markers stand where the game continues, so every cave is executed. A freed slot (-1) takes
 * the null path with an empty view; a live one goes on unchanged. The job pool's real spin loop runs empty: a second
 * thread fills it, a short budget runs out. Included by stats_test.c. */

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
static DWORD WINAPI RgFillLater(void* parameter)
{
    Sleep(40);
    InterlockedExchange(g_rgTestHead, 0x5);
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
    RgPut(image, RG_POOL_FAIL, kRgPoolFailPin, sizeof kRgPoolFailPin);
    /* the allocator's own spin loop, as in the build: free-list head empty -> count r9d to 0x21 -> the give-up */
    static const uint8_t spin[] = {0x41,0x8b,0x7a,0x40, 0x8b,0xc7, 0x25,0xff,0xff,0x7f,0x00, 0x75,0x12, 0x41,0x8b,0xc1,
        0x41,0xff,0xc1, 0x83,0xf8,0x20, 0x76,0xe8};
    RgPut(image, RG_POOL_RETRY, spin, sizeof spin);
    RgPut(image, RG_POOL_EMPTY, kRgPoolPin, sizeof kRgPoolPin);
    assert(RgPinned(exe));
    image[kRgSites[2].rva + 3] ^= 1;
    assert(!RgPinned(exe)); /* another build's code is left alone */
    image[kRgSites[2].rva + 3] ^= 1;

    assert(RenderGuardsInstallAt(exe));
    for (size_t i = 0; i < RG_SITES; i++) assert(image[kRgSites[i].rva] == 0xe9);
    assert(image[RG_POOL_EMPTY] == 0xe9);

    /* the markers, after the install (they replace pinned bytes): a live slot comes back, the null path shows r8 */
    static const uint8_t slotEcx[] = {0x89,0xc8,0xc3}, slotR8d[] = {0x44,0x89,0xc0,0xc3}, nullView[] = {0x49,0x8d,0x40,0x77,0xc3};
    RgPut(image, kRgSites[0].rva + kRgSites[0].length, slotEcx, sizeof slotEcx);
    RgPut(image, kRgSites[1].rva + kRgSites[1].length, slotEcx, sizeof slotEcx);
    RgPut(image, kRgSites[2].rva + kRgSites[2].length, slotR8d, sizeof slotR8d);
    RgPut(image, kRgSites[3].rva + kRgSites[3].length, slotEcx, sizeof slotEcx);
    RgPut(image, RG_BIND_NULL, nullView, sizeof nullView);
    static const uint8_t r8Mgr[] = {0x49,0x89,0xc8, 0x89,0xd1};               /* mov r8, rcx; mov ecx, edx */
    static const uint8_t r9Mgr[] = {0x49,0x89,0xc9, 0x49,0x89,0xc8, 0x89,0xd1}; /* mov r9, rcx; mov r8, rcx; mov ecx, edx */
    static const uint8_t rcxMgr[] = {0x41,0x89,0xd1};                         /* mov r9d, edx */
    RgCall calls[RG_SITES] = {
        RgThunk(image, RG_THUNKS, r8Mgr, sizeof r8Mgr, kRgSites[0].rva),
        RgThunk(image, RG_THUNKS + 0x40, r9Mgr, sizeof r9Mgr, kRgSites[1].rva),
        RgThunk(image, RG_THUNKS + 0x80, r9Mgr, sizeof r9Mgr, kRgSites[2].rva),
        RgThunk(image, RG_THUNKS + 0xC0, rcxMgr, sizeof rcxMgr, kRgSites[3].rva)};
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
        uint64_t freed = calls[i](mgr, 1);
        assert(kRgSites[i].resolver ? freed == 0 : freed == 0x77); /* resolver: no view; binding: r8 = 0 on the null path */
        assert(g_rgHits[i] == before + 1);
    }

    /* the pool: thunk = the allocator's frame (push rdi, which the loop writes, leaves rsp 16-aligned as the game's
     * does), r10 the pool, r9d 0. Found: r10 back; failed: 2 */
    static const uint8_t found[] = {0x4c,0x89,0xd0, 0x5f, 0xc3}, failed[] = {0xb8,0x02,0x00,0x00,0x00, 0x5f, 0xc3};
    RgPut(image, RG_POOL_EMPTY + 7, found, sizeof found);
    RgPut(image, RG_POOL_FAIL, failed, sizeof failed);
    static const uint8_t frame[] = {0x57, 0x49,0x89,0xca, 0x45,0x31,0xc9};
    RgCall take = RgThunk(image, RG_THUNKS + 0x100, frame, sizeof frame, RG_POOL_RETRY);
    FlushInstructionCache(GetCurrentProcess(), image, RG_IMAGE);
    _Alignas(64) uint8_t pool[0x180] = {0};
    volatile LONG* head = (volatile LONG*)(pool + 0x40);
    g_rgTestHead = head;

    *head = 0x3;
    LONG waits = g_rgPoolWaits;
    assert(take(pool, 0) == (uintptr_t)pool && g_rgPoolWaits == waits); /* a free slot: no wait */

    *head = 0;
    HANDLE filler = CreateThread(NULL, 0, RgFillLater, NULL, 0, NULL);
    assert(filler != NULL);
    assert(take(pool, 0) == (uintptr_t)pool); /* waited for the worker, r10 kept */
    WaitForSingleObject(filler, INFINITE);
    CloseHandle(filler);
    assert(g_rgPoolWaits == waits + 1 && g_rgPoolLongestMs >= 20);

    *head = 0;
    ULONGLONG budget = g_rgPoolWaitMs;
    LONG gaveUp = g_rgPoolGaveUp;
    g_rgPoolWaitMs = 50;
    SetLastError(1234);
    assert(take(pool, 0) == 2); /* the budget ran out: the game's own failure */
    assert(GetLastError() == 1234 && g_rgPoolGaveUp == gaveUp + 1 && g_rgPoolWaits == waits + 2);
    g_rgPoolWaitMs = budget;
    puts("PASS render guards: pins, a freed slot on the null path at all four sites and live ones unchanged, the job pool waited out with r10 kept, its budget running out to the game's failure with the last error kept");
}
