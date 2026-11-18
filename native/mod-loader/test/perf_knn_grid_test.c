/* knn-grid (perf_knn_grid.c): the game's own neighbour pass, its 734 bytes copied from build 25480438, against the grid
 * on random, Poisson-like, clustered, duplicate, collinear, boundary, non-finite and extreme point sets: every node byte
 * must match. Then every pair that passes the game's test is checked to lie within the 3 x 3 cells, and the hook on a
 * synthetic image: pins, HELLFORGE_PERF_OFF, both paths. Included by stats_test.c. */

#define KGT_IMAGE 0x177D000u /* past KnnLink's end */
#define KGT_STUB 0x300u      /* the CRT sqrt's place in the reference copy: ud2, never reached */

typedef struct KgtSet
{
    float* pos;
    uint32_t count;
    float radius;
} KgtSet;

static uint64_t g_kgtSeed = 0x9E3779B97F4A7C15ull;
static KgLinkFn g_kgtGame;

static uint32_t KgtNext(void)
{
    g_kgtSeed ^= g_kgtSeed >> 12;
    g_kgtSeed ^= g_kgtSeed << 25;
    g_kgtSeed ^= g_kgtSeed >> 27;
    return (uint32_t)((g_kgtSeed * 0x2545F4914F6CDD1Dull) >> 32);
}

static double KgtUniform(double low, double high) { return low + (high - low) * (KgtNext() / 4294967296.0); }

/* The game's function in its own page, its one call (a negative distance's CRT sqrt) sent to a ud2. */
static KgLinkFn KgtGameCopy(void)
{
    uint8_t* page = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(page != NULL && kKgLinkCode[KG_CALL_AT] == 0xe8);
    int32_t rel;
    memcpy(&rel, kKgLinkCode + KG_CALL_AT + 1, 4);
    assert(KG_LINK + KG_CALL_AT + 5 + rel == 0x210CFE8u);
    memcpy(page, kKgLinkCode, KG_LINK_SIZE);
    rel = (int32_t)(KGT_STUB - (KG_CALL_AT + 5));
    memcpy(page + KG_CALL_AT + 1, &rel, 4);
    page[KGT_STUB] = 0x0f;
    page[KGT_STUB + 1] = 0x0b;
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    return (KgLinkFn)(void*)page;
}

static KgtSet KgtNew(uint32_t count, float radius)
{
    KgtSet s = {(float*)calloc((size_t)count * 3 + 3, sizeof(float)), count, radius};
    assert(s.pos != NULL);
    for (uint32_t i = 0; i < count; i++) s.pos[3 * i + 2] = (i % 7 == 0) ? NAN : (float)KgtUniform(-50, 50); /* z: never read */
    return s;
}

static void KgtPut(KgtSet* s, uint32_t i, double x, double y) { s->pos[3 * i] = (float)x; s->pos[3 * i + 1] = (float)y; }

/* The graph as its maker leaves it: 0x30 bytes, zeroed nodes. */
static void KgtGraph(uint8_t graph[0x30], const KgtSet* s, KgNode* nodes)
{
    memset(graph, 0, 0x30);
    memcpy(graph, &s->pos, 8);
    memcpy(graph + 8, &nodes, 8);
    memcpy(graph + 0x18, &s->count, 4);
    memcpy(graph + 0x2C, &s->radius, 4);
}

/* The game's pass and `link` on zeroed nodes: 1 when every byte matches. `grid` says whether the grid must have run. */
static int KgtSame(const KgtSet* s, KgLinkFn link, int grid)
{
    size_t bytes = ((size_t)s->count + 1) * sizeof(KgNode);
    KgNode* game = (KgNode*)calloc(1, bytes);
    KgNode* ours = (KgNode*)calloc(1, bytes);
    uint8_t graph[0x30];
    assert(game != NULL && ours != NULL);
    KgtGraph(graph, s, game);
    g_kgtGame(graph);
    KgtGraph(graph, s, ours);
    if (link != NULL) link(graph);
    else assert(KgLinkGrid(graph) == grid);
    int same = memcmp(game, ours, bytes) == 0;
    if (!same)
        for (uint32_t i = 0; i < s->count; i++)
            if (memcmp(&game[i], &ours[i], sizeof(KgNode)) != 0) { printf("knn grid: node %u of %u differs\n", i, s->count); break; }
    free(game);
    free(ours);
    return same;
}

/* Every finite pair the game's test passes (the same float steps) lies in neighbouring cells. */
static void KgtCovered(const KgtSet* s)
{
    KgPass p = {s->pos, NULL, s->count, s->radius * s->radius};
    KgGrid g;
    assert(KgGridBuild(&g, &p));
    for (uint32_t i = 0; i < s->count; i++)
    {
        const float* a = s->pos + 3 * (size_t)i;
        if (!KgFinite(a)) continue;
        for (uint32_t j = i + 1; j < s->count; j++)
        {
            const float* b = s->pos + 3 * (size_t)j;
            float dx = a[0] - b[0], dy = a[1] - b[1], xx = dx * dx, yy = dy * dy, d2 = xx + yy;
            if (!KgFinite(b) || d2 > p.r2) continue;
            int64_t ax = KgAxis(a[0], g.minX, g.inverse, g.cellsX), bx = KgAxis(b[0], g.minX, g.inverse, g.cellsX);
            int64_t ay = KgAxis(a[1], g.minY, g.inverse, g.cellsY), by = KgAxis(b[1], g.minY, g.inverse, g.cellsY);
            assert(llabs(ax - bx) <= 1 && llabs(ay - by) <= 1);
        }
    }
    VirtualFree(g.block, 0, MEM_RELEASE);
}

static void KgtUniformSet(KgtSet* s, double low, double high)
{
    for (uint32_t i = 0; i < s->count; i++) KgtPut(s, i, KgtUniform(low, high), KgtUniform(low, high));
}

/* A jittered lattice in a disk: Poisson-disc-like spacing, as the graph's sampler gives. */
static void KgtDiskSet(KgtSet* s, double spacing)
{
    double radius = spacing * sqrt(s->count / 3.14159265);
    for (uint32_t i = 0; i < s->count; i++)
    {
        double x, y;
        do { x = KgtUniform(-radius, radius); y = KgtUniform(-radius, radius); } while (x * x + y * y > radius * radius);
        KgtPut(s, i, floor(x / spacing) * spacing + KgtUniform(0, spacing * 0.4), floor(y / spacing) * spacing + KgtUniform(0, spacing * 0.4));
    }
}

static void KgtClusterSet(KgtSet* s, uint32_t clusters, double spread, double sigma)
{
    double centre[64][2];
    for (uint32_t c = 0; c < clusters; c++) { centre[c][0] = KgtUniform(-spread, spread); centre[c][1] = KgtUniform(-spread, spread); }
    for (uint32_t i = 0; i < s->count; i++)
    {
        uint32_t c = KgtNext() % clusters;
        double gx = KgtUniform(-1, 1) + KgtUniform(-1, 1) + KgtUniform(-1, 1), gy = KgtUniform(-1, 1) + KgtUniform(-1, 1) + KgtUniform(-1, 1);
        KgtPut(s, i, centre[c][0] + gx * sigma, centre[c][1] + gy * sigma);
    }
}

/* Copies of earlier points: distance 0, the value that marks a free slot. */
static void KgtDuplicates(KgtSet* s, uint32_t percent)
{
    for (uint32_t i = 1; i < s->count; i++)
        if (KgtNext() % 100 < percent)
        {
            uint32_t from = KgtNext() % i;
            KgtPut(s, i, s->pos[3 * from], s->pos[3 * from + 1]);
        }
}

/* Pairs a few float steps either side of the radius, far from the origin where a step is large. */
static void KgtBoundarySet(KgtSet* s, double offset)
{
    static const double kDirections[][2] = {{1, 0}, {0, 1}, {0.6, 0.8}, {0.70710678118654752, 0.70710678118654752}, {-1, 0}};
    for (uint32_t i = 0; i + 1 < s->count; i += 2)
    {
        double x = offset + KgtUniform(0, 40 * fabs(s->radius)), y = offset + KgtUniform(0, 40 * fabs(s->radius));
        const double* d = kDirections[KgtNext() % 5];
        double r = fabs(s->radius) * (1.0 + ((int)(KgtNext() % 17) - 8) * 0x1p-23);
        KgtPut(s, i, x, y);
        KgtPut(s, i + 1, x + d[0] * r, y + d[1] * r);
    }
}

/* A lattice exactly the radius apart: distances equal to the radius, ties everywhere. */
static void KgtLatticeSet(KgtSet* s, double offset)
{
    uint32_t side = (uint32_t)sqrt((double)s->count) + 1;
    for (uint32_t i = 0; i < s->count; i++) KgtPut(s, i, offset + (i % side) * fabs(s->radius), offset + (i / side) * fabs(s->radius));
}

static void KgtLooseCoordinates(KgtSet* s, uint32_t percent)
{
    static const float kLoose[] = {NAN, INFINITY, -INFINITY};
    for (uint32_t i = 0; i < s->count; i++)
        if (KgtNext() % 100 < percent) s->pos[3 * i + KgtNext() % 2] = kLoose[KgtNext() % 3];
}

static void KgtExtremes(KgtSet* s)
{
    static const float kFar[] = {3.0e38f, -3.0e38f, 1.0e38f, -1.0e30f, 1.0e20f};
    for (uint32_t i = 0; i < s->count; i += 97) KgtPut(s, i, kFar[KgtNext() % 5], kFar[KgtNext() % 5]);
}

static float KgtRadius(void)
{
    static const float kRadii[] = {18.0f, 18.0f, 18.0f, 0.25f, 5.0f, 250.0f, -18.0f, 1.0e-18f};
    return kRadii[KgtNext() % (sizeof kRadii / sizeof kRadii[0])];
}

/* One random set: a shape, its scale from the radius, and sometimes duplicates, loose coordinates or far points. */
static KgtSet KgtRandomSet(uint32_t count)
{
    KgtSet s = KgtNew(count, KgtRadius());
    double r = fabs(s.radius), offset = (KgtNext() % 3 == 0) ? r * 1.0e5 : 0.0;
    switch (KgtNext() % 6)
    {
    case 0: KgtUniformSet(&s, offset, offset + r * sqrt((double)count) * KgtUniform(0.3, 4)); break;
    case 1: KgtDiskSet(&s, r * KgtUniform(0.3, 1.2)); break;
    case 2: KgtClusterSet(&s, 1 + KgtNext() % 40, r * 30, r * KgtUniform(0.1, 3)); break;
    case 3: KgtBoundarySet(&s, offset); break;
    case 4: KgtLatticeSet(&s, offset); break;
    default: for (uint32_t i = 0; i < count; i++) KgtPut(&s, i, offset + KgtUniform(0, r * count * 0.5), offset); break;
    }
    if (KgtNext() % 3 == 0) KgtDuplicates(&s, 1 + KgtNext() % 30);
    if (KgtNext() % 5 == 0) KgtLooseCoordinates(&s, 1 + KgtNext() % 5);
    if (KgtNext() % 8 == 0) KgtExtremes(&s);
    return s;
}

/* Fixed degenerate sets: one point, all equal, all on a line, lattice ties at 2^20, every point not finite. */
static void KgtDegenerate(void)
{
    static const uint32_t kCounts[] = {1, 2, 3, 4, 5, 9, 600};
    for (size_t c = 0; c < sizeof kCounts / sizeof kCounts[0]; c++)
    {
        KgtSet s = KgtNew(kCounts[c], 18.0f);
        for (uint32_t i = 0; i < s.count; i++) KgtPut(&s, i, 1024.5, -77.25);
        assert(KgtSame(&s, NULL, 1));
        KgtLatticeSet(&s, 1048576.0);
        assert(KgtSame(&s, NULL, 1));
        for (uint32_t i = 0; i < s.count; i++) KgtPut(&s, i, NAN, (double)i);
        assert(KgtSame(&s, NULL, 1));
        free(s.pos);
    }
}

/* A pair 16 + 2^-21 apart whose float distance rounds to the radius 16, straddling two cell borders from the box's
 * corner: cells exactly the radius wide would put it two cells apart and miss it. Along x, then along y. */
static void KgtStraddle(void)
{
    static const double kLine[3] = {-20.0, -4.0 - 0x1p-21, 12.0};
    for (int axis = 0; axis < 2; axis++)
    {
        KgtSet s = KgtNew(3, 16.0f);
        for (uint32_t i = 0; i < 3; i++) KgtPut(&s, i, axis ? 5.0 : kLine[i], axis ? kLine[i] : 5.0);
        assert(KgtSame(&s, NULL, 1));
        KgtCovered(&s);
        free(s.pos);
    }
}

/* Radii the grid is not proven for, and nodes not as made: the grid writes nothing and leaves the pass to the game. */
static void KgtRefusals(void)
{
    static const float kRadii[] = {1.0e20f, -1.0e20f, 1.0e-20f, 0.0f, -0.0f, NAN, INFINITY};
    for (size_t r = 0; r < sizeof kRadii / sizeof kRadii[0]; r++)
    {
        KgtSet s = KgtNew(300, kRadii[r]);
        KgtUniformSet(&s, 0, 300);
        KgNode* nodes = (KgNode*)calloc(s.count, sizeof(KgNode));
        uint8_t graph[0x30];
        KgtGraph(graph, &s, nodes);
        assert(KgLinkGrid(graph) == 0 && KgNodesZero(nodes, s.count));
        if (r == 0)
        {
            nodes[299].dist[3] = 1.0f;
            s.radius = 18.0f;
            KgtGraph(graph, &s, nodes);
            assert(KgLinkGrid(graph) == 0 && nodes[0].dist[0] == 0.0f);
        }
        free(nodes);
        free(s.pos);
    }
}

static double KgtMs(KgLinkFn link, const KgtSet* s, int repeat)
{
    KgNode* nodes = (KgNode*)malloc(((size_t)s->count + 1) * sizeof(KgNode));
    uint8_t graph[0x30];
    LARGE_INTEGER frequency, start, end;
    double best = 1e30;
    assert(nodes != NULL);
    QueryPerformanceFrequency(&frequency);
    for (int r = 0; r < repeat; r++)
    {
        memset(nodes, 0, (size_t)s->count * sizeof(KgNode));
        KgtGraph(graph, s, nodes);
        QueryPerformanceCounter(&start);
        if (link != NULL) link(graph);
        else assert(KgLinkGrid(graph));
        QueryPerformanceCounter(&end);
        double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
        best = ms < best ? ms : best;
    }
    free(nodes);
    return best;
}

/* The game's pass and the grid on Poisson-like disks at about 12.8 m spacing; not asserted, only printed. */
static void KgtTimes(void)
{
    static const uint32_t kCounts[] = {256, 512, 1024, 2048, 20000};
    for (size_t c = 0; c < sizeof kCounts / sizeof kCounts[0]; c++)
    {
        KgtSet s = KgtNew(kCounts[c], 18.0f);
        KgtDiskSet(&s, 12.8);
        assert(KgtSame(&s, NULL, 1));
        int repeat = kCounts[c] > 4096 ? 1 : 9;
        printf("knn grid: %u points, the game's pass %.3f ms, the grid %.3f ms\n", s.count, KgtMs(g_kgtGame, &s, repeat), KgtMs(NULL, &s, repeat));
        free(s.pos);
    }
}

/* The hook on a synthetic game.dll: pins, the switch, then both paths through the hooked entry. */
static void KgtHook(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, KGT_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    memcpy(image + KG_LINK, kKgLinkCode, KG_LINK_SIZE);
    assert(KgPinned(game));
    image[KG_LINK + 0x2A0] ^= 1;
    assert(!KgPinned(game)); /* the second half of the pin: another build */
    g_kgState = 0;
    KnnGridInstallAt(game);
    assert(g_kgState == -1 && memcmp(image + KG_LINK, kKgLinkCode, 16) == 0);
    image[KG_LINK + 0x2A0] ^= 1;
    g_kgState = 0;
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "knn-grid");
    KnnGridInstallAt(game);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(g_kgState == -1 && memcmp(image + KG_LINK, kKgLinkCode, 16) == 0);
    g_kgState = 0;
    KnnGridInstallAt(0);
    assert(g_kgState == 0); /* no game.dll yet */
    KnnGridInstallAt(game);
    assert(g_kgState == 1 && memcmp(image + KG_LINK, kKgLinkCode, 16) != 0);
    KnnGridInstallAt(game); /* once */

    KgLinkFn hooked = (KgLinkFn)(void*)(image + KG_LINK);
    LONG runs = g_kgGridRuns;
    KgtSet small = KgtNew(KG_MIN_POINTS - 1, 18.0f), large = KgtNew(3000, 18.0f), refused = KgtNew(3000, NAN);
    KgtDiskSet(&small, 12.8);
    KgtDiskSet(&large, 12.8);
    KgtDiskSet(&refused, 12.8);
    assert(KgtSame(&small, hooked, 0) && g_kgGridRuns == runs);       /* below the threshold: the game's pass */
    assert(KgtSame(&large, hooked, 1) && g_kgGridRuns == runs + 1);   /* the grid */
    assert(KgtSame(&refused, hooked, 0) && g_kgGridRuns == runs + 1); /* refused: the game's pass */
    free(small.pos);
    free(large.pos);
    free(refused.pos);
    assert(HookRemove(NULL, game + KG_LINK, (void*)KnnGridDetour) == 0);
    large = KgtNew(3000, 18.0f);
    KgtDiskSet(&large, 12.8);
    assert(KgtSame(&large, hooked, 0) && g_kgGridRuns == runs + 1); /* unhooked: the game's own */
    free(large.pos);
    VirtualFree(image, 0, MEM_RELEASE);
    g_kgState = 0;
}

/* Random sets under each rounding mode and with flush-to-zero and denormals-are-zero: the proof's margin holds for
 * every MXCSR the game's thread may run with, and the game's copy runs under the same one. */
static int KgtModes(void)
{
    static const unsigned kModes[] = {0x1F80u, 0x3F80u, 0x5F80u, 0x7F80u, 0x9FC0u};
    unsigned saved = _mm_getcsr();
    int sets = 0;
    for (size_t m = 0; m < sizeof kModes / sizeof kModes[0]; m++)
        for (int n = 0; n < 40; n++, sets++)
        {
            KgtSet s = KgtRandomSet(2 + KgtNext() % 1500);
            _mm_setcsr(kModes[m]);
            int same = KgtSame(&s, NULL, 1);
            _mm_setcsr(saved);
            if (!same) { printf("knn grid: MXCSR 0x%X, set %d differs\n", kModes[m], n); assert(0); }
            free(s.pos);
        }
    return sets;
}

static void KnnGridFixture(void)
{
    g_kgtGame = KgtGameCopy();
    KgtDegenerate();
    KgtStraddle();
    KgtRefusals();
    int sets = 0;
    for (int n = 0; n < 700; n++, sets++)
    {
        KgtSet s = KgtRandomSet(2 + KgtNext() % (n < 600 ? 1500 : 6000));
        if (!KgtSame(&s, NULL, 1)) { printf("knn grid: set %d (radius %g, %u points) differs\n", n, s.radius, s.count); assert(0); }
        if (s.count <= 1500) KgtCovered(&s);
        free(s.pos);
    }
    sets += KgtModes();
    KgtTimes();
    KgtHook();
    printf("PASS knn grid: the game's pass and the grid alike on %d random sets and the degenerate ones; refusals; every "
        "passing pair within the 3 x 3 cells; the hook's pins, switch and both paths\n", sets);
}
