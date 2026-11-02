/* XL-6: the stamp-placement overlap test on a grid (game.dll 0xF30B60, the loop 0xF31010..0xF3113F).
 * Included by hellforge_modloader.c after difficulty_world_plan.c (it reads g_world, and the stub macros are
 * difficulty_cities.c's).
 *
 * PlaceStampWithWeightRule (op 0x2B, 0xF34CC0) runs 8 jobs (0xF322F0). Each job fills one slice of the generation arena's
 * instance buffer (0x130-byte records at slice base + 8, the job's slice at [rbp-0x30] and its current end at [rbp-0x7C] in
 * the caller) and, for every candidate stamp, calls 0xF30B60, which tests the candidate's box against every record already
 * placed in the slice (the loop 0xF31010..0xF3113F). The slice grows as the job places stamps (24576 records a slice at
 * 16 km; difficulty_world.c's kPlaceBuffer), so the pass is O(n^2): about 80 s at 16 km.
 *
 * The loop body (verified against .deepcode/xl6/overlap_f30b60.asm and callers.asm):
 *   r14 = [rbp-0x50] (the 5th argument, the slice's record base: buffer + sliceStart * 0x130; it is reloaded at 0xF3113B
 *         after the body borrows r14 for the 0xFCBC20 call), r15d = [rbp+0x20C8] (the 6th argument, count = end -
 *         sliceStart), rdi = the record index 0..count-1 (set to 0 at 0xF30FCA).
 *   record i's box is at base + 8 + i * 0x130 + 0xB0: x0, y0 at +0xB0 and x1, y1 at +0xB8 (0xF3102A, 0xF31045).
 *   The query box is computed at 0xF30FD5..0xF30FFF from the two points [rsp+0x40] and [rsp+0x50]:
 *     xmm6 = (xmm4 + xmm15) * 0.5 = centre x    (xmm4 = [rsp+0x40], xmm15 = [rsp+0x50])
 *     xmm8 = (xmm3 + xmm13) * 0.5 = centre y    (xmm3 = [rsp+0x44], xmm13 = [rsp+0x54])
 *     xmm15 = (xmm15 - xmm4) * 0.5 = half x
 *     xmm13 = (xmm13 - xmm3) * 0.5 = half y
 *   For a record (0xF3102A..0xF310B6) the body computes the same centre and half extents in float, then in double:
 *     sx >= |dx|  and  sy >= |dy|, with dx = record centre x - query centre x, sx = record half x + query half x, etc.
 *     (0xF3108F cvtps2pd/andps/comisd/jb and 0xF310A8 for y). Only a record passing both is passed to 0xF30A20 and
 *     0xFCBC20; the loop leaves with a result on the first one in index order whose second check passes (0xF311B3,
 *     0xF31156). Extra records in a candidate list are harmless: the body still runs the exact test above and skips them.
 *
 * What this file does: per buffer (r14), a 64 m grid of the records' boxes, built incrementally as r15d grows. For the query
 * box it gathers the records in the cells the query (plus a margin) covers, deduplicates them, sorts them ascending, keeps
 * only indices below r15d, and hands them to the loop. Two stubs are patched in:
 *   0xF31004 (12 NOPs, after the query box is computed, before the loop): calls OverlapBegin, which builds/extends the
 *     buffer's grid, makes the candidate list in this thread's TLS state and returns its first index (or -1: none; -2:
 *     use the original loop). edi is set to the index.
 *   0xF31130 (inc edi; cmp edi, r15d; jae 0xF311F8): calls OverlapNext for the next index, or emulates the original
 *     stepping when the level is not extra large (g_overlap.level) or this loop fell back. Every non-negative index jumps
 *     to 0xF3113B, which reloads r14 and re-enters the body at 0xF31010; -1 jumps to 0xF311F8 exactly as the original
 *     jae does.
 * The stubs preserve rax rcx rdx r8 r9 r10 r11 (rax is deliberately not restored: it is dead at both sites and carries
 * the C result), xmm0..xmm5, and rely on the Windows ABI for rbx rsi rdi rbp r12-r15 and xmm6-xmm15 (the query values in
 * xmm6/xmm8/xmm13/xmm15 and the 0.5 in xmm9 must survive; the C compiler keeps them).
 *
 * Cell size: 64 m, fixed for every buffer. With 24576 records over a 16384 m world that is a 256 x 256 grid and about
 * 0.4 records a cell; a stamp's query box covers a handful of cells, so the loop sees a few records instead of 24576.
 * A median-driven cell size was considered (the task's other option), but the first call for a buffer may hold only the
 * candidate's record, and a fixed size keeps the grid's cell indices stable while the buffer grows. A record whose box
 * spans more than OVERLAP_MAX_RECORD_CELLS cells, or whose coordinates are not finite or beyond OVERLAP_LIMIT, goes to
 * the buffer's always list (every query includes it; such records cannot pass the original test when non-finite, and the
 * always list keeps the proof below true).
 *
 * The margin: 8 m on both sides of both boxes. The original computes the centres/half extents in float (each operation
 * rounds within 1/2 ulp) and converts to double exactly; the comparison itself adds no error. For |coordinate| <=
 * OVERLAP_LIMIT = 2^20 m one float ulp is at most 2^-4 m, and even including the sums of two such coordinates (one more
 * exponent) a handful of operations stays below 8 m. A record that passes the original box test therefore has its exact
 * interval [min(x0,x1), max(x0,x1)] within 8 m of [qcx - |qhx|, qcx + |qhx|]; the record is indexed under every cell of
 * its interval plus 8 m, and the query looks at every cell of its interval plus 8 m, so the two cell ranges share a cell
 * whenever the record can pass. That, the ascending order and the < r15d filter together make the list a superset of the
 * records the original would test further (check mode below verifies the box test itself on small buffers).
 *
 * The grids live for one run of one pass record (g_worldOpRun, difficulty_world.c: bumped at level init and whenever the
 * dispatcher moves to another record), so a later op 0x2B whose arena buffer lands at the same address starts again; the
 * job only appends records (the box is written before [rbp-0x7C] counts it, 0xF349E1 .. 0xF34ABB), so within a run the
 * records below the count never change.
 *
 * Only extra large levels take this path: g_world.current > 1 (difficulty_world.c) is checked on every OverlapBegin, and
 * the continuation stub reads g_overlap.level, which OverlapBegin clears on a normal level, so a normal map pays one C
 * call per candidate (not per record) and keeps the original stepping.
 *
 * Research: one call in OVERLAP_CHECK_EVERY a thread, when count <= OVERLAP_CHECK_LIMIT (4000), OverlapBegin also scans 0..r15d-1 with the exact double-precision box
 * test above and notes any record that passes but is not in the list (the first four). Per level generation it notes the
 * number of calls, the average list length, the average r15d, the fallbacks and the misses.
 *
 * Wiring (the calling files are not part of this task): hellforge_modloader.c includes this file after
 * difficulty_world_plan.c, and difficulty_world.c's InstallWorld calls InstallWorldOverlap(game) after
 * InstallWorldPlan(game) (a forward declaration beside InstallWorldPlan's). The install is optional: a pin mismatch or
 * no memory leaves the original loop, it does not refuse the level.
 */

#define OVERLAP_FUNCTION 0xF30B60u     /* the function's first bytes (the frame and its callee saves) */
#define OVERLAP_ENTER_SITE 0xF31004u   /* the two NOPs after the query box is computed */
#define OVERLAP_LOOP_SITE 0xF31010u    /* the loop body's first instruction (the jump-back target) */
#define OVERLAP_NEXT_SITE 0xF31130u    /* inc edi; cmp edi, r15d (five bytes) */
#define OVERLAP_BACK_SITE 0xF3113Bu    /* mov r14, [rbp-0x50]; jmp the loop body */
#define OVERLAP_END_SITE 0xF311F8u     /* mov eax, [rbp-0x80]; the loop's no-result end */
#define OVERLAP_BOX_A_SITE 0xF3102Au   /* mov rax, [rbx+0xb0] (x0, y0) */
#define OVERLAP_BOX_B_SITE 0xF31045u   /* mov rax, [rbx+0xb8] (x1, y1) */
#define OVERLAP_COUNT_SITE 0xF30FC3u   /* mov r15d, [rbp+0x20c8]; mov edi, edx; test r15d, r15d */
#define OVERLAP_QUERY_SITE 0xF30FD5u   /* the query box's centre x/y computation */
#define OVERLAP_QUERY_2_SITE 0xF30FE6u /* the query box's half extents */
#define OVERLAP_TEST_A_SITE 0xF3108Fu  /* the x box test (cvtps2pd, andps, comisd, jb) */
#define OVERLAP_TEST_B_SITE 0xF310A8u  /* the y box test */

/* The reviewed windows. The 0xF31130 pin includes the jae's distance to 0xF311F8 and the 0xF30Fxx pins include the
 * displacements our stubs rely on; a build whose code differs does not take this path. */
static const char kOverlapFunctionPin[] = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 90 DF FF FF";
/* The frame: five pushes, then lea rbp, [rsp-0x2070], so the return address is at [rbp+0x2098] (the 5th argument, the
 * base, at [rbp+0x20C0] as 0xF30C22 reads it). The callers whose records are the job's own slice (append only): */
#define OVERLAP_FRAME_RETURN 0x2098u
#define OVERLAP_CALLER_A 0xF33FA6u /* after the call at 0xF33FA1 */
#define OVERLAP_CALLER_B 0xF34642u /* after the call at 0xF3463D */
static const char kOverlapEnterPin[] = "0F 1F 40 00 0F 1F 84 00 00 00 00 00";
static const char kOverlapLoopPin[] = "8B C7 49 8D 5E 08 48 69 C8 30 01 00 00";
static const char kOverlapNextPin[] = "FF C7 41 3B FF 0F 83 BD 00 00 00";
static const char kOverlapBackPin[] = "4C 8B 75 B0 E9 CC FE FF FF";
static const char kOverlapCountPin[] = "44 8B BD C8 20 00 00 8B FA 45 85 FF";
static const char kOverlapQueryPin[] = "0F 28 F4 44 0F 28 C3 F3 41 0F 58 F7 F3 45 0F 58 C5";
static const char kOverlapQuery2Pin[] = "F3 44 0F 5C FC F3 44 0F 5C EB";
static const char kOverlapTestAPin[] = "0F 5A CA 0F 5A C3 41 0F 54 CC 66 0F 2F C1 0F 82 8D 00 00 00";
static const char kOverlapTestBPin[] = "0F 5A CD 0F 5A C4 41 0F 54 CC 66 0F 2F C1 72 78";
static const char kOverlapBoxAPin[] = "48 8B 83 B0 00 00 00 48 89 44 24 50 F3 0F 10 4C 24 50";
static const char kOverlapBoxBPin[] = "48 8B 83 B8 00 00 00 0F 28 E8 48 89 45 88 F3 0F 10 5D 88";

#define OVERLAP_CELL 64.0f                   /* grid cell, m (fixed: see the comment above) */
#define OVERLAP_MARGIN 8.0f                  /* the proof's margin, m (see above) */
#define OVERLAP_LIMIT 1048576.0f             /* 2^20 m: the largest coordinate the margin covers */
#define OVERLAP_CHECK_LIMIT 4000u            /* check mode's largest count */
#define OVERLAP_MAX_COUNT 1048576u           /* a larger count falls back */
#define OVERLAP_MAX_QUERY_CELLS 262144u      /* a query covering more cells falls back */
#define OVERLAP_MAX_RECORD_CELLS 4096u       /* a record spanning more cells goes to the always list */
#define OVERLAP_MAX_CELLS_TABLE 1048576u     /* the per-buffer cell hash's largest table */
#define OVERLAP_MAX_CELL_LIST 1048576u       /* one cell's largest list */
#define OVERLAP_MAX_ALWAYS 1048576u          /* the always list's largest length */
#define OVERLAP_MAX_MEMBERS (8ull << 20)     /* all cell memberships a buffer may hold */
#define OVERLAP_MAX_SCRATCH (2u << 20)       /* one query's gathered indices */
#define OVERLAP_BUFFERS 64u                  /* the buffers (8 jobs' slices and 0xF314A0's, room for a second run) */
#define OVERLAP_CHECK_EVERY 256u             /* check mode takes one call in this many, per thread */
#define OVERLAP_STATS_BATCH 4096u            /* a thread adds its counters to the shared ones this often */

typedef struct OverlapCell
{
    int32_t x, y;          /* cell coordinates */
    uint32_t count, cap;   /* list[0..count) is live */
    uint32_t* list;        /* record indices, ascending (records are appended in index order) */
} OverlapCell;

typedef struct OverlapBuffer
{
    SRWLOCK lock;          /* one job has the buffer; the lock only makes the diagnosis safe */
    LONG used;
    uintptr_t base;        /* r14: the slice's record base */
    uint32_t generation;   /* g_world.levels when claimed */
    uint32_t built;        /* records 0..built-1 are in the grid */
    uint32_t failed;       /* no more additions or queries for this buffer */
    OverlapCell* cells;    /* open-addressing hash of cell coordinates */
    uint32_t cellCap, cellUsed;
    uint64_t members;      /* the sum of the cells' list counts */
    uint32_t* always;      /* records a query always includes */
    uint32_t alwaysCount, alwaysCap;
    float* boxes;          /* each indexed record's box as it was indexed (check mode compares) */
    uint32_t boxCap;
} OverlapBuffer;

typedef struct OverlapThread
{
    uint32_t* scratch;     /* one query's raw memberships (with duplicates) */
    uint32_t scratchCount, scratchCap;
    uint32_t* list;        /* the query's sorted, unique candidate indices */
    uint32_t listCount, listCap;
    uint32_t pos;          /* the next index OverlapNext returns */
    int active;            /* this loop uses the list (0: fall back) */
    uintptr_t frame;       /* the rbp of the 0xF30B60 call the list belongs to */
    LONG cacheGeneration;  /* the buffer this thread used last (no table lock while it stays the same) */
    uintptr_t cacheBase;
    struct OverlapBuffer* cacheEntry;
    uint32_t sinceCheck;   /* calls since check mode last ran on this thread */
    uint64_t calls, listSum, countSum, fallbacks[4]; /* not yet added to the shared counters */
} OverlapThread;

typedef struct OverlapGlobal
{
    volatile LONG installed;
    volatile LONG level;   /* 1 while an extra large level (the continuation stub reads it) */
    DWORD tls;             /* the per-thread OverlapThread */
    uintptr_t game;
} OverlapGlobal;

static OverlapGlobal g_overlap = {0, 0, TLS_OUT_OF_INDEXES, 0};
static OverlapBuffer g_overlapBuffers[OVERLAP_BUFFERS];
static SRWLOCK g_overlapTableLock = SRWLOCK_INIT;
static volatile LONG g_overlapTableGeneration;
static volatile LONG64 g_overlapCalls, g_overlapListSum, g_overlapCountSum, g_overlapFallbacks, g_overlapMisses;
/* Fallbacks by reason: the query or count out of range, no free buffer slot, the slot taken over between the claim and
 * its lock, the grid or list could not be made. */
static volatile LONG64 g_overlapFallbackWhy[4];
static volatile LONG64 g_overlapChecks;
static SRWLOCK g_overlapStatsLock = SRWLOCK_INIT;

/* The original's box test (0xF31010..0xF310B6), in the same float operations and the same double comparison:
 *   centre = (a + b) * 0.5f, half = (b - a) * 0.5f, then (double)(halfRecord + halfQuery) >= fabs((double)(centreRecord
 *   - centreQuery)) on x and on y. comisd + jb skips when the comparison is unordered, which is what the double >=
 *   does for a NaN. */
static inline int OverlapBoxPass(float qcx, float qcy, float qhx, float qhy, const float* box)
{
    float rx = (box[0] + box[2]) * 0.5f;
    float ry = (box[1] + box[3]) * 0.5f;
    float hx = (box[2] - box[0]) * 0.5f;
    float hy = (box[3] - box[1]) * 0.5f;
    float dx = rx - qcx;
    float dy = ry - qcy;
    float sx = hx + qhx;
    float sy = hy + qhy;
    return (double)sx >= fabs((double)dx) && (double)sy >= fabs((double)dy);
}

/* A coordinate's cell. The callers bound every value by OVERLAP_LIMIT (records beyond it go to the always list, queries
 * beyond it fall back), and a half extent is at most 2^20 m, so the quotient stays well inside int32_t. */
static inline int32_t OverlapCellCoord(double value)
{
    return (int32_t)floor(value / (double)OVERLAP_CELL);
}

static inline uint32_t OverlapHash(int32_t x, int32_t y)
{
    uint64_t h = (uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)y * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (uint32_t)h;
}

/* The first empty or matching slot of a table. An empty slot ends a probe chain, so it is also the answer for a lookup
 * that misses. */
static OverlapCell* OverlapSlot(OverlapCell* cells, uint32_t cap, int32_t x, int32_t y)
{
    uint32_t mask = cap - 1u;
    uint32_t at = OverlapHash(x, y) & mask;
    for (uint32_t n = 0; n < cap; n++)
    {
        OverlapCell* c = &cells[at];
        if (c->count == 0 || (c->x == x && c->y == y)) return c;
        at = (at + 1u) & mask;
    }
    return NULL;
}

static int OverlapGrowCells(OverlapBuffer* e)
{
    uint32_t cap = e->cellCap != 0 ? e->cellCap * 2u : 1024u;
    if (cap < e->cellCap || cap > OVERLAP_MAX_CELLS_TABLE) return 1;
    OverlapCell* cells = (OverlapCell*)calloc(cap, sizeof *cells);
    if (cells == NULL) return 1;
    for (uint32_t i = 0; i < e->cellCap; i++)
    {
        OverlapCell* old = &e->cells[i];
        if (old->count == 0) continue;
        OverlapCell* slot = OverlapSlot(cells, cap, old->x, old->y);
        if (slot == NULL) { free(cells); return 1; }
        *slot = *old;
    }
    free(e->cells);
    e->cells = cells;
    e->cellCap = cap;
    return 0;
}

/* A cell, created when asked for; NULL (and e->failed) when the table or its growth cannot be made. */
static OverlapCell* OverlapFindCell(OverlapBuffer* e, int32_t x, int32_t y, int create)
{
    if (e->cells == NULL)
    {
        if (!create) return NULL;
        if (OverlapGrowCells(e)) { e->failed = 1; return NULL; }
    }
    else if (create && (uint64_t)(e->cellUsed + 1u) * 4u >= (uint64_t)e->cellCap * 3u)
    {
        if (OverlapGrowCells(e)) { e->failed = 1; return NULL; }
    }
    OverlapCell* c = OverlapSlot(e->cells, e->cellCap, x, y);
    if (c == NULL) { e->failed = 1; return NULL; }
    if (c->count == 0 && create)
    {
        c->x = x;
        c->y = y;
        e->cellUsed++;
    }
    return c;
}

static void OverlapFreeEntry(OverlapBuffer* e)
{
    if (e->cells != NULL)
    {
        for (uint32_t i = 0; i < e->cellCap; i++)
            if (e->cells[i].count != 0) free(e->cells[i].list);
        free(e->cells);
    }
    free(e->always);
    free(e->boxes);
    e->boxes = NULL;
    e->boxCap = 0;
    e->cells = NULL;
    e->cellCap = 0;
    e->cellUsed = 0;
    e->members = 0;
    e->always = NULL;
    e->alwaysCount = 0;
    e->alwaysCap = 0;
    e->built = 0;
    e->failed = 0;
}

static void OverlapResetEntry(OverlapBuffer* e, uint32_t generation)
{
    OverlapFreeEntry(e);
    e->generation = generation;
}

static int OverlapAlwaysAdd(OverlapBuffer* e, uint32_t index)
{
    if (e->alwaysCount == e->alwaysCap)
    {
        uint32_t cap = e->alwaysCap != 0 ? e->alwaysCap * 2u : 64u;
        if (cap < e->alwaysCap || cap > OVERLAP_MAX_ALWAYS) return 1;
        uint32_t* grown = (uint32_t*)realloc(e->always, (size_t)cap * sizeof *grown);
        if (grown == NULL) return 1;
        e->always = grown;
        e->alwaysCap = cap;
    }
    e->always[e->alwaysCount++] = index;
    return 0;
}

static int OverlapAddRecord(OverlapBuffer* e, uintptr_t base, uint32_t index)
{
    float box[4];
    memcpy(box, (const void*)(uintptr_t)(base + 8u + (uint64_t)index * 0x130u + 0xB0u), sizeof box);
    if (index >= e->boxCap)
    {
        uint32_t cap = e->boxCap != 0 ? e->boxCap * 2u : 1024u;
        while (cap <= index) cap *= 2u;
        float* grown = (float*)realloc(e->boxes, (size_t)cap * 4u * sizeof *grown);
        if (grown == NULL) return 1;
        e->boxes = grown;
        e->boxCap = cap;
    }
    memcpy(e->boxes + (size_t)index * 4u, box, sizeof box);
    float x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        fabsf(x0) > OVERLAP_LIMIT || fabsf(y0) > OVERLAP_LIMIT || fabsf(x1) > OVERLAP_LIMIT || fabsf(y1) > OVERLAP_LIMIT)
        return OverlapAlwaysAdd(e, index);
    float xmin = x0 < x1 ? x0 : x1;
    float xmax = x0 < x1 ? x1 : x0;
    float ymin = y0 < y1 ? y0 : y1;
    float ymax = y0 < y1 ? y1 : y0;
    int32_t cx0 = OverlapCellCoord((double)xmin - (double)OVERLAP_MARGIN);
    int32_t cx1 = OverlapCellCoord((double)xmax + (double)OVERLAP_MARGIN);
    int32_t cy0 = OverlapCellCoord((double)ymin - (double)OVERLAP_MARGIN);
    int32_t cy1 = OverlapCellCoord((double)ymax + (double)OVERLAP_MARGIN);
    int64_t nx = (int64_t)cx1 - (int64_t)cx0 + 1;
    int64_t ny = (int64_t)cy1 - (int64_t)cy0 + 1;
    if (nx <= 0 || ny <= 0 || nx > (int64_t)OVERLAP_MAX_RECORD_CELLS || ny > (int64_t)OVERLAP_MAX_RECORD_CELLS ||
        nx * ny > (int64_t)OVERLAP_MAX_RECORD_CELLS)
        return OverlapAlwaysAdd(e, index);
    for (int32_t cy = cy0; cy <= cy1; cy++)
    {
        for (int32_t cx = cx0; cx <= cx1; cx++)
        {
            OverlapCell* c = OverlapFindCell(e, cx, cy, 1);
            if (c == NULL) return 1;
            if (c->count == c->cap)
            {
                uint32_t cap = c->cap != 0 ? c->cap * 2u : 4u;
                if (cap < c->cap || cap > OVERLAP_MAX_CELL_LIST) return 1;
                uint32_t* grown = (uint32_t*)realloc(c->list, (size_t)cap * sizeof *grown);
                if (grown == NULL) return 1;
                c->list = grown;
                c->cap = cap;
            }
            c->list[c->count++] = index;
            e->members++;
            if (e->members > OVERLAP_MAX_MEMBERS) return 1;
        }
    }
    return 0;
}

static int OverlapCompare(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int OverlapScratchAdd(OverlapThread* t, uint32_t index)
{
    if (t->scratchCount == t->scratchCap)
    {
        uint32_t cap = t->scratchCap != 0 ? t->scratchCap * 2u : 256u;
        if (cap < t->scratchCap || cap > OVERLAP_MAX_SCRATCH) return 1;
        uint32_t* grown = (uint32_t*)realloc(t->scratch, (size_t)cap * sizeof *grown);
        if (grown == NULL) return 1;
        t->scratch = grown;
        t->scratchCap = cap;
    }
    t->scratch[t->scratchCount++] = index;
    return 0;
}

static int OverlapListSet(OverlapThread* t, const uint32_t* values, uint32_t count)
{
    if (count > t->listCap)
    {
        uint32_t cap = t->listCap != 0 ? t->listCap : 256u;
        while (cap < count)
        {
            if (cap > OVERLAP_MAX_SCRATCH / 2u) { cap = count; break; }
            cap *= 2u;
        }
        uint32_t* grown = (uint32_t*)realloc(t->list, (size_t)cap * sizeof *grown);
        if (grown == NULL) return 1;
        t->list = grown;
        t->listCap = cap;
    }
    if (count != 0) memcpy(t->list, values, (size_t)count * sizeof *values);
    t->listCount = count;
    return 0;
}

/* The candidate list: the always list plus every record in the cells the query's interval plus the margin covers,
 * sorted and deduplicated. Returns 1 when the query is too wide or an allocation failed (the loop falls back). */
static int OverlapBuildList(OverlapBuffer* e, uint32_t count, const float* q, OverlapThread* t)
{
    double cx = (double)q[0], cy = (double)q[1];
    double hx = fabs((double)q[2]), hy = fabs((double)q[3]);
    double margin = (double)OVERLAP_MARGIN;
    int32_t x0 = OverlapCellCoord(cx - hx - margin);
    int32_t x1 = OverlapCellCoord(cx + hx + margin);
    int32_t y0 = OverlapCellCoord(cy - hy - margin);
    int32_t y1 = OverlapCellCoord(cy + hy + margin);
    int64_t nx = (int64_t)x1 - (int64_t)x0 + 1;
    int64_t ny = (int64_t)y1 - (int64_t)y0 + 1;
    if (nx <= 0 || ny <= 0 || nx > (int64_t)OVERLAP_MAX_QUERY_CELLS || ny > (int64_t)OVERLAP_MAX_QUERY_CELLS ||
        nx * ny > (int64_t)OVERLAP_MAX_QUERY_CELLS)
        return 1;
    t->scratchCount = 0;
    for (uint32_t i = 0; i < e->alwaysCount; i++)
    {
        uint32_t index = e->always[i];
        if (index < count && OverlapScratchAdd(t, index)) return 1;
    }
    if (e->cells != NULL)
    {
        for (int32_t cy = y0; cy <= y1; cy++)
        {
            for (int32_t cx = x0; cx <= x1; cx++)
            {
                OverlapCell* c = OverlapFindCell(e, cx, cy, 0);
                if (e->failed) return 1;
                if (c == NULL || c->count == 0) continue;
                for (uint32_t k = 0; k < c->count; k++)
                {
                    uint32_t index = c->list[k];
                    if (index < count && OverlapScratchAdd(t, index)) return 1;
                }
            }
        }
    }
    if (t->scratchCount > 1) qsort(t->scratch, t->scratchCount, sizeof t->scratch[0], OverlapCompare);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < t->scratchCount; i++)
        if (i == 0 || t->scratch[i] != t->scratch[i - 1]) t->scratch[unique++] = t->scratch[i];
    return OverlapListSet(t, t->scratch, unique);
}

static int OverlapListHas(const uint32_t* list, uint32_t count, uint32_t value)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi)
    {
        uint32_t at = lo + (hi - lo) / 2u;
        if (list[at] < value) lo = at + 1u;
        else if (list[at] > value) hi = at;
        else return 1;
    }
    return 0;
}

/* Check mode: every record the original's box test passes must be in the list. It reads the same record boxes the loop
 * body reads, so it also exercises the record layout this file assumes. */
static void OverlapCheck(OverlapBuffer* e, uintptr_t base, uint32_t count, const float* q, const OverlapThread* t)
{
    for (uint32_t i = 0; i < count; i++)
    {
        float box[4];
        memcpy(box, (const void*)(uintptr_t)(base + 8u + (uint64_t)i * 0x130u + 0xB0u), sizeof box);
        if (i < e->built && memcmp(box, e->boxes + (size_t)i * 4u, sizeof box) != 0)
        {
            /* A record below the count changed after it was indexed: this buffer is not append-only; stop using it. */
            e->failed = 1;
            LONG64 n = InterlockedIncrement64(&g_overlapMisses);
            if (n <= 4)
                Note("difficulty: extra large maps: the stamp overlap grid found record %u of %u rewritten after it was indexed; that buffer goes back to the original loop",
                    (unsigned)i, (unsigned)count);
        }
        if (!OverlapBoxPass(q[0], q[1], q[2], q[3], box)) continue;
        if (OverlapListHas(t->list, t->listCount, i)) continue;
        LONG64 n = InterlockedIncrement64(&g_overlapMisses);
        if (n <= 4)
            Note("difficulty: extra large maps: the stamp overlap grid missed record %u of %u (query centre %.3f, %.3f half %.3f, %.3f; box %.3f, %.3f .. %.3f, %.3f)",
                (unsigned)i, (unsigned)count, (double)q[0], (double)q[1], (double)q[2], (double)q[3],
                (double)box[0], (double)box[1], (double)box[2], (double)box[3]);
    }
}

static OverlapThread* OverlapThreadGet(void)
{
    if (g_overlap.tls == TLS_OUT_OF_INDEXES) return NULL;
    OverlapThread* t = (OverlapThread*)TlsGetValue(g_overlap.tls);
    if (t != NULL) return t;
    t = (OverlapThread*)calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    if (!TlsSetValue(g_overlap.tls, t)) { free(t); return NULL; }
    return t;
}

/* The research counters, kept per thread and added to the shared ones every OVERLAP_STATS_BATCH calls (eight jobs
 * writing shared counters on every call would fight over their cache line); a note can miss a thread's last batch. */
static void OverlapStatsAdd(OverlapThread* t)
{
    InterlockedExchangeAdd64(&g_overlapCalls, (LONG64)t->calls);
    InterlockedExchangeAdd64(&g_overlapListSum, (LONG64)t->listSum);
    InterlockedExchangeAdd64(&g_overlapCountSum, (LONG64)t->countSum);
    LONG64 all = 0;
    for (int i = 0; i < 4; i++) { InterlockedExchangeAdd64(&g_overlapFallbackWhy[i], (LONG64)t->fallbacks[i]); all += (LONG64)t->fallbacks[i]; }
    InterlockedExchangeAdd64(&g_overlapFallbacks, all);
    t->calls = t->listSum = t->countSum = 0;
    memset(t->fallbacks, 0, sizeof t->fallbacks);
}
static void OverlapStats(OverlapThread* t, uint32_t list, uint32_t count, int why)
{
    t->calls++;
    t->listSum += list;
    t->countSum += count;
    if (why >= 0) t->fallbacks[why]++;
    if (t->calls >= OVERLAP_STATS_BATCH) OverlapStatsAdd(t);
}

/* After op 0x2B returns (difficulty_world.c's dispatcher): the run's counters, noted now rather than at the next run. */
static void OverlapFlush(void)
{
    AcquireSRWLockExclusive(&g_overlapStatsLock);
    LONG64 calls = g_overlapCalls;
    if (calls > 0)
        Note("difficulty: extra large maps: the stamp overlap grid for pass run %ld: ~%lld calls, average list %.1f, average count %.1f, %lld fallbacks (range %lld, no slot %lld, slot lost %lld, grid %lld), %lld checks, %lld check misses",
            (long)g_worldOpRun, (long long)calls, (double)g_overlapListSum / (double)calls,
            (double)g_overlapCountSum / (double)calls, (long long)g_overlapFallbacks, (long long)g_overlapFallbackWhy[0],
            (long long)g_overlapFallbackWhy[1], (long long)g_overlapFallbackWhy[2], (long long)g_overlapFallbackWhy[3],
            (long long)g_overlapChecks, (long long)g_overlapMisses);
    else Note("difficulty: extra large maps: the stamp overlap grid saw no calls in this op 0x2B run");
    g_overlapCalls = 0;
    g_overlapListSum = 0;
    g_overlapCountSum = 0;
    g_overlapFallbacks = 0;
    g_overlapMisses = 0;
    g_overlapChecks = 0;
    for (int i = 0; i < 4; i++) g_overlapFallbackWhy[i] = 0;
    ReleaseSRWLockExclusive(&g_overlapStatsLock);
}

/* All the buffers' grids belong to one level generation; a new generation frees them (the old jobs have finished). */
static void OverlapTableForGeneration(LONG generation)
{
    if (generation == g_overlapTableGeneration) return;
    AcquireSRWLockExclusive(&g_overlapTableLock);
    if (generation != g_overlapTableGeneration)
    {
        for (uint32_t i = 0; i < OVERLAP_BUFFERS; i++)
        {
            OverlapBuffer* e = &g_overlapBuffers[i];
            AcquireSRWLockExclusive(&e->lock);
            OverlapFreeEntry(e);
            e->used = 0;
            e->base = 0;
            e->generation = 0;
            ReleaseSRWLockExclusive(&e->lock);
        }
        g_overlapTableGeneration = generation;
    }
    ReleaseSRWLockExclusive(&g_overlapTableLock);
}

static OverlapBuffer* OverlapBufferFor(uintptr_t base, LONG generation)
{
    OverlapTableForGeneration(generation);
    OverlapBuffer* found = NULL;
    AcquireSRWLockExclusive(&g_overlapTableLock);
    for (uint32_t i = 0; i < OVERLAP_BUFFERS && found == NULL; i++)
        if (g_overlapBuffers[i].used && g_overlapBuffers[i].base == base) found = &g_overlapBuffers[i];
    if (found == NULL)
    {
        for (uint32_t i = 0; i < OVERLAP_BUFFERS; i++)
        {
            OverlapBuffer* e = &g_overlapBuffers[i];
            if (e->used) continue;
            e->used = 1;
            e->base = base;
            e->generation = (uint32_t)generation;
            e->built = 0;
            e->failed = 0;
            found = e;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_overlapTableLock);
    return found;
}

/* The stub at 0xF31004: rcx = r14 (base), edx = r15d (count), r8 = the query (centre x, centre y, half x, half y), r9 =
 * rbp (the call's frame). Returns the first candidate index, -1 for none, or -2 for the original loop. Only the job's own
 * slice (callers 0xF33FA1, 0xF3463D) takes the grid: its records are appended and never rewritten below the count.
 * 0xF314A0's batches (0xF3175F) start again at the same addresses when one is discarded, so they keep the original loop. */
static int OverlapBegin(uintptr_t base, uint32_t count, const float* q, uintptr_t frame)
{
    OverlapThread* t = OverlapThreadGet();
    if (t == NULL) return -2;
    t->active = 0;
    if (g_overlap.installed == 0) return -2;
    uintptr_t caller = *(const uintptr_t*)(frame + OVERLAP_FRAME_RETURN);
    if (caller != g_overlap.game + OVERLAP_CALLER_A && caller != g_overlap.game + OVERLAP_CALLER_B) return -2;
    if (!(g_world.current > 1.0f)) { if (g_overlap.level != 0) g_overlap.level = 0; return -2; }
    if (g_overlap.level != 1) g_overlap.level = 1; /* written only on a change: eight jobs share its cache line */
    LONG generation = g_worldOpRun; /* one run of one pass record (difficulty_world.c) */
    if (base == 0 || count == 0 || count > OVERLAP_MAX_COUNT || q == NULL ||
        !isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3]) ||
        fabsf(q[0]) > OVERLAP_LIMIT || fabsf(q[1]) > OVERLAP_LIMIT || fabsf(q[2]) > OVERLAP_LIMIT || fabsf(q[3]) > OVERLAP_LIMIT)
    {
        static volatile LONG notes;
        if (InterlockedIncrement(&notes) <= 6)
            Note("difficulty: extra large maps: the stamp overlap grid passed a query to the original loop: base 0x%llX count %u, centre %g, %g half %g, %g",
                (unsigned long long)base, (unsigned)count, q ? (double)q[0] : 0.0, q ? (double)q[1] : 0.0, q ? (double)q[2] : 0.0, q ? (double)q[3] : 0.0);
        OverlapStats(t, 0, count, 0);
        return -2;
    }
    OverlapBuffer* e;
    if (t->cacheEntry != NULL && t->cacheGeneration == generation && t->cacheBase == base) e = t->cacheEntry;
    else
    {
        /* Each stamp item starts its own slice at the job's count (0xF326B5), so a thread moves on to a new base per
         * item and never returns to the last one: its entry is freed for the next. */
        if (t->cacheEntry != NULL && t->cacheGeneration == generation)
        {
            OverlapBuffer* old = t->cacheEntry;
            AcquireSRWLockExclusive(&g_overlapTableLock);
            AcquireSRWLockExclusive(&old->lock);
            if (old->used && old->base == t->cacheBase)
            {
                OverlapFreeEntry(old);
                old->used = 0;
                old->base = 0;
            }
            ReleaseSRWLockExclusive(&old->lock);
            ReleaseSRWLockExclusive(&g_overlapTableLock);
        }
        t->cacheEntry = NULL;
        e = OverlapBufferFor(base, generation);
        if (e == NULL)
        {
            OverlapStats(t, 0, count, 1);
            return -2;
        }
        t->cacheEntry = e;
        t->cacheGeneration = generation;
        t->cacheBase = base;
    }
    int failed = 0;
    AcquireSRWLockExclusive(&e->lock); /* uncontended: one job owns a slice */
    if (!e->used || e->base != base) /* a new generation freed the entry between the claim and the lock */
    {
        ReleaseSRWLockExclusive(&e->lock);
        t->cacheEntry = NULL;
        OverlapStats(t, 0, count, 2);
        return -2;
    }
    if (e->generation != (uint32_t)generation || count < e->built) OverlapResetEntry(e, (uint32_t)generation);
    if (!e->failed && e->built < count)
    {
        for (uint32_t i = e->built; i < count; i++)
        {
            if (OverlapAddRecord(e, base, i)) { e->failed = 1; break; }
            e->built = i + 1u;
        }
    }
    if (e->failed) failed = 1;
    else if (OverlapBuildList(e, count, q, t)) failed = 1;
    ReleaseSRWLockExclusive(&e->lock);
    OverlapStats(t, failed ? 0u : t->listCount, count, failed ? 3 : -1);
    if (failed) return -2;
    if (count <= OVERLAP_CHECK_LIMIT && ++t->sinceCheck >= OVERLAP_CHECK_EVERY)
    {
        t->sinceCheck = 0;
        InterlockedIncrement64(&g_overlapChecks);
        AcquireSRWLockExclusive(&e->lock);
        OverlapCheck(e, base, count, q, t);
        int rewritten = e->failed;
        ReleaseSRWLockExclusive(&e->lock);
        if (rewritten) return -2;
    }
    t->frame = frame;
    t->active = 1;
    t->pos = 0;
    if (t->listCount == 0) return -1;
    t->pos = 1;
    return (int)t->list[0];
}

/* The stub at 0xF31130 (rcx = rbp): the next candidate, -1 at the end, -2 for the original stepping (also when the list
 * is another call's: stepping on from the current index visits a superset of the list). */
static int OverlapNext(uintptr_t frame)
{
    if (g_overlap.installed == 0) return -2;
    OverlapThread* t = OverlapThreadGet();
    if (t == NULL || !t->active || t->frame != frame) return -2;
    if (t->pos >= t->listCount) { t->active = 0; return -1; }
    return (int)t->list[t->pos++];
}

/* The loop entry's stub. It is jumped to from 0xF31004 (the two NOPs before the loop); its C call is made with rsp
 * still 16-byte aligned (0xF30B60's dynamic frame leaves it so, and 11 pushes + 0xa8 is a multiple of 16). The saved rax
 * is discarded rather than restored: it is dead at 0xF31010 and holds the C result. */
static const uint8_t kOverlapEnterStub[] = {
    STUB_PUSH_VOLATILE,                                               /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0xa8 */
    STUB_SAVE_XMM,                                                    /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0xF3, 0x0F, 0x11, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00,             /*  54 movss [rsp+0x80], xmm6 (query centre x) */
    0xF3, 0x44, 0x0F, 0x11, 0x84, 0x24, 0x84, 0x00, 0x00, 0x00,       /*  63 movss [rsp+0x84], xmm8 (query centre y) */
    0xF3, 0x44, 0x0F, 0x11, 0xBC, 0x24, 0x88, 0x00, 0x00, 0x00,       /*  73 movss [rsp+0x88], xmm15 (query half x) */
    0xF3, 0x44, 0x0F, 0x11, 0xAC, 0x24, 0x8C, 0x00, 0x00, 0x00,       /*  83 movss [rsp+0x8c], xmm13 (query half y) */
    0x4C, 0x89, 0xF1,                                                 /*  93 mov rcx, r14 (the slice's base) */
    0x44, 0x89, 0xFA,                                                 /*  96 mov edx, r15d (the record count) */
    0x4C, 0x8D, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00,                   /*  99 lea r8, [rsp+0x80] (the query) */
    0x49, 0x89, 0xE9,                                                 /* 107 mov r9, rbp (the call's frame) */
    STUB_CALL_IMM64,                                                  /* 110 mov rax, OverlapBegin (imm64 at 112); call rax */
    STUB_LOAD_XMM,                                                    /* 122 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00,                         /* 158 add rsp, 0xa8 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,       /* 165 pop r11 r10 r9 r8 rdx rcx (rax stays) */
    0x48, 0x83, 0xC4, 0x08,                                           /* 175 add rsp, 8 (discard the saved rax) */
    0x85, 0xC0,                                                       /* 179 test eax, eax */
    0x78, 0x07,                                                       /* 181 js 190 (a negative answer) */
    0x89, 0xC7,                                                       /* 183 mov edi, eax (the first index) */
    0xE9, 0, 0, 0, 0,                                                 /* 185 jmp 0xf31010 (rel32 at 186) */
    0x83, 0xF8, 0xFF,                                                 /* 190 cmp eax, -1 */
    0x0F, 0x84, 0, 0, 0, 0,                                           /* 193 je 0xf311f8 (rel32 at 195) */
    0x33, 0xFF,                                                       /* 199 xor edi, edi */
    0xE9, 0, 0, 0, 0};                                                /* 201 jmp 0xf31010 (rel32 at 202) */
_Static_assert(sizeof kOverlapEnterStub == 206, "the loop entry stub's offsets");

/* The index step's stub. The first check is the level flag (a normal level never enters the C path); the rest is the same
 * call/preserve sequence as the entry stub, with rbp (the call's frame) in rcx. rax is not restored: it is dead at
 * 0xF3113B/0xF31010 and holds OverlapNext's answer. edi is left untouched by the C call (non-volatile), which is what the
 * fallback stepping increments. */
static const uint8_t kOverlapNextStub[] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*   0 mov rax, &g_overlap.level (imm64 at 2) */
    0x83, 0x38, 0x00,                                                 /*  10 cmp dword [rax], 0 */
    0x0F, 0x84, 0, 0, 0, 0,                                           /*  13 je 165 (the original stepping; rel32 at 15) */
    STUB_PUSH_VOLATILE,                                               /*  19 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,                         /*  30 sub rsp, 0xa8 */
    STUB_SAVE_XMM,                                                    /*  37 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0x48, 0x89, 0xE9,                                                 /*  73 mov rcx, rbp (the call's frame) */
    STUB_CALL_IMM64,                                                  /*  76 mov rax, OverlapNext (imm64 at 78); call rax */
    STUB_LOAD_XMM,                                                    /*  88 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00,                         /* 124 add rsp, 0xa8 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,       /* 131 pop r11 r10 r9 r8 rdx rcx (rax stays) */
    0x48, 0x83, 0xC4, 0x08,                                           /* 141 add rsp, 8 (discard the saved rax) */
    0x85, 0xC0,                                                       /* 145 test eax, eax */
    0x78, 0x07,                                                       /* 147 js 156 (a negative answer) */
    0x89, 0xC7,                                                       /* 149 mov edi, eax (the next index) */
    0xE9, 0, 0, 0, 0,                                                 /* 151 jmp 0xf3113b (rel32 at 152) */
    0x83, 0xF8, 0xFF,                                                 /* 156 cmp eax, -1 */
    0x0F, 0x84, 0, 0, 0, 0,                                           /* 159 je 0xf311f8 (rel32 at 161) */
    0xFF, 0xC7,                                                       /* 165 inc edi (the original stepping) */
    0x41, 0x3B, 0xFF,                                                 /* 167 cmp edi, r15d */
    0x0F, 0x83, 0, 0, 0, 0,                                           /* 170 jae 0xf311f8 (rel32 at 172) */
    0xE9, 0, 0, 0, 0};                                                /* 176 jmp 0xf3113b (rel32 at 177) */
_Static_assert(sizeof kOverlapNextStub == 181, "the index step stub's offsets");

static int32_t OverlapRel(uintptr_t next, uintptr_t target, int* ok)
{
    int64_t distance = (int64_t)target - (int64_t)next;
    if (!InRel32(distance)) { *ok = 0; return 0; }
    return (int32_t)distance;
}

/* Installs the two stubs and their jumps when the reviewed code is there. A refusal is noted and leaves the original
 * loop (the caller does not refuse the level): the pattern is InstallWorldPlan's. */
static const char* InstallWorldOverlap(uintptr_t game)
{
    if (g_overlap.installed != 0) return NULL;
    if (game == 0) return "game.dll is not loaded";
    char off[8];
    if (GetEnvironmentVariableA("HELLFORGE_XL_NO_OVERLAP", off, sizeof off) != 0) return "HELLFORGE_XL_NO_OVERLAP is set (research A/B runs)";
    if (!ApiCodeMatches(game + OVERLAP_FUNCTION, kOverlapFunctionPin) ||
        !ApiCodeMatches(game + OVERLAP_ENTER_SITE, kOverlapEnterPin) ||
        !ApiCodeMatches(game + OVERLAP_LOOP_SITE, kOverlapLoopPin) ||
        !ApiCodeMatches(game + OVERLAP_NEXT_SITE, kOverlapNextPin) ||
        !ApiCodeMatches(game + OVERLAP_BACK_SITE, kOverlapBackPin) ||
        !ApiCodeMatches(game + OVERLAP_COUNT_SITE, kOverlapCountPin) ||
        !ApiCodeMatches(game + OVERLAP_QUERY_SITE, kOverlapQueryPin) ||
        !ApiCodeMatches(game + OVERLAP_QUERY_2_SITE, kOverlapQuery2Pin) ||
        !ApiCodeMatches(game + OVERLAP_TEST_A_SITE, kOverlapTestAPin) ||
        !ApiCodeMatches(game + OVERLAP_TEST_B_SITE, kOverlapTestBPin) ||
        !ApiCodeMatches(game + OVERLAP_BOX_A_SITE, kOverlapBoxAPin) ||
        !ApiCodeMatches(game + OVERLAP_BOX_B_SITE, kOverlapBoxBPin))
        return "the stamp placement's overlap test is not the reviewed one (an extra large map needs it)";
    uint8_t* block = AllocateNearGame(game, 0x1000);
    if (block == NULL) return "no memory within 2 GB of game.dll for the stamp overlap grid";
    DWORD tls = TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES) { VirtualFree(block, 0, MEM_RELEASE); return "no TLS slot for the stamp overlap grid"; }
    uint8_t* enter = block;
    uint8_t* next = block + 0x400;
    memcpy(enter, kOverlapEnterStub, sizeof kOverlapEnterStub);
    memcpy(next, kOverlapNextStub, sizeof kOverlapNextStub);
    uint64_t begin = (uint64_t)(uintptr_t)&OverlapBegin;
    uint64_t step = (uint64_t)(uintptr_t)&OverlapNext;
    uint64_t level = (uint64_t)(uintptr_t)&g_overlap.level;
    memcpy(enter + 112, &begin, 8);
    memcpy(next + 2, &level, 8);
    memcpy(next + 78, &step, 8);
    int ok = 1;
    int32_t rel;
    rel = OverlapRel((uintptr_t)(enter + 190), game + OVERLAP_LOOP_SITE, &ok); memcpy(enter + 186, &rel, 4);
    rel = OverlapRel((uintptr_t)(enter + 199), game + OVERLAP_END_SITE, &ok); memcpy(enter + 195, &rel, 4);
    rel = OverlapRel((uintptr_t)(enter + 206), game + OVERLAP_LOOP_SITE, &ok); memcpy(enter + 202, &rel, 4);
    rel = OverlapRel((uintptr_t)(next + 19), (uintptr_t)(next + 165), &ok); memcpy(next + 15, &rel, 4);
    rel = OverlapRel((uintptr_t)(next + 156), game + OVERLAP_BACK_SITE, &ok); memcpy(next + 152, &rel, 4);
    rel = OverlapRel((uintptr_t)(next + 165), game + OVERLAP_END_SITE, &ok); memcpy(next + 161, &rel, 4);
    rel = OverlapRel((uintptr_t)(next + 176), game + OVERLAP_END_SITE, &ok); memcpy(next + 172, &rel, 4);
    rel = OverlapRel((uintptr_t)(next + 181), game + OVERLAP_BACK_SITE, &ok); memcpy(next + 177, &rel, 4);
    if (!ok) { TlsFree(tls); VirtualFree(block, 0, MEM_RELEASE); return "the stamp overlap grid's stubs are out of reach of game.dll"; }
    DWORD previous;
    if (!VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &previous))
    {
        TlsFree(tls);
        VirtualFree(block, 0, MEM_RELEASE);
        return "the stamp overlap grid's stub page could not be made executable";
    }
    FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t enterJump[12] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + OVERLAP_ENTER_SITE, (uintptr_t)enter);
    memcpy(enterJump + 1, &rel, 4);
    uint8_t nextJump[5] = {0xE9, 0, 0, 0, 0};
    rel = DifficultyJumpRel(game + OVERLAP_NEXT_SITE, (uintptr_t)next);
    memcpy(nextJump + 1, &rel, 4);
    if (ApiWrite(game + OVERLAP_ENTER_SITE, enterJump, sizeof enterJump))
    {
        TlsFree(tls);
        VirtualFree(block, 0, MEM_RELEASE);
        return "the stamp overlap grid's loop entry could not be installed";
    }
    /* From here the entry stub may run; a failure leaves it calling OverlapBegin, which falls back while installed is 0. */
    if (ApiWrite(game + OVERLAP_NEXT_SITE, nextJump, sizeof nextJump))
        return "the stamp overlap grid's index step could not be installed";
    g_overlap.tls = tls;
    g_overlap.game = game;
    g_overlap.level = 0;
    g_overlapTableGeneration = 0;
    g_overlap.installed = 1;
    Note("difficulty: extra large maps: the stamp overlap grid is installed (%.0f m cells, %.1f m margin; the box test is checked against the original up to %u records)",
        (double)OVERLAP_CELL, (double)OVERLAP_MARGIN, (unsigned)OVERLAP_CHECK_LIMIT);
    return NULL;
}
