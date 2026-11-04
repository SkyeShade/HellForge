/* ---- Extra large maps: km-scale relief (h_macro) -------------------------------------------------------------------
 * A 16 km level's terrain is vanilla relief (about -30..120 m) repeated over the whole map: flat from afar. h_macro adds
 * ranges, basins and valleys at kilometre scale (0..MACRO_MAX m, slopes clamped), drawn into the level's own terrain
 * merge so every later pass follows it (static, KB xl-world-streaming "km-scale relief" and "h_macro masks"):
 *
 * - Where: additively (material 0x13DD2DCAD2968560, as NearRingDraw / NearConformDraw, one gui job a batch) after the
 *   level's 0x3C and every composite tile, before 0x40 runs (TerrainStep's end, the level waits). The captures
 *   (0x5B/0x5C, 0x5D/0x5E), 0x3E, 0x3F, 0x0C, 0x41/0x42, the final u16, collision, scatter, the layer heightfield and the
 *   near tiles (their base is the level's capture) follow it. Caps (0x3C's MAX of a constant) stay relative: max(base,
 *   c) + macro. In a near tile's replay the composite cells' caps are lifted by h_macro instead (TerrainTri), since its
 *   base already holds it.
 * - G+0x1F8A30 (vertex heights) stays vanilla: rules, the generation hash and pathfinding costs are unchanged. The stamp
 *   records' +0xC0 (height / 512, read only by 0x6C at 0xF46C26) get h_macro / 512 once before the level's first 0x6C,
 *   so stamp layers stand on the raised ground.
 * - Held at 0 (with a smooth fall-off over MACRO_FALL m) near water: corners (G+0x9F310, 0x48, xy +0; count
 *   G+0x11A7090) whose type (G+0x21AE88 into [[level+0x2D0]]) has +0xE4 > -500, corners below the sea plane
 *   ([[settings+0xB0]+0x44]) + MACRO_SEA by their vanilla height (G+0x1F8A30 x 512), and under pads whose zone type has
 *   an absolute height (+0x204 != FLT_MAX: 0x3F would put them at that height whatever the ground).
 * - Held constant (the value at the centre) over the important locations (G+0x8996B0, 0x108, xy +0xA4; count
 *   G+0x11A715C) and the flatten zones (subregions whose type has +0x208 != FLT_MAX: their route cell's box +0x88..+0x94
 *   grown by +0x208), so pads and flattened areas sit level.
 * - Slopes: at most MACRO_SLOPE degrees, MACRO_ROAD on the routes 0x0D draws (G+0x937348, points G+0x9568E4), by a
 *   two-sided Lipschitz projection on the grid with the held nodes fixed.
 * - Seed: u32 [level+0x8], the level descriptor's first seed (kept across FixUnreachableAreas restarts); every peer
 *   builds the same level from the same descriptor, so every peer draws the same field.
 * Coordinates: generator metres from the map's corner (world + L/2, no flip); the grid is MACRO_N x MACRO_N nodes,
 * MACRO_STEP apart, node (i, j) at (i, j) x MACRO_STEP. HELLFORGE_XL_MACRO=1 turns it on (off until confirmed in game). */
#define MACRO_N 257
#define MACRO_STEP 64.0f
#define MACRO_MAX 340.0f   /* metres above the vanilla relief at most (vanilla -30..120: the u16 holds -500..500) */
#define MACRO_SLOPE 34.0f  /* degrees, anywhere */
#define MACRO_ROAD 9.0f    /* degrees, on the drawn routes */
#define MACRO_FALL 900.0f  /* metres from a held-at-0 area to full height */
#define MACRO_WET 256.0f   /* metres around a wet corner held at 0 (corners stand ~300 m apart on the coarse plan) */
#define MACRO_SEA 6.0f     /* metres above the sea plane still held at 0 */
#define MACRO_HOLD 220.0f  /* metres around a location held flat */
#define MACRO_RAMP 260.0f  /* metres over which a held plateau blends into the field */

static struct
{
    LONG level;          /* g_world.levels the field is for (-1: none) */
    int ready, drawn, phase;
    uint32_t next, waits;
    uint32_t seed;
    float* h;            /* MACRO_N x MACRO_N, row j at y = j x MACRO_STEP */
    uint8_t* fixed;      /* 1 held at 0, 2 held at a plateau's value */
    float* slope;        /* the node's slope limit, metres per metre */
    LONG stampsLevel;
    double ms;
} g_macro = {.level = -1, .stampsLevel = -1};
static float g_macroH[MACRO_N * MACRO_N], g_macroSlope[MACRO_N * MACRO_N];
static uint8_t g_macroFixed[MACRO_N * MACRO_N];

static int MacroOn(void)
{
    static int on = -1;
    /* off until confirmed in game: HELLFORGE_XL_MACRO=1 turns it on */
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_XL_MACRO", t, sizeof t) > 0 && t[0] == '1'; }
    return on;
}

/* h_macro at a point in generator metres (0 when there is no field for this level). */
static float MacroAt(float gx, float gy)
{
    if (!g_macro.ready || g_macro.level != g_world.levels) return 0.0f;
    float fx = gx / MACRO_STEP, fy = gy / MACRO_STEP;
    if (!(fx >= 0.0f)) fx = 0.0f; if (!(fy >= 0.0f)) fy = 0.0f;
    if (fx > MACRO_N - 1.001f) fx = MACRO_N - 1.001f; if (fy > MACRO_N - 1.001f) fy = MACRO_N - 1.001f;
    int i = (int)fx, j = (int)fy;
    float ax = fx - i, ay = fy - j;
    const float* r0 = g_macro.h + (size_t)j * MACRO_N, *r1 = r0 + MACRO_N;
    return (r0[i] * (1 - ax) + r0[i + 1] * ax) * (1 - ay) + (r1[i] * (1 - ax) + r1[i + 1] * ax) * ay;
}

static uint32_t MacroHash(int32_t a, int32_t b, uint32_t s)
{
    uint32_t x = (uint32_t)a * 0x27D4EB2Du ^ (uint32_t)b * 0x165667B1u ^ s * 0x9E3779B9u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

/* Value noise in [-1, 1], quintic, lattice units. */
static float MacroValue(float x, float y, uint32_t s)
{
    float fx = floorf(x), fy = floorf(y);
    int32_t xi = (int32_t)fx, yi = (int32_t)fy;
    float u = x - fx, v = y - fy;
    u = u * u * u * (u * (u * 6 - 15) + 10); v = v * v * v * (v * (v * 6 - 15) + 10);
    const float k = 1.0f / 4294967295.0f;
    float a = MacroHash(xi, yi, s) * k, b = MacroHash(xi + 1, yi, s) * k, c = MacroHash(xi, yi + 1, s) * k, d = MacroHash(xi + 1, yi + 1, s) * k;
    return 2.0f * ((a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v) - 1.0f;
}

static float MacroFbm(float x, float y, uint32_t s, int octaves)
{
    float t = 0, amp = 1, f = 1, norm = 0;
    for (int o = 0; o < octaves; o++) { t += amp * MacroValue(x * f, y * f, s + (uint32_t)o * 101u); norm += amp; amp *= 0.5f; f *= 2.0f; }
    return t / norm;
}

/* The shaped field before the holds: domain-warped ranges (4-8 km) with ridged detail (~1.8 km), valleys flat-bottomed
 * by the power, 0..1 (offline prototype: scratch hmacro/proto.py). */
static float MacroShape(float gx, float gy, uint32_t seed)
{
    const float L = 4096.0f;
    float x = gx / L, y = gy / L;
    float wx = MacroFbm(x, y, seed + 7u, 3) * 0.35f, wy = MacroFbm(x, y, seed + 13u, 3) * 0.35f;
    x += wx; y += wy;
    float base = MacroFbm(x, y, seed, 2);
    float ridge = 1.0f - fabsf(MacroFbm(x * 2.2f, y * 2.2f, seed + 3u, 4));
    ridge *= ridge;
    float m = (base + 0.15f) / 0.7f; m = m < 0 ? 0 : m > 1 ? 1 : m; m = m * m * (3 - 2 * m);
    float h = m * (0.55f * ridge + 0.45f * (base * 0.5f + 0.5f));
    return h > 0 ? powf(h, 1.4f) : 0.0f;
}

/* Marks the nodes within r of (gx, gy): the callback gets the node and its distance. */
#define MACRO_FOR_NODES(gx, gy, r, BODY) \
    do { int i0_ = (int)floorf(((gx) - (r)) / MACRO_STEP), i1_ = (int)ceilf(((gx) + (r)) / MACRO_STEP); \
         int j0_ = (int)floorf(((gy) - (r)) / MACRO_STEP), j1_ = (int)ceilf(((gy) + (r)) / MACRO_STEP); \
         if (i0_ < 0) i0_ = 0; if (j0_ < 0) j0_ = 0; if (i1_ > MACRO_N - 1) i1_ = MACRO_N - 1; if (j1_ > MACRO_N - 1) j1_ = MACRO_N - 1; \
         for (int j_ = j0_; j_ <= j1_; j_++) for (int i_ = i0_; i_ <= i1_; i_++) { \
             float dx_ = i_ * MACRO_STEP - (gx), dy_ = j_ * MACRO_STEP - (gy), d_ = sqrtf(dx_ * dx_ + dy_ * dy_); \
             if (d_ > (r)) continue; size_t n_ = (size_t)j_ * MACRO_N + i_; (void)n_; BODY } } while (0)

/* The type descriptor of type index t, 0 when there is none. */
static uintptr_t MacroType(uintptr_t table, uint32_t count, int32_t t)
{
    uintptr_t d = 0;
    if (table == 0 || t < 0 || (uint32_t)t >= count || SafeRead(table + 8ull * (uint32_t)t, &d, 8)) return 0;
    return d;
}

/* The corner nearest a point (brute force over the corners; at most a few hundred calls a level). */
static int32_t MacroNearestCorner(uintptr_t gen, uint32_t corners, float gx, float gy)
{
    int32_t best = -1; float bd = 1e30f;
    for (uint32_t c = 0; c < corners; c++)
    {
        float xy[2];
        if (SafeRead(gen + 0x9F310u + 0x48ull * c, xy, 8)) break;
        float dx = xy[0] - gx, dy = xy[1] - gy, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = (int32_t)c; }
    }
    return best;
}

/* The field for this level: 1 when made. */
static int MacroPlan(uintptr_t gen, uint64_t context)
{
    LARGE_INTEGER hz, t0, t1;
    QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&t0);
    g_macro.ready = 0;
    g_macro.h = g_macroH; g_macro.fixed = g_macroFixed; g_macro.slope = g_macroSlope;
    uint64_t settings = 0, P = 0, ts = 0, table = 0;
    uint32_t size = 0, seed = 0, types = 0, corners = 0, typeCount = 0, heightCount = 0, locs = 0, subs = 0, routes = 0, points = 0;
    float sea = -1000.0f, worldL = 0;
    if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(settings + 0x2C, &size, 4) || size != 16384u ||
        SafeRead(g_worldLevel + 0x8, &seed, 4) || SafeRead(g_worldLevel + 0x1E0, &worldL, 4) || SafeRead(g_worldLevel + 0x2D0, &ts, 8) || ts == 0 ||
        SafeRead(ts, &table, 8) || SafeRead(ts + 0xA0, &types, 4) || SafeRead(gen + 0x11A7090u, &corners, 4) || SafeRead(gen + 0x11A70D0u, &typeCount, 4) ||
        SafeRead(gen + 0x11A70C0u, &heightCount, 4) || SafeRead(gen + 0x11A715Cu, &locs, 4) || SafeRead(gen + 0x11A71C4u, &subs, 4) ||
        SafeRead(gen + 0x11A71CCu, &routes, 4) || SafeRead(gen + 0x11A71D4u, &points, 4))
    { Note("difficulty: relief: the level's data did not read; no km-scale relief"); return 0; }
    if (!SafeRead(settings + 0xB0, &P, 8) && P != 0) SafeRead(P + 0x44, &sea, 4);
    if (corners > 5849) corners = 5849;
    if (locs > 256) locs = 256;
    if (subs > 1024) subs = 1024;
    g_macro.seed = seed;
    const float W = 16384.0f, sMax = tanf(MACRO_SLOPE * 3.14159265f / 180.0f), sRoad = tanf(MACRO_ROAD * 3.14159265f / 180.0f);
    static float D[MACRO_N * MACRO_N], hold[MACRO_N * MACRO_N], holdW[MACRO_N * MACRO_N];
    memset(g_macro.fixed, 0, (size_t)MACRO_N * MACRO_N);
    for (size_t n = 0; n < (size_t)MACRO_N * MACRO_N; n++) { D[n] = 1e9f; hold[n] = 0; holdW[n] = 0; g_macro.slope[n] = sMax; }
    /* held at 0: wet corners, corners at the sea, the map's edge */
    uint32_t wet = 0, low = 0;
    for (uint32_t c = 0; c < corners; c++)
    {
        float xy[2]; int32_t t = -1; float h512 = 0;
        if (SafeRead(gen + 0x9F310u + 0x48ull * c, xy, 8)) break;
        int isWet = 0;
        if (c < typeCount && !SafeRead(gen + 0x21AE88u + 4ull * c, &t, 4))
        { uintptr_t d = MacroType((uintptr_t)table, types, t); float level = -1000.0f; if (d && !SafeRead(d + 0xE4, &level, 4) && level > -500.0f) { isWet = 1; wet++; } }
        if (!isWet && sea > -500.0f && c < heightCount && !SafeRead(gen + 0x1F8A30u + 4ull * c, &h512, 4) && h512 * 512.0f < sea + MACRO_SEA) { isWet = 1; low++; }
        if (isWet) MACRO_FOR_NODES(xy[0], xy[1], MACRO_WET, { D[n_] = 0; g_macro.fixed[n_] = 1; });
    }
    for (int k = 0; k < MACRO_N; k++)
    {
        size_t e[4] = {(size_t)k, (size_t)(MACRO_N - 1) * MACRO_N + k, (size_t)k * MACRO_N, (size_t)k * MACRO_N + MACRO_N - 1};
        for (int q = 0; q < 4; q++) { D[e[q]] = 0; g_macro.fixed[e[q]] = 1; }
    }
    /* held at 0: pads with an absolute height (the zone type of the location's nearest corner, as 0x3F looks it up) */
    uint32_t absolute = 0, held = 0, flat = 0;
    float locXY[256][2]; uint8_t locAbs[256];
    for (uint32_t l = 0; l < locs; l++)
    {
        locAbs[l] = 0; locXY[l][0] = locXY[l][1] = -1.0f;
        if (SafeRead(gen + g_importantList + 0x108ull * l + 0xA4, locXY[l], 8) || !isfinite(locXY[l][0]) || !isfinite(locXY[l][1])) { locXY[l][0] = -1.0f; continue; }
        int32_t c = MacroNearestCorner(gen, corners, locXY[l][0], locXY[l][1]), t = -1;
        float pad = 3.4e38f;
        if (c >= 0 && (uint32_t)c < typeCount && !SafeRead(gen + 0x21AE88u + 4ull * (uint32_t)c, &t, 4))
        { uintptr_t d = MacroType((uintptr_t)table, types, t); if (d && !SafeRead(d + 0x204, &pad, 4) && pad < 3.0e38f) locAbs[l] = 1; }
        if (locAbs[l]) { absolute++; MACRO_FOR_NODES(locXY[l][0], locXY[l][1], MACRO_HOLD, { D[n_] = 0; g_macro.fixed[n_] = 1; }); }
    }
    /* the fall-off: distance to the nearest node held at 0 (chamfer, two passes each way) */
    const float d1 = MACRO_STEP, d2 = MACRO_STEP * 1.41421356f;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int j = 0; j < MACRO_N; j++)
            for (int i = 0; i < MACRO_N; i++)
            {
                size_t n = (size_t)j * MACRO_N + i; float b = D[n];
                if (i > 0 && D[n - 1] + d1 < b) b = D[n - 1] + d1;
                if (j > 0 && D[n - MACRO_N] + d1 < b) b = D[n - MACRO_N] + d1;
                if (i > 0 && j > 0 && D[n - MACRO_N - 1] + d2 < b) b = D[n - MACRO_N - 1] + d2;
                if (i < MACRO_N - 1 && j > 0 && D[n - MACRO_N + 1] + d2 < b) b = D[n - MACRO_N + 1] + d2;
                D[n] = b;
            }
        for (int j = MACRO_N - 1; j >= 0; j--)
            for (int i = MACRO_N - 1; i >= 0; i--)
            {
                size_t n = (size_t)j * MACRO_N + i; float b = D[n];
                if (i < MACRO_N - 1 && D[n + 1] + d1 < b) b = D[n + 1] + d1;
                if (j < MACRO_N - 1 && D[n + MACRO_N] + d1 < b) b = D[n + MACRO_N] + d1;
                if (i < MACRO_N - 1 && j < MACRO_N - 1 && D[n + MACRO_N + 1] + d2 < b) b = D[n + MACRO_N + 1] + d2;
                if (i > 0 && j < MACRO_N - 1 && D[n + MACRO_N - 1] + d2 < b) b = D[n + MACRO_N - 1] + d2;
                D[n] = b;
            }
    }
    /* the shaped field with its fall-off */
    for (int j = 0; j < MACRO_N; j++)
        for (int i = 0; i < MACRO_N; i++)
        {
            size_t n = (size_t)j * MACRO_N + i;
            if (g_macro.fixed[n]) { g_macro.h[n] = 0; continue; }
            float t = D[n] / MACRO_FALL; t = t > 1 ? 1 : t; t = t * t * (3 - 2 * t);
            g_macro.h[n] = MACRO_MAX * MacroShape(i * MACRO_STEP, j * MACRO_STEP, seed) * t;
        }
    /* held flat: locations (not the absolute ones) and flatten zones, at their centre's value, blended over the ramp */
    for (uint32_t l = 0; l < locs; l++)
    {
        if (locXY[l][0] < 0 || locAbs[l]) continue;
        int ci = (int)lroundf(locXY[l][0] / MACRO_STEP), cj = (int)lroundf(locXY[l][1] / MACRO_STEP);
        if (ci < 0 || cj < 0 || ci >= MACRO_N || cj >= MACRO_N) continue;
        float centre = g_macro.h[(size_t)cj * MACRO_N + ci];
        held++;
        MACRO_FOR_NODES(locXY[l][0], locXY[l][1], MACRO_HOLD + MACRO_RAMP, {
            float w = d_ <= MACRO_HOLD ? 1.0f : 1.0f - (d_ - MACRO_HOLD) / MACRO_RAMP; w = w * w * (3 - 2 * w);
            if (w > holdW[n_]) { holdW[n_] = w; hold[n_] = centre; }
            if (d_ <= MACRO_HOLD && g_macro.fixed[n_] != 1) g_macro.fixed[n_] = 2; });
    }
    for (uint32_t s = 0; s < subs; s++)
    {
        int32_t t = -1; uint64_t rc = 0; float margin = 3.4e38f, box[4];
        if (SafeRead(gen + 0x92F328u + 0x20ull * s + 0xC, &t, 4) || SafeRead(gen + 0x92F328u + 0x20ull * s + 0x18, &rc, 8) || rc == 0) continue;
        uintptr_t d = MacroType((uintptr_t)table, types, t);
        if (d == 0 || SafeRead(d + 0x208, &margin, 4) || !(margin < 3.0e38f) || margin < 0 || margin > 2000.0f || SafeRead(rc + 0x88, box, 16)) continue;
        float x0 = fminf(box[0], box[2]) - margin, x1 = fmaxf(box[0], box[2]) + margin, y0 = fminf(box[1], box[3]) - margin, y1 = fmaxf(box[1], box[3]) + margin;
        if (!(x1 - x0 < 6000.0f && y1 - y0 < 6000.0f)) continue;
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, r = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) / 2;
        int ci = (int)lroundf(cx / MACRO_STEP), cj = (int)lroundf(cy / MACRO_STEP);
        if (ci < 0 || cj < 0 || ci >= MACRO_N || cj >= MACRO_N) continue;
        float centre = g_macro.h[(size_t)cj * MACRO_N + ci];
        flat++;
        MACRO_FOR_NODES(cx, cy, r + MACRO_RAMP, {
            float w = d_ <= r ? 1.0f : 1.0f - (d_ - r) / MACRO_RAMP; w = w * w * (3 - 2 * w);
            if (w > holdW[n_]) { holdW[n_] = w; hold[n_] = centre; }
            if (d_ <= r && g_macro.fixed[n_] != 1) g_macro.fixed[n_] = 2; });
    }
    for (size_t n = 0; n < (size_t)MACRO_N * MACRO_N; n++)
        if (g_macro.fixed[n] != 1 && holdW[n] > 0) g_macro.h[n] = g_macro.h[n] * (1 - holdW[n]) + hold[n] * holdW[n];
    /* the drawn routes: their nodes' slope limit (0x0D's rule: points, not class 2, no flag of 0x103) */
    uint32_t roadNodes = 0;
    for (uint32_t r = 0; r < routes; r++)
    {
        uint8_t rec[0x48];
        if (SafeRead(gen + g_routeEdges + 0x48ull * r, rec, sizeof rec)) break;
        uint32_t first, end; float width; uint16_t flags;
        memcpy(&first, rec + 0x38, 4); memcpy(&end, rec + 0x3C, 4); memcpy(&width, rec + 0x30, 4); memcpy(&flags, rec + 0x44, 2);
        if (rec[0x34] == 2 || (flags & 0x103) || end <= first + 1 || end > points || !(width >= 0 && width < 200)) continue;
        for (uint32_t p = first; p < end; p += 8)
        {
            float xy[2];
            if (SafeRead(gen + g_routePoints + 8ull * p, xy, 8)) break;
            MACRO_FOR_NODES(xy[0], xy[1], width / 2 + MACRO_STEP, { if (g_macro.slope[n_] > sRoad) { g_macro.slope[n_] = sRoad; roadNodes++; } });
        }
    }
    /* the slopes: h_i <= h_j + s d and h_i >= h_j - s d over the 8 neighbours (s the smaller of the two limits), held nodes
     * fixed; Gauss-Seidel sweeps both ways until nothing moves */
    int sweeps = 0;
    for (; sweeps < 400; sweeps++)
    {
        float moved = 0;
        for (int dir = 0; dir < 2; dir++)
            for (int jj = 0; jj < MACRO_N; jj++)
                for (int ii = 0; ii < MACRO_N; ii++)
                {
                    int i = dir ? MACRO_N - 1 - ii : ii, j = dir ? MACRO_N - 1 - jj : jj;
                    size_t n = (size_t)j * MACRO_N + i;
                    if (g_macro.fixed[n]) continue;
                    float v = g_macro.h[n], lo = -1e9f, hi = 1e9f;
                    for (int dj = -1; dj <= 1; dj++)
                        for (int di = -1; di <= 1; di++)
                        {
                            if (!di && !dj) continue;
                            int a = i + di, b = j + dj;
                            if (a < 0 || b < 0 || a >= MACRO_N || b >= MACRO_N) continue;
                            size_t m = (size_t)b * MACRO_N + a;
                            float s = g_macro.slope[n] < g_macro.slope[m] ? g_macro.slope[n] : g_macro.slope[m];
                            float dd = (di && dj) ? d2 : d1;
                            if (g_macro.h[m] + s * dd < hi) hi = g_macro.h[m] + s * dd;
                            if (g_macro.h[m] - s * dd > lo) lo = g_macro.h[m] - s * dd;
                        }
                    float nv = v > hi ? hi : v;
                    if (nv < lo) nv = lo > hi ? (lo + hi) / 2 : lo;
                    if (nv < 0) nv = 0; if (nv > MACRO_MAX) nv = MACRO_MAX;
                    if (fabsf(nv - v) > moved) moved = fabsf(nv - v);
                    g_macro.h[n] = nv;
                }
        if (moved < 0.01f) break;
    }
    float lo = 1e9f, hi = -1e9f, steep = 0; double sum = 0;
    for (int j = 0; j < MACRO_N; j++)
        for (int i = 0; i < MACRO_N; i++)
        {
            float v = g_macro.h[(size_t)j * MACRO_N + i];
            if (v < lo) lo = v; if (v > hi) hi = v; sum += v;
            if (i + 1 < MACRO_N) { float s = fabsf(g_macro.h[(size_t)j * MACRO_N + i + 1] - v) / MACRO_STEP; if (s > steep) steep = s; }
            if (j + 1 < MACRO_N) { float s = fabsf(g_macro.h[(size_t)(j + 1) * MACRO_N + i] - v) / MACRO_STEP; if (s > steep) steep = s; }
        }
    QueryPerformanceCounter(&t1);
    g_macro.ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
    g_macro.level = g_world.levels; g_macro.ready = 1;
    Note("difficulty: relief: seed %08X; %.0f..%.0f m (mean %.1f), steepest %.1f deg; held at 0: %u wet and %u sea corners (sea %.1f m), %u absolute pads; "
        "held flat: %u locations, %u flatten zones; %u road nodes at %.0f deg; %d sweeps, %.0f ms (world %.0f m)", seed, (double)lo, (double)hi,
        sum / ((double)MACRO_N * MACRO_N), (double)(atanf(steep) * 180.0f / 3.14159265f), wet, low, (double)sea, absolute, held, flat, roadNodes,
        (double)MACRO_ROAD, sweeps, g_macro.ms, (double)worldL);
    (void)W;
    return 1;
}

/* One batch of the field's cells (2 triangles each, level pixels: x = g x samples / 16384, y up) and its job. */
static int MacroDraw(uint64_t context)
{
    typedef uint64_t (*MatFn)(uint64_t gui, uint64_t hash);
    typedef void (*TriFn)(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour, uint64_t material,
        const float* uv0, const float* uv1, const float* uv2);
    MatFn mat = (MatFn)NearApi(0xD0, 0x230);
    TriFn tri = (TriFn)NearApi(0xD0, 0x0);
    uintptr_t gen = 0, lg = 0;
    uint64_t R = 0, gui = 0, world = 0, settings = 0, queue = 0, camera = 0, generator = 0, r108 = 0;
    float samples = 0, colour[4] = {0};
    if (mat == NULL || tri == NULL || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || (lg = gen + 0x8758A8u, 0) || SafeRead(gen, &R, 8) || R == 0 ||
        SafeRead(R + 0x110, &gui, 8) || gui == 0 || SafeRead(context + 0x40, &world, 8) || world == 0 || SafeRead(context + 8, &settings, 8) || settings == 0 ||
        SafeRead(lg + 0x120, &samples, 4) || !(samples >= 1024.0f && samples <= 8192.0f) || SafeRead(context + 0x50, &queue, 8) || queue == 0 ||
        SafeRead(R, &camera, 8) || SafeRead(R + 0x28, &generator, 8) || SafeRead(R + 0x108, &r108, 8) || SafeRead(g_world.game + 0x23C8540u, colour, 16)) return 0;
    uint64_t material = mat(gui, 0x13DD2DCAD2968560ull);
    const float px = samples / 16384.0f;
    const uint32_t cells = (MACRO_N - 1) * (MACRO_N - 1);
    uint32_t end = g_macro.next + NEAR_BATCH / 2;
    if (end > cells) end = cells;
    for (uint32_t k = g_macro.next; k < end; k++)
    {
        int i = (int)(k % (MACRO_N - 1)), j = (int)(k / (MACRO_N - 1));
        float v[4][3];
        for (int q = 0; q < 4; q++)
        {
            int a = i + (q == 1 || q == 2), b = j + (q >= 2);
            v[q][0] = a * MACRO_STEP * px; v[q][1] = b * MACRO_STEP * px; v[q][2] = g_macro.h[(size_t)b * MACRO_N + a];
        }
        if (v[0][2] == 0 && v[1][2] == 0 && v[2][2] == 0 && v[3][2] == 0) continue;
        tri(gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
        tri(gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
    }
    g_macro.next = end;
    NearGuiJob(queue, world, camera, generator, r108, settings);
    return 1;
}

/* TerrainStep's end (the level at its 0x40, every composite tile drawn): 1 while the field is drawn (the level waits). */
static int MacroStep(uint64_t context)
{
    if (!MacroOn() || g_macro.drawn == (int)g_world.levels + 1) return 0;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return 0;
    if (g_macro.phase == 0)
    {
        g_macro.drawn = (int)g_world.levels + 1; /* once a level, whatever happens below */
        if (!MacroPlan(gen, context)) return 0;
        g_macro.drawn = 0;
        g_macro.next = 0; g_macro.waits = 0; g_macro.phase = 1;
    }
    if (g_macro.phase == 1)
    {
        uint32_t q = NearQueued(context);
        if (q != 0 && q != 0xFFFFFFFFu && ++g_macro.waits < 2400) return 1;
        g_macro.waits = 0;
        if (g_macro.next >= (MACRO_N - 1) * (MACRO_N - 1))
        {
            g_macro.phase = 0; g_macro.drawn = (int)g_world.levels + 1;
            Note("difficulty: relief: drawn into the level's merge (%u cells)", (MACRO_N - 1) * (MACRO_N - 1));
            return 0;
        }
        if (!MacroDraw(context))
        {
            g_macro.phase = 0; g_macro.drawn = (int)g_world.levels + 1; g_macro.ready = 0;
            Note("difficulty: relief: the level's merge is not there at its 0x40; no km-scale relief");
            return 0;
        }
        return 1;
    }
    return 0;
}

/* Before the level's first 0x6C: the stamp records' +0xC0 (height / 512) get h_macro / 512 once. */
static void MacroStamps(void)
{
    if (!g_macro.ready || g_macro.level != g_world.levels || g_macro.stampsLevel == g_world.levels) return;
    g_macro.stampsLevel = g_world.levels;
    uintptr_t gen = 0; uint32_t count = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x11A7108u, &count, 4)) return;
    if (count > DIF_LOCATIONS) count = DIF_LOCATIONS;
    uint32_t raised = 0; float most = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        uintptr_t rec = gen + 0x11A7280u + 0xE0ull * k;
        float xy[2], h = 0;
        if (SafeRead(rec, xy, 8) || SafeRead(rec + 0xC0, &h, 4) || !isfinite(xy[0]) || !isfinite(xy[1]) || !isfinite(h)) continue;
        float m = MacroAt(xy[0], xy[1]);
        if (m == 0) continue;
        *(volatile float*)(rec + 0xC0) = h + m / 512.0f;
        raised++; if (m > most) most = m;
    }
    Note("difficulty: relief: %u of %u stamp records raised before SpawnLevelLayers (up to %.0f m)", raised, count, (double)most);
}
