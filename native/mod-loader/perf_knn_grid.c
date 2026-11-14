/* knn-grid (docs/performance.md): the Hive Lord graph's neighbour pass, game.dll 0x177C9E0 (KnnLink), tries every
 * pair of the graph's points, about 1,400 ms on a 4 km map and 400 s on a 16 km one. Points farther apart than its
 * radius do nothing in it, so a grid that visits only the pairs that can pass its test, in its order, through a copy of
 * its per-pair code, writes the same bytes. Included by hellforge_modloader.c after render_guards.c: PinMatches
 * (packages.c), LoaderHook (hooks.c), PerfFixOn, Note, g_build. */

#include <emmintrin.h>
#include <float.h>

#define KG_LINK 0x177C9E0u
#define KG_LINK_SIZE 734u
#define KG_CALL_AT 0x256u        /* call 0x210CFE8, the CRT sqrt, for a negative distance only */
#define KG_MIN_POINTS 1024u      /* fewer: the game's own pass, under a millisecond */
#define KG_MARGIN (1.0 + 1.0 / 1024.0)
#define KG_AXIS_CAP 0x1000000u   /* cells along one axis */

/* The whole function as build 25480438 has it: the pin, and the code the test runs. */
static const uint8_t kKgLinkCode[KG_LINK_SIZE] = {
    0x48, 0x8b, 0xc4, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x48, 0x8b, 0x51, 0x18, 0x45, 0x33, 0xff, 0x0f, 0x29, 0x78, 0xc8, 0x48, 0x8b, 0xf1, 0xf3,
    0x0f, 0x10, 0x79, 0x2c, 0xf3, 0x0f, 0x59, 0xff, 0x85, 0xd2, 0x0f, 0x84, 0xa9, 0x02, 0x00, 0x00, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
    0x48, 0x89, 0x78, 0x18, 0x4c, 0x89, 0x60, 0x20, 0x4c, 0x89, 0x70, 0xe8, 0x0f, 0x29, 0x70, 0xd8, 0x0f, 0x57, 0xf6, 0x0f, 0x1f, 0x40, 0x00, 0x66,
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x36, 0x45, 0x8b, 0xd7, 0x4b, 0x8d, 0x2c, 0x52, 0x49, 0xc1, 0xe2, 0x05, 0x4c, 0x03,
    0x56, 0x08, 0x33, 0xff, 0x49, 0x8d, 0x42, 0x10, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x00, 0x0f, 0x2e, 0xc6, 0x7a,
    0x02, 0x74, 0x10, 0xff, 0xc7, 0x48, 0x83, 0xc0, 0x04, 0x83, 0xff, 0x04, 0x72, 0xea, 0xbf, 0x04, 0x00, 0x00, 0x00, 0x45, 0x8d, 0x67, 0x01, 0x0f,
    0x28, 0xce, 0x45, 0x8b, 0xdc, 0x44, 0x3b, 0xe2, 0x0f, 0x83, 0x85, 0x01, 0x00, 0x00, 0x66, 0x90, 0x48, 0x8b, 0x06, 0xf3, 0x41, 0x0f, 0x10, 0x44,
    0xae, 0x04, 0xf3, 0x41, 0x0f, 0x10, 0x14, 0xae, 0x41, 0x8b, 0xd3, 0x48, 0x8d, 0x0c, 0x52, 0xf3, 0x0f, 0x5c, 0x14, 0x88, 0xf3, 0x0f, 0x5c, 0x44,
    0x88, 0x04, 0xf3, 0x0f, 0x59, 0xd2, 0xf3, 0x0f, 0x59, 0xc0, 0xf3, 0x0f, 0x58, 0xd0, 0x0f, 0x2f, 0xd7, 0x0f, 0x87, 0x3f, 0x01, 0x00, 0x00, 0x48,
    0x8b, 0x46, 0x08, 0x48, 0xc1, 0xe2, 0x05, 0x83, 0x7c, 0x02, 0x0c, 0x00, 0x0f, 0x85, 0x2c, 0x01, 0x00, 0x00, 0x0f, 0x2f, 0xd1, 0x41, 0x8b, 0x42,
    0x0c, 0x76, 0x2e, 0x85, 0xc0, 0x0f, 0x85, 0x1b, 0x01, 0x00, 0x00, 0x0f, 0x28, 0xca, 0x49, 0x8d, 0x4a, 0x10, 0xf3, 0x0f, 0x10, 0x01, 0x0f, 0x2e,
    0xc6, 0x7a, 0x06, 0x0f, 0x84, 0xfa, 0x00, 0x00, 0x00, 0xff, 0xc0, 0x48, 0x83, 0xc1, 0x04, 0x83, 0xf8, 0x04, 0x72, 0xe6, 0xe9, 0xf5, 0x00, 0x00,
    0x00, 0x85, 0xc0, 0x0f, 0x84, 0xc4, 0x00, 0x00, 0x00, 0x8b, 0xc7, 0x83, 0xff, 0x04, 0x0f, 0x83, 0xe2, 0x00, 0x00, 0x00, 0x44, 0x8b, 0xcf, 0x49,
    0x8d, 0x49, 0x04, 0x49, 0x8d, 0x0c, 0x8a, 0x90, 0xf3, 0x0f, 0x10, 0x01, 0x0f, 0x2e, 0xc1, 0x7a, 0x02, 0x74, 0x10, 0xff, 0xc0, 0x48, 0x83, 0xc1,
    0x04, 0x83, 0xf8, 0x04, 0x72, 0xea, 0xe9, 0xbb, 0x00, 0x00, 0x00, 0xf3, 0x41, 0x0f, 0x11, 0x54, 0x82, 0x10, 0x0f, 0x28, 0xca, 0x45, 0x89, 0x1c,
    0x82, 0x44, 0x8b, 0xc7, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x2b, 0xc7, 0x83, 0xf8, 0x04, 0x72, 0x4e, 0x8b, 0xc7, 0x49, 0x8d, 0x49, 0x06, 0xf7, 0xd8,
    0x49, 0x8d, 0x0c, 0x8a, 0xc1, 0xe8, 0x02, 0xff, 0xc0, 0x8b, 0xd0, 0x44, 0x8d, 0x04, 0x87, 0x4d, 0x8d, 0x0c, 0x81, 0xf3, 0x0f, 0x10, 0x41, 0xf8,
    0xf3, 0x0f, 0x10, 0x11, 0xf3, 0x0f, 0x5f, 0xc1, 0xf3, 0x0f, 0x10, 0x49, 0xfc, 0x48, 0x8d, 0x49, 0x10, 0xf3, 0x0f, 0x5f, 0xc8, 0xf3, 0x0f, 0x5f,
    0xd1, 0xf3, 0x0f, 0x10, 0x49, 0xf4, 0xf3, 0x0f, 0x5f, 0xca, 0x48, 0x83, 0xea, 0x01, 0x75, 0xd3, 0x41, 0x83, 0xf8, 0x04, 0x73, 0x50, 0x49, 0x8d,
    0x49, 0x04, 0xba, 0x04, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x0c, 0x8a, 0x41, 0x2b, 0xd0, 0xf3, 0x0f, 0x10, 0x01, 0x48, 0x8d, 0x49, 0x04, 0xf3, 0x0f,
    0x5f, 0xc1, 0x0f, 0x28, 0xc8, 0x48, 0x83, 0xea, 0x01, 0x75, 0xeb, 0xeb, 0x29, 0x33, 0xc0, 0x49, 0x8d, 0x4a, 0x10, 0xf3, 0x0f, 0x10, 0x01, 0x0f,
    0x2e, 0xc6, 0x7a, 0x02, 0x74, 0x0d, 0xff, 0xc0, 0x48, 0x83, 0xc1, 0x04, 0x83, 0xf8, 0x04, 0x72, 0xea, 0xeb, 0x0b, 0xf3, 0x41, 0x0f, 0x11, 0x54,
    0x82, 0x10, 0x45, 0x89, 0x1c, 0x82, 0x41, 0xff, 0xc3, 0x44, 0x3b, 0x5e, 0x18, 0x0f, 0x82, 0x7d, 0xfe, 0xff, 0xff, 0x83, 0xff, 0x04, 0x0f, 0x83,
    0x78, 0x00, 0x00, 0x00, 0x8b, 0xdf, 0x48, 0x83, 0xc3, 0x04, 0x49, 0x8d, 0x1c, 0x9a, 0xf3, 0x0f, 0x10, 0x03, 0x0f, 0x2e, 0xc6, 0x7a, 0x02, 0x74,
    0x63, 0x0f, 0x5a, 0xc8, 0x0f, 0x57, 0xc0, 0x66, 0x0f, 0x2e, 0xc1, 0x77, 0x06, 0x66, 0x0f, 0x51, 0xc1, 0xeb, 0x08, 0x0f, 0x28, 0xc1, 0xe8, 0xad,
    0x03, 0x99, 0x00, 0x8b, 0x53, 0xf0, 0x0f, 0x57, 0xc9, 0x48, 0xc1, 0xe2, 0x05, 0xf2, 0x0f, 0x5a, 0xc8, 0xf3, 0x0f, 0x11, 0x0b, 0x48, 0x03, 0x56,
    0x08, 0x33, 0xc0, 0x48, 0x8d, 0x4a, 0x10, 0xf3, 0x0f, 0x10, 0x01, 0x0f, 0x2e, 0xc6, 0x7a, 0x02, 0x74, 0x0d, 0xff, 0xc0, 0x48, 0x83, 0xc1, 0x04,
    0x83, 0xf8, 0x04, 0x72, 0xea, 0xeb, 0x0a, 0xf3, 0x0f, 0x11, 0x4c, 0x82, 0x10, 0x44, 0x89, 0x3c, 0x82, 0xff, 0xc7, 0x48, 0x83, 0xc3, 0x04, 0x83,
    0xff, 0x04, 0x72, 0x92, 0x8b, 0x56, 0x18, 0x45, 0x8b, 0xfc, 0x44, 0x3b, 0xe2, 0x0f, 0x82, 0x9d, 0xfd, 0xff, 0xff, 0x0f, 0x28, 0x74, 0x24, 0x30,
    0x4c, 0x8b, 0x74, 0x24, 0x40, 0x4c, 0x8b, 0x64, 0x24, 0x78, 0x48, 0x8b, 0x7c, 0x24, 0x70, 0x48, 0x8b, 0x6c, 0x24, 0x68, 0x48, 0x8b, 0x5c, 0x24,
    0x60, 0x0f, 0x28, 0x7c, 0x24, 0x20, 0x48, 0x83, 0xc4, 0x48, 0x41, 0x5f, 0x5e, 0xc3};

/* The graph (made at 0xF43976): points of 3 floats at +0, nodes at +8, the count at +0x18, the radius at +0x2C
 * (18.0). A node holds 4 links and their distances; a distance of 0 marks a free slot. */
typedef struct KgNode
{
    uint32_t link[4];
    float dist[4];
} KgNode;

typedef struct KgPass
{
    const float* pos;
    KgNode* nodes;
    uint32_t count;
    float r2;
} KgPass;

typedef struct KgGrid
{
    double minX, minY, inverse;
    uint32_t cellsX, cellsY;
    uint32_t* starts;  /* cell c's points are indices[starts[c] .. starts[c + 1]), ascending */
    uint32_t* indices;
    uint32_t* loose;   /* points with a coordinate that is not finite, ascending */
    uint32_t looseCount;
    void* block;
} KgGrid;

typedef struct KgRun
{
    const uint32_t* at;
    const uint32_t* end;
} KgRun;

typedef void (*KgLinkFn)(void* graph);
static KgLinkFn g_kgOriginal;
static int g_kgState; /* 0 waiting for game.dll, 1 installed, -1 not */
static volatile LONG g_kgGridRuns;

static int KgPinned(uintptr_t game)
{
    return PinMatches(game + KG_LINK, kKgLinkCode, 512) && PinMatches(game + KG_LINK + 512, kKgLinkCode + 512, KG_LINK_SIZE - 512);
}

static int KgFinite(const float* p) { return isfinite(p[0]) && isfinite(p[1]); }

/* The first slot from `from` whose distance compares equal to 0 (ucomiss: a NaN is taken, -0 is free); 4 for none. */
static uint32_t KgFree(const KgNode* n, uint32_t from)
{
    while (from < 4 && n->dist[from] != 0.0f) from++;
    return from;
}

static void KgTakeFree(KgNode* n, uint32_t j, float d2)
{
    uint32_t k = KgFree(n, 0);
    if (k < 4) { n->dist[k] = d2; n->link[k] = j; }
}

/* 0x177CB09..0x177CBCB: the first slot from firstFree holding the running maximum takes the pair, then the maximum is
 * found again over those slots (the maxss chain, which keeps the old value on a NaN). */
static void KgReplaceMax(KgNode* n, uint32_t j, float d2, uint32_t firstFree, float* maxv)
{
    for (uint32_t k = firstFree; k < 4; k++)
    {
        if (n->dist[k] != *maxv) continue;
        n->dist[k] = d2;
        n->link[k] = j;
        float m = d2;
        for (uint32_t q = firstFree; q < 4; q++)
            if (n->dist[q] > m) m = n->dist[q];
        *maxv = m;
        return;
    }
}

/* The per-pair block, 0x177CA80..0x177CBF6. The distance is float: x then y, each subtracted and squared, then added,
 * one rounding a step (no FMA on the target). A pair farther than the radius returns at once; a NaN goes on, as there. */
static void KgPair(const KgPass* p, uint32_t i, uint32_t j, uint32_t firstFree, float* maxv)
{
    const float* a = p->pos + (size_t)i * 3;
    const float* b = p->pos + (size_t)j * 3;
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float xx = dx * dx;
    float yy = dy * dy;
    float d2 = xx + yy;
    KgNode* n = &p->nodes[i];
    if (d2 > p->r2 || p->nodes[j].link[3] != 0) return;
    if (d2 > *maxv)
    {
        if (n->link[3] != 0) return;
        *maxv = d2;
        KgTakeFree(n, j, d2);
        return;
    }
    if (n->link[3] == 0) KgTakeFree(n, j, d2);
    else KgReplaceMax(n, j, d2, firstFree, maxv);
}

/* cvtps2pd, sqrtpd, cvtsd2ss, as 0x177CC21..0x177CC45. Intrinsics, so no errno path can change a NaN. */
static float KgLength(float d2)
{
    return _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), _mm_sqrt_pd(_mm_cvtps_pd(_mm_set_ss(d2)))));
}

/* After i's pairs, 0x177CC03..0x177CC84: each new slot's length, and the link back in the neighbour's first free slot. */
static void KgFinish(const KgPass* p, uint32_t i, uint32_t firstFree)
{
    KgNode* n = &p->nodes[i];
    for (uint32_t k = firstFree; k < 4; k++)
    {
        float d2 = n->dist[k];
        if (d2 == 0.0f) return;
        float length = KgLength(d2);
        uint32_t j = n->link[k];
        n->dist[k] = length;
        KgNode* back = &p->nodes[j];
        uint32_t m = KgFree(back, 0);
        if (m < 4) { back->dist[m] = length; back->link[m] = i; }
    }
}

/* A point's cell along one axis; the same call files a point and finds its neighbours, so both agree. */
static uint32_t KgAxis(float v, double min, double inverse, uint32_t cells)
{
    double at = ((double)v - min) * inverse;
    if (!(at > 0.0)) return 0;
    if (at >= (double)cells) return cells - 1;
    return (uint32_t)at;
}

static uint32_t KgCell(const KgGrid* g, const float* p)
{
    return KgAxis(p[1], g->minY, g->inverse, g->cellsY) * g->cellsX + KgAxis(p[0], g->minX, g->inverse, g->cellsX);
}

/* The finite points' box (zeros when there are none); the number of points that are not finite. */
static uint32_t KgBounds(const KgPass* p, double box[4])
{
    uint32_t loose = 0, finite = 0;
    for (uint32_t i = 0; i < p->count; i++)
    {
        const float* at = p->pos + (size_t)i * 3;
        if (!KgFinite(at)) { loose++; continue; }
        double x = at[0], y = at[1];
        if (finite++ == 0) { box[0] = box[2] = x; box[1] = box[3] = y; continue; }
        box[0] = x < box[0] ? x : box[0];
        box[1] = y < box[1] ? y : box[1];
        box[2] = x > box[2] ? x : box[2];
        box[3] = y > box[3] ? y : box[3];
    }
    if (finite == 0) box[0] = box[1] = box[2] = box[3] = 0.0;
    return loose;
}

/* Cells at least the radius wide, with room for float rounding (docs/performance.md#knn-grid); wider while there
 * would be more than 2 per point. Wider cells only visit more pairs. */
static void KgSize(KgGrid* g, const double box[4], float r2, uint32_t count)
{
    double size = sqrt((double)r2) * KG_MARGIN, cap = 2.0 * count + 1024.0;
    for (;;)
    {
        double x = floor((box[2] - box[0]) / size) + 1.0, y = floor((box[3] - box[1]) / size) + 1.0;
        if (x <= KG_AXIS_CAP && y <= KG_AXIS_CAP && x * y <= cap)
        {
            g->cellsX = (uint32_t)x;
            g->cellsY = (uint32_t)y;
            break;
        }
        size *= 2.0;
    }
    g->minX = box[0];
    g->minY = box[1];
    g->inverse = 1.0 / size;
}

/* A counting sort by cell: each cell's points ascending, as the pass visits them. 0 without memory. */
static int KgGridBuild(KgGrid* g, const KgPass* p)
{
    double box[4];
    g->looseCount = KgBounds(p, box);
    KgSize(g, box, p->r2, p->count);
    size_t cells = (size_t)g->cellsX * g->cellsY;
    g->block = VirtualAlloc(NULL, (cells + 1 + p->count) * sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g->block == NULL) return 0;
    g->starts = (uint32_t*)g->block;
    g->indices = g->starts + cells + 1;
    g->loose = g->indices + (p->count - g->looseCount);
    uint32_t loose = 0;
    for (uint32_t i = 0; i < p->count; i++)
        if (KgFinite(p->pos + (size_t)i * 3)) g->starts[KgCell(g, p->pos + (size_t)i * 3)]++;
    for (size_t c = 0, sum = 0; c <= cells; c++) { uint32_t n = g->starts[c]; g->starts[c] = (uint32_t)sum; sum += n; }
    for (uint32_t i = 0; i < p->count; i++)
    {
        const float* at = p->pos + (size_t)i * 3;
        if (KgFinite(at)) g->indices[g->starts[KgCell(g, at)]++] = i;
        else g->loose[loose++] = i;
    }
    for (size_t c = cells; c > 0; c--) g->starts[c] = g->starts[c - 1]; /* each cursor ended at its cell's end */
    g->starts[0] = 0;
    return 1;
}

/* The part of an ascending run after i. */
static KgRun KgAfter(const uint32_t* at, const uint32_t* end, uint32_t i)
{
    KgRun run = {at, end};
    while (at < end)
    {
        const uint32_t* mid = at + (end - at) / 2;
        if (*mid <= i) at = mid + 1;
        else end = mid;
    }
    run.at = at;
    return run;
}

/* The 3 x 3 cells around a finite point i and the loose points: every j > i its pair can pass for. */
static int KgRuns(const KgGrid* g, const float* at, uint32_t i, KgRun runs[10])
{
    int n = 0;
    int cx = (int)KgAxis(at[0], g->minX, g->inverse, g->cellsX), cy = (int)KgAxis(at[1], g->minY, g->inverse, g->cellsY);
    for (int y = cy - 1; y <= cy + 1; y++)
        for (int x = cx - 1; x <= cx + 1; x++)
        {
            if (y < 0 || x < 0 || y >= (int)g->cellsY || x >= (int)g->cellsX) continue;
            uint32_t c = (uint32_t)y * g->cellsX + (uint32_t)x;
            runs[n++] = KgAfter(g->indices + g->starts[c], g->indices + g->starts[c + 1], i);
        }
    runs[n++] = KgAfter(g->loose, g->loose + g->looseCount, i);
    return n;
}

/* The runs merged: each j once, ascending, as the game's j loop meets them. */
static void KgVisit(const KgPass* p, uint32_t i, KgRun* runs, int count, uint32_t firstFree, float* maxv)
{
    for (;;)
    {
        int best = -1;
        for (int r = 0; r < count; r++)
            if (runs[r].at < runs[r].end && (best < 0 || *runs[r].at < *runs[best].at)) best = r;
        if (best < 0) return;
        KgPair(p, i, *runs[best].at++, firstFree, maxv);
    }
}

/* One i of the game's outer loop (0x177CA30): its free slot and maximum, its pairs, its finish. A point that is not
 * finite can make a NaN distance with any point, which no test skips: it visits every j, as the game does. */
static void KgLinkOne(const KgPass* p, const KgGrid* g, uint32_t i)
{
    uint32_t firstFree = KgFree(&p->nodes[i], 0);
    float maxv = 0.0f;
    const float* at = p->pos + (size_t)i * 3;
    if (KgFinite(at))
    {
        KgRun runs[10];
        int n = KgRuns(g, at, i, runs);
        KgVisit(p, i, runs, n, firstFree, &maxv);
    }
    else
        for (uint32_t j = i + 1; j < p->count; j++) KgPair(p, i, j, firstFree, &maxv);
    KgFinish(p, i, firstFree);
}

/* The graph's maker zeroes the nodes, and the only caller (0xF43FAD) changes points and count, never nodes. */
static int KgNodesZero(const KgNode* nodes, uint32_t count)
{
    const uint64_t* at = (const uint64_t*)nodes;
    for (size_t i = 0; i < (size_t)count * 4; i++)
        if (at[i] != 0) return 0;
    return 1;
}

/* The pass on a grid: 1 done, 0 left to the game with nothing written. A radius whose square is not a normal float, or
 * nodes not as made, are outside what the grid is proven for. */
static int KgLinkGrid(void* graph)
{
    KgPass p;
    KgGrid g;
    const uint8_t* at = (const uint8_t*)graph;
    memcpy(&p.pos, at, 8);
    memcpy(&p.nodes, at + 8, 8);
    memcpy(&p.count, at + 0x18, 4);
    float radius;
    memcpy(&radius, at + 0x2C, 4);
    p.r2 = radius * radius; /* 0x177C9FC: one float multiply */
    if (p.count == 0 || !(p.r2 >= FLT_MIN && p.r2 <= FLT_MAX) || !KgNodesZero(p.nodes, p.count)) return 0;
    if (!KgGridBuild(&g, &p)) return 0;
    for (uint32_t i = 0; i < p.count; i++) KgLinkOne(&p, &g, i);
    VirtualFree(g.block, 0, MEM_RELEASE);
    return 1;
}

/* HellForge's hook is the innermost link, so the original is the game's own. */
static void KnnGridDetour(void* graph)
{
    uint32_t count = 0;
    if (graph != NULL) memcpy(&count, (const uint8_t*)graph + 0x18, 4);
    if (count >= KG_MIN_POINTS && KgLinkGrid(graph)) { InterlockedIncrement(&g_kgGridRuns); return; }
    g_kgOriginal(graph);
}

static void KnnGridInstallAt(uintptr_t game)
{
    if (g_kgState != 0 || game == 0) return;
    g_kgState = -1;
    if (!PerfFixOn("knn-grid")) return;
    if (!KgPinned(game)) { Note("performance fix knn-grid: not installed; the Hive Lord graph's neighbour pass is not the reviewed one"); return; }
    if (LoaderHook("knn-grid: the Hive Lord graph's neighbour pass", game + KG_LINK, (void*)KnnGridDetour, (void**)&g_kgOriginal))
    {
        Note("performance fix knn-grid: not installed; the Hive Lord graph's neighbour pass could not be hooked");
        return;
    }
    g_kgState = 1;
    Note("performance fix knn-grid: on; the Hive Lord graph links %u points or more through a grid", KG_MIN_POINTS);
}

/* Once, from the watch thread as soon as game.dll is decoded (WatchLoaded), before any level is generated. */
static void KnnGridInstall(void)
{
    if (g_kgState != 0) return;
    if (strcmp(g_build, "25480438")) { g_kgState = -1; Note("performance fix knn-grid: not installed; not game build 25480438"); return; }
    KnnGridInstallAt(ApiModuleBase("game.dll", NULL));
}
