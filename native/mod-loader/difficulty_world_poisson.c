/* XL-5: Poisson-disk sampling for extra large maps from a periodic tile (game.dll 0xF2D370).
 * Included by hellforge_modloader.c after difficulty_world_plan.c (it reads g_world).
 *
 * 0xF2D370 is Bridson's sampler: (rcx the generation arena, xmm1 the radius, r8 a rectangle {x0, y0, x1, y1}, r9 the PCG
 * state, [rsp+0x28] a uint32_t* receiving the count) -> the points (8 bytes each: float x, y) from the arena (0x173B8D0
 * (arena, bytes, 8)). Cells are radius / sqrt(2) (0x23C7110: 2.0), the grid and a point array of 2 x cells come from the
 * arena, and every active point tries 30 candidates (0xF2D82C) from 0xF2CEF0, which draws an annulus position with two
 * fmod-based angle reductions. Its callers give it the whole world square, so the work grows with the map's area: at
 * 16384 m GenerateExplorationLocations' call (0xF9632E) took ~65 s and ran three times. The other callers are
 * GenerateClearingPoints (0xF42D0A), SpreadLocations (0xF7C6AE), a generation job with a 10 m radius (0xF432B5; the Hive
 * Lord graph's nodes) and 0xFD0F3F.
 *
 * For an extra large level the points come from a tile instead: Bridson's algorithm run once on a periodic square of
 * POISSON_TILE x POISSON_TILE cells (a torus: distances and the grid wrap at its edges, so a point near one edge keeps the
 * radius from the points near the opposite one), then that tile repeated over the rectangle from a random offset. Copies
 * of a periodic set keep its minimum distance across every seam, and the density is Bridson's own, so the callers see the
 * same kind of set: well spread, at least the radius apart, about as many points. The repetition (every 128 cells, about
 * 90 radii) does not survive the callers' use: they thin the points by a chance each, test them against the map and keep
 * a few.
 *
 * A rectangle within one tile's size gets plain Bridson over it instead (the same code without the wrap): the cost
 * follows the call, and a torus that small would lose points along its seams.
 *
 * Order. Bridson's output grows from one random seed outward. Two callers (0xF42D0A, 0xF9632E) walk every point with a
 * chance each or shuffle them first; SpreadLocations and the job keep the order, so the tiles are emitted ring by ring
 * around a random seed tile (each tile's points in its own Bridson order) and the set still grows from one place.
 *
 * Determinism. Peers must build the same world. Everything here is integer arithmetic, the game's PCG (multiplier
 * 0x5851F42D4C957F2D, increment 0x14057B7EF767814F, output state >> 32, x 2^-32), and IEEE single/double +, -, *, / and
 * sqrt, which round the same on every x86-64 CPU and under Wine. No libm trigonometry (Windows' and Wine's sinf may
 * differ in the last bit): the annulus candidate is a rejection sample from the square around the point.
 *
 * Research: for the first POISSON_CHECK_CALLS calls whose rectangle holds at most POISSON_CHECK_CELLS cells (4 km maps),
 * the game's own sampler also runs on a copy of the PCG state; both counts and times are noted, with the tiled set's
 * minimum distance / radius (must be >= 1 up to float rounding). Larger calls note their count and time.
 */

#define POISSON_SAMPLE 0xF2D370u
#define POISSON_TILE 128u            /* cells a side of the periodic tile */
#define POISSON_TRIES 30u            /* candidates per active point (the original's 0x1E) */
#define POISSON_ANNULUS_TRIES 32u    /* square draws for one annulus candidate (59% land in it) */
#define POISSON_CHECK_CELLS 4194304u /* check mode up to this many cells in the caller's rectangle */
#define POISSON_CHECK_CALLS 6
#define POISSON_MAX_POINTS 0x40000000ull

typedef void* (*PoissonFn)(void* arena, float radius, const float* rect, uint64_t* state, uint32_t* count);
typedef void* (*PoissonAllocFn)(void* arena, uint64_t bytes, uint64_t align);
static PoissonFn g_poissonOriginal;
static PoissonAllocFn g_poissonAlloc;
static int g_poissonInstalled;
static volatile LONG g_poissonCalls, g_poissonChecks, g_poissonFallbacks;

static inline uint32_t PoissonNext(uint64_t* state)
{
    *state = *state * 0x5851F42D4C957F2Dull + 0x14057B7EF767814Full;
    return (uint32_t)(*state >> 32);
}

/* [0, 1] like the game's (cvtsi2ss of the 32-bit output, x 2^-32: rounding can give exactly 1). */
static inline float PoissonUnit(uint64_t* state)
{
    return (float)(int64_t)PoissonNext(state) * 2.3283064365386963e-10f;
}

/* A coordinate moved by less than one period back into [0, size). */
static inline float PoissonWrap(float v, float size)
{
    if (v < 0.0f) v += size;
    else if (v >= size) v -= size;
    if (!(v >= 0.0f) || v >= size) v = 0.0f; /* a rounding step onto the edge */
    return v;
}

static inline uint32_t PoissonCellOf(float v, float cell, uint32_t cells)
{
    float q = v / cell;
    uint32_t c = q > 0.0f ? (uint32_t)q : 0u;
    return c < cells ? c : cells - 1u;
}

/* Bridson over cols x rows cells of `cell` (<= radius / sqrt(2): one point a cell). Periodic: a torus of exactly
 * cols x cell by rows x cell (distances and the grid wrap at its edges). Not periodic: the rectangle [0, w] x [0, h]
 * (cols x cell >= w), candidates strictly inside it, as the game's (0xF2D6AC..0xF2D6E2). Returns the point count; the
 * points in *out, malloc'ed; 0 and *out NULL when memory is short. */
static uint32_t PoissonBuild(float radius, float cell, uint32_t cols, uint32_t rows, float w, float h, int periodic,
    uint64_t* state, float** out)
{
    *out = NULL;
    size_t cells = (size_t)cols * rows;
    float halfW = w * 0.5f, halfH = h * 0.5f;
    float r2 = radius * radius, outer = 4.0f * r2;
    int32_t* grid = (int32_t*)malloc(cells * sizeof *grid);
    float* xy = (float*)malloc(cells * 2u * sizeof *xy);
    uint32_t* active = (uint32_t*)malloc(cells * sizeof *active);
    if (grid == NULL || xy == NULL || active == NULL) { free(grid); free(xy); free(active); return 0; }
    memset(grid, 0xFF, cells * sizeof *grid);
    uint32_t count = 0, live = 0;
    {
        float x = PoissonUnit(state) * w, y = PoissonUnit(state) * h;
        if (periodic) { x = PoissonWrap(x, w); y = PoissonWrap(y, h); }
        grid[(size_t)PoissonCellOf(y, cell, rows) * cols + PoissonCellOf(x, cell, cols)] = 0;
        xy[0] = x; xy[1] = y;
        active[live++] = count++;
    }
    while (live > 0)
    {
        uint32_t k = PoissonNext(state) % live;
        uint32_t p = active[k];
        float px = xy[2u * p], py = xy[2u * p + 1u];
        int placed = 0;
        for (uint32_t t = 0; t < POISSON_TRIES && !placed; t++)
        {
            float dx = 0.0f, dy = 0.0f;
            int inside = 0;
            for (uint32_t a = 0; a < POISSON_ANNULUS_TRIES && !inside; a++)
            {
                dx = (PoissonUnit(state) * 4.0f - 2.0f) * radius;
                dy = (PoissonUnit(state) * 4.0f - 2.0f) * radius;
                float d2 = dx * dx + dy * dy;
                inside = d2 >= r2 && d2 <= outer;
            }
            if (!inside) continue;
            float cx = px + dx, cy = py + dy;
            if (periodic) { cx = PoissonWrap(cx, w); cy = PoissonWrap(cy, h); }
            else if (!(cx > 0.0f && cy > 0.0f && cx < w && cy < h)) continue;
            uint32_t gx = PoissonCellOf(cx, cell, cols), gy = PoissonCellOf(cy, cell, rows);
            if (grid[(size_t)gy * cols + gx] >= 0) continue;
            int close = 0;
            for (int oy = -2; oy <= 2 && !close; oy++)
            {
                int64_t ny = (int64_t)gy + oy;
                if (periodic) ny = (ny + rows) % rows;
                else if (ny < 0 || ny >= rows) continue;
                for (int ox = -2; ox <= 2 && !close; ox++)
                {
                    int64_t nx = (int64_t)gx + ox;
                    if (periodic) nx = (nx + cols) % cols;
                    else if (nx < 0 || nx >= cols) continue;
                    int32_t q = grid[(size_t)ny * cols + (size_t)nx];
                    if (q < 0) continue;
                    float ex = cx - xy[2u * (uint32_t)q], ey = cy - xy[2u * (uint32_t)q + 1u];
                    if (periodic)
                    {
                        if (ex > halfW) ex -= w; else if (ex < -halfW) ex += w;
                        if (ey > halfH) ey -= h; else if (ey < -halfH) ey += h;
                    }
                    close = ex * ex + ey * ey < r2;
                }
            }
            if (close) continue;
            grid[(size_t)gy * cols + gx] = (int32_t)count;
            xy[2u * count] = cx; xy[2u * count + 1u] = cy;
            active[live++] = count++;
            placed = 1;
        }
        if (!placed) active[k] = active[--live];
    }
    free(grid);
    free(active);
    *out = xy;
    return count;
}

/* floor(v) as an integer, without libm. */
static inline int64_t PoissonFloor(double v)
{
    int64_t i = (int64_t)v;
    return (double)i > v ? i - 1 : i;
}

typedef struct PoissonLayout
{
    double ox, oy, size;       /* the tiles' origin and period */
    int64_t i0, i1, j0, j1;    /* the tiles that reach the rectangle */
    int64_t si, sj;            /* the seed tile */
} PoissonLayout;

/* The tile's copies ring by ring around the seed tile; with `out` NULL only counts. The rectangle test is the
 * original's (0xF2D6AC..0xF2D6E2: strictly inside on both axes). */
static uint64_t PoissonEmit(const PoissonLayout* l, const float* tile, uint32_t tileCount, const float* rect, float* out)
{
    uint64_t n = 0;
    int64_t rings = l->si - l->i0;
    if (l->i1 - l->si > rings) rings = l->i1 - l->si;
    if (l->sj - l->j0 > rings) rings = l->sj - l->j0;
    if (l->j1 - l->sj > rings) rings = l->j1 - l->sj;
    for (int64_t d = 0; d <= rings; d++)
    {
        for (int64_t j = l->sj - d; j <= l->sj + d; j++)
        {
            if (j < l->j0 || j > l->j1) continue;
            int64_t edge = j == l->sj - d || j == l->sj + d;
            for (int64_t i = l->si - d; i <= l->si + d; i += edge || d == 0 ? 1 : 2 * d)
            {
                if (i < l->i0 || i > l->i1) continue;
                double bx = l->ox + (double)i * l->size, by = l->oy + (double)j * l->size;
                for (uint32_t p = 0; p < tileCount; p++)
                {
                    float x = (float)(bx + (double)tile[2u * p]), y = (float)(by + (double)tile[2u * p + 1u]);
                    if (!(x > rect[0] && y > rect[1] && rect[2] > x && rect[3] > y)) continue;
                    if (out != NULL) { out[2u * n] = x; out[2u * n + 1u] = y; }
                    n++;
                }
            }
        }
    }
    return n;
}

/* The tiled set for one call; NULL (and nothing drawn from the state) when it cannot be made, so the caller can fall
 * back to the original. */
static void* PoissonTiled(void* arena, float radius, const float* rect, uint64_t* state, uint32_t* count, uint64_t* made)
{
    float cell = radius / sqrtf(2.0f);
    float w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (!(cell > 0.0f) || !(w > 0.0f) || !(h > 0.0f) || !(w / cell < 2.0e9f) || !(h / cell < 2.0e9f)) return NULL;
    uint64_t local = *state;
    float* tile = NULL;
    float longer = w > h ? w : h;
    if (longer / cell + 1.0f < (float)POISSON_TILE)
    {
        /* A rectangle within one tile: plain Bridson over it (a torus that small would lose points at its seams). */
        uint32_t cols = (uint32_t)(w / cell) + 1u, rows = (uint32_t)(h / cell) + 1u;
        uint32_t made0 = PoissonBuild(radius, cell, cols, rows, w, h, 0, &local, &tile);
        if (tile == NULL) return NULL;
        float* points = (float*)g_poissonAlloc(arena, (made0 != 0 ? made0 : 1u) * 8u, 8u);
        if (points == NULL) { free(tile); return NULL; }
        uint32_t n = 0;
        for (uint32_t p = 0; p < made0; p++)
        {
            float x = rect[0] + tile[2u * p], y = rect[1] + tile[2u * p + 1u];
            if (!(x > rect[0] && y > rect[1] && rect[2] > x && rect[3] > y)) continue;
            points[2u * n] = x; points[2u * n + 1u] = y;
            n++;
        }
        free(tile);
        *state = local;
        *count = n;
        *made = n;
        return points;
    }
    float size = cell * (float)POISSON_TILE;
    uint32_t tileCount = PoissonBuild(radius, cell, POISSON_TILE, POISSON_TILE, size, size, 1, &local, &tile);
    if (tile == NULL) return NULL;
    PoissonLayout l;
    l.size = (double)size;
    l.ox = (double)rect[0] - (double)PoissonUnit(&local) * l.size;
    l.oy = (double)rect[1] - (double)PoissonUnit(&local) * l.size;
    l.i0 = PoissonFloor(((double)rect[0] - l.ox) / l.size); l.i1 = PoissonFloor(((double)rect[2] - l.ox) / l.size);
    l.j0 = PoissonFloor(((double)rect[1] - l.oy) / l.size); l.j1 = PoissonFloor(((double)rect[3] - l.oy) / l.size);
    l.si = l.i0 + (int64_t)(PoissonNext(&local) % (uint32_t)(l.i1 - l.i0 + 1));
    l.sj = l.j0 + (int64_t)(PoissonNext(&local) % (uint32_t)(l.j1 - l.j0 + 1));
    uint64_t n = PoissonEmit(&l, tile, tileCount, rect, NULL);
    if (n > POISSON_MAX_POINTS) { free(tile); return NULL; }
    float* points = (float*)g_poissonAlloc(arena, (n != 0 ? n : 1u) * 8u, 8u);
    if (points == NULL) { free(tile); return NULL; }
    PoissonEmit(&l, tile, tileCount, rect, points);
    free(tile);
    *state = local;
    *count = (uint32_t)n;
    *made = n;
    return points;
}

/* Check mode: the smallest distance between two of the points over the radius (a grid of radius / sqrt(2) cells over
 * the rectangle; 5 x 5 neighbours cover the radius). Returns -1 when the grid could not be made. */
static double PoissonMinRatio(const float* points, uint32_t n, float radius, const float* rect)
{
    double cell = (double)radius / 1.4142135623730951;
    uint32_t cols = (uint32_t)(((double)rect[2] - rect[0]) / cell) + 1u, rows = (uint32_t)(((double)rect[3] - rect[1]) / cell) + 1u;
    size_t cells = (size_t)cols * rows;
    int32_t* head = (int32_t*)malloc(cells * sizeof *head);
    int32_t* next = (int32_t*)malloc(((size_t)n + 1u) * sizeof *next);
    if (head == NULL || next == NULL) { free(head); free(next); return -1.0; }
    memset(head, 0xFF, cells * sizeof *head);
    double best = 1e300;
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t cx = (uint32_t)(((double)points[2u * i] - rect[0]) / cell), cy = (uint32_t)(((double)points[2u * i + 1u] - rect[1]) / cell);
        if (cx >= cols) cx = cols - 1u;
        if (cy >= rows) cy = rows - 1u;
        for (int oy = -2; oy <= 2; oy++)
            for (int ox = -2; ox <= 2; ox++)
            {
                int64_t nx = (int64_t)cx + ox, ny = (int64_t)cy + oy;
                if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
                for (int32_t q = head[(size_t)ny * cols + (size_t)nx]; q >= 0; q = next[q])
                {
                    double ex = (double)points[2u * i] - points[2u * (uint32_t)q], ey = (double)points[2u * i + 1u] - points[2u * (uint32_t)q + 1u];
                    double d = sqrt(ex * ex + ey * ey);
                    if (d < best) best = d;
                }
            }
        next[i] = head[(size_t)cy * cols + cx];
        head[(size_t)cy * cols + cx] = (int32_t)i;
    }
    free(head);
    free(next);
    return n < 2 ? 0.0 : best / (double)radius;
}

static double PoissonMs(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)hz.QuadPart;
}

static void* PoissonDetour(void* arena, float radius, const float* rect, uint64_t* state, uint32_t* count)
{
    if (!(g_world.current > 1.0f) || rect == NULL || state == NULL || count == NULL)
        return g_poissonOriginal(arena, radius, rect, state, count);
    /* The original sizes its grid as the rectangle over radius / sqrt(2) in 32-bit integers, unchecked: a rectangle that
     * is not finite or a radius that is not positive and finite overflows it and writes far outside (a terrain tile's
     * route cell, 2026-11-02: game.dll+0xF2D5B0). Such a call gets no points; the first few are noted. */
    if (!(radius > 0.0f && radius < 1e6f) || !(rect[2] - rect[0] >= 0.0f && rect[2] - rect[0] < 1e7f) || !(rect[3] - rect[1] >= 0.0f && rect[3] - rect[1] < 1e7f) ||
        (double)(rect[2] - rect[0]) / ((double)radius / 1.4142135623730951) > 1e6 || (double)(rect[3] - rect[1]) / ((double)radius / 1.4142135623730951) > 1e6)
    {
        static volatile LONG refused;
        if (InterlockedIncrement(&refused) <= 8)
            Note("difficulty: Poisson: refused a call with radius %g over (%g, %g)..(%g, %g) from game+0x%llX", (double)radius, (double)rect[0], (double)rect[1],
                (double)rect[2], (double)rect[3], (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_world.game));
        *count = 0;
        return g_poissonAlloc(arena, 8u, 8u);
    }
    LONG call = InterlockedIncrement(&g_poissonCalls);
    double cell = (double)radius / 1.4142135623730951;
    double cells = cell > 0.0 ? ((double)rect[2] - rect[0]) / cell * (((double)rect[3] - rect[1]) / cell) : 0.0;
    uint32_t originalCount = 0;
    double originalMs = -1.0;
    if (cells > 0.0 && cells <= (double)POISSON_CHECK_CELLS && InterlockedIncrement(&g_poissonChecks) <= POISSON_CHECK_CALLS)
    {
        uint64_t copy = *state;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        g_poissonOriginal(arena, radius, rect, &copy, &originalCount);
        QueryPerformanceCounter(&b);
        originalMs = PoissonMs(a, b);
    }
    LARGE_INTEGER a, b;
    uint64_t made = 0;
    QueryPerformanceCounter(&a);
    void* points = PoissonTiled(arena, radius, rect, state, count, &made);
    QueryPerformanceCounter(&b);
    if (points == NULL)
    {
        if (InterlockedIncrement(&g_poissonFallbacks) <= 4)
            Note("difficulty: extra large maps: Poisson call %ld (radius %.2f, %.0f x %.0f m) fell back to the game's sampler", (long)call,
                (double)radius, (double)(rect[2] - rect[0]), (double)(rect[3] - rect[1]));
        return g_poissonOriginal(arena, radius, rect, state, count);
    }
    if (originalMs >= 0.0)
        Note("difficulty: extra large maps: Poisson call %ld (radius %.2f, %.0f x %.0f m): tiled %llu points in %.1f ms (min distance %.4f radii), the game's %u in %.1f ms",
            (long)call, (double)radius, (double)(rect[2] - rect[0]), (double)(rect[3] - rect[1]), (unsigned long long)made, PoissonMs(a, b),
            PoissonMinRatio((const float*)points, (uint32_t)made, radius, rect), (unsigned)originalCount, originalMs);
    else if (call <= 4 || PoissonMs(a, b) > 100.0)
        Note("difficulty: extra large maps: Poisson call %ld (radius %.2f, %.0f x %.0f m): tiled %llu points in %.1f ms", (long)call,
            (double)radius, (double)(rect[2] - rect[0]), (double)(rect[3] - rect[1]), (unsigned long long)made, PoissonMs(a, b));
    return points;
}

static const char* InstallWorldPoisson(uintptr_t game)
{
    if (g_poissonInstalled) return NULL;
    if (game == 0) return "game.dll is not loaded";
    char off[8];
    if (GetEnvironmentVariableA("HELLFORGE_XL_NO_POISSON", off, sizeof off) != 0) return "HELLFORGE_XL_NO_POISSON is set (research A/B runs)";
    if (!ApiCodeMatches(game + POISSON_SAMPLE, "48 8B C4 4C 89 48 20 55 56 57 41 54 41 55 41 56 48 81 EC 98 00 00 00 F3 0F 10 15") ||
        !ApiCodeMatches(game + DIF_ARENA_ALLOCATE, "40 56 41 56 48 83 EC 38"))
        return "the Poisson-disk sampler is not the reviewed one (an extra large map needs it)";
    g_poissonAlloc = (PoissonAllocFn)(game + DIF_ARENA_ALLOCATE);
    if (LoaderHook("extra large maps' Poisson-disk sampler", game + POISSON_SAMPLE, (void*)PoissonDetour, (void**)&g_poissonOriginal) != 0)
        return "the Poisson-disk sampler could not be hooked";
    g_poissonInstalled = 1;
    Note("difficulty: extra large maps: Poisson-disk sets come from a %u-cell periodic tile (checked against the game's sampler for %d calls up to %u cells)",
        POISSON_TILE, POISSON_CHECK_CALLS, POISSON_CHECK_CELLS);
    return NULL;
}
