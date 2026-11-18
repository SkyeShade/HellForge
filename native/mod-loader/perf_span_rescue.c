/* span-rescue (docs/performance.md). The game's allocator (rpmalloc) strands memory: a block that another thread frees
 * waits on its span's deferred list (exe 0x66A360), and a span that was full rejoins its heap only once every block is
 * free (0x66A37E). One live block keeps the rest from reuse, so the heap maps new spans. When a load ends, each heap's
 * own thread moves such spans to its partial list, as its own free does with a full span (0x66A3C6). Included by
 * hellforge_modloader.c after perf_record_hash.c: PinMatches, LoaderHook, ReadState, PerfFixOn, Note, g_build. */
#include "perf_span_rescue_pins.h"

#define SR_MAP_AT 0x27B8C60u      /* rpmalloc's span map and unmap pointers; only init stores them (0x66AABF) */
#define SR_UNMAP_AT 0x27B8C68u
#define SR_CONFIG_REST 0x27B8C70u /* two more config pointers; one is a map-fail callback called inside rpmalloc */
#define SR_MAP_DEFAULT 0x667850u
#define SR_UNMAP_DEFAULT 0x667920u
#define SR_INITIALISED 0x2360FC4u
#define SR_MAP_COUNT 0x27B0DC0u   /* spans a map makes: 1, so every 64 KiB region holds one span */
#define SR_GRANULARITY 0x27B0DE0u
#define SR_SIZES 0x27B0DF0u       /* {u32 block size, u16 count, u16 class}[96] */
#define SR_TRIM 0x6302F0u
#define SR_RENDER_STEP 0x113010u
#define SR_TASK_STEP 0x99DB30u
#define SR_ENGINE 0x1A10208u      /* [engine + 0x318]: the job pool, workers [+0x90], count [+0x88], trim flag [w + 0x7D] */
#define SR_SPAN 0x10000u
#define SR_CLASSES 96u
#define SR_LOCKED UINT64_MAX      /* a deferred list while a free pushes onto it */
#define SR_TOP (1u << 15)         /* a leaf for each 4 GiB of the user address space */
#define SR_LEAF_WORDS 1024u       /* a bit for each 64 KiB in it */
#define SR_HEAPS 64
#define SR_PER_HEAP 4096u
#define SR_TAKE_MS 5000ull        /* how long each heap's thread has to take its list */
/* span: free list +0, blocks +8, class +0xC, free list limit +0x10, used +0x14, deferred list +0x18, deferred count
 * +0x20, block size +0x24, heap +0x40, next +0x48, prev +0x50. heap: owner TEB +0, partial list of class c
 * +0x10 + 0x10 c, full span count +0x718, finalising +0x73C. A thread's own heap: [[gs:0x58]] + 0x4350. */

enum { SR_IDLE, SR_SCANNING, SR_PUBLISHED };
enum { SR_SLOT_FREE, SR_SLOT_READY, SR_SLOT_BUSY, SR_SLOT_DONE, SR_SLOT_GONE };
enum { SR_VIA_TRIM, SR_VIA_TICK, SR_VIA_RENDER, SR_VIA_TASK, SR_VIAS };
typedef void* (*SrMapFn)(size_t size, size_t* offset);
typedef void (*SrUnmapFn)(void* address, size_t size, size_t offset, size_t release);
typedef uint64_t (*SrTrimFn)(uint64_t global);
typedef uint64_t (*SrRenderFn)(uint64_t renderer, uint64_t message, uint64_t payload);
typedef uint64_t (*SrTaskFn)(uint64_t router);
typedef BOOLEAN (WINAPI* SrShutdownFn)(void);

typedef struct SrSlot
{
    volatile uintptr_t heap;
    volatile LONG state;
    uint32_t count, rescued, changed, via;
    LONG64 found, bytes; /* free block bytes in the spans found, and in those made reusable */
    uint64_t* spans;
} SrSlot;

static SRWLOCK g_srLock = SRWLOCK_INIT; /* shared while span headers are read; exclusive while a span's bit is cleared */
static void (*volatile g_srPause)(void);      /* a fixture widens the time between a span's check and its lock */
static uintptr_t (*volatile g_srHeapHere)(void); /* a fixture's own heaps; the game's come from TLS */
static struct
{
    int on;                                   /* 0 waiting, 1 on, -1 refused */
    uintptr_t exe, game;
    SrMapFn map; SrUnmapFn unmap; SrTrimFn trim; SrRenderFn render; SrTaskFn task; SrShutdownFn shutdown;
    volatile uint64_t* volatile* top;
    volatile LONG64 registered, waitMax;
    uint32_t size[SR_CLASSES], count[SR_CLASSES];
    SrSlot slot[SR_HEAPS];
    HANDLE wake;
    int sync;                                 /* the scan runs on the caller (a fixture) */
    volatile LONG scanned;
    volatile int phase;
    uint32_t state, passState, passes, seen, over;
    double scanMs;
    ULONGLONG deadline, loadedAt;
    struct { uint32_t found, rescued, changed, untaken, heaps, threads, via[SR_VIAS]; LONG64 foundBytes, bytes; } last;
} g_sr;

/* ---- The registry: every 64 KiB region rpmalloc maps, while it is mapped -------------------------------------------- */
static volatile uint64_t* SrLeaf(uintptr_t span, int make)
{
    uintptr_t t = span >> 32;
    if (g_sr.top == NULL || t >= SR_TOP) return NULL;
    volatile uint64_t* leaf = g_sr.top[t];
    if (leaf != NULL || !make) return leaf;
    volatile uint64_t* fresh = (volatile uint64_t*)VirtualAlloc(NULL, SR_LEAF_WORDS * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (fresh == NULL) return NULL;
    leaf = (volatile uint64_t*)InterlockedCompareExchangePointer((void* volatile*)&g_sr.top[t], (void*)fresh, NULL);
    if (leaf == NULL) return fresh;
    VirtualFree((void*)fresh, 0, MEM_RELEASE);
    return leaf;
}
static int SrMarked(uintptr_t span)
{
    volatile uint64_t* leaf = SrLeaf(span, 0);
    uint32_t i = (uint32_t)(span >> 16) & 0xFFFFu;
    return leaf != NULL && (leaf[i >> 6] >> (i & 63)) & 1;
}
/* Small and medium blocks live only in regions of one span (one span a map); the bit goes in after the map returns, so
 * a reader under the lock finds the region mapped. */
static void* SrMapped(size_t size, size_t* offset)
{
    void* p = g_sr.map(size, offset);
    uintptr_t at = (uintptr_t)p;
    volatile uint64_t* leaf = (p != NULL && size == SR_SPAN && (at & (SR_SPAN - 1)) == 0) ? SrLeaf(at, 1) : NULL;
    if (leaf != NULL)
    {
        uint32_t i = (uint32_t)(at >> 16) & 0xFFFFu;
        LONG64 bit = (LONG64)1 << (i & 63);
        if (!(InterlockedOr64((volatile LONG64*)&leaf[i >> 6], bit) & bit)) InterlockedIncrement64(&g_sr.registered);
    }
    return p;
}
/* The bit goes before the region does, under the lock, so no reader is inside a header being unmapped. Once the process
 * detaches its other threads are gone, and a lock one of them held stays held: then no reader is left to wait for. */
static void SrUnmapped(void* address, size_t size, size_t offset, size_t release)
{
    uintptr_t span = (uintptr_t)address & ~(uintptr_t)(SR_SPAN - 1);
    if (SrMarked(span))
    {
        volatile uint64_t* leaf = SrLeaf(span, 0);
        uint32_t i = (uint32_t)(span >> 16) & 0xFFFFu;
        int alone = g_sr.shutdown() != 0;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (!alone) AcquireSRWLockExclusive(&g_srLock);
        QueryPerformanceCounter(&b);
        InterlockedAnd64((volatile LONG64*)&leaf[i >> 6], ~((LONG64)1 << (i & 63)));
        if (!alone) ReleaseSRWLockExclusive(&g_srLock);
        InterlockedDecrement64(&g_sr.registered);
        for (LONG64 m = g_sr.waitMax; b.QuadPart - a.QuadPart > m; m = g_sr.waitMax)
            if (InterlockedCompareExchange64(&g_sr.waitMax, b.QuadPart - a.QuadPart, m) == m) break;
    }
    g_sr.unmap(address, size, offset, release);
}

/* ---- A stranded span ------------------------------------------------------------------------------------------- */
/* Its heap when the span is full (no free list, every block handed out) and holds some deferred blocks but not all of
 * them (all: it is on its heap's +0x710 list, or about to be), else 0. A span in a cache always fails. The heap is read
 * last: a span set up for another heap gets its heap before its lists (0x669949, 0x667E87), and x86 keeps loads in
 * order, so a stale heap here comes with the cached span's old fields. */
static uintptr_t SrStranded(uintptr_t span, uint32_t* deferredCount)
{
    uint32_t n = *(volatile uint32_t*)(span + 0x20);
    uint64_t freeList = *(volatile uint64_t*)span;
    uint32_t limit = *(volatile uint32_t*)(span + 0x10), count = *(volatile uint32_t*)(span + 8), cls = *(volatile uint32_t*)(span + 0xC);
    uint32_t size = *(volatile uint32_t*)(span + 0x24);
    uint64_t deferred = *(volatile uint64_t*)(span + 0x18);
    uintptr_t heap = (uintptr_t)*(volatile uint64_t*)(span + 0x40);
    if (cls >= SR_CLASSES || count == 0 || count != g_sr.count[cls] || size != g_sr.size[cls] || freeList != 0 || limit < count ||
        n == 0 || n >= count || deferred == 0 || deferred == SR_LOCKED || heap == 0) return 0;
    if (deferredCount != NULL) *deferredCount = n;
    return heap;
}

/* On the heap's owner thread, outside any allocator call: only that thread changes the heap's lists and a stranded
 * span's header. Other threads' frees take the deferred list's -1 lock (0x66A367); under it the count is final, and
 * a count at the block count means the span went to +0x710. Then the allocation's extract (0x6698C0) and the owner's
 * free of a full span (0x66A3D7). 1 done, 0 not stranded, -1 changed. */
static int SrRescue(uintptr_t heap, uintptr_t span, uint32_t* blocks)
{
    if (SrStranded(span, NULL) != heap) return 0;
    if (g_srPause != NULL) g_srPause();
    volatile LONG64* deferred = (volatile LONG64*)(span + 0x18);
    LONG64 list = (LONG64)SR_LOCKED;
    for (int i = 0; i < 1024 && (list = InterlockedExchange64(deferred, (LONG64)SR_LOCKED)) == (LONG64)SR_LOCKED; i++) YieldProcessor();
    if (list == (LONG64)SR_LOCKED) return -1; /* another thread's free still holds it: nothing was written */
    uint32_t n = *(volatile uint32_t*)(span + 0x20), count = *(volatile uint32_t*)(span + 8);
    if (list == 0 || n == 0 || n >= count) { InterlockedExchange64(deferred, list); return -1; }
    *(volatile uint64_t*)span = (uint64_t)list;
    *(volatile uint32_t*)(span + 0x14) -= n;
    *(volatile uint32_t*)(span + 0x20) = 0;
    InterlockedExchange64(deferred, 0);
    uint32_t cls = *(volatile uint32_t*)(span + 0xC);
    volatile uint64_t* head = (volatile uint64_t*)(heap + 0x10 + 0x10 * (uintptr_t)cls);
    uint64_t old = *head;
    if (old != 0) *(volatile uint64_t*)((uintptr_t)old + 0x50) = span;
    *(volatile uint64_t*)(span + 0x48) = old;
    *head = span;
    *(volatile uint64_t*)(heap + 0x718) -= 1;
    *blocks = n;
    return 1;
}

/* The calling thread's heap, read as the trim reads it (0x6302F7..0x630317). */
static uintptr_t SrHeapHere(void)
{
    if (g_srHeapHere != NULL) return g_srHeapHere();
    uintptr_t tls = 0;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(tls));
    uintptr_t block = tls != 0 ? *(volatile uintptr_t*)tls : 0;
    return block != 0 ? *(volatile uintptr_t*)(block + 0x4350) : 0;
}

/* A heap's list, on its thread: chunks of 64 spans under the shared lock, so a span whose bit is set stays mapped. A
 * heap leaves its owner only at that thread's own exit (0x66966C), never while it runs this. */
static void SrTakeList(SrSlot* s, uintptr_t heap)
{
    int mine = *(volatile uint64_t*)heap == (uint64_t)(uintptr_t)NtCurrentTeb() && *(volatile uint32_t*)(heap + 0x73C) == 0;
    if (!mine) { s->changed += s->count; return; }
    for (uint32_t i = 0; i < s->count; i += 64)
    {
        AcquireSRWLockShared(&g_srLock);
        for (uint32_t j = i; j < s->count && j < i + 64; j++)
        {
            uintptr_t span = (uintptr_t)s->spans[j];
            uint32_t n = 0;
            if (!SrMarked(span) || SrRescue(heap, span, &n) != 1) { s->changed++; continue; }
            s->rescued++;
            s->bytes += (LONG64)n * *(volatile uint32_t*)(span + 0x24);
        }
        ReleaseSRWLockShared(&g_srLock);
    }
}
static void SrRescueHere(int via)
{
    if (g_sr.phase != SR_PUBLISHED) return;
    uintptr_t heap = SrHeapHere();
    for (int k = 0; heap != 0 && k < SR_HEAPS; k++)
    {
        SrSlot* s = &g_sr.slot[k];
        if (s->heap != heap) continue;
        if (InterlockedCompareExchange(&s->state, SR_SLOT_BUSY, SR_SLOT_READY) != SR_SLOT_READY) return;
        if (s->heap != heap) { InterlockedExchange(&s->state, SR_SLOT_READY); return; } /* a later pass's slot */
        s->via = (uint32_t)via;
        SrTakeList(s, heap);
        InterlockedExchange(&s->state, SR_SLOT_DONE);
        return;
    }
}

/* Every trim runs on its own thread outside any allocator call (its ten callers, none in rpmalloc); a pool worker's
 * comes after its jobs while its +0x7D is set (0x6253BF). The renderer and Havok's task threads never trim on their
 * loops, so they take their lists between messages and between tasks. Arguments and results pass through. */
static uint64_t SrTrimmed(uint64_t global) { SrRescueHere(SR_VIA_TRIM); return g_sr.trim(global); }
static uint64_t SrRendered(uint64_t renderer, uint64_t message, uint64_t payload)
{
    SrRescueHere(SR_VIA_RENDER);
    return g_sr.render(renderer, message, payload);
}
static uint64_t SrTasked(uint64_t router) { SrRescueHere(SR_VIA_TASK); return g_sr.task(router); }

/* ---- The scan: the registry read on a thread of its own, the stranded spans listed by heap ------------------------ */
static SrSlot* SrSlotFor(uintptr_t heap)
{
    for (int k = 0; k < SR_HEAPS; k++) if (g_sr.slot[k].heap == heap) return &g_sr.slot[k];
    for (int k = 0; k < SR_HEAPS; k++)
        if (g_sr.slot[k].heap == 0) { g_sr.slot[k].heap = heap; return &g_sr.slot[k]; }
    return NULL;
}
static void SrScanWord(uint32_t t, uint32_t w, uint64_t bits)
{
    AcquireSRWLockShared(&g_srLock);
    for (bits &= g_sr.top[t][w]; bits != 0; bits &= bits - 1)
    {
        uintptr_t span = ((uintptr_t)t << 32) | ((uintptr_t)(w * 64 + (uint32_t)__builtin_ctzll(bits)) << 16);
        uint32_t n = 0;
        uintptr_t heap = SrStranded(span, &n);
        g_sr.seen++;
        if (heap == 0) continue;
        SrSlot* s = SrSlotFor(heap);
        if (s == NULL || s->count >= SR_PER_HEAP) { g_sr.over++; continue; }
        s->spans[s->count++] = span;
        s->found += (LONG64)n * *(volatile uint32_t*)(span + 0x24);
    }
    ReleaseSRWLockShared(&g_srLock);
}
static void SrScan(void)
{
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    g_sr.seen = g_sr.over = 0;
    for (uint32_t t = 0; t < SR_TOP; t++)
    {
        volatile uint64_t* leaf = g_sr.top[t];
        for (uint32_t w = 0; leaf != NULL && w < SR_LEAF_WORDS; w++)
            if (leaf[w] != 0) SrScanWord(t, w, leaf[w]);
    }
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    g_sr.scanMs = f.QuadPart > 0 ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
    for (int k = 0; k < SR_HEAPS; k++)
        if (g_sr.slot[k].heap != 0) InterlockedExchange(&g_sr.slot[k].state, SR_SLOT_READY);
    InterlockedExchange(&g_sr.scanned, 1);
}
static DWORD WINAPI SrScanThread(void* wake)
{
    while (WaitForSingleObject((HANDLE)wake, INFINITE) == WAIT_OBJECT_0) SrScan();
    return 0;
}

/* Each pool worker trims, and so takes its list, after its jobs: the store the game's trim-all makes (0x324637). */
static void SrWakeWorkers(void)
{
    uint64_t engine = 0, pool = 0, workers = 0;
    uint32_t n = 0;
    if (SafeRead(g_sr.exe + SR_ENGINE, &engine, 8) || engine == 0 || SafeRead((uintptr_t)engine + 0x318, &pool, 8) || pool == 0 ||
        SafeRead((uintptr_t)pool + 0x88, &n, 4) || SafeRead((uintptr_t)pool + 0x90, &workers, 8) || workers == 0 || n > 64) return;
    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t w = 0;
        uint8_t flag = 0;
        if (!SafeRead((uintptr_t)workers + 8ull * i, &w, 8) && w != 0 && !SafeRead((uintptr_t)w + 0x7D, &flag, 1))
            *(volatile uint8_t*)((uintptr_t)w + 0x7D) = 1;
    }
}

/* ---- The pass, on the game thread ------------------------------------------------------------------------------- */
/* 1 once every list is taken, or its time is up and none is being worked on. */
static int SrClose(ULONGLONG now)
{
    int busy = 0, ready = 0;
    for (int k = 0; k < SR_HEAPS; k++)
    {
        LONG s = g_sr.slot[k].state;
        busy += s == SR_SLOT_BUSY; ready += s == SR_SLOT_READY;
    }
    if (busy || (ready && now < g_sr.deadline)) return 0;
    for (int k = 0; k < SR_HEAPS; k++)
        if (g_sr.slot[k].heap != 0 && InterlockedCompareExchange(&g_sr.slot[k].state, SR_SLOT_GONE, SR_SLOT_READY) == SR_SLOT_BUSY) return 0;
    memset(&g_sr.last, 0, sizeof g_sr.last);
    for (int k = 0; k < SR_HEAPS; k++)
    {
        SrSlot* s = &g_sr.slot[k];
        if (s->heap == 0) continue;
        g_sr.last.heaps++; g_sr.last.found += s->count; g_sr.last.foundBytes += s->found; g_sr.last.bytes += s->bytes;
        if (s->state == SR_SLOT_GONE) { g_sr.last.untaken += s->count; continue; }
        g_sr.last.threads++; g_sr.last.rescued += s->rescued; g_sr.last.changed += s->changed;
        g_sr.last.via[s->via < SR_VIAS ? s->via : 0]++;
    }
    return 1;
}
static void SrPassNote(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double mb = 1.0 / 1048576.0, ms = f.QuadPart > 0 ? 1000.0 / (double)f.QuadPart : 0.0;
    g_sr.passes++;
    Note("performance fix span-rescue: pass %u, %s reached: %lld spans registered, %u read in %.1f ms; %u stranded in %u heaps "
        "(%.1f MB free in them); %u rescued (%.1f MB) on %u threads, %u changed first, %u untaken, %u over the room; lists taken "
        "at trims %u, the game thread %u, render messages %u, Havok task steps %u; an unmap waited at most %.3f ms",
        g_sr.passes, g_sr.passState == HF_STATE_SHIP ? "the ship" : "a mission", (long long)g_sr.registered, g_sr.seen, g_sr.scanMs,
        g_sr.last.found, g_sr.last.heaps, (double)g_sr.last.foundBytes * mb, g_sr.last.rescued, (double)g_sr.last.bytes * mb,
        g_sr.last.threads, g_sr.last.changed, g_sr.last.untaken, g_sr.over, g_sr.last.via[SR_VIA_TRIM], g_sr.last.via[SR_VIA_TICK],
        g_sr.last.via[SR_VIA_RENDER], g_sr.last.via[SR_VIA_TASK], (double)InterlockedExchange64(&g_sr.waitMax, 0) * ms);
}
static void SrReset(void)
{
    for (int k = 0; k < SR_HEAPS; k++)
    {
        SrSlot* s = &g_sr.slot[k];
        InterlockedExchange(&s->state, SR_SLOT_FREE);
        s->heap = 0; s->count = s->rescued = s->changed = s->via = 0; s->found = s->bytes = 0;
    }
}

/* One game-thread update. A load ends where the state turns to the ship or a mission; the first state read is only the
 * baseline. The game thread takes its own list at the publish: the update is outside any allocator call. */
static void SrStep(uint32_t state, ULONGLONG now)
{
    uint32_t from = g_sr.state;
    if (state != HF_STATE_NONE) g_sr.state = state;
    if (g_sr.phase == SR_IDLE)
    {
        if (from == HF_STATE_NONE || state == from || (state != HF_STATE_SHIP && state != HF_STATE_MISSION)) return;
        g_sr.phase = SR_SCANNING;
        g_sr.passState = state;
        if (g_sr.sync) SrScan(); else SetEvent(g_sr.wake);
        return;
    }
    if (g_sr.phase == SR_SCANNING)
    {
        if (!InterlockedCompareExchange(&g_sr.scanned, 0, 1)) return;
        g_sr.phase = SR_PUBLISHED;
        g_sr.deadline = now + SR_TAKE_MS;
        SrRescueHere(SR_VIA_TICK);
        SrWakeWorkers();
        return;
    }
    if (!SrClose(now)) return;
    SrPassNote();
    SrReset();
    g_sr.phase = SR_IDLE;
}

/* From packages.c's TickDetour: the game thread, in the frame update. */
static void SpanRescueTick(void)
{
    if (g_sr.on == 1) SrStep(ReadState(g_sr.game), GetTickCount64());
}

/* ---- Install ------------------------------------------------------------------------------------------------------ */
/* -1 when every pin of the group matches, else the first that does not. PinMatches reads up to 512 bytes at a time. */
static int SrPinMatches(uintptr_t at, const uint8_t* bytes, uint32_t size)
{
    for (uint32_t done = 0; done < size; done += 512)
        if (!PinMatches(at + done, bytes + done, size - done < 512 ? size - done : 512)) return 0;
    return 1;
}
static int SrPinned(uintptr_t exe, int group)
{
    for (int i = 0; i < (int)(sizeof kSrPins / sizeof kSrPins[0]); i++)
        if (kSrPins[i].group == group && !SrPinMatches(exe + kSrPins[i].rva, kSrPins[i].bytes, kSrPins[i].size)) return i;
    return -1;
}

/* NULL when rpmalloc is set up as reviewed: its own span map and unmap, no other config, one span a map. The size
 * classes are copied for the scan. */
static const char* SrReady(uintptr_t exe)
{
    uint32_t initialised = 0;
    uint64_t map = 0, unmap = 0, rest[2] = {1, 1}, mapCount = 0, granularity = 0;
    uint8_t sizes[SR_CLASSES * 8];
    if (SafeRead(exe + SR_INITIALISED, &initialised, 4) || initialised != 1) return "rpmalloc is not set up";
    if (SafeRead(exe + SR_MAP_AT, &map, 8) || SafeRead(exe + SR_UNMAP_AT, &unmap, 8) || map != exe + SR_MAP_DEFAULT ||
        unmap != exe + SR_UNMAP_DEFAULT) return "rpmalloc's span map or unmap is not its own";
    if (SafeRead(exe + SR_CONFIG_REST, rest, sizeof rest) || rest[0] != 0 || rest[1] != 0) return "rpmalloc has a map-fail callback or another config set";
    if (SafeRead(exe + SR_MAP_COUNT, &mapCount, 8) || SafeRead(exe + SR_GRANULARITY, &granularity, 8) || mapCount != 1 || granularity != SR_SPAN)
        return "rpmalloc maps more than one span at a time";
    if (SafeRead(exe + SR_SIZES, sizes, sizeof sizes)) return "rpmalloc's size classes cannot be read";
    for (uint32_t c = 0; c < SR_CLASSES; c++)
    {
        memcpy(&g_sr.size[c], sizes + 8 * c, 4);
        g_sr.count[c] = (uint32_t)sizes[8 * c + 4] | (uint32_t)sizes[8 * c + 5] << 8;
    }
    return g_sr.size[34] == 1040 && g_sr.count[34] == 62 ? NULL : "rpmalloc's size classes are not the reviewed ones";
}

/* One more thread kind's reach: a refused one leaves that kind's lists untaken, and no hook. */
static int SrReach(uintptr_t exe, int group, const char* who, uint32_t rva, void* detour, void** original)
{
    char what[80];
    snprintf(what, sizeof what, "performance fix span-rescue (%s)", who);
    if (SrPinned(exe, group) >= 0 || LoaderHook(what, exe + rva, detour, original) || *original == NULL)
    {
        *original = NULL;
        Note("performance fix span-rescue: %s does not take its lists; its code is not the reviewed one or the hook was refused", who);
        return 0;
    }
    return 1;
}

/* The memory, the scan thread, then the unmap swap before the map's: no span is registered before every unmap clears
 * its bit. Each swap is a compare-exchange from the default; the trim hook goes last, so a refusal leaves no hook. */
static const char* SrWrap(uintptr_t exe)
{
    void* volatile* mapAt = (void* volatile*)(exe + SR_MAP_AT);
    void* volatile* unmapAt = (void* volatile*)(exe + SR_UNMAP_AT);
    g_sr.top = (volatile uint64_t* volatile*)VirtualAlloc(NULL, SR_TOP * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t* spans = (uint64_t*)VirtualAlloc(NULL, (size_t)SR_HEAPS * SR_PER_HEAP * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_sr.top == NULL || spans == NULL) return "no memory for the registry";
    for (int k = 0; k < SR_HEAPS; k++) g_sr.slot[k].spans = spans + (size_t)k * SR_PER_HEAP;
    g_sr.wake = g_sr.sync ? NULL : CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE thread = g_sr.wake != NULL ? CreateThread(NULL, 64 * 1024, SrScanThread, g_sr.wake, 0, NULL) : NULL;
    if (!g_sr.sync && thread == NULL) return "no thread for the scan";
    if (thread != NULL) CloseHandle(thread);
    g_sr.map = (SrMapFn)(exe + SR_MAP_DEFAULT);
    g_sr.unmap = (SrUnmapFn)(exe + SR_UNMAP_DEFAULT);
    if (InterlockedCompareExchangePointer(unmapAt, (void*)SrUnmapped, (void*)g_sr.unmap) != (void*)g_sr.unmap) return "rpmalloc's span unmap changed";
    if (InterlockedCompareExchangePointer(mapAt, (void*)SrMapped, (void*)g_sr.map) != (void*)g_sr.map)
    {
        InterlockedCompareExchangePointer(unmapAt, (void*)g_sr.unmap, (void*)SrUnmapped);
        return "rpmalloc's span map changed";
    }
    if (LoaderHook("performance fix span-rescue (rpmalloc's trim)", exe + SR_TRIM, (void*)SrTrimmed, (void**)&g_sr.trim) || g_sr.trim == NULL)
    {
        InterlockedCompareExchangePointer(mapAt, (void*)g_sr.map, (void*)SrMapped);
        InterlockedCompareExchangePointer(unmapAt, (void*)g_sr.unmap, (void*)SrUnmapped);
        return "the trim could not be hooked";
    }
    return NULL;
}

/* 1 on, 0 the code is not the reviewed one (yet), -1 refused. A refusal after the swaps leaves bits that nothing reads:
 * no pass runs while `on` is not 1. */
static int SpanRescueInstallAt(uintptr_t exe, uintptr_t game)
{
    if (g_sr.on != 0) return g_sr.on;
    if (SrPinned(exe, 0) >= 0) return 0;
    g_sr.on = -1;
    if (!PerfFixOn("span-rescue")) return -1;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    g_sr.shutdown = ntdll != NULL ? (SrShutdownFn)(void*)GetProcAddress(ntdll, "RtlDllShutdownInProgress") : NULL;
    const char* why = g_sr.shutdown == NULL ? "no RtlDllShutdownInProgress" : SrReady(exe);
    if (why == NULL) { g_sr.exe = exe; why = SrWrap(exe); }
    if (why != NULL) { Note("performance fix span-rescue: not installed; %s", why); return -1; }
    int render = SrReach(exe, 1, "the renderer", SR_RENDER_STEP, (void*)SrRendered, (void**)&g_sr.render);
    int task = SrReach(exe, 2, "threads that run Havok tasks", SR_TASK_STEP, (void*)SrTasked, (void**)&g_sr.task);
    g_sr.game = game;
    g_sr.on = 1;
    Note("performance fix span-rescue: on; when a load ends, full rpmalloc spans that hold blocks freed by other threads go back "
        "to their heap's partial list, on that heap's thread; lists are taken at the game thread's update and at trims%s%s",
        render ? ", the renderer's messages" : "", task ? ", Havok's task steps" : "");
    return 1;
}

/* From the watch thread, once the game has loaded and its update is hooked; a minute after the load, code that is not
 * the reviewed one never will be. */
static void SpanRescueInstall(void)
{
    if (g_sr.on != 0 || !g_fired[HF_EVENT_GAME_LOADED] || g_tickState == 0) return;
    const char* why = strcmp(g_build, "25480438") ? "not game build 25480438" : g_tickState < 0 ? "the game thread's update is not hooked" : NULL;
    if (why == NULL)
    {
        if (SpanRescueInstallAt((uintptr_t)GetModuleHandleW(NULL), ApiModuleBase("game.dll", NULL)) != 0) return;
        if (g_sr.loadedAt == 0) g_sr.loadedAt = GetTickCount64();
        if (GetTickCount64() - g_sr.loadedAt < 60000) return;
        why = "rpmalloc's code is not the reviewed one";
    }
    g_sr.on = -1;
    Note("performance fix span-rescue: not installed; %s", why);
}
