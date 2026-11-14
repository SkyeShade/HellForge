/* The overlap grid (perf_overlap_grid.c) against the game's own code: two synthetic game.dll images hold the overlap
 * test 0xF30B60 as build 25480438 has it, its callees replaced by C that logs every record the loop hands on. One
 * image keeps the game's loop, the other gets the fix installed. Every query runs on both, on the same records, and
 * the logs and results must match byte for byte: random boxes, boxes that touch, zero, inverted, non-finite and far
 * boxes, new items, a new job on the same memory, an overrun, and a list too long. Included by stats_test.c. */

#include "overlap_grid_code.h"

#define OGT_IMAGE 0x2700000u
#define OGT_HARNESS 0x1000000u /* the region harness: calls the memset site as the job does */
#define OGT_CAP 4096u          /* records in the test's region */
#define OGT_LOG 8192u

typedef void* (*OgtFn)(void* out, void* a2, void* a3, void* ctx, uintptr_t base, uint64_t count, void* a7, void* a8, void* a9,
    float a10, uint64_t a11, uint64_t a12);
typedef void (*OgtRegionFn)(void* start, size_t size);

static uintptr_t g_ogtLog[OGT_LOG];
static uint32_t g_ogtLogCount;
static float g_ogtCorners[4]; /* the candidate's two corners, as 0xFC9440 makes them */
static uint64_t g_ogtResource;
static uint8_t g_ogtContext[0x2100];

static void OgtLog(uintptr_t value)
{
    if (g_ogtLogCount < OGT_LOG) g_ogtLog[g_ogtLogCount] = value;
    g_ogtLogCount++;
}

static void OgtCorners(void* a, void* b, float c, float* first, float* second)
{
    memcpy(first, g_ogtCorners, 8);
    memcpy(second, g_ogtCorners + 2, 8);
}
static uint32_t OgtCell(void* g, float x, float y) { return 0; }
static uint32_t OgtHits(void* g, uint32_t a, uint32_t b, void* buffer, uint32_t cap) { return 0; } /* the first loop stays empty */
static void OgtLimits(void* rule, void* record, uint32_t* a, float* b)
{
    OgtLog((uintptr_t)record);
    *a = 0;
    *b = 1.1920929e-7f;
}
/* 0xFCBC20: the record's score (its float at +8, the loop's sixth argument) decides it. */
static float OgtScore(void* a, void* b, float c, void* d, void* record, float score, uint64_t* out)
{
    OgtLog((uintptr_t)record | 1);
    *out = (uintptr_t)record * 3;
    return score;
}

static void OgtThunk(uint8_t* image, uint32_t rva, const void* to)
{
    uint8_t* c = image + rva;
    c[0] = 0x48; c[1] = 0xb8;
    uint64_t v = (uint64_t)(uintptr_t)to;
    memcpy(c + 2, &v, 8);
    c[10] = 0xff; c[11] = 0xe0; /* jmp rax */
}

static uint8_t* OgtImage(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, OGT_IMAGE, MEM_RESERVE, PAGE_NOACCESS);
    assert(image != NULL);
    static const uint32_t pages[][2] = {{0xC04000u, 2}, {0xF30000u, 5}, {0xFC9000u, 1}, {0xFCB000u, 1}, {OGT_HARNESS, 1},
        {0x2088000u, 1}, {0x208A000u, 1}, {0x2104000u, 1}, {0x23C6000u, 6}, {0x263C000u, 1}};
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(VirtualAlloc(image + pages[i][0], pages[i][1] * 0x1000u, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    memcpy(image + OG_FUNCTION, kOgtFunction, sizeof kOgtFunction);
    memcpy(image + 0x21045C0u, kOgtChkstk, sizeof kOgtChkstk);
    memcpy(image + OG_MEMSET_AT, kOgMemsetPin, sizeof kOgMemsetPin);
    image[OG_MEMSET_AT + sizeof kOgMemsetPin] = 0xc3; /* stands for the rest of the job: back to the harness */
    for (size_t i = 0; i < OG_WRITERS; i++) memcpy(image + kOgWriters[i].rva, kOgWriters[i].pin, kOgWriters[i].size);
    OgtThunk(image, 0xFC9440u, (const void*)OgtCorners);
    OgtThunk(image, 0xC04860u, (const void*)OgtCell);
    OgtThunk(image, 0xC05360u, (const void*)OgtHits);
    OgtThunk(image, 0xF30A20u, (const void*)OgtLimits);
    OgtThunk(image, 0xFCBC20u, (const void*)OgtScore);
    OgtThunk(image, OG_MEMSET, (const void*)memset);
    image[0x2088770u] = 0xc3; /* the cookie check */
    const float one = 1.0f, half = 0.5f, epsilon = 1.1920929e-7f, fifty5 = 0.55f, hundredth = 0.01f;
    const double halfD = 0.5;
    const uint64_t mask[2] = {0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull};
    memcpy(image + 0x23C6D70u, &one, 4);
    memcpy(image + 0x23CBFB0u, mask, 16);
    memcpy(image + 0x23C6AD0u, &half, 4);
    memcpy(image + 0x23C6550u, &epsilon, 4);
    memcpy(image + 0x23C6B08u, &fifty5, 4);
    memcpy(image + 0x23C6630u, &hundredth, 4);
    memcpy(image + 0x23C6FC8u, &halfD, 8);
    /* the harness: rcx the region, rdx its size; called from C, so rsp is 16-aligned at the site as in the job */
    static const uint8_t harness[] = {0x49,0x89,0xd0, 0x31,0xd2, 0xe8,0,0,0,0, 0xc3};
    memcpy(image + OGT_HARNESS, harness, sizeof harness);
    int32_t rel = (int32_t)(OG_MEMSET_CALL - (OGT_HARNESS + 10));
    memcpy(image + OGT_HARNESS + 6, &rel, 4);
    return image;
}

typedef struct OgtRun
{
    uintptr_t log[OGT_LOG];
    uint32_t logCount;
    uint8_t out[0x20];
} OgtRun;

static void OgtQuery(uint8_t* image, uintptr_t base, uint32_t count, OgtRun* run)
{
    g_ogtLogCount = 0;
    memset(run->out, 0xcc, sizeof run->out);
    ((OgtFn)(void*)(image + OG_FUNCTION))(run->out, NULL, NULL, g_ogtContext, base, count, NULL, NULL, NULL, 1.0f, 1, 0);
    run->logCount = g_ogtLogCount;
    memcpy(run->log, g_ogtLog, sizeof(uintptr_t) * (g_ogtLogCount < OGT_LOG ? g_ogtLogCount : OGT_LOG));
}

static uint64_t g_ogtSeed = 0x9E3779B97F4A7C15ull;
static uint32_t OgtRand(void)
{
    g_ogtSeed ^= g_ogtSeed << 13; g_ogtSeed ^= g_ogtSeed >> 7; g_ogtSeed ^= g_ogtSeed << 17;
    return (uint32_t)(g_ogtSeed >> 16);
}
static float OgtUnit(void) { return (float)(OgtRand() & 0xFFFFFF) / 16777216.0f; }

/* A box edge pair: mostly ordinary stamps, some on cell lines, inverted, zero, huge, non-finite or far. */
static void OgtBox(float* box, int edgeCases)
{
    float cx = OgtUnit() * 2048.0f, cy = OgtUnit() * 2048.0f;
    float hx = OgtUnit() * 40.0f, hy = OgtUnit() * 40.0f;
    uint32_t kind = OgtRand() % 32;
    if (kind == 0) { hx = 200.0f + OgtUnit() * 1500.0f; hy = 200.0f + OgtUnit() * 900.0f; }
    if (kind == 1) { hx = 0.0f; hy = 0.0f; }
    if (kind == 2) { cx = (float)(OgtRand() % 33) * 64.0f; cy = (float)(OgtRand() % 33) * 64.0f; hx = (float)(OgtRand() % 3) * 32.0f; }
    if (kind == 3) { cx = floorf(cx); cy = floorf(cy); hx = floorf(hx); hy = floorf(hy); } /* exact: touching boxes tie */
    box[0] = cx - hx; box[1] = cy - hy; box[2] = cx + hx; box[3] = cy + hy;
    if (kind == 4) { float t = box[0]; box[0] = box[2]; box[2] = t; }
    if (!edgeCases) return;
    static const float odd[] = {NAN, INFINITY, -INFINITY, 3.0e7f, -3.0e7f, 1.0e-40f, 1048577.0f};
    if (kind == 5 || kind == 6) box[OgtRand() % 4] = odd[OgtRand() % (sizeof odd / sizeof odd[0])];
}

/* A record at index i of the item at base: its box, its resource and its score. */
static void OgtRecord(uintptr_t base, uint32_t i, const float* box, int accept)
{
    uint8_t* r = (uint8_t*)(base + (uintptr_t)i * OG_RECORD);
    memset(r, 0, OG_RECORD);
    uintptr_t resource = (uintptr_t)&g_ogtResource;
    float score = accept ? 1.0f : 0.0f;
    memcpy(r + 0x98, &resource, 8);
    memcpy(r + 0x10, &score, 4);
    memcpy(r + OG_BOX, box, 16);
}

static uint32_t g_ogtQueries, g_ogtFound;

/* One query on both images; they must agree. */
static void OgtCompare(uint8_t* game, uint8_t* fixed, uintptr_t base, uint32_t count)
{
    static OgtRun a, b;
    OgtQuery(game, base, count, &a);
    OgtQuery(fixed, base, count, &b);
    assert(a.logCount == b.logCount && a.logCount < OGT_LOG);
    assert(!memcmp(a.log, b.log, sizeof(uintptr_t) * a.logCount));
    assert(!memcmp(a.out, b.out, sizeof a.out));
    g_ogtQueries++;
    g_ogtFound += a.logCount != 0 && (a.log[a.logCount - 1] & 1);
}

static void OgtCandidate(int edgeCases)
{
    float q[4];
    OgtBox(q, edgeCases);
    if (OgtRand() % 2) memcpy(g_ogtCorners, q, sizeof q); /* corners in either order: half sizes of either sign */
    else { g_ogtCorners[0] = q[2]; g_ogtCorners[1] = q[3]; g_ogtCorners[2] = q[0]; g_ogtCorners[3] = q[1]; }
}

/* A job as 0xF322F0 runs one: its memset, then items, each appending records between candidates. */
static void OgtJob(uint8_t* game, uint8_t* fixed, uint8_t* region, uint32_t items, uint32_t perItem, int edgeCases, uint32_t acceptOneIn)
{
    ((OgtRegionFn)(void*)(fixed + OGT_HARNESS))(region, (size_t)OGT_CAP * OG_RECORD);
    uint32_t total = 0;
    for (uint32_t item = 0; item < items && total < OGT_CAP; item++)
    {
        uintptr_t base = (uintptr_t)region + (uintptr_t)total * OG_RECORD;
        uint32_t count = 0;
        for (uint32_t k = 0; k < perItem && total + count < OGT_CAP; k++)
        {
            float box[4];
            OgtBox(box, edgeCases);
            OgtRecord(base, count++, box, OgtRand() % acceptOneIn == 0);
            if (OgtRand() % 3 == 0) continue; /* records placed with no test between them */
            OgtCandidate(edgeCases);
            OgtCompare(game, fixed, base, count);
        }
        total += count;
    }
}

static void PerfOverlapGridFixture(void)
{
    uint8_t* game = OgtImage();
    uint8_t* fixed = OgtImage();
    uint8_t* region = (uint8_t*)VirtualAlloc(NULL, (size_t)OGT_CAP * OG_RECORD, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(region != NULL);

    /* the pins: a changed byte in the function, a writer or the memset site refuses */
    assert(OverlapGridPinned((uintptr_t)fixed));
    static const uint32_t changed[] = {OG_FUNCTION + 0x2F3u, 0xF341CAu + 13u, OG_MEMSET_AT + 0x11u, OG_STEP + 3u};
    for (size_t i = 0; i < sizeof changed / sizeof changed[0]; i++)
    {
        game[changed[i]] ^= 0x40;
        assert(!OverlapGridPinned((uintptr_t)game));
        game[changed[i]] ^= 0x40;
    }
    assert(OverlapGridPinned((uintptr_t)game));
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "speedtree-empty-cells, overlap-grid");
    assert(!PerfFixOn("overlap-grid"));
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(PerfFixOn("overlap-grid"));

    /* a job with the game's loop on both before the fix is in */
    OgtJob(game, fixed, region, 2, 80, 0, 10);
    assert(OverlapGridInstallAt((uintptr_t)fixed));
    assert(!memcmp(fixed + OG_FUNCTION, kOgtFunction, OG_ENTER - OG_FUNCTION) && fixed[OG_ENTER] == 0xe9 && fixed[OG_STEP] == 0xe9);
    assert(fixed[OG_MEMSET_CALL] == 0xe8 && memcmp(fixed + OG_MEMSET_CALL, game + OG_MEMSET_CALL, 5));

    /* that job's region was never registered: its later items keep the game's loop */
    LONG served = g_ogServed;
    uintptr_t later = (uintptr_t)region + 200u * OG_RECORD;
    for (uint32_t i = 0; i < 100; i++) { float box[4]; OgtBox(box, 0); OgtRecord(later, i, box, 0); }
    OgtCandidate(0);
    OgtCompare(game, fixed, later, 100);
    assert(g_ogServed == served && g_ogPassed > 0);

    /* jobs: ordinary maps, edge cases, dense items, rare and frequent acceptance */
    for (int j = 0; j < 24; j++) OgtJob(game, fixed, region, 1 + OgtRand() % 6, 50 + OgtRand() % 700, j % 3 == 0, j % 2 ? 50 : 4);
    assert(g_ogServed - served > 2000 && g_ogtFound > 100);

    /* a new job on the same memory, same base, other boxes: the old grid must not answer */
    ((OgtRegionFn)(void*)(fixed + OGT_HARNESS))(region, (size_t)OGT_CAP * OG_RECORD);
    for (uint32_t i = 0; i < 300; i++) { float box[4]; OgtBox(box, 0); OgtRecord((uintptr_t)region, i, box, i % 7 == 0); }
    for (int k = 0; k < 200; k++) { OgtCandidate(0); OgtCompare(game, fixed, (uintptr_t)region, 300); }
    for (uint32_t i = 0; i < 300; i++) { float box[4]; OgtBox(box, 0); OgtRecord((uintptr_t)region, i, box, i % 5 == 0); }
    ((OgtRegionFn)(void*)(fixed + OGT_HARNESS))(region, (size_t)OGT_CAP * OG_RECORD); /* zeroed: rewrite them */
    for (uint32_t i = 0; i < 300; i++) { float box[4]; OgtBox(box, 0); OgtRecord((uintptr_t)region, i, box, i % 5 == 0); }
    for (int k = 0; k < 200; k++) { OgtCandidate(0); OgtCompare(game, fixed, (uintptr_t)region, 300); }
    /* a shorter count than the grid holds starts it again */
    for (int k = 0; k < 50; k++) { OgtCandidate(0); OgtCompare(game, fixed, (uintptr_t)region, 120 + OgtRand() % 180); }

    /* half the records or more as candidates: the game's loop, for this query and the next OG_SKIP ones */
    ((OgtRegionFn)(void*)(fixed + OGT_HARNESS))(region, (size_t)OGT_CAP * OG_RECORD);
    for (uint32_t i = 0; i < 2400; i++)
    {
        float box[4] = {1000.0f - (float)(i % 13), 1000.0f, 1010.0f + (float)(i % 7), 1012.0f};
        OgtRecord((uintptr_t)region, i, box, i == 2300);
    }
    served = g_ogServed;
    float wide[4] = {990.0f, 990.0f, 1020.0f, 1020.0f};
    memcpy(g_ogtCorners, wide, sizeof wide);
    OgtCompare(game, fixed, (uintptr_t)region, 2400);
    assert(g_ogServed == served);
    float away[4] = {5000.0f, 5000.0f, 5010.0f, 5010.0f};
    memcpy(g_ogtCorners, away, sizeof away);
    for (int k = 0; k < OG_SKIP; k++) OgtCompare(game, fixed, (uintptr_t)region, 2400);
    assert(g_ogServed == served);
    OgtCompare(game, fixed, (uintptr_t)region, 2400);
    assert(g_ogServed == served + 1);

    /* an item whose count runs past its job's region: every grid is off until the next job */
    OgtCandidate(0);
    uintptr_t last = (uintptr_t)region + (size_t)(OGT_CAP - 50) * OG_RECORD;
    uint8_t* tail = (uint8_t*)VirtualAlloc(NULL, (size_t)OGT_CAP * OG_RECORD * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(tail != NULL);
    ((OgtRegionFn)(void*)(fixed + OGT_HARNESS))(tail, (size_t)OGT_CAP * OG_RECORD);
    last = (uintptr_t)tail + (size_t)(OGT_CAP - 50) * OG_RECORD;
    for (uint32_t i = 0; i < 100; i++) { float box[4]; OgtBox(box, 0); OgtRecord(last, i, box, 0); }
    OgtCompare(game, fixed, last, 100);
    served = g_ogServed;
    OgtCompare(game, fixed, (uintptr_t)region, 300);
    assert(g_ogServed == served);
    OgtJob(game, fixed, region, 2, 200, 1, 10); /* a new job registers: on again */
    assert(g_ogServed > served);
    VirtualFree(tail, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
    printf("PASS overlap grid: %u queries identical to the game's loop, %u reaching a record's own test, %ld on the grid\n",
        g_ogtQueries, g_ogtFound, (long)g_ogServed);
}
