/* Two crashes of the game's own renderer, build 25480438, that a slow PC on Direct3D 11 meets in the Armory and at
 * mission start. Both are the game reading memory it never checked; HellForge sends each read to the path the game
 * already has for that case, so nothing new is drawn or skipped. Included by hellforge_modloader.c after
 * shield_block.c: AllocateNearGame (damage.c), PinMatches (packages.c), Note, g_build, g_fired. */

/* A resource is freed while a material still binds it: its slot in the resource slot table holds 0xFFFFFFFF, and the
 * material's binding reads entry -1. The guarded sites are every slot read of a handle taken from a material's data
 * that has a null-handle (0xFFFFFF) path: a cave repeats the moved instructions and sends -1 to that path. */
#define RG_BIND_NULL 0x7322EBu    /* the binding loop's null path: stores r8 as the view */
#define RG_RESOLVE_NULL 0x7306DFu /* the view lookup's null path: xor eax, eax; ret */
#define RG_COMPUTE_NULL 0x732E72u /* the compute binding's null path: stores nothing */

typedef struct RgSite
{
    uint32_t rva;
    uint8_t length;   /* the moved instructions, the slot load among them */
    uint8_t slotIsR8; /* the slot lands in r8d, else ecx */
    uint32_t null;    /* where a freed slot goes; 0: return 0 (the view lookup) */
    uint8_t zeroR8;   /* the null path stores r8, which is then 0 */
    const char* name;
    uint8_t pin[19];  /* the moved bytes and the next 8, which stay */
} RgSite;

static const RgSite kRgSites[] = {
    {0x732019u, 10, 0, RG_BIND_NULL, 1, "binding, textures", {0x49,0x8b,0x80,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x88, 0x49,0x8b,0x80,0xa0,0x02,0x00,0x00,0x48}},
    {0x732068u, 10, 0, RG_BIND_NULL, 1, "binding, other views", {0x49,0x8b,0x81,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x88, 0x4c,0x8d,0x04,0x49,0x4d,0x03,0xc0,0x83}},
    {0x7320C2u, 10, 0, RG_BIND_NULL, 1, "binding, buffers", {0x48,0x8b,0x82,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x88, 0x41,0x8b,0x45,0x08,0x48,0x69,0xd9,0xd0}},
    {0x732358u, 11, 1, RG_BIND_NULL, 1, "binding, buffers and UAVs", {0x49,0x8b,0x81,0x28,0x02,0x00,0x00, 0x44,0x8b,0x04,0x88, 0x8d,0x42,0xff,0x83,0xf8,0x03,0x76,0x33}},
    {0x730677u, 11, 0, 0, 0, "view lookup", {0x48,0x8b,0x81,0x28,0x02,0x00,0x00, 0x42,0x8b,0x0c,0x88, 0x8d,0x42,0xff,0x83,0xf8,0x03,0x76,0x30}},
    {0x732DAEu, 10, 0, RG_COMPUTE_NULL, 0, "compute binding, buffers", {0x49,0x8b,0x80,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x90, 0x48,0x8b,0x44,0x24,0x38,0x48,0x69,0xd1}},
    {0x732E1Cu, 9, 0, RG_COMPUTE_NULL, 0, "compute binding, views", {0x81,0xe1,0xff,0xff,0xff,0x00, 0x8b,0x0c,0x88, 0x48,0x8b,0x44,0x24,0x50,0x48,0x8d,0x14}},
    {0x732E43u, 8, 0, RG_COMPUTE_NULL, 0, "compute binding, structured buffers", {0x8b,0x0c,0x90, 0x48,0x8b,0x44,0x24,0x38, 0x48,0x69,0xd1,0xd0,0x00,0x00,0x00,0x41}},
};
#define RG_SITES (sizeof kRgSites / sizeof kRgSites[0])
static const uint8_t kRgBindNullPin[] = {0x41,0x8b,0x4d,0x08,0x41,0xff,0xc6};
static const uint8_t kRgResolveNullPin[] = {0x33,0xc0,0xc3};
static const uint8_t kRgComputeNullPin[] = {0x8b,0x44,0x24,0x78,0x4c,0x8b,0xb5,0xa0,0x08,0x00,0x00};

/* The job pool (8192 slots) can run empty for a moment on a slow CPU. Its allocator then gives up after 33 spins
 * without writing the handles, and its only caller reads them anyway, uninitialised. The cave waits while workers
 * give slots back ([pool+0x100] counts them); r9d, the spin count, keeps rising, so each pass is the same call. With
 * no slot given back for a while (the workers stopped, as at quit), or past the budget: the old failure. */
#define RG_POOL_EMPTY 0x623FC8u /* xor eax, eax; jmp fail: the allocator giving up */
#define RG_POOL_RETRY 0x623FB0u /* the loop's head: reads the free list again */
#define RG_POOL_FAIL 0x6240A9u
#define RG_POOL_SPINS 0x21u     /* r9d at the first give-up is one more */
#define RG_POOL_FREED 0x100u    /* lock xadd'ed by every give-back (0x62443E) */
static const uint8_t kRgPoolPin[] = {0x33,0xc0,0xe9,0xda,0x00,0x00,0x00, 0x41,0xb8,0x01,0x00,0x00,0x00,0x89,0x03};
static const uint8_t kRgPoolRetryPin[] = {0x41,0x8b,0x7a,0x40,0x8b,0xc7,0x25,0xff,0xff,0x7f,0x00};
static const uint8_t kRgPoolFailPin[] = {0x48,0x8b,0x5c,0x24,0x40};
static const uint8_t kRgPoolFreedPin[] = {0xf0,0x0f,0xc1,0xb1,0x00,0x01,0x00,0x00}; /* lock xadd [rcx+0x100], esi */
#define RG_POOL_FREED_AT 0x62443Eu

typedef struct RgWait
{
    ULONGLONG start, moved; /* the wait's start; the last give-back seen */
    LONG freed;
} RgWait;

static int g_rgState;      /* 0 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rgLoadedAt;
static volatile LONG g_rgHits[RG_SITES];
static volatile LONG g_rgPoolWaits, g_rgPoolGaveUp, g_rgPoolStalled, g_rgPoolLongestMs;
static LONG g_rgReported[RG_SITES], g_rgPoolReported, g_rgPoolGaveUpReported, g_rgPoolStalledReported;
static ULONGLONG g_rgReportedAt;
static DWORD g_rgTls = TLS_OUT_OF_INDEXES;
static ULONGLONG g_rgPoolWaitMs = 10000, g_rgPoolStallMs = 500; /* a fixture shortens them */

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

/* Called from the pool cave on the game's thread, r9d as it stands and the pool. 1: wait again; 0: give up as the
 * game did. */
static int RenderGuardPoolWait(uint32_t spins, const uint8_t* pool)
{
    DWORD error = GetLastError(); /* the game's code around it may read it */
    RgWait* w = (RgWait*)TlsGetValue(g_rgTls);
    if (w == NULL)
    {
        w = (RgWait*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w); /* one per thread that waits, kept */
        if (w == NULL) { SetLastError(error); return 0; }
        TlsSetValue(g_rgTls, w);
    }
    ULONGLONG now = GetTickCount64();
    LONG freed = *(volatile const LONG*)(pool + RG_POOL_FREED);
    if (spins == RG_POOL_SPINS + 1)
    {
        w->start = w->moved = now;
        w->freed = freed;
        InterlockedIncrement(&g_rgPoolWaits);
    }
    if (freed != w->freed) { w->freed = freed; w->moved = now; }
    ULONGLONG waited = now - w->start;
    LONG ms = waited > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)waited, longest;
    while ((longest = g_rgPoolLongestMs) < ms && InterlockedCompareExchange(&g_rgPoolLongestMs, ms, longest) != longest) {}
    int again = 0;
    /* logged here, not by the watch thread: the game crashes as soon as this returns 0 */
    if (now - w->moved >= g_rgPoolStallMs)
    {
        InterlockedIncrement(&g_rgPoolStalled);
        Note("render guards: a full job pool saw no job finish for %llu ms (the game quitting, or stuck); the game's own failure follows", g_rgPoolStallMs);
    }
    else if (waited >= g_rgPoolWaitMs)
    {
        InterlockedIncrement(&g_rgPoolGaveUp);
        Note("render guards: a full job pool was still full after %llu s while jobs finished; the game's own failure follows", g_rgPoolWaitMs / 1000);
    }
    else again = 1;
    if (again) Sleep(1);
    SetLastError(error);
    return again;
}

static uint8_t* RgJump(uint8_t* c, uint8_t op, uintptr_t to)
{
    int32_t rel = (int32_t)((intptr_t)to - (intptr_t)(c + 5));
    *c = op;
    memcpy(c + 1, &rel, 4);
    return c + 5;
}

static uint8_t* RgImm64(uint8_t* c, uint8_t rexOp0, uint8_t rexOp1, uint64_t value)
{
    *c++ = rexOp0;
    *c++ = rexOp1;
    memcpy(c, &value, 8);
    return c + 8;
}

/* One freed-slot cave; its end. rax is dead on every null path (reloaded or the return value). */
static uint8_t* RgSiteCave(uint8_t* c, uintptr_t exe, const RgSite* s, volatile LONG* hits)
{
    memcpy(c, s->pin, s->length);
    c += s->length;
    static const uint8_t cmpEcx[] = {0x83, 0xf9, 0xff}, cmpR8d[] = {0x41, 0x83, 0xf8, 0xff};
    if (s->slotIsR8) { memcpy(c, cmpR8d, sizeof cmpR8d); c += sizeof cmpR8d; }
    else { memcpy(c, cmpEcx, sizeof cmpEcx); c += sizeof cmpEcx; }
    *c++ = 0x74; *c++ = 5;                    /* je freed */
    c = RgJump(c, 0xe9, exe + s->rva + s->length);
    c = RgImm64(c, 0x48, 0xb8, (uint64_t)(uintptr_t)hits); /* freed: mov rax, &hits */
    *c++ = 0xf0; *c++ = 0xff; *c++ = 0x00;    /* lock inc dword [rax] */
    if (s->null == 0)
    {
        *c++ = 0x31; *c++ = 0xc0; *c++ = 0xc3; /* xor eax, eax; ret */
        return c;
    }
    if (s->zeroR8) { *c++ = 0x45; *c++ = 0x31; *c++ = 0xc0; } /* xor r8d, r8d: the view stored is null */
    return RgJump(c, 0xe9, exe + s->null);
}

/* The pool cave. At the give-up rsp is 16-aligned (three pushes and sub 0x20 after the call), so three pushes and
 * 0x28 align the call. r9, r10 (the pool) and r11 (the count) live on; rax, rcx, rdx and r8 are reloaded after. */
static uint8_t* RgPoolCave(uint8_t* c, uintptr_t exe)
{
    static const uint8_t save[] = {0x41,0x51, 0x41,0x52, 0x41,0x53, 0x48,0x83,0xec,0x28, 0x44,0x89,0xc9, 0x4c,0x89,0xd2};
    memcpy(c, save, sizeof save); c += sizeof save;           /* push r9, r10, r11; sub rsp, 0x28; ecx = r9d; rdx = r10 */
    c = RgImm64(c, 0x48, 0xb8, (uint64_t)(uintptr_t)RenderGuardPoolWait);
    static const uint8_t call[] = {0xff,0xd0, 0x48,0x83,0xc4,0x28, 0x41,0x5b, 0x41,0x5a, 0x41,0x59, 0x85,0xc0, 0x74,0x05};
    memcpy(c, call, sizeof call); c += sizeof call;           /* call rax; restore; test eax, eax; jz fail */
    c = RgJump(c, 0xe9, exe + RG_POOL_RETRY);
    *c++ = 0x31; *c++ = 0xc0;                                 /* fail: xor eax, eax, as the game */
    return RgJump(c, 0xe9, exe + RG_POOL_FAIL);
}

static int RgPinned(uintptr_t exe)
{
    for (size_t i = 0; i < RG_SITES; i++)
        if (!PinMatches(exe + kRgSites[i].rva, kRgSites[i].pin, kRgSites[i].length + 8)) return 0;
    return PinMatches(exe + RG_BIND_NULL, kRgBindNullPin, sizeof kRgBindNullPin) &&
        PinMatches(exe + RG_RESOLVE_NULL, kRgResolveNullPin, sizeof kRgResolveNullPin) &&
        PinMatches(exe + RG_COMPUTE_NULL, kRgComputeNullPin, sizeof kRgComputeNullPin) &&
        PinMatches(exe + RG_POOL_EMPTY, kRgPoolPin, sizeof kRgPoolPin) &&
        PinMatches(exe + RG_POOL_RETRY, kRgPoolRetryPin, sizeof kRgPoolRetryPin) &&
        PinMatches(exe + RG_POOL_FAIL, kRgPoolFailPin, sizeof kRgPoolFailPin) &&
        PinMatches(exe + RG_POOL_FREED_AT, kRgPoolFreedPin, sizeof kRgPoolFreedPin);
}

/* Every rel32 between a cave page and the patched code, both ways: the near search may land below the exe. */
static int RgInReach(uintptr_t cave, uintptr_t exe)
{
    int64_t up = (int64_t)(exe + RG_COMPUTE_NULL + 0x100 - cave), down = (int64_t)(cave + 0x1000 - (exe + RG_POOL_RETRY));
    return up < 0x7FFF0000 && up > -0x7FFF0000 && down < 0x7FFF0000 && down > -0x7FFF0000;
}

/* A page for the caves that every jump reaches, or NULL. */
static uint8_t* RgCavePage(uintptr_t exe)
{
    uint8_t* cave = AllocateNearGame(exe, 0x1000);
    return cave != NULL && RgInReach((uintptr_t)cave, exe) ? cave : NULL; /* one unused page kept: it may be the shared arena's */
}

/* The caves, then each site's jump, written with no thread in it. A site is whole on its own, so one that fails leaves
 * the others guarding. 1 installed (maybe in part), 0 not. */
static int RenderGuardsInstallAt(uintptr_t exe)
{
    if (g_rgTls == TLS_OUT_OF_INDEXES) g_rgTls = TlsAlloc();
    uint8_t* cave = g_rgTls != TLS_OUT_OF_INDEXES ? RgCavePage(exe) : NULL;
    if (cave == NULL) { Note("render guards: not installed; no memory near the game's exe"); return 0; }
    uint8_t* at[RG_SITES + 1];
    uint8_t* c = cave;
    for (size_t i = 0; i < RG_SITES; i++) { at[i] = c; c = RgSiteCave(c, exe, &kRgSites[i], &g_rgHits[i]); }
    at[RG_SITES] = c;
    RgPoolCave(c, exe);
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    int written = 0;
    for (size_t i = 0; i <= RG_SITES; i++)
    {
        uintptr_t site = exe + (i < RG_SITES ? kRgSites[i].rva : RG_POOL_EMPTY);
        size_t length = i < RG_SITES ? kRgSites[i].length : sizeof kRgPoolPin - 8;
        uint8_t jump[16];
        memset(jump, 0x90, sizeof jump); /* nops after the jump: never run, the cave comes back past them */
        int32_t rel = (int32_t)((intptr_t)at[i] - (intptr_t)(site + 5));
        jump[0] = 0xe9;
        memcpy(jump + 1, &rel, 4);
        if (JournalPut(site, jump, length)) Note("render guards: %s not guarded; its code could not be rewritten", i < RG_SITES ? kRgSites[i].name : "the job pool");
        else written++;
    }
    Note("render guards: %d of %d installed: freed resources bind as empty slots, a full job pool is waited out", written, (int)RG_SITES + 1);
    return written > 0;
}

/* From the watch thread. The exe's code is decoded some time after start; a minute after the game loaded, it never
 * will be the reviewed one. */
static void RenderGuardsInstall(void)
{
    if (g_rgState != 0) return;
    if (strcmp(g_build, "25480438")) { g_rgState = -1; return; }
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!RgPinned(exe))
    {
        if (!g_fired[HF_EVENT_GAME_LOADED]) return;
        if (g_rgLoadedAt == 0) g_rgLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_rgLoadedAt < 60000) return;
        g_rgState = -1;
        Note("render guards: not installed; the renderer's code is not the reviewed one");
        return;
    }
    g_rgState = RenderGuardsInstallAt(exe) ? 1 : -1;
}

/* What the guards caught, at most every 10 s, and at once when a wait gave up (the game crashes next). */
static void RenderGuardsReport(void)
{
    if (g_rgState != 1) return;
    LONG gaveUp = g_rgPoolGaveUp, stalled = g_rgPoolStalled;
    int ended = gaveUp != g_rgPoolGaveUpReported || stalled != g_rgPoolStalledReported;
    if (!ended && GetTickCount64() - g_rgReportedAt < 10000) return;
    int changed = ended || g_rgPoolWaits != g_rgPoolReported;
    for (size_t i = 0; i < RG_SITES; i++) changed |= g_rgHits[i] != g_rgReported[i];
    if (!changed) return;
    g_rgReportedAt = GetTickCount64();
    for (size_t i = 0; i < RG_SITES; i++)
        if (g_rgHits[i] != g_rgReported[i])
        {
            g_rgReported[i] = g_rgHits[i];
            Note("render guards: %ld freed resource(s) bound as empty in the %s so far", (long)g_rgReported[i], kRgSites[i].name);
        }
    if (!changed || (g_rgPoolWaits == g_rgPoolReported && !ended)) return;
    g_rgPoolReported = g_rgPoolWaits;
    Note("render guards: the job pool ran full %ld time(s), longest wait %ld ms", (long)g_rgPoolReported, (long)g_rgPoolLongestMs);
    g_rgPoolStalledReported = stalled;
    g_rgPoolGaveUpReported = gaveUp;
}

static void RenderGuardsWatch(void)
{
    RenderGuardsInstall();
    RenderGuardsReport();
}
