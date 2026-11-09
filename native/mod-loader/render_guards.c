/* Two crashes of the game's own renderer, build 25480438, that a slow PC on Direct3D 11 meets in the Armory and at
 * mission start. Both are the game reading memory it never checked; HellForge sends each read to the path the game
 * already has for that case, so nothing new is drawn or skipped. Included by hellforge_modloader.c after
 * shield_block.c: AllocateNearGame (damage.c), PinMatches (packages.c), Note, g_build, g_fired. */

/* A texture is freed while a material still binds it: its slot in the resource slot table holds 0xFFFFFFFF, and the
 * material binding loop reads entry -1. Slot indices are never reused, so a freed one always reads -1. Each site's
 * cave repeats the two moved loads, then sends -1 to the null-handle path (0xFFFFFF) right after: an empty slot. */
#define RG_BIND_NULL 0x7322EBu    /* the binding loop's null path: stores r8 as the view */
#define RG_RESOLVE_NULL 0x7306DFu /* the resolver's null path: xor eax, eax; ret */

typedef struct RgSite
{
    uint32_t rva;
    uint8_t length;    /* the moved bytes: two loads, the second the slot */
    uint8_t slotIsR8;  /* the slot lands in r8d, else ecx */
    uint8_t resolver;  /* the null path returns 0 instead of jumping */
    const char* name;
    uint8_t pin[19];   /* the moved bytes and the next 8, which stay */
} RgSite;

static const RgSite kRgSites[] = {
    {0x732019u, 10, 0, 0, "binding, textures", {0x49,0x8b,0x80,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x88, 0x49,0x8b,0x80,0xa0,0x02,0x00,0x00,0x48}},
    {0x732068u, 10, 0, 0, "binding, other views", {0x49,0x8b,0x81,0x28,0x02,0x00,0x00, 0x8b,0x0c,0x88, 0x4c,0x8d,0x04,0x49,0x4d,0x03,0xc0,0x83}},
    {0x732358u, 11, 1, 0, "binding, buffers and UAVs", {0x49,0x8b,0x81,0x28,0x02,0x00,0x00, 0x44,0x8b,0x04,0x88, 0x8d,0x42,0xff,0x83,0xf8,0x03,0x76,0x33}},
    {0x730677u, 11, 0, 1, "view lookup", {0x48,0x8b,0x81,0x28,0x02,0x00,0x00, 0x42,0x8b,0x0c,0x88, 0x8d,0x42,0xff,0x83,0xf8,0x03,0x76,0x30}},
};
#define RG_SITES (sizeof kRgSites / sizeof kRgSites[0])
static const uint8_t kRgBindNullPin[] = {0x41,0x8b,0x4d,0x08,0x41,0xff,0xc6};
static const uint8_t kRgResolveNullPin[] = {0x33,0xc0,0xc3};

/* The job pool (8192 slots) can run empty for a moment on a slow CPU. Its allocator then gives up after 33 spins
 * without writing the handles, and its only caller reads them anyway, uninitialised. The cave waits for a worker to
 * give a slot back: r9d, the spin count, keeps rising, so each pass is the same call; past the budget, the old failure. */
#define RG_POOL_EMPTY 0x623FC8u /* xor eax, eax; jmp fail: the allocator giving up */
#define RG_POOL_RETRY 0x623FB0u /* the loop's head: reads the free list again */
#define RG_POOL_FAIL 0x6240A9u
#define RG_POOL_SPINS 0x21u     /* r9d at the first give-up is one more */
static const uint8_t kRgPoolPin[] = {0x33,0xc0,0xe9,0xda,0x00,0x00,0x00, 0x41,0xb8,0x01,0x00,0x00,0x00,0x89,0x03};
static const uint8_t kRgPoolRetryPin[] = {0x41,0x8b,0x7a,0x40,0x8b,0xc7,0x25,0xff,0xff,0x7f,0x00};
static const uint8_t kRgPoolFailPin[] = {0x48,0x8b,0x5c,0x24,0x40};

static int g_rgState;      /* 0 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rgLoadedAt;
static volatile LONG g_rgHits[RG_SITES];
static volatile LONG g_rgPoolWaits, g_rgPoolGaveUp, g_rgPoolLongestMs;
static LONG g_rgReported[RG_SITES], g_rgPoolReported, g_rgPoolGaveUpReported;
static ULONGLONG g_rgReportedAt;
static DWORD g_rgTls = TLS_OUT_OF_INDEXES;
static ULONGLONG g_rgPoolWaitMs = 10000; /* a fixture shortens it */

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

/* Called from the pool cave on the game's thread, r9d as it stands. 1: wait again; 0: give up as the game did. */
static int RenderGuardPoolWait(uint32_t spins)
{
    DWORD error = GetLastError(); /* the game's code around it may read it */
    ULONGLONG now = GetTickCount64();
    if (spins == RG_POOL_SPINS + 1)
    {
        TlsSetValue(g_rgTls, (void*)(uintptr_t)now);
        InterlockedIncrement(&g_rgPoolWaits);
    }
    ULONGLONG waited = now - (ULONGLONG)(uintptr_t)TlsGetValue(g_rgTls);
    LONG ms = waited > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)waited, longest;
    while ((longest = g_rgPoolLongestMs) < ms && InterlockedCompareExchange(&g_rgPoolLongestMs, ms, longest) != longest) {}
    int again = waited < g_rgPoolWaitMs;
    if (again) Sleep(1);
    else InterlockedIncrement(&g_rgPoolGaveUp);
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

/* One freed-slot cave; its end. */
static uint8_t* RgSiteCave(uint8_t* c, uintptr_t exe, const RgSite* s, volatile LONG* hits)
{
    memcpy(c, s->pin, s->length);
    c += s->length;
    static const uint8_t cmpEcx[] = {0x83, 0xf9, 0xff}, cmpR8d[] = {0x41, 0x83, 0xf8, 0xff};
    if (s->slotIsR8) { memcpy(c, cmpR8d, sizeof cmpR8d); c += sizeof cmpR8d; }
    else { memcpy(c, cmpEcx, sizeof cmpEcx); c += sizeof cmpEcx; }
    *c++ = 0x74; *c++ = 5;                    /* je freed */
    c = RgJump(c, 0xe9, exe + s->rva + s->length);
    c = RgImm64(c, 0x48, 0xb8, (uint64_t)(uintptr_t)hits); /* freed: mov rax, &hits; rax is dead on both null paths */
    *c++ = 0xf0; *c++ = 0xff; *c++ = 0x00;    /* lock inc dword [rax] */
    if (s->resolver)
    {
        *c++ = 0x31; *c++ = 0xc0; *c++ = 0xc3; /* xor eax, eax; ret */
        return c;
    }
    *c++ = 0x45; *c++ = 0x31; *c++ = 0xc0;    /* xor r8d, r8d: the view stored is null */
    return RgJump(c, 0xe9, exe + RG_BIND_NULL);
}

/* The pool cave. At the give-up rsp is 16-aligned (three pushes and sub 0x20 after the call), so three pushes and
 * 0x28 align the call. r9, r10 (the pool) and r11 (the count) live on; rax, rcx, rdx and r8 are reloaded after. */
static uint8_t* RgPoolCave(uint8_t* c, uintptr_t exe)
{
    static const uint8_t save[] = {0x41,0x51, 0x41,0x52, 0x41,0x53, 0x48,0x83,0xec,0x28, 0x44,0x89,0xc9};
    memcpy(c, save, sizeof save); c += sizeof save;           /* push r9, r10, r11; sub rsp, 0x28; mov ecx, r9d */
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
        PinMatches(exe + RG_POOL_EMPTY, kRgPoolPin, sizeof kRgPoolPin) &&
        PinMatches(exe + RG_POOL_RETRY, kRgPoolRetryPin, sizeof kRgPoolRetryPin) &&
        PinMatches(exe + RG_POOL_FAIL, kRgPoolFailPin, sizeof kRgPoolFailPin);
}

/* The caves, then each site's jump, written with no thread in it. A site is whole on its own, so one that fails leaves
 * the others guarding. 1 installed (maybe in part), 0 not. */
static int RenderGuardsInstallAt(uintptr_t exe)
{
    if (g_rgTls == TLS_OUT_OF_INDEXES) g_rgTls = TlsAlloc();
    uint8_t* cave = g_rgTls != TLS_OUT_OF_INDEXES ? AllocateNearGame(exe, 0x1000) : NULL;
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
    Note("render guards: %d of %d installed: freed textures draw as empty slots, a full job pool is waited out", written, (int)RG_SITES + 1);
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

/* What the guards caught, at most every 10 s, and at once when a wait ran out (the game crashes next). */
static void RenderGuardsReport(void)
{
    if (g_rgState != 1) return;
    LONG gaveUp = g_rgPoolGaveUp;
    if (gaveUp == g_rgPoolGaveUpReported && GetTickCount64() - g_rgReportedAt < 10000) return;
    int changed = gaveUp != g_rgPoolGaveUpReported || g_rgPoolWaits != g_rgPoolReported;
    for (size_t i = 0; i < RG_SITES; i++) changed |= g_rgHits[i] != g_rgReported[i];
    if (!changed) return;
    g_rgReportedAt = GetTickCount64();
    for (size_t i = 0; i < RG_SITES; i++)
        if (g_rgHits[i] != g_rgReported[i])
        {
            g_rgReported[i] = g_rgHits[i];
            Note("render guards: %ld freed resource(s) drawn as empty in the %s so far", (long)g_rgReported[i], kRgSites[i].name);
        }
    if (g_rgPoolWaits != g_rgPoolReported || gaveUp != g_rgPoolGaveUpReported)
    {
        g_rgPoolReported = g_rgPoolWaits;
        g_rgPoolGaveUpReported = gaveUp;
        Note("render guards: the job pool ran full %ld time(s), longest wait %ld ms%s", (long)g_rgPoolReported, (long)g_rgPoolLongestMs,
            gaveUp ? "; a wait ran out, so the game's own failure follows" : "");
    }
}

static void RenderGuardsWatch(void)
{
    RenderGuardsInstall();
    RenderGuardsReport();
}
