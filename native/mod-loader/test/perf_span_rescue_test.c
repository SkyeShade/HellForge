/* span-rescue (perf_span_rescue.c) on a synthetic exe image. The pins and refusals first; then the game's own
 * allocation (0x669AB0, 0x6697E0) and free (0x66A2F0), placed from the pins, run on synthetic heaps with this build's
 * layout: other threads' frees strand a span and a pass gives it back on the heap's own thread. Live, orphaned and
 * adopted heaps; an owner that frees before its turn, another thread that is not the owner; a last free, a held
 * deferred lock and racing frees during the rescue; the unmap's lock and the process's detach. Included by
 * stats_test.c. */

#define SRT_IMAGE 0x27BA000u   /* past rpmalloc's config at 0x27B8C60 */
#define SRT_CLASS 34u
#define SRT_BLOCK 1040u
#define SRT_COUNT 62u

typedef void* (*SrtAllocFn)(uintptr_t heap, size_t size);
typedef void (*SrtFreeFn)(uintptr_t span, void* block);
typedef uint64_t (*SrtCall1)(uint64_t a);

static uint8_t* g_srtExe;
static DWORD g_srtTls;
static volatile LONG g_srtNewSpans, g_srtReleased, g_srtUnmaps, g_srtUnmapSawBit, g_srtTrims, g_srtTasks, g_srtRenders;
static volatile uintptr_t g_srtReleasedSpan;

static uintptr_t SrtHeapHere(void) { return (uintptr_t)TlsGetValue(g_srtTls); }
static uint64_t SrtQ(uintptr_t at) { return *(volatile uint64_t*)at; }
static uint32_t SrtD(uintptr_t at) { return *(volatile uint32_t*)at; }
static uintptr_t SrtSpanOf(void* block) { return (uintptr_t)block & ~(uintptr_t)0xFFFF; }
static uintptr_t SrtPartial(uintptr_t heap) { return (uintptr_t)SrtQ(heap + 0x10 + 0x10 * SRT_CLASS); }

/* What the game's code calls outside the bytes under test: a new span (0x668D30) mapped through the config pointer,
 * as rpmalloc's own map sites do; a span released to a cache (0x668860) or unmapped (0x667C80). */
static void* SrtMap(size_t size, size_t* offset) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static void SrtUnmap(void* address, size_t size, size_t offset, size_t release)
{
    InterlockedIncrement(&g_srtUnmaps);
    InterlockedExchange(&g_srtUnmapSawBit, SrMarked((uintptr_t)address));
    if (release) VirtualFree(address, 0, MEM_RELEASE);
}
static uintptr_t SrtNewSpan(uintptr_t heap, uintptr_t heapClass, uint32_t spanCount, uint32_t cls)
{
    size_t offset = 0;
    uint8_t* span = ((SrMapFn) * (void**)(g_srtExe + SR_MAP_AT))(SR_SPAN, &offset);
    InterlockedIncrement(&g_srtNewSpans);
    if (span != NULL) *(uint32_t*)(span + 0x2C) = 1;
    return (uintptr_t)span;
}
static void SrtRelease(uintptr_t heap, uintptr_t span) { InterlockedIncrement(&g_srtReleased); g_srtReleasedSpan = span; }
static void SrtFinalised(void) { assert(!"a span unmapped by a finalising heap"); }
static uint64_t SrtTrim(uint64_t global) { InterlockedIncrement(&g_srtTrims); return global + 0x70; }
static uint64_t SrtTask(uint64_t router) { InterlockedIncrement(&g_srtTasks); return router + 0x60; }
static uint64_t SrtRender(uint64_t a, uint64_t b, uint64_t c) { InterlockedIncrement(&g_srtRenders); return a + b + c; }
static BOOLEAN WINAPI SrtAlone(void) { return 1; }

static void SrtThunk(uint32_t rva, void* to)
{
    uint8_t code[12] = {0x48, 0xb8};
    uint64_t at = (uint64_t)(uintptr_t)to;
    memcpy(code + 2, &at, 8);
    code[10] = 0xff; code[11] = 0xe0; /* mov rax, to; jmp rax */
    memcpy(g_srtExe + rva, code, sizeof code);
}

static uintptr_t SrtHeap(void)
{
    uint8_t* heap = (uint8_t*)VirtualAlloc(NULL, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(heap != NULL);
    *(uint64_t*)heap = (uint64_t)(uintptr_t)NtCurrentTeb();
    return (uintptr_t)heap;
}
static void* SrtAlloc(uintptr_t heap) { return ((SrtAllocFn)(void*)(g_srtExe + 0x669AB0u))(heap, SRT_BLOCK); }
static void SrtFree(void* block) { ((SrtFreeFn)(void*)(g_srtExe + 0x66A2F0u))(SrtSpanOf(block), block); }
static uintptr_t SrtFill(uintptr_t heap, void** blocks)
{
    for (uint32_t i = 0; i < SRT_COUNT; i++) blocks[i] = SrtAlloc(heap);
    return SrtSpanOf(blocks[0]);
}
static int SrtLen(uint64_t list) { int n = 0; for (; list != 0 && n < 1000; n++) list = SrtQ((uintptr_t)list); return n; }

typedef struct { void** blocks; int first, n, spin; } SrtFrees;
static DWORD WINAPI SrtFreeThread(void* p)
{
    SrtFrees* j = (SrtFrees*)p;
    for (int i = j->first; i < j->first + j->n; i++)
    {
        SrtFree(j->blocks[i]);
        for (volatile int k = 0; k < j->spin; k++) {}
    }
    return 0;
}
static HANDLE SrtFreeOnOther(SrtFrees* job) { HANDLE t = CreateThread(NULL, 0, SrtFreeThread, job, 0, NULL); assert(t != NULL); return t; }
static void SrtJoin(HANDLE t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void SrtFreeElsewhere(void** blocks, int first, int n) { SrtFrees job = {blocks, first, n, 0}; SrtJoin(SrtFreeOnOther(&job)); }

/* A pass: the baseline state, the load's end (the scan, on this thread), the publish (this thread's own list). */
static void SrtPass(ULONGLONG now)
{
    assert(g_sr.phase == SR_IDLE);
    SrStep(HF_STATE_PREPARE_SHIP, now);
    SrStep(HF_STATE_SHIP, now);
    assert(g_sr.phase == SR_SCANNING);
    SrStep(HF_STATE_SHIP, now + 1);
    assert(g_sr.phase == SR_PUBLISHED);
}
static void SrtClose(ULONGLONG now) { SrStep(HF_STATE_SHIP, now); assert(g_sr.phase == SR_IDLE); }
static void SrtDrop(uintptr_t span) { ((SrUnmapFn) * (void**)(g_srtExe + SR_UNMAP_AT))((void*)span, SR_SPAN, 0, SR_SPAN); }

/* A span set by hand: the stranded shape unless told otherwise. */
static uintptr_t SrtHand(uintptr_t heap, uint64_t freeList, uint32_t n, uint64_t deferred, uint32_t size, uint32_t cls)
{
    size_t offset = 0;
    uintptr_t span = (uintptr_t)((SrMapFn) * (void**)(g_srtExe + SR_MAP_AT))(SR_SPAN, &offset);
    *(uint64_t*)span = freeList; *(uint32_t*)(span + 8) = SRT_COUNT; *(uint32_t*)(span + 0xC) = cls;
    *(uint32_t*)(span + 0x10) = SRT_COUNT; *(uint32_t*)(span + 0x14) = SRT_COUNT; *(uint64_t*)(span + 0x18) = deferred;
    *(uint32_t*)(span + 0x20) = n; *(uint32_t*)(span + 0x24) = size; *(uint64_t*)(span + 0x40) = heap;
    return span;
}

static void SrtImage(void)
{
    g_srtExe = (uint8_t*)VirtualAlloc(NULL, SRT_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(g_srtExe != NULL);
    for (size_t i = 0; i < sizeof kSrPins / sizeof kSrPins[0]; i++) memcpy(g_srtExe + kSrPins[i].rva, kSrPins[i].bytes, kSrPins[i].size);
    uint32_t sizes[] = {SRT_BLOCK, SRT_COUNT | SRT_CLASS << 16, 1552, 42 | 35u << 16};
    memcpy(g_srtExe + SR_SIZES + 8 * SRT_CLASS, sizes, sizeof sizes);
    *(uint64_t*)(g_srtExe + 0x27B10F0u) = 0x1000; /* the page size the allocation's free-list setup reads */
    *(uint32_t*)(g_srtExe + SR_INITIALISED) = 1;
    *(uint64_t*)(g_srtExe + SR_MAP_AT) = (uint64_t)(uintptr_t)(g_srtExe + SR_MAP_DEFAULT);
    *(uint64_t*)(g_srtExe + SR_UNMAP_AT) = (uint64_t)(uintptr_t)(g_srtExe + SR_UNMAP_DEFAULT);
    *(uint64_t*)(g_srtExe + SR_MAP_COUNT) = 1;
    *(uint64_t*)(g_srtExe + SR_GRANULARITY) = SR_SPAN;
    SrtThunk(0x668D30u, (void*)SrtNewSpan);
    SrtThunk(0x668860u, (void*)SrtRelease);
    SrtThunk(0x667C80u, (void*)SrtFinalised);
}

/* Every pinned byte refuses its group; rpmalloc set up otherwise refuses the install; off by name. */
static void SrtRefusals(uintptr_t exe)
{
    assert(SrPinned(exe, 0) < 0 && SrPinned(exe, 1) < 0 && SrPinned(exe, 2) < 0);
    for (int i = 0; i < (int)(sizeof kSrPins / sizeof kSrPins[0]); i++)
        for (uint32_t b = 0; b < kSrPins[i].size; b++)
        {
            g_srtExe[kSrPins[i].rva + b] ^= 0x20;
            assert(SrPinned(exe, kSrPins[i].group) == i);
            g_srtExe[kSrPins[i].rva + b] ^= 0x20;
        }
    assert(SrReady(exe) == NULL);
    struct { uint32_t at; uint64_t value; } bad[] = {{SR_INITIALISED, 0}, {SR_MAP_AT, 0x1234}, {SR_UNMAP_AT, 0x1234}, {SR_CONFIG_REST + 8, 0x1234},
        {SR_MAP_COUNT, 2}, {SR_GRANULARITY, 0x1000}, {SR_SIZES + 8 * SRT_CLASS, 1024}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        uint64_t keep = *(uint64_t*)(g_srtExe + bad[i].at);
        memcpy(g_srtExe + bad[i].at, &bad[i].value, bad[i].at == SR_INITIALISED ? 4 : 8);
        assert(SrReady(exe) != NULL);
        *(uint64_t*)(g_srtExe + bad[i].at) = keep;
    }
    assert(SrReady(exe) == NULL);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "record-hash, Span-Rescue");
    assert(SpanRescueInstallAt(exe, 0x1000) == -1 && g_sr.trim == NULL);
    assert(SrtQ(exe + SR_MAP_AT) == exe + SR_MAP_DEFAULT && SrtQ(exe + SR_UNMAP_AT) == exe + SR_UNMAP_DEFAULT);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    g_sr.on = 0;
    g_srtExe[kSrPins[0].rva] ^= 1;
    assert(SpanRescueInstallAt(exe, 0x1000) == 0 && g_sr.on == 0); /* not the reviewed code (yet): wait */
    g_srtExe[kSrPins[0].rva] ^= 1;
    g_srtExe[0x113010u] ^= 1;
}

/* The game's code alone: 61 frees from another thread leave a full span on no list, and the next block is a new span. */
static void SrtVanilla(uintptr_t heap)
{
    void* b[SRT_COUNT];
    LONG spans = g_srtNewSpans, released = g_srtReleased;
    uintptr_t span = SrtFill(heap, b);
    assert(g_srtNewSpans == spans + 1 && SrtSpanOf(b[SRT_COUNT - 1]) == span && SrMarked(span));
    assert(SrtQ(span) == 0 && SrtD(span + 0x14) == SRT_COUNT && SrtQ(heap + 0x718) == 1 && SrtPartial(heap) == 0);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    assert(SrtD(span + 0x20) == SRT_COUNT - 1 && SrtQ(span + 0x18) != 0 && SrtQ(span) == 0);
    void* next = SrtAlloc(heap);
    assert(g_srtNewSpans == spans + 2 && SrtSpanOf(next) != span);
    SrtFree(b[0]);
    assert(g_srtReleased == released + 1 && g_srtReleasedSpan == span); /* only the last live block lets the 61 go */
    SrtFree(next);
    assert(SrtQ(heap + 0x718) == 0);
}

/* A live heap's list taken on its own thread: the 61 blocks are handed out again with no new span, and the span goes
 * back with its last block. */
static void SrtRescue(uintptr_t heap, ULONGLONG* now)
{
    void* b[SRT_COUNT], *again[SRT_COUNT];
    uintptr_t span = SrtFill(heap, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    SrtPass(*now);
    assert(g_sr.slot[0].state == SR_SLOT_DONE && g_sr.slot[0].rescued == 1 && g_sr.slot[0].via == SR_VIA_TICK);
    assert(SrtLen(SrtQ(span)) == SRT_COUNT - 1 && SrtD(span + 0x14) == 1 && SrtD(span + 0x20) == 0 && SrtQ(span + 0x18) == 0);
    assert(SrtPartial(heap) == span && SrtQ(span + 0x48) == 0 && SrtQ(heap + 0x718) == 0);
    SrtClose(*now + 2);
    assert(g_sr.last.found == 1 && g_sr.last.rescued == 1 && g_sr.last.bytes == (LONG64)(SRT_COUNT - 1) * SRT_BLOCK && g_sr.last.untaken == 0);
    LONG spans = g_srtNewSpans, released = g_srtReleased;
    for (uint32_t i = 0; i < SRT_COUNT - 1; i++)
    {
        again[i] = SrtAlloc(heap);
        assert(SrtSpanOf(again[i]) == span);
    }
    assert(g_srtNewSpans == spans && SrtQ(heap + 0x718) == 1 && SrtPartial(heap) == 0); /* full again, off the list */
    again[SRT_COUNT - 1] = b[0];
    for (uint32_t i = 0; i < SRT_COUNT; i++)
    {
        SrtFree(again[i]);
        assert(g_srtReleased == released + (i + 1 == SRT_COUNT));
    }
    assert(g_srtReleasedSpan == span && SrtQ(heap + 0x718) == 0);
    SrtDrop(span);
    assert(g_srtUnmapSawBit == 0 && !SrMarked(span)); /* the bit went before the region */
    *now += 10;
}

/* Only a full span holding some deferred blocks, of a heap that a thread owns, is listed and changed. */
static void SrtKinds(uintptr_t heap, ULONGLONG* now)
{
    uint8_t* blocks = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t list = (uint64_t)(uintptr_t)blocks;
    uintptr_t other = SrtHeap(), stranded = SrtHand(heap, 0, 3, list, SRT_BLOCK, SRT_CLASS);
    uintptr_t kinds[] = {SrtHand(heap, list, 3, list, SRT_BLOCK, SRT_CLASS), SrtHand(heap, 0, SRT_COUNT, list, SRT_BLOCK, SRT_CLASS),
        SrtHand(heap, 0, 3, SR_LOCKED, SRT_BLOCK, SRT_CLASS), SrtHand(heap, 0, 0, 0, SRT_BLOCK, SRT_CLASS), SrtHand(heap, 0, 3, list, 1552, SRT_CLASS),
        SrtHand(heap, 0, 3, list, SRT_BLOCK, 0x60), SrtHand(0, 0, 3, list, SRT_BLOCK, SRT_CLASS)};
    *(uint32_t*)(kinds[3] + 0x10) = 20; /* lazy: blocks not yet handed out */
    *(uint32_t*)(kinds[3] + 0x20) = 3; *(uint64_t*)(kinds[3] + 0x18) = list;
    uint8_t* loose = (uint8_t*)VirtualAlloc(NULL, SR_SPAN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); /* not mapped by rpmalloc */
    memcpy(loose, (void*)stranded, 0x80);
    uintptr_t theirs = SrtHand(other, 0, 3, list, SRT_BLOCK, SRT_CLASS);
    *(uint64_t*)other = 0x1234; /* another thread's heap: it never comes */
    SrtPass(*now);
    SrSlot* mine = g_sr.slot[0].heap == heap ? &g_sr.slot[0] : &g_sr.slot[1];
    SrSlot* theirSlot = mine == &g_sr.slot[0] ? &g_sr.slot[1] : &g_sr.slot[0];
    assert(mine->heap == heap && mine->count == 1 && mine->spans[0] == stranded && theirSlot->heap == other && theirSlot->count == 1);
    assert(g_sr.slot[2].heap == 0);
    assert(SrtPartial(heap) == stranded && SrtD(stranded + 0x20) == 0 && SrtQ(stranded) == list);
    SrStep(HF_STATE_SHIP, *now + 2);
    assert(g_sr.phase == SR_PUBLISHED); /* the other heap's list waits for its thread */
    SrtClose(*now + 2 + SR_TAKE_MS);
    assert(g_sr.last.found == 2 && g_sr.last.rescued == 1 && g_sr.last.untaken == 1 && g_sr.last.threads == 1);
    assert(SrtD(theirs + 0x20) == 3 && SrtQ(theirs + 0x18) == list && SrtQ(theirs) == 0 && SrtPartial(other) == 0);
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) { assert(SrtQ(kinds[i] + 0x48) == 0); SrtDrop(kinds[i]); }
    *(uint64_t*)(heap + 0x10 + 0x10 * SRT_CLASS) = 0;
    *(uint64_t*)(heap + 0x718) = 0;
    SrtDrop(stranded); SrtDrop(theirs);
    VirtualFree(loose, 0, MEM_RELEASE); VirtualFree(blocks, 0, MEM_RELEASE); VirtualFree((void*)other, 0, MEM_RELEASE);
    *now += 10 + SR_TAKE_MS;
}

/* A thread's heap and its trim: the worker's own view (TLS) and the hooked trim at the image. */
typedef struct { uintptr_t heap; int own, viaTask; uint64_t result; } SrtWorker;
static DWORD WINAPI SrtWorkerThread(void* p)
{
    SrtWorker* w = (SrtWorker*)p;
    TlsSetValue(g_srtTls, (void*)w->heap);
    if (w->own) *(uint64_t*)w->heap = (uint64_t)(uintptr_t)NtCurrentTeb(); /* a thread's heap: TLS, then the owner (0x66B178) */
    w->result = ((SrtCall1)(void*)(g_srtExe + (w->viaTask ? SR_TASK_STEP : SR_TRIM)))(w->viaTask ? 0x500 : 1);
    return 0;
}
static void SrtOnWorker(SrtWorker* w) { HANDLE t = CreateThread(NULL, 0, SrtWorkerThread, w, 0, NULL); assert(t != NULL); SrtJoin(t); }

/* An orphaned heap (owner -1 at its thread's exit) has no thread to take its list, so nothing touches it. A thread that
 * adopts it is its owner and takes the list at its trim; a thread that is not the owner never does. */
static void SrtOrphan(ULONGLONG* now)
{
    void* b[SRT_COUNT];
    uintptr_t orphan = SrtHeap(), span = SrtFill(orphan, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    *(uint64_t*)orphan = UINT64_MAX;
    LONG trims = g_srtTrims;
    SrtPass(*now);
    SrtWorker stranger = {orphan, 0, 0, 0};
    SrtOnWorker(&stranger); /* its TLS names the heap, but the heap is not its own */
    assert(stranger.result == 0x71 && g_sr.slot[0].state == SR_SLOT_DONE && g_sr.slot[0].changed == 1 && g_sr.slot[0].rescued == 0);
    assert(SrtD(span + 0x20) == SRT_COUNT - 1 && SrtQ(span) == 0 && SrtPartial(orphan) == 0 && SrtQ(orphan + 0x718) == 1);
    SrtClose(*now + 2);
    SrtPass(*now + 10);
    SrtClose(*now + 12 + SR_TAKE_MS);
    assert(g_sr.last.untaken == 1 && SrtD(span + 0x20) == SRT_COUNT - 1 && SrtPartial(orphan) == 0);
    SrtPass(*now + 20 + SR_TAKE_MS);
    SrtWorker adopter = {orphan, 1, 0, 0};
    SrtOnWorker(&adopter);
    assert(adopter.result == 0x71 && g_srtTrims == trims + 2 && g_sr.slot[0].via == SR_VIA_TRIM && g_sr.slot[0].rescued == 1);
    assert(SrtPartial(orphan) == span && SrtD(span + 0x20) == 0 && SrtLen(SrtQ(span)) == SRT_COUNT - 1 && SrtQ(orphan + 0x718) == 0);
    SrtClose(*now + 22 + SR_TAKE_MS);
    *now += 30 + SR_TAKE_MS;
}

/* The owner frees into its own span after the scan: the span is on its partial list by the game's free, so the take
 * finds it changed and writes nothing. */
static void SrtOwnerFirst(uintptr_t heap, ULONGLONG* now)
{
    void* b[SRT_COUNT];
    uintptr_t span = SrtFill(heap, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    TlsSetValue(g_srtTls, NULL); /* the game thread's publish takes nothing */
    SrtPass(*now);
    TlsSetValue(g_srtTls, (void*)heap);
    SrtFree(b[0]); /* the owner's own free of a full span (0x66A3C6) */
    assert(g_srtReleased > 0 && g_srtReleasedSpan == span); /* every block free: the game released it */
    SrRescueHere(SR_VIA_TRIM);
    assert(g_sr.slot[0].changed == 1 && g_sr.slot[0].rescued == 0 && SrtPartial(heap) == 0 && SrtQ(heap + 0x718) == 0);
    SrtClose(*now + 2);
    SrtDrop(span);
    *now += 10;
}

/* During the rescue, between its check and its lock: another thread's free of the last live block sends the span to
 * +0x710 (0x66A37E), or a free holds the deferred lock past the spin. The rescue writes nothing either way. */
static void* g_srtLastBlock;
static uintptr_t g_srtHeldSpan;
static void SrtLastFree(void) { g_srPause = NULL; SrtFreeElsewhere(&g_srtLastBlock, 0, 1); }
static void SrtHoldLock(void) { g_srPause = NULL; *(volatile uint64_t*)(g_srtHeldSpan + 0x18) = SR_LOCKED; }
static void SrtDuring(uintptr_t heap, ULONGLONG* now)
{
    void* b[SRT_COUNT];
    uintptr_t span = SrtFill(heap, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    g_srtLastBlock = b[0];
    g_srPause = SrtLastFree;
    SrtPass(*now);
    assert(g_sr.slot[0].changed == 1 && SrtD(span + 0x20) == SRT_COUNT && SrtQ(heap + 0x710) == span && SrtQ(span) == 0);
    assert(SrtPartial(heap) == 0 && SrtQ(heap + 0x718) == 1 && SrtQ(span + 0x18) != SR_LOCKED);
    SrtClose(*now + 2);
    *(uint64_t*)(heap + 0x710) = 0;
    *(uint64_t*)(heap + 0x718) = 0;
    SrtDrop(span);

    span = SrtFill(heap, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    uint64_t list = SrtQ(span + 0x18);
    g_srtHeldSpan = span;
    g_srPause = SrtHoldLock;
    SrtPass(*now + 10);
    assert(g_sr.slot[0].changed == 1 && SrtQ(span + 0x18) == SR_LOCKED && SrtQ(span) == 0 && SrtD(span + 0x20) == SRT_COUNT - 1);
    assert(SrtPartial(heap) == 0 && SrtQ(heap + 0x718) == 1);
    *(volatile uint64_t*)(span + 0x18) = list; /* the free lets go */
    SrtClose(*now + 12);
    SrtFree(b[0]);
    assert(g_srtReleasedSpan == span && SrtPartial(heap) == 0 && SrtQ(heap + 0x718) == 0);
    SrtDrop(span);
    *now += 20;
}

/* Other threads' frees race the rescue: every block ends on exactly one list, the counts agree, and a span rescued is
 * at the head of the partial list. */
static void SrtRace(ULONGLONG* now)
{
    for (int round = 0; round < 200; round++)
    {
        void* b[SRT_COUNT];
        uintptr_t heap = SrtHeap();
        TlsSetValue(g_srtTls, (void*)heap);
        uintptr_t span = SrtFill(heap, b);
        SrtFreeElsewhere(b, 1, 30);
        SrtFrees job = {b, 31, SRT_COUNT - 31, round % 7 * 50};
        HANDLE t = SrtFreeOnOther(&job);
        SrtPass(*now);
        SrtJoin(t);
        SrtClose(*now + 2);
        int rescued = SrtPartial(heap) == span, own = SrtLen(SrtQ(span)), deferred = SrtLen(SrtQ(span + 0x18));
        assert(own + deferred == SRT_COUNT - 1 && (int)SrtD(span + 0x20) == deferred);
        assert(SrtD(span + 0x14) == SRT_COUNT - (uint32_t)own && SrtQ(heap + 0x718) == (uint64_t)!rescued && (rescued || own == 0));
        for (uint64_t p = SrtQ(span); p != 0; p = SrtQ((uintptr_t)p))
            for (uint64_t q = SrtQ(span + 0x18); q != 0; q = SrtQ((uintptr_t)q)) assert(p != q);
        SrtDrop(span);
        VirtualFree((void*)heap, 0, MEM_RELEASE);
        *now += 10;
    }
}

/* The hooks pass arguments and results through; pool workers are flagged after the publish; the scan thread. */
static void SrtReach(uintptr_t heap, ULONGLONG* now)
{
    uint8_t* engine = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t workers[2][0x80] = {{0}};
    uint64_t list[2] = {(uint64_t)(uintptr_t)workers[0], (uint64_t)(uintptr_t)workers[1]};
    *(uint64_t*)(g_srtExe + SR_ENGINE) = (uint64_t)(uintptr_t)engine;
    *(uint64_t*)(engine + 0x318) = (uint64_t)(uintptr_t)(engine + 0x400);
    *(uint32_t*)(engine + 0x400 + 0x88) = 2;
    *(uint64_t*)(engine + 0x400 + 0x90) = (uint64_t)(uintptr_t)list;
    void* b[SRT_COUNT];
    uintptr_t worker = SrtHeap(), span = SrtFill(worker, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    SrtPass(*now);
    assert(workers[0][0x7D] == 1 && workers[1][0x7D] == 1 && workers[0][0x7C] == 0);
    SrtWorker havok = {worker, 1, 1, 0};
    SrtOnWorker(&havok);
    assert(havok.result == 0x560 && g_srtTasks == 1 && g_sr.slot[0].via == SR_VIA_TASK && SrtPartial(worker) == span);
    assert(SrRendered(1, 2, 3) == 6 && g_srtRenders == 1 && SrTrimmed(5) == 0x75); /* nothing published for this thread */
    SrtClose(*now + 2);

    /* the scan on its own thread */
    g_sr.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 64 * 1024, SrScanThread, g_sr.wake, 0, NULL);
    assert(g_sr.wake != NULL && thread != NULL);
    g_sr.sync = 0;
    span = SrtFill(heap, b);
    SrtFreeElsewhere(b, 1, SRT_COUNT - 1);
    SrStep(HF_STATE_PREPARE_MISSION, *now + 10);
    SrStep(HF_STATE_MISSION, *now + 10);
    for (int i = 0; i < 5000 && g_sr.phase != SR_PUBLISHED; i++) { Sleep(1); SrStep(HF_STATE_MISSION, *now + 11); }
    assert(g_sr.phase == SR_PUBLISHED && SrtPartial(heap) == span);
    SrtClose(*now + 12);
    SrtFree(b[0]); /* every block free again: the game releases it */
    assert(g_srtReleasedSpan == span && SrtPartial(heap) == 0);
    SrtDrop(span);
    g_sr.sync = 1;
    CloseHandle(thread);
    *now += 20;
}

/* An unmap of a registered span waits while a scan or a take reads headers, and clears the bit before the region goes;
 * once the process detaches, it does not wait for a lock that a terminated thread held. */
static volatile LONG g_srtHolding, g_srtLetGo;
static DWORD WINAPI SrtHolder(void* p)
{
    AcquireSRWLockShared(&g_srLock);
    InterlockedExchange(&g_srtHolding, 1);
    while (!g_srtLetGo) Sleep(1);
    ReleaseSRWLockShared(&g_srLock);
    return 0;
}
static DWORD WINAPI SrtUnmapper(void* p) { SrtDrop((uintptr_t)p); return 0; }
static void SrtLocks(uintptr_t heap)
{
    void* b[SRT_COUNT];
    uintptr_t span = SrtFill(heap, b);
    for (uint32_t i = 0; i < SRT_COUNT; i++) SrtFree(b[i]);
    g_srtHolding = g_srtLetGo = 0;
    HANDLE holder = CreateThread(NULL, 0, SrtHolder, NULL, 0, NULL);
    while (!g_srtHolding) Sleep(1);
    LONG unmaps = g_srtUnmaps;
    HANDLE unmapper = CreateThread(NULL, 0, SrtUnmapper, (void*)span, 0, NULL);
    assert(WaitForSingleObject(unmapper, 100) == WAIT_TIMEOUT && SrMarked(span) && g_srtUnmaps == unmaps);
    InterlockedExchange(&g_srtLetGo, 1);
    SrtJoin(unmapper);
    SrtJoin(holder);
    assert(g_srtUnmaps == unmaps + 1 && g_srtUnmapSawBit == 0 && !SrMarked(span));

    span = SrtFill(heap, b);
    for (uint32_t i = 0; i < SRT_COUNT; i++) SrtFree(b[i]);
    g_srtHolding = g_srtLetGo = 0;
    holder = CreateThread(NULL, 0, SrtHolder, NULL, 0, NULL);
    while (!g_srtHolding) Sleep(1);
    SrShutdownFn shutdown = g_sr.shutdown;
    g_sr.shutdown = SrtAlone; /* the detach: the holder counts as terminated */
    SrtDrop(span);
    assert(!SrMarked(span) && g_srtUnmapSawBit == 0);
    g_sr.shutdown = shutdown;
    InterlockedExchange(&g_srtLetGo, 1);
    SrtJoin(holder);
}

static void SpanRescueFixture(void)
{
    SrtImage();
    uintptr_t exe = (uintptr_t)g_srtExe;
    g_srtTls = TlsAlloc();
    g_srHeapHere = SrtHeapHere;
    g_sr.sync = 1;
    SrtRefusals(exe);

    /* installed with the renderer's code changed: that reach alone stays out */
    assert(SpanRescueInstallAt(exe, 0x1000) == 1 && g_sr.on == 1 && g_sr.render == NULL && g_sr.trim != NULL && g_sr.task != NULL);
    assert(SrtQ(exe + SR_MAP_AT) == (uint64_t)(uintptr_t)SrMapped && SrtQ(exe + SR_UNMAP_AT) == (uint64_t)(uintptr_t)SrUnmapped);
    assert(SpanRescueInstallAt(exe, 0x1000) == 1); /* once */
    g_srtExe[0x113010u] ^= 1;
    SrtThunk(SR_MAP_DEFAULT, (void*)SrtMap); /* the wrappers' originals: the pinned prologues were only read */
    SrtThunk(SR_UNMAP_DEFAULT, (void*)SrtUnmap);
    g_sr.trim = SrtTrim; g_sr.task = SrtTask; g_sr.render = SrtRender;

    uintptr_t heap = SrtHeap();
    TlsSetValue(g_srtTls, (void*)heap);
    ULONGLONG now = 100000;
    SrtVanilla(heap);
    SrtRescue(heap, &now);
    SrtKinds(heap, &now);
    SrtOrphan(&now);
    SrtOwnerFirst(heap, &now);
    SrtDuring(heap, &now);
    SrtRace(&now);
    TlsSetValue(g_srtTls, (void*)heap);
    SrtReach(heap, &now);
    SrtLocks(heap);
    puts("PASS span rescue: pins refuse any changed byte, rpmalloc set up otherwise and off by name refuse, a reach whose code changed stays out; "
        "the game's own code strands a span and a pass gives it back on its heap's thread, reused before any new span; only stranded spans of a "
        "thread's own heap; orphaned heaps untouched until adopted; an owner's free first, a last free and a held lock during the rescue, racing "
        "frees; hooks pass through, workers flagged, the scan thread; the unmap waits for readers but not at the detach");
}
