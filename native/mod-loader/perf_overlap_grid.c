/* The stamp placement's overlap test on a grid (overlap-grid, docs/performance.md), build 25480438. Op 0x2B's jobs
 * (0xF322F0) test each candidate stamp against every record the job placed for its item so far (0xF30B60, the loop
 * 0xF31010..0xF3113F): quadratic in the item's records. A grid of the records' boxes, one per job, gives the loop only
 * the records near the candidate, in index order. The loop's own test still runs on each, so the same records reach
 * the rest of the test in the same order. Included by hellforge_modloader.c after render_guards.c: AllocateNearGame
 * (damage.c), PinMatches (packages.c), HookedCode (hooks.c), JournalPut (live_code.c), PerfFixOn, Note, g_build. */

#define OG_FUNCTION 0xF30B60u
#define OG_FUNCTION_SIZE 0x70Cu
#define OG_FUNCTION_HASH 0xACFFFEC68543D9A0ull /* FNV-1a of all of it: the frame, the dead hit buffer, the loop */
#define OG_ENTER 0xF31004u       /* two NOPs, after the candidate's box is in xmm6, xmm8, xmm15, xmm13 */
#define OG_LOOP 0xF31010u        /* the loop body: edi the record, r14 the item's base, r15d the count */
#define OG_STEP 0xF31130u        /* inc edi; cmp edi, r15d; jae end */
#define OG_BACK 0xF3113Bu        /* mov r14, [rbp-0x50]; jmp body */
#define OG_END 0xF311F8u         /* no record decided the test */
#define OG_MEMSET_AT 0xF3251Eu   /* the job's start: rcx its region, r8 cap x 0x130, then the call */
#define OG_MEMSET_CALL 0xF3253Bu /* call memset: the job zeroes its region before its first record */
#define OG_MEMSET 0x208AAA0u
static const uint8_t kOgEnterPin[] = {0x0f,0x1f,0x40,0x00,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00};
static const uint8_t kOgStepPin[] = {0xff,0xc7,0x41,0x3b,0xff,0x0f,0x83,0xbd,0x00,0x00,0x00};
static const uint8_t kOgMemsetPin[] = {0x49,0x8b,0x4a,0x20,0x48,0x89,0x4d,0x60,0xf3,0x48,0x0f,0x2a,0xc0,0x41,0x8b,0x42,0x38,
    0x4c,0x69,0xc0,0x30,0x01,0x00,0x00,0xf3,0x0f,0x11,0x45,0x6c,0xe8,0x60,0x85,0x15,0x01};

/* Why a job's grid stays true: the job writes a record only at index count, before counting it, and never again.
 * These are its writers and its count updates; a change to any of them leaves the fix off. */
static const struct { uint32_t rva; uint8_t size; uint8_t pin[36]; } kOgWriters[] = {
    {0xF31A1Bu, 34, {0x48,0x8b,0x4c,0x24,0x68,0x41,0x0f,0x28,0xd7,0x8b,0x94,0x24,0x28,0x03,0x00,0x00,0x8b,0x01,0x4c,0x69,0xc8,
        0x30,0x01,0x00,0x00,0x4c,0x03,0x4c,0x24,0x70,0xff,0xc0,0x89,0x01}}, /* 0xF314A0's record at *count, then *count + 1 */
    {0xF341CAu, 28, {0x8b,0x45,0x84,0x48,0x8b,0xb5,0xb8,0x00,0x00,0x00,0x48,0x69,0xd8,0x30,0x01,0x00,0x00,0x48,0x8b,0x85,0xa8,
        0x01,0x00,0x00,0x48,0x03,0x5d,0x60}}, /* the job's record at its count */
    {0xF348E4u, 28, {0x8b,0x45,0x84,0x48,0x8b,0xb5,0xb8,0x00,0x00,0x00,0x48,0x69,0xd8,0x30,0x01,0x00,0x00,0x48,0x8b,0x85,0xa8,
        0x01,0x00,0x00,0x48,0x03,0x5d,0x60}}, /* the same, on the other path */
    {0xF34AADu, 18, {0x44,0x8b,0x65,0x84,0x41,0xff,0xc4,0xc6,0x83,0x28,0x01,0x00,0x00,0x01,0x44,0x89,0x65,0x84}}, /* count + 1 */
    {0xF32F3Fu, 11, {0x8b,0x85,0x94,0x00,0x00,0x00,0xff,0xc3,0x03,0x45,0xd0}}, /* count from 0xF314A0's */
    {0xF334D4u, 12, {0x44,0x8b,0xa5,0x90,0x00,0x00,0x00,0xff,0xc3,0x45,0x03,0xe6}},
};
#define OG_WRITERS (sizeof kOgWriters / sizeof kOgWriters[0])

#define OG_RECORD 0x130u
#define OG_BOX 0xB8u            /* x0, y0, x1, y1: the loop's [record + 8 + 0xB0] */
#define OG_CELL 64.0            /* m; a power of two, so a cell index is exact */
#define OG_MARGIN 4.0           /* m, on both boxes: the loop's float rounding is under 0.75 m (docs) */
#define OG_LIMIT 1048576.0f     /* 2^20 m: the largest coordinate the margin covers */
#define OG_MIN_COUNT 64u        /* fewer records: the game's loop is as fast or faster */
#define OG_LIST 2047u           /* the hit buffer at [rbp-0x40] is 0x2000 bytes: indices and the end mark */
#define OG_LIST_END 0xFFFFFFFFu
#define OG_REGIONS 32u
#define OG_RECORD_CELLS 64      /* a larger record goes on the always list */
#define OG_QUERY_CELLS 1024     /* a larger candidate box takes the game's loop */
#define OG_MEMBERS (4u << 20)
#define OG_SKIP 8               /* queries the game's loop takes after an item proved dense */

typedef struct OgCell
{
    int32_t x, y;
    uint32_t count, cap; /* count 0: an empty slot, its list kept for reuse */
    uint32_t* list;
} OgCell;

typedef struct OgRegion
{
    SRWLOCK lock;
    uintptr_t start, end; /* a job's region, as its memset zeroed it */
    LONG epoch;           /* its registration; 0: free */
    uintptr_t base;       /* the item the grid holds */
    uint32_t built, failed;
    OgCell* cells;
    uint32_t cellCap, cellUsed;
    uint64_t members;
    uint32_t* always; /* records every query gets: not finite, beyond OG_LIMIT, or spanning many cells */
    uint32_t alwaysCount, alwaysCap;
    uint64_t* marks; /* one bit a record: a query's candidates, once each and in order */
    uint32_t markWords, skip;
} OgRegion;

static OgRegion g_ogRegions[OG_REGIONS];
static SRWLOCK g_ogTable = SRWLOCK_INIT;
static LONG g_ogEpoch;
static volatile LONG g_ogOverrun; /* regions up to this registration are off: a job's records passed its region */
static volatile LONG g_ogServed, g_ogPassed;
static LONG g_ogServedNoted, g_ogPassedNoted;
static ULONGLONG g_ogNotedAt;
static int g_ogState; /* 0 waiting for game.dll's code, 1 installed, -1 not */
static ULONGLONG g_ogFirstTry;

static uint32_t OgHash(int32_t x, int32_t y)
{
    uint64_t h = (uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)y * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h ^ h >> 32);
}

/* The cell's slot or the first empty one, which ends its probe chain. */
static OgCell* OgSlot(OgCell* cells, uint32_t cap, int32_t x, int32_t y)
{
    for (uint32_t n = 0, at = OgHash(x, y) & (cap - 1); n < cap; n++, at = (at + 1) & (cap - 1))
        if (cells[at].count == 0 || (cells[at].x == x && cells[at].y == y)) return &cells[at];
    return NULL;
}

static int OgGrow(OgRegion* r)
{
    uint32_t cap = r->cellCap ? r->cellCap * 2 : 256;
    OgCell* cells = cap > (1u << 20) ? NULL : (OgCell*)calloc(cap, sizeof *cells);
    if (cells == NULL) return 1;
    for (uint32_t i = 0; i < r->cellCap; i++)
    {
        if (r->cells[i].count == 0) { free(r->cells[i].list); continue; }
        *OgSlot(cells, cap, r->cells[i].x, r->cells[i].y) = r->cells[i];
    }
    free(r->cells);
    r->cells = cells;
    r->cellCap = cap;
    return 0;
}

static int OgPush(uint32_t** list, uint32_t* count, uint32_t* cap, uint32_t value)
{
    if (*count == *cap)
    {
        uint32_t grown = *cap ? *cap * 2 : 8;
        uint32_t* p = (uint32_t*)realloc(*list, (size_t)grown * sizeof *p);
        if (p == NULL) return 1;
        *list = p;
        *cap = grown;
    }
    (*list)[(*count)++] = value;
    return 0;
}

static int32_t OgCellOf(double v) { return (int32_t)floor(v / OG_CELL); }

static void OgReset(OgRegion* r, uintptr_t base)
{
    for (uint32_t i = 0; i < r->cellCap; i++) r->cells[i].count = 0;
    r->cellUsed = 0;
    r->members = 0;
    r->alwaysCount = 0;
    r->built = 0;
    r->failed = 0;
    r->skip = 0;
    r->base = base;
}

/* The cells box b plus the margin covers; 0 for a box every query gets (not finite, beyond OG_LIMIT, or many cells). */
static int OgBoxCells(const float* b, int32_t* cells)
{
    for (int i = 0; i < 4; i++)
        if (!(fabsf(b[i]) <= OG_LIMIT)) return 0; /* NaN too: an infinite edge can pass the test */
    cells[0] = OgCellOf(fmin(b[0], b[2]) - OG_MARGIN), cells[2] = OgCellOf(fmax(b[0], b[2]) + OG_MARGIN);
    cells[1] = OgCellOf(fmin(b[1], b[3]) - OG_MARGIN), cells[3] = OgCellOf(fmax(b[1], b[3]) + OG_MARGIN);
    return (int64_t)(cells[2] - cells[0] + 1) * (cells[3] - cells[1] + 1) <= OG_RECORD_CELLS;
}

static int OgAddCell(OgRegion* r, int32_t x, int32_t y, uint32_t index)
{
    if ((r->cellUsed + 1) * 2 > r->cellCap && OgGrow(r)) return 1;
    OgCell* c = OgSlot(r->cells, r->cellCap, x, y);
    if (c->count == 0) { c->x = x; c->y = y; r->cellUsed++; }
    return OgPush(&c->list, &c->count, &c->cap, index) || ++r->members > OG_MEMBERS;
}

/* Record `index` into the grid: under every cell its box plus the margin covers. 1 when memory ran out. */
static int OgAdd(OgRegion* r, uintptr_t base, uint32_t index)
{
    float b[4];
    int32_t cells[4];
    memcpy(b, (const void*)(base + (uintptr_t)index * OG_RECORD + OG_BOX), sizeof b);
    if (!OgBoxCells(b, cells)) return OgPush(&r->always, &r->alwaysCount, &r->alwaysCap, index);
    for (int32_t y = cells[1]; y <= cells[3]; y++)
        for (int32_t x = cells[0]; x <= cells[2]; x++)
            if (OgAddCell(r, x, y, index)) return 1;
    return 0;
}

static int OgMarksFor(OgRegion* r, uint32_t words)
{
    if (words <= r->markWords) return 1;
    uint64_t* marks = (uint64_t*)calloc((size_t)words * 2, sizeof *marks);
    if (marks == NULL) return 0;
    free(r->marks);
    r->marks = marks;
    r->markWords = words * 2;
    return 1;
}

/* Marks the records under `count` in list; their number. */
static uint32_t OgMark(OgRegion* r, const uint32_t* list, uint32_t n, uint32_t count)
{
    uint32_t marked = 0;
    for (uint32_t k = 0; k < n; k++)
        if (list[k] < count) { r->marks[list[k] >> 6] |= 1ull << (list[k] & 63); marked++; }
    return marked;
}

/* The marked records ascending, then the end mark, clearing the marks; -1 when `list` is off (or they overflow it). */
static int OgList(OgRegion* r, uint32_t words, int n, uint32_t* list)
{
    for (uint32_t w = 0; w < words; w++)
    {
        for (uint64_t v = r->marks[w]; v != 0 && n >= 0; v &= v - 1)
            if (n == (int)OG_LIST) n = -1;
            else list[n++] = w * 64 + (uint32_t)__builtin_ctzll(v);
        r->marks[w] = 0;
    }
    if (n >= 0) list[n] = OG_LIST_END;
    return n;
}

/* The candidates for query q (centre x, y, half x, y): the always list and the records in the cells its box plus the
 * margin covers. Their number, or -1 for the game's loop: when they are half the records or more, the loop is as
 * fast, and the next few queries go to it too. */
static int OgGather(OgRegion* r, uint32_t count, const float* q, uint32_t* list)
{
    double hx = fabs((double)q[2]), hy = fabs((double)q[3]);
    int32_t x0 = OgCellOf(q[0] - hx - OG_MARGIN), x1 = OgCellOf(q[0] + hx + OG_MARGIN);
    int32_t y0 = OgCellOf(q[1] - hy - OG_MARGIN), y1 = OgCellOf(q[1] + hy + OG_MARGIN);
    uint32_t words = (count + 63) / 64;
    if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > OG_QUERY_CELLS || !OgMarksFor(r, words)) return -1;
    uint32_t marked = OgMark(r, r->always, r->alwaysCount, count);
    for (int32_t y = y0; y <= y1 && r->cellCap; y++)
        for (int32_t x = x0; x <= x1; x++)
        {
            const OgCell* c = OgSlot(r->cells, r->cellCap, x, y);
            if (c != NULL) marked += OgMark(r, c->list, c->count, count);
        }
    int dense = marked * 2 >= count;
    if (dense) r->skip = OG_SKIP;
    return OgList(r, words, dense ? -1 : 0, list);
}

/* Under r's lock. A new item (another base) or a shorter count starts the grid again from the records as they are. */
static int OgServe(OgRegion* r, uintptr_t base, uint32_t count, const float* q, uint32_t* list)
{
    if (r->base != base || count < r->built) OgReset(r, base);
    for (; !r->failed && r->built < count; r->built++)
        if (OgAdd(r, base, r->built)) r->failed = 1;
    if (r->failed) return -1;
    if (r->skip) { r->skip--; return -1; }
    return OgGather(r, count, q, list);
}

/* The registered region holding base, locked, or NULL. Records past the region's end mean its job's region is not
 * what the grid took it for: every grid registered so far turns off. */
static OgRegion* OgFind(uintptr_t base, uint32_t count)
{
    OgRegion* r = NULL;
    AcquireSRWLockShared(&g_ogTable);
    for (uint32_t i = 0; i < OG_REGIONS && r == NULL; i++)
    {
        OgRegion* g = &g_ogRegions[i];
        if (g->epoch > g_ogOverrun && g->start <= base && base < g->end) r = g;
    }
    if (r != NULL) AcquireSRWLockExclusive(&r->lock);
    if (r != NULL && count > (r->end - base) / OG_RECORD)
    {
        InterlockedExchange(&g_ogOverrun, g_ogEpoch);
        ReleaseSRWLockExclusive(&r->lock);
        r = NULL;
    }
    ReleaseSRWLockShared(&g_ogTable);
    return r;
}

/* The entry stub's call (0xF31004): rcx the item's base, edx the count, r8 the candidate's box, r9 the hit buffer.
 * The number of candidates written there, or -1: the game's loop over every record. */
static int OverlapGridList(uintptr_t base, uint32_t count, const float* q, uint32_t* list)
{
    if (count < OG_MIN_COUNT) return -1;
    int ok = 1;
    for (int i = 0; i < 4; i++) ok &= fabsf(q[i]) <= OG_LIMIT;
    OgRegion* r = ok ? OgFind(base, count) : NULL;
    int n = -1;
    if (r != NULL)
    {
        n = OgServe(r, base, count, q, list);
        ReleaseSRWLockExclusive(&r->lock);
    }
    InterlockedIncrement(n >= 0 ? &g_ogServed : &g_ogPassed);
    return n;
}

static void OgNoteCounts(void)
{
    LONG served = g_ogServed, passed = g_ogPassed;
    if ((served == g_ogServedNoted && passed == g_ogPassedNoted) || GetTickCount64() - g_ogNotedAt < 10000) return;
    Note("performance fix overlap-grid: %ld stamp tests on the grid, %ld on the game's loop so far", (long)served, (long)passed);
    g_ogServedNoted = served;
    g_ogPassedNoted = passed;
    g_ogNotedAt = GetTickCount64();
}

/* The memset stub's call: a job starts on [start, start + size). Grids on memory it overlaps are stale; it takes a free
 * region, or the oldest (a live job that loses its region takes the game's loop). */
static void OverlapGridRegion(uintptr_t start, size_t size)
{
    if (size == 0 || start + size < start) return;
    AcquireSRWLockExclusive(&g_ogTable);
    OgRegion* pick = NULL;
    for (uint32_t i = 0; i < OG_REGIONS; i++)
    {
        OgRegion* g = &g_ogRegions[i];
        if (g->epoch != 0 && g->start < start + size && start < g->end) { AcquireSRWLockExclusive(&g->lock); g->epoch = 0; ReleaseSRWLockExclusive(&g->lock); }
        if (pick == NULL || g->epoch < pick->epoch) pick = g;
    }
    AcquireSRWLockExclusive(&pick->lock);
    pick->start = start;
    pick->end = start + size;
    pick->epoch = ++g_ogEpoch;
    OgReset(pick, 0);
    ReleaseSRWLockExclusive(&pick->lock);
    OgNoteCounts();
    ReleaseSRWLockExclusive(&g_ogTable);
}

/* Entry (0xF31004): with candidates, edi the first and r15 the next's address, bit 63 set (r15 is free in the loop and
 * a count never has it); none: the loop's end; -1: the game's loop. The C call keeps rdi, r14, r15 and xmm6-15; the
 * loop writes rax..r11 and xmm0-5 before it reads them. rsp is 16-aligned here and after the sub. */
static const uint8_t kOgEnterStub[] = {
    0x48,0x83,0xec,0x30, 0xf3,0x0f,0x11,0x74,0x24,0x20, 0xf3,0x44,0x0f,0x11,0x44,0x24,0x24,
    0xf3,0x44,0x0f,0x11,0x7c,0x24,0x28, 0xf3,0x44,0x0f,0x11,0x6c,0x24,0x2c,  /* the box to [rsp+0x20] */
    0x4c,0x89,0xf1, 0x44,0x89,0xfa, 0x4c,0x8d,0x44,0x24,0x20, 0x4c,0x8d,0x4d,0xc0, /* rcx r14, edx r15d, r8, r9 rbp-0x40 */
    0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0, 0x48,0x83,0xc4,0x30,           /* 46: mov rax, OverlapGridList; call */
    0x85,0xc0, 0x78,0x0e, 0x74,0x11,                                      /* test; js loop; je end */
    0x8b,0x7d,0xc0, 0x4c,0x8d,0x7d,0xc4, 0x49,0x0f,0xba,0xef,0x3f,        /* edi = list[0]; r15 = &list[1] | 1 << 63 */
    0xe9,0,0,0,0, 0xe9,0,0,0,0};                                          /* 80: jmp loop; 85: jmp end */
#define OG_ENTER_CALL 48
#define OG_ENTER_LOOP 81
#define OG_ENTER_END 86

/* Step (0xF31130): the game's stepping while r15 is a count, else the list's next until its end mark. rax and the
 * flags are dead at both targets. */
static const uint8_t kOgStepStub[] = {
    0x4d,0x85,0xff, 0x78,0x10, 0xff,0xc7, 0x44,0x39,0xff,  /* test r15, r15; js list; inc edi; cmp edi, r15d */
    0x0f,0x83,0,0,0,0, 0xe9,0,0,0,0,                       /* 12: jae end; 17: jmp back */
    0x4c,0x89,0xf8, 0x48,0x0f,0xba,0xf0,0x3f, 0x8b,0x00, 0x49,0x83,0xc7,0x04, /* list: eax = *(r15 & ~bit 63); r15 += 4 */
    0x83,0xf8,0xff, 0x0f,0x84,0,0,0,0, 0x89,0xc7, 0xe9,0,0,0,0}; /* 40: je end; edi = eax; 47: jmp back */
#define OG_STEP_END 12
#define OG_STEP_BACK 17
#define OG_STEP_LIST_END 40
#define OG_STEP_LIST_BACK 47

/* Memset (0xF3253B, called): rsp is 8 past a 16 boundary on entry, so four pushes and 0x28 align the call. The call's
 * arguments are kept for memset; the rest is a call's to clobber. */
static const uint8_t kOgRegionStub[] = {
    0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x48,0x83,0xec,0x28, 0x4c,0x89,0xc2,  /* push rcx rdx r8 r9; rdx = size */
    0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0, 0x48,0x83,0xc4,0x28,              /* 15: mov rax, OverlapGridRegion; call */
    0x41,0x59, 0x41,0x58, 0x5a, 0x59, 0xe9,0,0,0,0};                       /* pop; 36: jmp memset */
#define OG_REGION_CALL 15
#define OG_REGION_MEMSET 36

static int32_t OgDistance(uintptr_t next, uintptr_t target, int* ok)
{
    int64_t d = (int64_t)target - (int64_t)next;
    if (d > 0x7FFF0000 || d < -0x7FFF0000) *ok = 0;
    return (int32_t)d;
}

/* A stub's rel32 at `at`, to target. */
static void OgRel(uint8_t* at, uintptr_t target, int* ok)
{
    int32_t rel = OgDistance((uintptr_t)at + 4, target, ok);
    memcpy(at, &rel, 4);
}

/* A 5-byte jump or call at the game's site, to the stub. */
static void OgSiteRel(uint8_t* bytes, uintptr_t site, const uint8_t* stub, int* ok)
{
    int32_t rel = OgDistance(site + 5, (uintptr_t)stub, ok);
    memcpy(bytes + 1, &rel, 4);
}

static void OgImm(uint8_t* at, const void* value) { uint64_t v = (uint64_t)(uintptr_t)value; memcpy(at, &v, 8); }

static int OverlapGridPinned(uintptr_t game)
{
    uint8_t code[448];
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t at = 0; at < OG_FUNCTION_SIZE; at += sizeof code)
    {
        uint32_t size = OG_FUNCTION_SIZE - at < sizeof code ? OG_FUNCTION_SIZE - at : (uint32_t)sizeof code;
        if (HookedCode(game + OG_FUNCTION + at, code, size)) return 0;
        for (uint32_t i = 0; i < size; i++) hash = (hash ^ code[i]) * 0x100000001B3ull;
    }
    int pinned = hash == OG_FUNCTION_HASH && PinMatches(game + OG_ENTER, kOgEnterPin, sizeof kOgEnterPin) &&
        PinMatches(game + OG_STEP, kOgStepPin, sizeof kOgStepPin) && PinMatches(game + OG_MEMSET_AT, kOgMemsetPin, sizeof kOgMemsetPin);
    for (size_t i = 0; i < OG_WRITERS && pinned; i++) pinned = PinMatches(game + kOgWriters[i].rva, kOgWriters[i].pin, kOgWriters[i].size);
    return pinned;
}

/* The stubs, then the memset call, the step and the entry, in that order: a loop running meanwhile sees r15 as a count
 * until the entry is in, and a job that started before the memset call has no region, so the game's loop. */
static int OverlapGridInstallAt(uintptr_t game)
{
    uint8_t* page = AllocateNearGame(game, 0x1000);
    if (page == NULL) { Note("performance fix overlap-grid: not installed; no memory near game.dll"); return 0; }
    uint8_t *enter = page, *step = page + 0x80, *region = page + 0x100;
    memcpy(enter, kOgEnterStub, sizeof kOgEnterStub);
    memcpy(step, kOgStepStub, sizeof kOgStepStub);
    memcpy(region, kOgRegionStub, sizeof kOgRegionStub);
    OgImm(enter + OG_ENTER_CALL, (const void*)OverlapGridList);
    OgImm(region + OG_REGION_CALL, (const void*)OverlapGridRegion);
    int ok = 1;
    OgRel(enter + OG_ENTER_LOOP, game + OG_LOOP, &ok);
    OgRel(enter + OG_ENTER_END, game + OG_END, &ok);
    OgRel(step + OG_STEP_END, game + OG_END, &ok);
    OgRel(step + OG_STEP_BACK, game + OG_BACK, &ok);
    OgRel(step + OG_STEP_LIST_END, game + OG_END, &ok);
    OgRel(step + OG_STEP_LIST_BACK, game + OG_BACK, &ok);
    OgRel(region + OG_REGION_MEMSET, game + OG_MEMSET, &ok);
    uint8_t call[5] = {0xe8}, stepJump[5] = {0xe9}, enterJump[12] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    OgSiteRel(call, game + OG_MEMSET_CALL, region, &ok);
    OgSiteRel(stepJump, game + OG_STEP, step, &ok);
    OgSiteRel(enterJump, game + OG_ENTER, enter, &ok);
    if (!ok) { Note("performance fix overlap-grid: not installed; its stubs are out of reach of game.dll"); return 0; }
    DWORD previous;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    if (JournalPut(game + OG_MEMSET_CALL, call, sizeof call) || JournalPut(game + OG_STEP, stepJump, sizeof stepJump) ||
        JournalPut(game + OG_ENTER, enterJump, sizeof enterJump))
    {
        Note("performance fix overlap-grid: not installed; game.dll's code could not be rewritten");
        return 0;
    }
    Note("performance fix overlap-grid: installed; op 0x2B's overlap test walks a %.0f m grid from %u records up", OG_CELL, OG_MIN_COUNT);
    return 1;
}

/* After the game loaded (WatchInstalls). A minute later the code never will be the reviewed one. */
static void OverlapGridWatch(void)
{
    if (g_ogState != 0) return;
    if (strcmp(g_build, "25480438"))
    {
        g_ogState = -1;
        Note("performance fix overlap-grid: not installed; build %s is not the reviewed 25480438", g_build);
        return;
    }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (g_ogFirstTry == 0) g_ogFirstTry = GetTickCount64();
    if (!OverlapGridPinned(game))
    {
        if (GetTickCount64() - g_ogFirstTry < 60000) return;
        g_ogState = -1;
        Note("performance fix overlap-grid: not installed; the code is not the reviewed one");
        return;
    }
    g_ogState = PerfFixOn("overlap-grid") && OverlapGridInstallAt(game) ? 1 : -1;
}
