/* XL-1 research: the Hive Lord graph's neighbour pass on a grid (game.dll 0x177C9E0, KnnLink).
 * Included by hellforge_modloader.c after difficulty_world.c (it reads g_world.current).
 *
 * KnnLink links each point to its nearest ones: every i against every j > i, ascending (its disassembly is
 * .deepcode/xl1/knn_original.asm). A 16384 m world's graph has about a million points, so that is 5 x 10^11 pairs and a
 * ~400 s pass. KnnLinkGrid builds a grid of cells radius wide over the points' bounding box (counting sort) and, for each
 * i, visits only the j > i in the 3 x 3 cells around i's plus every point with a NaN or infinite coordinate (their
 * squared distance can be NaN, which the original's comiss/ja does not skip), sorts those ascending and runs the
 * original's per-pair block on each in that order. The original does nothing for a j it skips (d2 > radius^2, no NaN),
 * so the nodes come out the same; the check mode below runs the game's own pass on a copy of the nodes and compares.
 *
 * The graph as the function uses it (offsets verified against the asm):
 *   +0x00 float* pos; 12 bytes a point (x at +0, y at +4; +8 unused here).
 *   +0x08 Node* nodes; 32 bytes a node: uint32_t link[4] at +0, float dist[4] at +0x10.
 *   +0x18 uint32_t count; +0x2C float radius.
 * A slot is empty when its dist is exactly 0.0f (the asm's ucomiss/je; a NaN dist counts as taken). The original also
 * refuses a j whose nodes[j].link[3] is not 0: its last link slot is taken, so the back link could not be written there.
 */

typedef struct KnnNode
{
    uint32_t link[4]; /* +0x00: the neighbour's index */
    float dist[4];    /* +0x10: its distance (squared while the pair is linked, then sqrt) */
} KnnNode;
typedef struct KnnGraph
{
    const float* pos;    /* +0x00: 3 floats a point */
    KnnNode* nodes;      /* +0x08 */
    uint64_t reserved10; /* +0x10 */
    uint32_t count;      /* +0x18 */
    uint32_t reserved1C; /* +0x1C */
    uint64_t reserved20; /* +0x20 */
    uint32_t reserved28; /* +0x28 */
    float radius;        /* +0x2C: squared and compared with the squared distance */
} KnnGraph;
typedef void (*KnnLinkFn)(KnnGraph*);
_Static_assert(sizeof(KnnNode) == 0x20, "a graph node is 32 bytes");
_Static_assert(sizeof(KnnGraph) == 0x30, "the graph struct is 0x30 bytes through its radius");

#define KNN_LINK 0x177C9E0u        /* KnnLink's entry (the pin below is its first 20 bytes) */
#define KNN_CHECK_NODES 300000u    /* above this the check copy is not made (it is the original's time that is saved) */
#define KNN_MAX_CELLS 67108864u    /* 64 x 1024 x 1024: a larger grid would be absurd (the original runs instead) */
#define KNN_MIN_R2 1.17549435e-38f /* FLT_MIN: below it radius^2 loses the relative precision the 3 x 3 proof needs */

static KnnLinkFn g_knnOriginal;
static volatile LONG g_knnFallbacks, g_knnCheckNotes, g_knnRefusals;
static int g_knnInstalled;

/* The per-pair block, 0x177CA80..0x177CBF6, one C statement group a disassembly block. */
static inline void KnnPair(KnnGraph* g, uint32_t i, uint32_t j, uint32_t firstFree, float r2, float* maxv)
{
    const float* pi = g->pos + (size_t)i * 3;
    const float* pj = g->pos + (size_t)j * 3;
    KnnNode* ni = &g->nodes[i];
    /* 0x177CA80: subss x2, mulss x2, addss: (xi - xj)^2 + (yi - yj)^2, in that order. */
    float dx = pi[0] - pj[0];
    float dy = pi[1] - pj[1];
    float xx = dx * dx;
    float yy = dy * dy;
    float d2 = xx + yy;
    if (d2 > r2) return; /* 0x177CAAE comiss + ja 0x177CBF6: the pair does nothing (NaN passes on) */
    if (g->nodes[j].link[3] != 0) return; /* 0x177CABF cmp [nodes[j]+0xC],0; jne 0x177CBF6 */
    if (d2 > *maxv) /* 0x177CACA comiss with the running maximum; jbe 0x177CB01 (NaN as well) */
    {
        if (ni->link[3] != 0) return; /* 0x177CAD3 test [nodes[i]+0xC],0; jne 0x177CBF6 */
        *maxv = d2; /* 0x177CADB movaps xmm1,xmm2 */
        for (uint32_t k = 0; k < 4; k++) /* 0x177CADE..0x177CAFC: the first slot whose dist is 0.0f */
        {
            if (ni->dist[k] == 0.0f)
            {
                ni->dist[k] = d2; /* 0x177CBEB */
                ni->link[k] = j;  /* 0x177CBF2 */
                return;
            }
        }
        return; /* 0x177CAFC: no empty slot */
    }
    if (ni->link[3] == 0) /* 0x177CB01 test [nodes[i]+0xC],0; je 0x177CBCD */
    {
        for (uint32_t k = 0; k < 4; k++) /* 0x177CBCD..0x177CBE9: the same first empty slot, maximum kept */
        {
            if (ni->dist[k] == 0.0f)
            {
                ni->dist[k] = d2; /* 0x177CBEB */
                ni->link[k] = j;  /* 0x177CBF2 */
                return;
            }
        }
        return;
    }
    if (firstFree >= 4) return; /* 0x177CB0B cmp edi,4; jae 0x177CBF6 */
    for (uint32_t k = firstFree; k < 4; k++) /* 0x177CB14..0x177CB36: the first slot holding the running maximum */
    {
        if (ni->dist[k] == *maxv) /* 0x177CB20 ucomiss; jp continues, je 0x177CB3B */
        {
            ni->dist[k] = d2; /* 0x177CB3B */
            *maxv = d2;       /* 0x177CB42 movaps xmm1,xmm2 */
            ni->link[k] = j;  /* 0x177CB45 */
            for (uint32_t m = firstFree; m < 4; m++) /* 0x177CB49..0x177CBCB: the maxss chain over dist[firstFree..3] */
                if (ni->dist[m] > *maxv) *maxv = ni->dist[m];
            return;
        }
    }
}

/* After i's pairs, 0x177CC03..0x177CC84: sqrt the new slots (double sqrt, then float) and write each neighbour's back
 * link into its first empty slot, stopping at the first empty slot of node i. */
static void KnnFinish(KnnGraph* g, uint32_t i, uint32_t firstFree)
{
    KnnNode* ni = &g->nodes[i];
    for (uint32_t k = firstFree; k < 4; k++)
    {
        float d = ni->dist[k];
        if (d == 0.0f) break; /* 0x177CC1A ucomiss + je 0x177CC84 (NaN goes on to the sqrt) */
        float length = (float)sqrt((double)d); /* 0x177CC21 cvtps2pd, sqrtpd, 0x177CC45 cvtsd2ss */
        uint32_t j = ni->link[k];              /* 0x177CC3B mov edx,[rbx-0x10] */
        ni->dist[k] = length;                  /* 0x177CC49 movss [rbx],xmm1 */
        KnnNode* nj = &g->nodes[j];
        for (uint32_t m = 0; m < 4; m++) /* 0x177CC51..0x177CC6D: the neighbour's first empty slot */
        {
            if (nj->dist[m] == 0.0f)
            {
                nj->dist[m] = length; /* 0x177CC6F */
                nj->link[m] = i;      /* 0x177CC75 mov [rdx+rax*4],r15d (the node doing the linking) */
                break;
            }
        }
    }
}

/* Points whose x or y is not finite: the original may see a NaN distance for them, so they are always looked at. */

/* A point's cell along one axis. The same call builds the grid and finds i's 3 x 3, so both sides agree. */
static inline uint32_t KnnAxis(float value, double min, double inverse, uint32_t cells)
{
    double at = ((double)value - min) * inverse;
    if (!(at > 0.0)) return 0; /* a rounding miss or a zero-width axis: the first cell */
    if (at >= (double)cells) return cells - 1; /* the bounding box's last point can round past the last cell */
    return (uint32_t)at;
}

static inline uint32_t KnnCell(float x, float y, double minX, double minY, double inverse, uint32_t cellsX, uint32_t cellsY)
{
    return KnnAxis(y, minY, inverse, cellsY) * cellsX + KnnAxis(x, minX, inverse, cellsX);
}

static int KnnCompare(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* The original pass, run when the grid cannot be built: the game's own function (the hook's trampoline). */
static void KnnFallback(KnnGraph* g, const char* why)
{
    if (InterlockedIncrement(&g_knnFallbacks) == 1)
        Note("Hive Lord graph: the grid pass cannot run (%s); the original pass runs instead (noted once)", why);
    if (g_knnOriginal != NULL) g_knnOriginal(g);
}

static double KnnMs(const LARGE_INTEGER* from, const LARGE_INTEGER* to, const LARGE_INTEGER* frequency)
{
    return (double)(to->QuadPart - from->QuadPart) * 1000.0 / (double)frequency->QuadPart;
}

static void KnnNodeText(const KnnNode* node, char* text, size_t size)
{
    uint32_t dist[4];
    for (int k = 0; k < 4; k++) memcpy(&dist[k], &node->dist[k], 4);
    snprintf(text, size, "%08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)node->link[0], (unsigned)node->link[1],
        (unsigned)node->link[2], (unsigned)node->link[3], (unsigned)dist[0], (unsigned)dist[1], (unsigned)dist[2],
        (unsigned)dist[3]);
}

/* The grid replacement. */
static void KnnLinkGrid(KnnGraph* g)
{
    uint32_t count;
    float radius, r2;
    if (g == NULL) return;
    count = g->count;
    if (count == 0) return; /* 0x177CA00 test edx,edx; je the return */
    radius = g->radius;
    if (!(radius > 0.0f)) { KnnFallback(g, "radius is not above 0"); return; } /* includes a NaN radius */
    r2 = radius * radius; /* 0x177C9F7 movss + mulss: the same single rounding as the original */
    /* An infinite r2 skips no ordered pair (inf > inf is false); a zero or subnormal one loses the relative precision
     * behind the 3 x 3 cover (pairs whose d2 rounds to 0 far outside it are still visited). The original runs instead. */
    if (!isfinite(r2) || !(r2 >= KNN_MIN_R2)) { KnnFallback(g, "radius squared is not a normal positive number"); return; }

    /* The bounding box of the finite points and the always-visit list's size. */
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    uint32_t alwaysCount = 0;
    int haveFinite = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const float* p = g->pos + (size_t)i * 3;
        float x = p[0], y = p[1];
        if (!(isfinite(x) && isfinite(y))) { alwaysCount++; continue; }
        if (!haveFinite) { minX = maxX = (double)x; minY = maxY = (double)y; haveFinite = 1; }
        else
        {
            if ((double)x < minX) minX = (double)x;
            if ((double)x > maxX) maxX = (double)x;
            if ((double)y < minY) minY = (double)y;
            if ((double)y > maxY) maxY = (double)y;
        }
    }

    uint32_t cellsX = 1, cellsY = 1, cellCount = 1;
    double inverse = 0.0;
    if (haveFinite)
    {
        /* A cell a little wider than the radius: d2 is a float, so a pair can pass d2 <= r2 with |dx| a rounding step
         * above the radius; with cells exactly radius wide it could then sit two cells away and be missed. */
        double size = (double)radius * 1.001, widthX, widthY;
        widthX = (maxX - minX) / size + 1.0;
        widthY = (maxY - minY) / size + 1.0;
        if (!(widthX >= 1.0) || widthX > (double)KNN_MAX_CELLS || !(widthY >= 1.0) || widthY > (double)KNN_MAX_CELLS)
        {
            KnnFallback(g, "the grid's width is not usable");
            return;
        }
        cellsX = (uint32_t)widthX;
        cellsY = (uint32_t)widthY;
        uint64_t cells = (uint64_t)cellsX * cellsY;
        if (cells > KNN_MAX_CELLS) { KnnFallback(g, "the grid would have more than 64M cells"); return; }
        cellCount = (uint32_t)cells;
        inverse = 1.0 / size;
    }

    uint32_t finiteCount = count - alwaysCount;
    size_t startsBytes = haveFinite ? ((size_t)cellCount + 1) * sizeof(uint32_t) : 0;
    size_t indicesBytes = (size_t)finiteCount * sizeof(uint32_t);
    size_t alwaysBytes = (size_t)alwaysCount * sizeof(uint32_t);
    size_t candidatesBytes = (size_t)count * sizeof(uint32_t);
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, startsBytes + indicesBytes + alwaysBytes + candidatesBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) { KnnFallback(g, "no memory for the grid"); return; }
    uint8_t* at = block;
    uint32_t* starts = NULL;
    uint32_t* indices = NULL;
    uint32_t* always = (uint32_t*)at; /* filled below; the cell arrays come first when there are finite points */
    if (haveFinite)
    {
        starts = (uint32_t*)at;
        memset(starts, 0, startsBytes);
        at += startsBytes;
        indices = (uint32_t*)at;
        at += indicesBytes;
        always = (uint32_t*)at;
    }
    at += alwaysBytes;
    uint32_t* candidates = (uint32_t*)at;

    /* Counting sort: the always-visit list (ascending, appended in index order) and one count a cell. */
    uint32_t seen = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const float* p = g->pos + (size_t)i * 3;
        float x = p[0], y = p[1];
        if (!(isfinite(x) && isfinite(y))) { always[seen++] = i; continue; }
        if (haveFinite) starts[KnnCell(x, y, minX, minY, inverse, cellsX, cellsY) + 1]++;
    }
    if (haveFinite)
    {
        for (uint32_t c = 1; c <= cellCount; c++) starts[c] += starts[c - 1]; /* starts[c] is cell c-1's end */
        for (uint32_t i = count; i-- > 0;) /* fill backwards so each cell's indices come out ascending */
        {
            const float* p = g->pos + (size_t)i * 3;
            float x = p[0], y = p[1];
            if (!(isfinite(x) && isfinite(y))) continue;
            uint32_t c = KnnCell(x, y, minX, minY, inverse, cellsX, cellsY);
            indices[--starts[c + 1]] = i;
        }
    }

    uint32_t* scratch = candidates;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t n = 0;
        const float* p = g->pos + (size_t)i * 3;
        float x = p[0], y = p[1];
        int finite = isfinite(x) && isfinite(y);
        uint32_t firstFree = 0;
        float maxv = 0.0f; /* 0x177CA6F movaps xmm1,xmm6 (0.0f) */
        for (uint32_t k = 0; k < 4; k++) /* 0x177CA30..0x177CA66: the first slot whose dist is 0.0f */
        {
            if (g->nodes[i].dist[k] == 0.0f) break;
            firstFree++;
        }
        if (finite)
        {
            /* 0x177CA80's inner loop, limited to the 3 x 3 cells around i (plus the non-finite points). */
            uint32_t cx = KnnAxis(x, minX, inverse, cellsX);
            uint32_t cy = KnnAxis(y, minY, inverse, cellsY);
            for (int oy = -1; oy <= 1; oy++)
            {
                int row = (int)cy + oy;
                if (row < 0 || row >= (int)cellsY) continue;
                uint32_t base = (uint32_t)row * cellsX;
                for (int ox = -1; ox <= 1; ox++)
                {
                    int column = (int)cx + ox;
                    if (column < 0 || column >= (int)cellsX) continue;
                    uint32_t cell = base + (uint32_t)column;
                    /* After the fill starts[c + 1] is cell c's first index; starts[c + 2] the next cell's. */
                    uint32_t begin = starts[cell + 1];
                    uint32_t end = cell + 1 < cellCount ? starts[cell + 2] : finiteCount;
                    for (uint32_t k = begin; k < end; k++)
                    {
                        uint32_t j = indices[k];
                        if (j > i) scratch[n++] = j;
                    }
                }
            }
            for (uint32_t a = 0; a < alwaysCount; a++)
            {
                uint32_t j = always[a];
                if (j > i) scratch[n++] = j;
            }
            if (n > 1) qsort(scratch, n, sizeof scratch[0], KnnCompare); /* the asm visits ascending j */
            for (uint32_t k = 0; k < n; k++) KnnPair(g, i, scratch[k], firstFree, r2, &maxv);
        }
        else
        {
            /* This point's own distance can be NaN, which the original does not skip: every later j is visited. */
            for (uint32_t j = i + 1; j < count; j++) KnnPair(g, i, j, firstFree, r2, &maxv);
        }
        KnnFinish(g, i, firstFree); /* 0x177CC03..0x177CC84 */
    }
    VirtualFree(block, 0, MEM_RELEASE);
}

/* A research build's check: the game's own pass on a copy of the nodes, the grid on the real ones, memcmp. */
static void KnnLinkChecked(KnnGraph* g)
{
    uint32_t count = g->count;
    size_t bytes = (size_t)count * sizeof(KnnNode);
    LARGE_INTEGER frequency, start, middle, end;
    KnnNode* copy = (KnnNode*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (copy == NULL)
    {
        if (InterlockedIncrement(&g_knnCheckNotes) == 1)
            Note("Hive Lord graph: no memory to check the grid against the original (%u nodes); the grid runs alone",
                (unsigned)count);
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        KnnLinkGrid(g);
        QueryPerformanceCounter(&end);
        Note("Hive Lord graph: grid linked %u nodes in %.1f ms (no check: no memory for the copy)", (unsigned)count,
            KnnMs(&start, &end, &frequency));
        return;
    }
    memcpy(copy, g->nodes, bytes);
    KnnGraph temporary;
    memcpy(&temporary, g, sizeof temporary); /* the 0x30 bytes the function reads */
    temporary.nodes = copy;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    g_knnOriginal(&temporary); /* the game's own KnnLink, on the copied nodes */
    QueryPerformanceCounter(&middle);
    KnnLinkGrid(g);
    QueryPerformanceCounter(&end);
    if (memcmp(copy, g->nodes, bytes) == 0)
        Note("Hive Lord graph: grid result matches the original (%u nodes, %.1f ms vs %.1f ms)", (unsigned)count,
            KnnMs(&middle, &end, &frequency), KnnMs(&start, &middle, &frequency)); /* grid first, then the original */
    else
    {
        uint32_t at = 0;
        while (at < count && memcmp(&copy[at], &g->nodes[at], sizeof(KnnNode)) == 0) at++;
        char grid[128], original[128];
        KnnNodeText(&g->nodes[at], grid, sizeof grid);
        KnnNodeText(&copy[at], original, sizeof original);
        Note("Hive Lord graph: grid result differs from the original at node %u of %u: grid %s; original %s",
            (unsigned)at, (unsigned)count, grid, original);
    }
    VirtualFree(copy, 0, MEM_RELEASE);
}

/* The detour: normal maps go through untouched; an extra large world takes the grid (checked up to KNN_CHECK_NODES). */
static void KnnLinkDetour(KnnGraph* g)
{
    if (g == NULL || g_knnOriginal == NULL) return;
    if (g_world.current <= 1.0f) { g_knnOriginal(g); return; }
    uint32_t count = g->count;
    if (count == 0 || count > KNN_CHECK_NODES)
    {
        LARGE_INTEGER frequency, start, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        KnnLinkGrid(g);
        QueryPerformanceCounter(&end);
        if (count != 0)
            Note("Hive Lord graph: grid linked %u nodes in %.1f ms (no check above %u nodes)", (unsigned)count,
                KnnMs(&start, &end, &frequency), (unsigned)KNN_CHECK_NODES);
        return;
    }
    KnnLinkChecked(g);
}

/* The reviewed neighbour pass is at game.dll + 0x177C9E0: 48 8B C4 56 41 57 48 83 EC 48 8B 51 18 45 33 FF 0F 29 78 C8. */
static const char* KnnRefuse(const char* why)
{
    if (InterlockedIncrement(&g_knnRefusals) == 1)
        Note("difficulty: extra large maps: the Hive Lord graph is not on a grid: %s", why);
    return why;
}

static const char* InstallWorldPlan(uintptr_t game)
{
    if (g_knnInstalled) return NULL;
    if (game == 0) return KnnRefuse("game.dll is not loaded");
    if (!ApiCodeMatches(game + KNN_LINK, "48 8B C4 56 41 57 48 83 EC 48 8B 51 18 45 33 FF 0F 29 78 C8"))
        return KnnRefuse("the Hive Lord graph's neighbour pass is not the reviewed one (an extra large map needs it)");
    if (LoaderHook("extra large maps' Hive Lord graph", game + KNN_LINK, (void*)KnnLinkDetour, (void**)&g_knnOriginal) != 0)
        return KnnRefuse("the Hive Lord graph's neighbour pass could not be hooked");
    g_knnInstalled = 1;
    Note("difficulty: extra large maps: the Hive Lord graph links through a grid (checked against the original up to %u nodes)",
        (unsigned)KNN_CHECK_NODES);
    return NULL;
}
